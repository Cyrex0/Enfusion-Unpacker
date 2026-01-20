/**
 * Enfusion Unpacker - XOB Parser Implementation
 * Based on Python xob_to_obj.py from enfusion_toolkit
 * 
 * XOB9 Format (IFF/FORM container):
 * - HEAD chunk: Materials + LOD descriptors (116 bytes each)
 * - LODS chunk: LZ4 block-compressed mesh data with dictionary chaining
 * - LOD regions stored in REVERSE order (LOD0 at END)
 * - Data layout per LOD: Index1 -> Index2 -> Positions -> Normals -> UVs
 */

#include "enfusion/xob_parser.hpp"
#include "enfusion/compression.hpp"
#include <lz4.h>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cfloat>

namespace enfusion {

// Helper to read big-endian uint32 (IFF chunk sizes)
static inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// Helper to read little-endian values
static inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static inline uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static inline float read_f32_le(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, sizeof(float));
    return f;
}

/**
 * LZ4 dictionary chaining decompression
 * CRITICAL: Dictionary MUST persist across ALL blocks (Python: decompress_lods)
 * 
 * The has_more flag only indicates logical segments, NOT dictionary boundaries.
 * We must continue decompression until we run out of data or hit a zero block.
 */
static std::vector<uint8_t> decompress_lz4_chained(const uint8_t* data, size_t size) {
    std::vector<uint8_t> result;
    result.reserve(size * 4);

    std::cerr << "[XOB] Decompressing LZ4 chained, input size=" << size << "\n";

    size_t pos = 0;
    std::vector<uint8_t> prev_dict;
    prev_dict.reserve(65536);
    int block_count = 0;

    while (pos < size) {
        if (pos + 4 > size) break;

        uint32_t header = read_u32_le(data + pos);
        pos += 4;

        uint32_t block_size = header & 0x7FFFFFFF;
        // Note: has_more flag (bit 31) is NOT used for decompression control
        // The dictionary MUST be maintained across ALL blocks without resetting

        if (block_size == 0) break;
        if (block_size > 0x20000) break;
        if (pos + block_size > size) break;

        std::vector<uint8_t> decompressed(65536);
        int dec_size;

        if (!prev_dict.empty()) {
            dec_size = LZ4_decompress_safe_usingDict(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                65536,
                reinterpret_cast<const char*>(prev_dict.data()),
                static_cast<int>(prev_dict.size())
            );
        } else {
            dec_size = LZ4_decompress_safe(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                65536
            );
        }

        pos += block_size;
        if (dec_size <= 0) {
            std::cerr << "[XOB] Block " << block_count << " decompression failed, dec_size=" << dec_size << "\n";
            break;
        }

        decompressed.resize(dec_size);
        result.insert(result.end(), decompressed.begin(), decompressed.end());
        
        // Use this block as dictionary for next (keep last 64KB max)
        if (decompressed.size() <= 65536) {
            prev_dict = std::move(decompressed);
        } else {
            prev_dict.assign(decompressed.end() - 65536, decompressed.end());
        }
        
        block_count++;
        // Do NOT break on has_more=false - continue until end of data
    }

    std::cerr << "[XOB] Decompressed " << block_count << " blocks, total output=" << result.size() << "\n";
    return result;
}

/**
 * LZO4 Descriptor structure (116 bytes each)
 * Based on Python parse_lzo4_descriptors()
 * 
 * IMPORTANT: LZO4 Descriptor layout (all offsets from "LZO4" marker):
 * - +0:   "LZO4" magic (4 bytes)
 * - +28:  Decompressed size (4 bytes) 
 * - +32:  Format flags (4 bytes) - upper byte bit 4 determines position stride
 * - +76:  Triangle count (2 bytes)
 * - +78:  Vertex count (2 bytes)
 * 
 * Position stride: 16 if upper byte bit 4 set, else 12
 */
struct LzoDescriptorInternal {
    uint32_t decomp_size;      // +28 from LZO4
    uint32_t format_flags;     // +32 from LZO4 (full 32-bit value)
    uint16_t triangle_count;   // +76 from LZO4
    uint16_t vertex_count;     // +78 from LZO4
    int position_stride;       // Calculated: 12 or 16 bytes
    uint8_t flag_byte;         // Low byte of format_flags
    bool has_normals;
    bool has_uvs;
    bool has_tangents;
    bool has_skinning;
    bool has_extra_normals;
};

static std::vector<LzoDescriptorInternal> parse_lzo4_descriptors(const uint8_t* data, size_t size) {
    std::vector<LzoDescriptorInternal> descriptors;
    
    std::cerr << "[XOB] Parsing LZO4 descriptors from HEAD chunk, size=" << size << "\n";
    
    // Search for LZO4 markers
    size_t pos = 0;
    while (pos + 4 <= size) {
        // Look for LZO4 marker
        size_t found = SIZE_MAX;
        for (size_t i = pos; i + 4 <= size; i++) {
            if (data[i] == 'L' && data[i+1] == 'Z' && 
                data[i+2] == 'O' && data[i+3] == '4') {
                found = i;
                break;
            }
        }
        
        if (found == SIZE_MAX) break;
        
        // Ensure we have enough data (need at least 80 bytes from LZO4 marker)
        if (found + 80 > size) break;
        
        LzoDescriptorInternal d;
        
        // All offsets are from the LZO4 marker position
        // Python: desc = head_data[idx:idx+116] where idx is position of "LZO4"
        // So desc[28:32] means offset 28 from LZO4, etc.
        
        // Decompressed size at offset +28 from LZO4
        d.decomp_size = read_u32_le(data + found + 28);
        
        // Format flags at offset +32 from LZO4
        d.format_flags = read_u32_le(data + found + 32);
        d.flag_byte = d.format_flags & 0xFF;
        
        // Parse attribute flags from low byte
        d.has_normals = (d.flag_byte & 0x02) != 0;
        d.has_uvs = (d.flag_byte & 0x04) != 0;
        d.has_tangents = (d.flag_byte & 0x08) != 0;
        d.has_skinning = (d.flag_byte & 0x10) != 0;
        d.has_extra_normals = (d.flag_byte & 0x20) != 0;
        
        // From xob_to_obj.py: triangle_count at +76, vertex_count at +78 from LZO4
        d.triangle_count = read_u16_le(data + found + 76);
        d.vertex_count = read_u16_le(data + found + 78);
        
        // Position stride: determined by bit 4 of upper byte of format_flags
        // Upper byte pattern: 0x0F, 0x2F, 0x8F, 0xAF → 12-byte stride (XYZ only)
        // Upper byte pattern: 0x1F, 0x3F, 0x9F, 0xBF → 16-byte stride (XYZW, W=0)
        uint8_t upper_byte = (d.format_flags >> 24) & 0xFF;
        d.position_stride = (upper_byte & 0x10) ? 16 : 12;
        
        std::cerr << "[XOB] LOD " << descriptors.size() << ": offset=" << found 
                  << " decomp=" << d.decomp_size
                  << " tris=" << d.triangle_count 
                  << " verts=" << d.vertex_count 
                  << " stride=" << d.position_stride
                  << " flags=0x" << std::hex << d.format_flags << std::dec << "\n";
        
        descriptors.push_back(d);
        pos = found + 4;
    }
    
    std::cerr << "[XOB] Found " << descriptors.size() << " LOD descriptors\n";
    return descriptors;
}

/**
 * Extract LOD region from decompressed data
 * LOD regions are stored in REVERSE order - LOD0 (highest detail) is at END
 */
static std::vector<uint8_t> extract_lod_region_internal(
    const std::vector<uint8_t>& decompressed,
    const std::vector<LzoDescriptorInternal>& descriptors,
    size_t lod_index
) {
    if (lod_index >= descriptors.size()) return {};
    
    // Calculate end position for this LOD (from end of data)
    size_t end_pos = decompressed.size();
    for (size_t i = 0; i < lod_index; i++) {
        end_pos -= descriptors[i].decomp_size;
    }
    
    size_t start_pos = end_pos - descriptors[lod_index].decomp_size;
    if (start_pos >= end_pos || end_pos > decompressed.size()) return {};
    
    return std::vector<uint8_t>(decompressed.begin() + start_pos, decompressed.begin() + end_pos);
}

/**
 * Parse mesh from LOD region
 * 
 * FIXED Layout from Python: Positions -> Normals(4) -> UVs(4) -> [Tangents(4)] -> [Extra(8)]
 * UVs come directly after normals, NOT after tangents.
 */
static bool parse_mesh_from_region(
    const std::vector<uint8_t>& region,
    uint16_t vertex_count,
    uint16_t triangle_count,
    int position_stride,
    XobMesh& mesh
) {
    std::cerr << "[XOB] parse_mesh_from_region: region_size=" << region.size() 
              << " verts=" << vertex_count << " tris=" << triangle_count 
              << " stride=" << position_stride << "\n";
    
    if (vertex_count == 0 || triangle_count == 0 || region.empty()) {
        std::cerr << "[XOB] Invalid parameters\n";
        return false;
    }
    
    uint32_t index_count = static_cast<uint32_t>(triangle_count) * 3;
    size_t idx_array_size = index_count * 2; // 2 bytes per index
    
    // CRITICAL: TWO index arrays before positions (Python: pos_offset = idx_size * 2)
    size_t pos_offset = idx_array_size * 2;
    
    std::cerr << "[XOB] index_count=" << index_count << " idx_array_size=" << idx_array_size 
              << " pos_offset=" << pos_offset << "\n";
    
    if (pos_offset >= region.size()) {
        std::cerr << "[XOB] pos_offset >= region.size()\n";
        return false;
    }
    
    // Extract indices (first array only - the actual vertex indices)
    mesh.indices.clear();
    mesh.indices.reserve(index_count);
    for (uint32_t i = 0; i < index_count && i * 2 + 1 < idx_array_size; i++) {
        uint16_t idx = read_u16_le(region.data() + i * 2);
        mesh.indices.push_back(static_cast<uint32_t>(idx));
    }
    
    // Validate indices
    uint32_t max_idx = 0;
    for (uint32_t idx : mesh.indices) {
        if (idx > max_idx) max_idx = idx;
    }
    std::cerr << "[XOB] max_idx=" << max_idx << "\n";
    if (max_idx >= vertex_count) {
        // Bad indices - clamp them
        for (uint32_t& idx : mesh.indices) {
            if (idx >= vertex_count) idx = 0;
        }
    }
    
    // Extract positions
    mesh.vertices.clear();
    mesh.vertices.reserve(vertex_count);
    
    for (uint32_t i = 0; i < vertex_count; i++) {
        size_t off = pos_offset + i * position_stride;
        if (off + 12 > region.size()) {
            // Pad remaining vertices
            XobVertex vert;
            vert.position = glm::vec3(0.0f);
            vert.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            vert.uv = glm::vec2(0.0f);
            mesh.vertices.push_back(vert);
            continue;
        }
        
        XobVertex vert;
        vert.position.x = read_f32_le(region.data() + off);
        vert.position.y = read_f32_le(region.data() + off + 4);
        vert.position.z = read_f32_le(region.data() + off + 8);
        vert.normal = glm::vec3(0.0f, 0.0f, 1.0f);
        vert.uv = glm::vec2(0.0f);
        mesh.vertices.push_back(vert);
    }
    
    // Normals start after positions (4 bytes per vertex - packed signed bytes)
    size_t normal_offset = pos_offset + vertex_count * position_stride;
    std::cerr << "[XOB] normal_offset=" << normal_offset << "\n";
    
    for (uint32_t i = 0; i < vertex_count && i < mesh.vertices.size(); i++) {
        size_t off = normal_offset + i * 4;
        if (off + 4 > region.size()) break;
        
        int8_t nx = static_cast<int8_t>(region[off]);
        int8_t ny = static_cast<int8_t>(region[off + 1]);
        int8_t nz = static_cast<int8_t>(region[off + 2]);
        
        glm::vec3 normal(
            static_cast<float>(nx) / 127.0f,
            static_cast<float>(ny) / 127.0f,
            static_cast<float>(nz) / 127.0f
        );
        
        // Normalize the normal vector
        float len = glm::length(normal);
        if (len > 0.001f) {
            normal /= len;
        } else {
            normal = glm::vec3(0.0f, 1.0f, 0.0f);  // Default up
        }
        
        mesh.vertices[i].normal = normal;
    }
    
    // UVs come directly after normals (FIXED layout: pos -> normals(4) -> UVs(8) -> tangent -> extra)
    // UVs are 8 bytes per vertex: 4 bytes for UV0 (diffuse), 4 bytes for UV1 (lightmap/unused)
    // We only need UV0 which is the first 4 bytes
    size_t uv_offset = normal_offset + vertex_count * 4;
    size_t uv_stride = 8;  // Two UV channels: UV0 (4 bytes) + UV1 (4 bytes)
    std::cerr << "[XOB] uv_offset=" << uv_offset << " uv_stride=" << uv_stride << " region_size=" << region.size() << "\n";
    
    size_t valid_uvs = 0;
    for (uint32_t i = 0; i < vertex_count && i < mesh.vertices.size(); i++) {
        size_t off = uv_offset + i * uv_stride;  // Use 8-byte stride
        if (off + 4 > region.size()) break;
        
        uint16_t u_raw = read_u16_le(region.data() + off);
        uint16_t v_raw = read_u16_le(region.data() + off + 2);
        
        mesh.vertices[i].uv.x = static_cast<float>(u_raw) / 65535.0f;
        mesh.vertices[i].uv.y = 1.0f - static_cast<float>(v_raw) / 65535.0f; // Flip V
        valid_uvs++;
        
        // Debug first few raw values
        if (i < 5) {
            std::cerr << "[XOB] UV[" << i << "] raw: u=" << u_raw << " v=" << v_raw 
                      << " -> (" << mesh.vertices[i].uv.x << ", " << mesh.vertices[i].uv.y << ")\n";
        }
    }
    std::cerr << "[XOB] Parsed " << valid_uvs << " UVs\n";
    
    // Debug: show first few UVs
    uint32_t debug_count = (vertex_count < 5) ? vertex_count : 5;
    for (uint32_t i = 0; i < debug_count && i < mesh.vertices.size(); i++) {
        std::cerr << "[XOB] UV[" << i << "] = (" << mesh.vertices[i].uv.x << ", " << mesh.vertices[i].uv.y << ")\n";
    }
    
    return !mesh.vertices.empty() && !mesh.indices.empty();
}

/**
 * Extract materials from HEAD chunk
 * Materials are stored with GUID pattern {16 hex chars} followed by path
 */
static std::vector<XobMaterial> extract_materials_from_head(const uint8_t* data, size_t size) {
    std::vector<XobMaterial> materials;
    
    // Look for pattern: '{' followed by 16 hex chars followed by '}'
    // Then the path follows until null terminator
    for (size_t i = 0; i + 20 < size; i++) {
        if (data[i] == '{') {
            // Check if next 16 chars are hex
            bool is_guid = true;
            for (size_t j = 1; j <= 16 && is_guid; j++) {
                char c = static_cast<char>(data[i + j]);
                is_guid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
            }
            
            if (is_guid && data[i + 17] == '}') {
                // Found a GUID, extract path after it
                size_t path_start = i + 18;
                size_t path_end = path_start;
                while (path_end < size && data[path_end] != '\0' && data[path_end] >= 32) {
                    path_end++;
                }
                
                if (path_end > path_start) {
                    std::string path(reinterpret_cast<const char*>(data + path_start), path_end - path_start);
                    
                    // Extract material name from path
                    std::string name = path;
                    size_t last_slash = name.rfind('/');
                    if (last_slash != std::string::npos) {
                        name = name.substr(last_slash + 1);
                    }
                    // Remove extensions
                    size_t ext_pos = name.rfind(".emat");
                    if (ext_pos != std::string::npos) name = name.substr(0, ext_pos);
                    ext_pos = name.rfind(".gamemat");
                    if (ext_pos != std::string::npos) name = name.substr(0, ext_pos);
                    
                    XobMaterial mat;
                    mat.name = name;
                    mat.path = path;
                    mat.diffuse_texture = path;
                    materials.push_back(mat);
                    
                    std::cerr << "[XOB] Found material " << materials.size()-1 << ": " << name << " path=" << path << "\n";
                    
                    i = path_end - 1; // Skip past this material
                }
            }
        }
    }
    
    return materials;
}

/**
 * Parse material-to-triangle ranges from XOB HEAD chunk.
 * 
 * Based on Python parse_material_triangle_ranges():
 * Each material entry (except material 0) has a 0xFFFF marker preceded by:
 * - tri_start: uint16 at offset -10 (starting triangle index)
 * - mat_idx: uint16 at offset +2 (material index after 0xFFFF)
 * 
 * Material 0 covers triangles from 0 to the minimum tri_start of other materials.
 */
static std::vector<MaterialRange> extract_material_ranges(const uint8_t* data, size_t size, uint32_t total_triangles) {
    std::vector<MaterialRange> result;
    
    // Find HEAD chunk start and LZO4 position
    const uint8_t* head_start = nullptr;
    size_t head_size = 0;
    for (size_t i = 0; i + 8 < size; i++) {
        if (data[i] == 'H' && data[i+1] == 'E' && data[i+2] == 'A' && data[i+3] == 'D') {
            head_size = read_u32_be(data + i + 4);
            head_start = data + i + 8;
            break;
        }
    }
    if (!head_start || head_size == 0) return result;
    
    // Count LZO4 descriptors
    size_t lzo4_count = 0;
    for (size_t i = 0; i + 4 <= head_size; i++) {
        if (head_start[i] == 'L' && head_start[i+1] == 'Z' && 
            head_start[i+2] == 'O' && head_start[i+3] == '4') {
            lzo4_count++;
            i += 115; // Skip to next potential LZO4
        }
    }
    
    if (lzo4_count == 0) return result;
    
    // Find first LZO4 and skip past ALL LZO4 descriptors
    size_t lzo4_pos = SIZE_MAX;
    for (size_t i = 0; i + 4 <= head_size; i++) {
        if (head_start[i] == 'L' && head_start[i+1] == 'Z' && 
            head_start[i+2] == 'O' && head_start[i+3] == '4') {
            lzo4_pos = i;
            break;
        }
    }
    if (lzo4_pos == SIZE_MAX) return result;
    
    // Data after LZO4 descriptors
    size_t after_start = lzo4_pos + (116 * lzo4_count);
    if (after_start >= head_size) return result;
    
    const uint8_t* after = head_start + after_start;
    size_t after_size = head_size - after_start;
    
    // Find all 0xFFFF markers
    struct RangeEntry {
        uint32_t mat_idx;
        uint32_t tri_start;
    };
    std::vector<RangeEntry> entries;
    
    for (size_t pos = 10; pos + 4 <= after_size; pos++) {
        if (after[pos] == 0xFF && after[pos+1] == 0xFF) {
            // Parse structure: tri_start at -10, mat_idx at +2
            uint16_t tri_start = read_u16_le(after + pos - 10);
            uint16_t mat_idx = read_u16_le(after + pos + 2);
            
            // Filter garbage values
            if (tri_start <= total_triangles) {
                entries.push_back({mat_idx, tri_start});
                std::cerr << "[XOB] Material range: mat=" << mat_idx << " tri_start=" << tri_start << "\n";
            }
            pos += 3;
        }
    }
    
    // Sort by tri_start
    std::sort(entries.begin(), entries.end(), 
              [](const RangeEntry& a, const RangeEntry& b) { return a.tri_start < b.tri_start; });
    
    // Build result with tri_end calculated
    // Material 0 covers 0 to min(other tri_starts)
    if (!entries.empty()) {
        uint32_t min_start = entries[0].tri_start;
        if (min_start > 0) {
            MaterialRange r0;
            r0.material_index = 0;
            r0.triangle_start = 0;
            r0.triangle_end = min_start;
            r0.triangle_count = min_start;
            result.push_back(r0);
        }
        
        for (size_t i = 0; i < entries.size(); i++) {
            MaterialRange r;
            r.material_index = entries[i].mat_idx;
            r.triangle_start = entries[i].tri_start;
            r.triangle_end = (i + 1 < entries.size()) ? entries[i+1].tri_start : total_triangles;
            r.triangle_count = r.triangle_end - r.triangle_start;
            result.push_back(r);
        }
    } else {
        // Only material 0 - covers all triangles
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
    }
    
    std::cerr << "[XOB] Total material ranges: " << result.size() << "\n";
    return result;
}

XobParser::XobParser(std::span<const uint8_t> data) : data_(data) {}

std::optional<std::span<const uint8_t>> XobParser::find_chunk(const uint8_t* chunk_id) const {
    if (data_.size() < 12) return std::nullopt;
    
    // Simple search for chunk magic (like Python does with data.find(b'LODS'))
    // This is more robust than IFF iteration which can fail due to alignment issues
    for (size_t pos = 12; pos + 8 <= data_.size(); pos++) {
        if (std::memcmp(data_.data() + pos, chunk_id, 4) == 0) {
            uint32_t chunk_size = read_u32_be(data_.data() + pos + 4);
            
            // Sanity check
            if (chunk_size > 0 && chunk_size < 100000000 && pos + 8 + chunk_size <= data_.size()) {
                std::cerr << "[XOB] Found chunk '" << (char)chunk_id[0] << (char)chunk_id[1] 
                          << (char)chunk_id[2] << (char)chunk_id[3] << "' at pos=" << pos 
                          << " size=" << chunk_size << "\n";
                return data_.subspan(pos + 8, chunk_size);
            }
        }
    }
    
    return std::nullopt;
}

std::optional<XobMesh> XobParser::parse(uint32_t target_lod) {
    if (data_.size() < 12) return std::nullopt;
    
    // Verify FORM header
    if (std::memcmp(data_.data(), "FORM", 4) != 0) return std::nullopt;
    
    // Check XOB type
    const char* form_type = reinterpret_cast<const char*>(data_.data() + 8);
    if (form_type[0] != 'X' || form_type[1] != 'O' || form_type[2] != 'B') {
        return std::nullopt;
    }
    
    // Find HEAD chunk
    static constexpr uint8_t HEAD_ID[4] = {'H', 'E', 'A', 'D'};
    auto head_chunk = find_chunk(HEAD_ID);
    if (!head_chunk) {
        std::cerr << "[XOB] Failed to find HEAD chunk\n";
        return std::nullopt;
    }
    
    auto head_data = *head_chunk;
    std::cerr << "[XOB] HEAD chunk size=" << head_data.size() << "\n";
    
    // Extract materials from HEAD chunk
    materials_ = extract_materials_from_head(head_data.data(), head_data.size());
    
    // Parse LZO4 descriptors from HEAD chunk
    auto descriptors = parse_lzo4_descriptors(head_data.data(), head_data.size());
    if (descriptors.empty()) {
        std::cerr << "[XOB] No LZO4 descriptors found\n";
        return std::nullopt;
    }
    
    // Validate LOD index
    if (target_lod >= descriptors.size()) {
        target_lod = 0;
    }
    
    // Get descriptor for target LOD
    const auto& desc = descriptors[target_lod];
    if (desc.vertex_count == 0 || desc.triangle_count == 0) {
        std::cerr << "[XOB] Invalid descriptor: vertex_count=" << desc.vertex_count 
                  << " triangle_count=" << desc.triangle_count << "\n";
        return std::nullopt;
    }
    
    // Find LODS chunk
    static constexpr uint8_t LODS_ID[4] = {'L', 'O', 'D', 'S'};
    auto lods_chunk = find_chunk(LODS_ID);
    if (!lods_chunk) {
        std::cerr << "[XOB] Failed to find LODS chunk\n";
        return std::nullopt;
    }
    std::cerr << "[XOB] LODS chunk size=" << lods_chunk->size() << "\n";
    
    // Decompress LODS data with dictionary chaining
    auto decompressed = decompress_lz4_chained(lods_chunk->data(), lods_chunk->size());
    if (decompressed.empty()) {
        std::cerr << "[XOB] Decompression failed\n";
        return std::nullopt;
    }
    
    // Extract LOD region (REVERSE order - LOD0 at END)
    auto region = extract_lod_region_internal(decompressed, descriptors, target_lod);
    if (region.empty()) {
        std::cerr << "[XOB] Failed to extract LOD region\n";
        return std::nullopt;
    }
    std::cerr << "[XOB] LOD region size=" << region.size() << "\n";
    
    // Parse mesh from region
    XobMesh mesh;
    if (!parse_mesh_from_region(region, desc.vertex_count, desc.triangle_count, 
                                 desc.position_stride, mesh)) {
        std::cerr << "[XOB] Failed to parse mesh from region\n";
        return std::nullopt;
    }
    std::cerr << "[XOB] Mesh parsed: verts=" << mesh.vertices.size() 
              << " indices=" << mesh.indices.size() << "\n";
    
    // Calculate bounds
    mesh.bounds_min = glm::vec3(FLT_MAX);
    mesh.bounds_max = glm::vec3(-FLT_MAX);
    for (const auto& vert : mesh.vertices) {
        mesh.bounds_min = glm::min(mesh.bounds_min, vert.position);
        mesh.bounds_max = glm::max(mesh.bounds_max, vert.position);
    }
    
    // Create LOD entry
    XobLod lod_data;
    lod_data.distance = 0.0f;
    lod_data.index_offset = 0;
    lod_data.index_count = static_cast<uint32_t>(mesh.indices.size());
    lod_data.indices = mesh.indices;
    mesh.lods.push_back(lod_data);
    
    // Copy materials to mesh
    mesh.materials = materials_;
    
    // Extract material ranges (which triangles use which material)
    mesh.material_ranges = extract_material_ranges(data_.data(), data_.size(), desc.triangle_count);
    std::cerr << "[XOB] Material ranges assigned: " << mesh.material_ranges.size() << "\n";
    
    return mesh;
}

std::vector<LzoDescriptor> XobParser::parse_descriptors(std::span<const uint8_t> head_data) {
    return descriptors_;
}

std::vector<XobMaterial> XobParser::parse_materials(std::span<const uint8_t> head_data) {
    return materials_;
}

std::vector<MaterialRange> XobParser::parse_material_ranges(std::span<const uint8_t> head_data, uint32_t triangle_count) {
    return {};
}

std::span<const uint8_t> XobParser::extract_lod_region(std::span<const uint8_t> decompressed, uint32_t lod) {
    return {};
}

std::optional<XobMesh> XobParser::parse_mesh_region(std::span<const uint8_t> region, const LzoDescriptor& desc) {
    return std::nullopt;
}

void XobParser::calculate_bounds(XobMesh& mesh) {
    // Already done in parse()
}

} // namespace enfusion


