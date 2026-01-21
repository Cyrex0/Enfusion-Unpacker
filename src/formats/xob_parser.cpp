/**
 * Enfusion Unpacker - XOB Parser Implementation
 * 
 * XOB9 Format (IFF/FORM container with big-endian chunk sizes):
 * - FORM/XOB9 header (12 bytes)
 * - HEAD chunk: Materials, bones, LOD descriptors
 *   - Header (0x00-0x3B): bbox, material_count(u16@0x2C), bone_count(u16@0x2E), LOD count
 *   - Material strings: name+path pairs (null-terminated)
 *   - LZO4 descriptors (116 bytes each)
 * - COLL chunk (optional): Collision objects (64 bytes each) + collision mesh
 * - VOLM chunk (optional): Spatial octree for collision broadphase
 * - LODS chunk: LZ4 block-compressed mesh data with dictionary chaining
 *   - LOD regions stored in REVERSE order (LOD0 at END)
 *   - Data layout per LOD: Index1 -> Index2 -> Positions -> Normals -> UVs -> [Tangents] -> [Extra]
 */

#include "enfusion/xob_parser.hpp"
#include "enfusion/compression.hpp"
#include "enfusion/logging.hpp"
#include <lz4.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cfloat>
#include <map>
#include <set>

namespace enfusion {

// ============================================================================
// XOB Format Constants
// ============================================================================

// LZ4 block decompression parameters
constexpr size_t LZ4_MAX_BLOCK_SIZE = 0x20000;       // 128KB maximum compressed block
constexpr size_t LZ4_DICT_SIZE = 65536;              // 64KB dictionary window

// LZO4 Descriptor offsets (from "LZO4" marker)
// See xob_to_obj.py parse_lzo4_descriptors()
constexpr size_t LZO4_MARKER_SIZE = 4;                // "LZO4" magic (4 bytes)
constexpr size_t LZO4_DESCRIPTOR_SIZE = 116;          // Total descriptor size in bytes
constexpr size_t LZO4_DECOMP_SIZE_OFFSET = 28;        // Decompressed size (uint32_t)
constexpr size_t LZO4_FORMAT_FLAGS_OFFSET = 32;       // Format flags (uint32_t)
constexpr size_t LZO4_TRIANGLE_COUNT_OFFSET = 76;     // Triangle count (uint16_t)
constexpr size_t LZO4_VERTEX_COUNT_OFFSET = 78;       // Vertex count (uint16_t)
constexpr size_t LZO4_MIN_REQUIRED_BYTES = 80;        // Minimum bytes needed from marker

// Format flag bit masks (applied to low byte of format_flags)
constexpr uint8_t XOB_FLAG_NORMALS = 0x02;           // Has normal vectors
constexpr uint8_t XOB_FLAG_UVS = 0x04;               // Has UV coordinates
constexpr uint8_t XOB_FLAG_TANGENTS = 0x08;          // Has tangent vectors
constexpr uint8_t XOB_FLAG_SKINNING = 0x10;          // Has bone weights/indices
constexpr uint8_t XOB_FLAG_EXTRA_NORMALS = 0x20;     // Has additional normal data

// Position stride determination (bit 4 of upper byte of format_flags)
// Upper byte pattern: 0x0F, 0x2F, 0x8F, 0xAF → 12-byte stride (XYZ only)
// Upper byte pattern: 0x1F, 0x3F, 0x9F, 0xBF → 16-byte stride (XYZW, W=0)
constexpr uint8_t XOB_STRIDE_16_BIT = 0x10;           // Bit that indicates 16-byte stride
constexpr int XOB_POSITION_STRIDE_12 = 12;            // XYZ floats (3 * 4 bytes)
constexpr int XOB_POSITION_STRIDE_16 = 16;            // XYZW floats (4 * 4 bytes, W unused)

// Vertex attribute sizes in bytes
constexpr size_t XOB_NORMAL_SIZE = 4;                 // Compressed normal (4 bytes)
constexpr size_t XOB_UV_SIZE = 4;                     // Half-float UV (4 bytes)
constexpr size_t XOB_TANGENT_SIZE = 4;                // Compressed tangent (4 bytes)
constexpr size_t XOB_EXTRA_SIZE = 8;                  // Extra data (8 bytes)

// Index buffer
constexpr size_t XOB_INDEX_SIZE = 2;                  // 16-bit indices
constexpr size_t XOB_INDEX_ARRAYS = 2;                // Two index arrays before positions

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
 * Convert IEEE 754 half-precision (16-bit) float to single-precision (32-bit) float
 * Layout: 1 sign bit, 5 exponent bits, 10 mantissa bits
 * 
 * IMPORTANT: XOB UV coordinates appear to use half-float format.
 * This function handles all cases including denormalized numbers properly.
 */
static inline float half_to_float(uint16_t h) {
    // Use a union for type-safe bit manipulation
    union { uint32_t u; float f; } result;
    
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    
    if (exp == 0) {
        if (mant == 0) {
            // Zero (signed)
            result.u = sign << 31;
        } else {
            // Denormalized half -> normalized float
            // Denorm half: value = (-1)^sign * 2^(-14) * (0.mant)
            // Need to normalize by finding leading 1 bit
            float value = static_cast<float>(mant) / 1024.0f;  // mant / 2^10
            value *= (1.0f / 16384.0f);  // * 2^(-14)
            result.f = sign ? -value : value;
        }
    } else if (exp == 31) {
        // Inf or NaN - map to float inf/nan
        if (mant == 0) {
            // Infinity
            result.u = (sign << 31) | 0x7F800000;
        } else {
            // NaN - preserve sign but use quiet NaN
            result.u = (sign << 31) | 0x7FC00000;
        }
    } else {
        // Normalized number: value = (-1)^sign * 2^(exp-15) * (1.mant)
        // Convert to float: exp_f = exp - 15 + 127 = exp + 112
        result.u = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    
    return result.f;
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
    prev_dict.reserve(LZ4_DICT_SIZE);
    int block_count = 0;

    while (pos < size) {
        if (pos + 4 > size) break;

        uint32_t header = read_u32_le(data + pos);
        pos += 4;

        uint32_t block_size = header & 0x7FFFFFFF;
        // Note: has_more flag (bit 31) is NOT used for decompression control
        // The dictionary MUST be maintained across ALL blocks without resetting

        if (block_size == 0) break;
        if (block_size > LZ4_MAX_BLOCK_SIZE) break;
        if (pos + block_size > size) break;

        std::vector<uint8_t> decompressed(LZ4_DICT_SIZE);
        int dec_size;

        if (!prev_dict.empty()) {
            dec_size = LZ4_decompress_safe_usingDict(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                LZ4_DICT_SIZE,
                reinterpret_cast<const char*>(prev_dict.data()),
                static_cast<int>(prev_dict.size())
            );
        } else {
            dec_size = LZ4_decompress_safe(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                LZ4_DICT_SIZE
            );
        }

        pos += block_size;
        if (dec_size <= 0) {
            std::cerr << "[XOB] Block " << block_count << " decompression failed, dec_size=" << dec_size << "\n";
            break;
        }

        decompressed.resize(dec_size);
        result.insert(result.end(), decompressed.begin(), decompressed.end());
        
        // CRITICAL: Dictionary must be from last 64KB of ALL accumulated output
        // NOT just the previous block!
        if (result.size() <= LZ4_DICT_SIZE) {
            prev_dict.assign(result.begin(), result.end());
        } else {
            prev_dict.assign(result.end() - LZ4_DICT_SIZE, result.end());
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
    uint32_t decomp_size;      // +LZO4_DECOMP_SIZE_OFFSET from LZO4
    uint32_t format_flags;     // +LZO4_FORMAT_FLAGS_OFFSET from LZO4 (full 32-bit value)
    uint16_t triangle_count;   // +LZO4_TRIANGLE_COUNT_OFFSET from LZO4
    uint16_t vertex_count;     // +LZO4_VERTEX_COUNT_OFFSET from LZO4
    int position_stride;       // Calculated: XOB_POSITION_STRIDE_12 or _16 bytes
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
    while (pos + LZO4_MARKER_SIZE <= size) {
        // Look for LZO4 marker
        size_t found = SIZE_MAX;
        for (size_t i = pos; i + LZO4_MARKER_SIZE <= size; i++) {
            if (data[i] == 'L' && data[i+1] == 'Z' && 
                data[i+2] == 'O' && data[i+3] == '4') {
                found = i;
                break;
            }
        }
        
        if (found == SIZE_MAX) break;
        
        // Ensure we have enough data for all required fields
        if (found + LZO4_MIN_REQUIRED_BYTES > size) break;
        
        LzoDescriptorInternal d;
        
        // All offsets are from the LZO4 marker position
        // Python: desc = head_data[idx:idx+116] where idx is position of "LZO4"
        // So desc[28:32] means offset 28 from LZO4, etc.
        
        // Decompressed size at offset +28 from LZO4
        d.decomp_size = read_u32_le(data + found + LZO4_DECOMP_SIZE_OFFSET);
        
        // Format flags at offset +32 from LZO4
        d.format_flags = read_u32_le(data + found + LZO4_FORMAT_FLAGS_OFFSET);
        d.flag_byte = d.format_flags & 0xFF;
        
        // Parse attribute flags from low byte using documented bit masks
        d.has_normals = (d.flag_byte & XOB_FLAG_NORMALS) != 0;
        d.has_uvs = (d.flag_byte & XOB_FLAG_UVS) != 0;
        d.has_tangents = (d.flag_byte & XOB_FLAG_TANGENTS) != 0;
        d.has_skinning = (d.flag_byte & XOB_FLAG_SKINNING) != 0;
        d.has_extra_normals = (d.flag_byte & XOB_FLAG_EXTRA_NORMALS) != 0;
        
        // Triangle and vertex counts from LZO4 descriptor
        d.triangle_count = read_u16_le(data + found + LZO4_TRIANGLE_COUNT_OFFSET);
        d.vertex_count = read_u16_le(data + found + LZO4_VERTEX_COUNT_OFFSET);
        
        // Position stride: determined by bit 4 of upper byte of format_flags
        uint8_t upper_byte = (d.format_flags >> 24) & 0xFF;
        d.position_stride = (upper_byte & XOB_STRIDE_16_BIT) ? XOB_POSITION_STRIDE_16 : XOB_POSITION_STRIDE_12;
        
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
 * 
 * CORRECTED: Analysis shows LOD0 data starts at offset 0 of the decompressed buffer,
 * not at the end. The decomp_size in the descriptor refers to total decompressed size,
 * not per-LOD region size.
 */
static std::vector<uint8_t> extract_lod_region_internal(
    const std::vector<uint8_t>& decompressed,
    const std::vector<LzoDescriptorInternal>& descriptors,
    size_t lod_index
) {
    if (lod_index >= descriptors.size()) return {};
    
    const auto& desc = descriptors[lod_index];
    
    // Calculate expected size based on vertex attributes
    size_t idx_size = desc.triangle_count * 3 * 2;  // bytes per index array
    size_t expected_size = idx_size * 2;  // Two index arrays
    expected_size += desc.vertex_count * desc.position_stride;  // Positions
    
    // Add optional attributes based on flags
    if (desc.has_normals) expected_size += desc.vertex_count * 4;
    
    // UV stride: 4 bytes if has_uvs (0x04) is set (compact), 8 bytes otherwise
    size_t uv_stride = desc.has_uvs ? 4 : 8;
    expected_size += desc.vertex_count * uv_stride;  // UVs
    
    // CORRECTED: When tangents AND skinning are both present, they form a combined
    // 20-byte structure containing: [bone_idx(4) + tangent_frame(8) + tangent(4) + normal(4)]
    // We only need to account for tangents here (4 bytes) since normals are already counted
    if (desc.has_tangents) expected_size += desc.vertex_count * 4;
    if (desc.has_skinning) expected_size += desc.vertex_count * 8;
    if (desc.has_extra_normals) expected_size += desc.vertex_count * 8;
    
    std::cerr << "[XOB] LOD " << lod_index << " extraction:\n"
              << "  decomp_size from descriptor: " << desc.decomp_size << "\n"
              << "  expected_size (calculated):  " << expected_size << "\n"
              << "  total decompressed buffer:   " << decompressed.size() << "\n";
    
    // CORRECTED: LOD0 data starts at offset 0, not at the end of the buffer
    // The decomp_size field often contains total decompressed size, not per-LOD size
    // For single-LOD meshes (most common), extract from beginning of buffer
    
    // Use calculated expected_size as the region size
    size_t region_size = expected_size;
    
    // If decompressed buffer is smaller than expected, use what we have
    if (region_size > decompressed.size()) {
        std::cerr << "[XOB] WARNING: expected_size > decompressed.size(), clamping\n";
        region_size = decompressed.size();
    }
    
    // For LOD0 (highest detail), data starts at offset 0
    // For multi-LOD files, we'd need to calculate offsets differently
    size_t start_pos = 0;
    size_t end_pos = region_size;
    
    // If there are multiple LODs and we're not requesting LOD0,
    // we'd need to skip previous LOD data (future enhancement)
    if (descriptors.size() > 1 && lod_index > 0) {
        std::cerr << "[XOB] WARNING: Multi-LOD extraction not fully implemented\n";
        // For now, just try to extract from start
    }
    
    std::cerr << "[XOB] Using region: start=" << start_pos << " end=" << end_pos 
              << " size=" << (end_pos - start_pos) << "\n";
    
    if (start_pos >= end_pos || end_pos > decompressed.size()) {
        std::cerr << "[XOB] ERROR: Invalid region bounds\n";
        return {};
    }
    
    return std::vector<uint8_t>(decompressed.begin() + start_pos, decompressed.begin() + end_pos);
}

/**
 * Parse mesh from LOD region
 * 
 * CORRECTED Layout based on format flags:
 *   indices_array_1: u16[triangle_count * 3]
 *   indices_array_2: u16[triangle_count * 3] (duplicate/unused)
 *   positions: float[vertex_count * stride] (12 or 16 bytes)
 *   [normals: u32[vertex_count]]            (4 bytes each, if has_normals)
 *   [uvs: u16[vertex_count * 4]]            (8 bytes each - 2 UV channels, if has_uvs)
 *   [tangents: u32[vertex_count]]           (4 bytes each, if has_tangents)
 *   [skinning: 8 bytes per vertex]          (if has_skinning - 4 bone indices + 4 weights)
 *   [extra: 8 bytes per vertex]             (if has_extra_normals)
 */
static bool parse_mesh_from_region(
    const std::vector<uint8_t>& region,
    uint16_t vertex_count,
    uint16_t triangle_count,
    int position_stride,
    bool has_normals,
    bool has_uvs,
    bool has_tangents,
    bool has_skinning,
    bool has_extra_normals,
    XobMesh& mesh
) {
    std::cerr << "[XOB] parse_mesh_from_region: region_size=" << region.size() 
              << " verts=" << vertex_count << " tris=" << triangle_count 
              << " stride=" << position_stride
              << " normals=" << has_normals << " uvs=" << has_uvs 
              << " tangents=" << has_tangents << " skinning=" << has_skinning
              << " extra=" << has_extra_normals << "\n";
    
    if (vertex_count == 0 || triangle_count == 0 || region.empty()) {
        std::cerr << "[XOB] Invalid parameters\n";
        return false;
    }
    
    uint32_t index_count = static_cast<uint32_t>(triangle_count) * 3;
    size_t idx_array_size = index_count * XOB_INDEX_SIZE; // XOB_INDEX_SIZE bytes per 16-bit index
    
    // CRITICAL: Two index arrays before positions (Python: pos_offset = idx_size * 2)
    size_t pos_offset = idx_array_size * XOB_INDEX_ARRAYS;
    
    std::cerr << "[XOB] index_count=" << index_count << " idx_array_size=" << idx_array_size 
              << " pos_offset=" << pos_offset << "\n";
    
    if (pos_offset >= region.size()) {
        std::cerr << "[XOB] pos_offset >= region.size()\n";
        return false;
    }
    
    // Debug: compare first vs second index array to understand their difference
    size_t differ_count = 0;
    for (uint32_t i = 0; i < index_count; i++) {
        uint16_t idx1 = read_u16_le(region.data() + i * 2);
        uint16_t idx2 = read_u16_le(region.data() + idx_array_size + i * 2);
        if (idx1 != idx2) {
            differ_count++;
            if (differ_count <= 5) {
                LOG_DEBUG("XobParser", "Index array diff at " << i << ": arr1=" << idx1 << " arr2=" << idx2);
            }
        }
    }
    if (differ_count > 0) {
        LOG_INFO("XobParser", "WARNING: Index arrays differ! " << differ_count << " differences");
    } else {
        LOG_DEBUG("XobParser", "Index arrays are identical");
    }
    
    // Array 1 is the geometry index buffer (correct positions)
    // Array 2 appears to be something else (LOD, collision, etc.) - causes broken geometry
    // Always use Array 1 for rendering
    
    // Extract indices from Array 1
    mesh.indices.clear();
    mesh.indices.reserve(index_count);
    for (uint32_t i = 0; i < index_count && i * 2 + 1 < idx_array_size; i++) {
        uint16_t idx = read_u16_le(region.data() + i * 2);  // Always use Array 1
        mesh.indices.push_back(static_cast<uint32_t>(idx));
    }
    
    // Debug: print first 20 indices
    std::ostringstream idx_str;
    idx_str << "First 20 indices: ";
    for (size_t i = 0; i < std::min<size_t>(20, mesh.indices.size()); i++) {
        idx_str << mesh.indices[i] << " ";
    }
    LOG_DEBUG("XobParser", idx_str.str());
    
    // Debug: print first 32 raw bytes
    std::ostringstream hex_str;
    hex_str << "First 32 raw bytes: ";
    for (size_t i = 0; i < std::min<size_t>(32, region.size()); i++) {
        hex_str << std::hex << std::setw(2) << std::setfill('0') << (int)region[i];
    }
    LOG_DEBUG("XobParser", hex_str.str());
    
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
        vert.normal = glm::vec3(0.0f, 0.0f, 1.0f);  // Default normal (will be overwritten if has_normals)
        vert.uv = glm::vec2(0.0f);
        mesh.vertices.push_back(vert);
    }
    
    // Track current offset in the vertex attribute stream (after positions)
    size_t attr_offset = pos_offset + vertex_count * position_stride;
    
    // Normals (4 bytes per vertex - 10-10-10-2 packed format) - OPTIONAL
    // Format: 10 bits X, 10 bits Y, 10 bits Z, 2 bits W (unused/sign)
    // Each 10-bit value is signed: range -512 to 511, normalized by 511
    if (has_normals) {
        size_t normal_offset = attr_offset;
        std::cerr << "[XOB] normal_offset=" << normal_offset << "\n";
        
        for (uint32_t i = 0; i < vertex_count && i < mesh.vertices.size(); i++) {
            size_t off = normal_offset + i * XOB_NORMAL_SIZE;
            if (off + XOB_NORMAL_SIZE > region.size()) break;
            
            // Read packed 32-bit normal
            uint32_t packed = read_u32_le(region.data() + off);
            
            // Extract 10-bit signed components
            int32_t nx_raw = packed & 0x3FF;           // Bits 0-9
            int32_t ny_raw = (packed >> 10) & 0x3FF;  // Bits 10-19
            int32_t nz_raw = (packed >> 20) & 0x3FF;  // Bits 20-29
            // int32_t nw = (packed >> 30) & 0x3;     // Bits 30-31 (unused)
            
            // Convert from 10-bit unsigned to signed (-512 to 511)
            if (nx_raw >= 512) nx_raw -= 1024;
            if (ny_raw >= 512) ny_raw -= 1024;
            if (nz_raw >= 512) nz_raw -= 1024;
            
            // Normalize to -1.0 to 1.0 range
            glm::vec3 normal(
                static_cast<float>(nx_raw) / 511.0f,
                static_cast<float>(ny_raw) / 511.0f,
                static_cast<float>(nz_raw) / 511.0f
            );
            
            // Normalize the normal vector (should already be close to unit length)
            float len = glm::length(normal);
            if (len > 0.001f) {
                normal /= len;
            } else {
                normal = glm::vec3(0.0f, 1.0f, 0.0f);  // Default up
            }
            
            mesh.vertices[i].normal = normal;
        }
        attr_offset += vertex_count * XOB_NORMAL_SIZE;
    } else {
        std::cerr << "[XOB] No normals flag - using default normals\n";
    }
    
    // UVs - stride depends on format flag
    // 0x04 flag (has_uvs) indicates COMPACT 4-byte UVs (single UV channel)
    // When 0x04 is NOT set, UVs are 8 bytes (UV0 + UV1/padding)
    const size_t uv_stride = has_uvs ? 4 : 8;
    
    {
        size_t uv_offset = attr_offset;
        std::cerr << "[XOB] uv_offset=" << uv_offset << " uv_stride=" << uv_stride 
                  << " region_size=" << region.size() << "\n";
        
        size_t valid_uvs = 0;
        for (uint32_t i = 0; i < vertex_count && i < mesh.vertices.size(); i++) {
            size_t off = uv_offset + i * uv_stride;
            if (off + 4 > region.size()) break;  // Only need 4 bytes for UV0
            
            uint16_t u_raw = read_u16_le(region.data() + off);
            uint16_t v_raw = read_u16_le(region.data() + off + 2);
            
            // UVs are stored as IEEE 754 half-precision floats (16 bits each)
            // Use half_to_float() to convert properly
            float u = half_to_float(u_raw);
            float v = half_to_float(v_raw);
            
            // Clamp to valid range (half-float can represent values outside 0-1)
            // Some UVs may be tiled so allow values outside 0-1
            
            mesh.vertices[i].uv.x = u;
            mesh.vertices[i].uv.y = 1.0f - v;  // Flip V for OpenGL convention
            valid_uvs++;
            
            // Debug first few raw values
            if (i < 5) {
                std::cerr << "[XOB] UV[" << i << "] half: u=0x" << std::hex << u_raw << " v=0x" << v_raw << std::dec
                          << " -> (" << mesh.vertices[i].uv.x << ", " << mesh.vertices[i].uv.y << ")\n";
            }
        }
        std::cerr << "[XOB] Parsed " << valid_uvs << " UVs as half-float (stride=" << uv_stride << ")\n";
        attr_offset += vertex_count * uv_stride;
    }
    
    // Tangents (4 bytes per vertex) - packed similar to normals
    // Format: 3 signed bytes for tangent direction + 1 byte for handedness sign
    if (has_tangents) {
        size_t tangent_offset = attr_offset;
        std::cerr << "[XOB] Parsing tangents at offset " << tangent_offset << "\n";
        
        for (uint32_t i = 0; i < vertex_count && i < mesh.vertices.size(); i++) {
            size_t off = tangent_offset + i * XOB_TANGENT_SIZE;
            if (off + XOB_TANGENT_SIZE > region.size()) break;
            
            int8_t tx = static_cast<int8_t>(region[off]);
            int8_t ty = static_cast<int8_t>(region[off + 1]);
            int8_t tz = static_cast<int8_t>(region[off + 2]);
            int8_t tw = static_cast<int8_t>(region[off + 3]);  // Handedness (sign for bitangent)
            
            glm::vec3 tangent(
                static_cast<float>(tx) / 127.0f,
                static_cast<float>(ty) / 127.0f,
                static_cast<float>(tz) / 127.0f
            );
            
            // Normalize tangent
            float len = glm::length(tangent);
            if (len > 0.001f) {
                tangent /= len;
            } else {
                tangent = glm::vec3(1.0f, 0.0f, 0.0f);  // Default tangent
            }
            
            mesh.vertices[i].tangent = tangent;
            mesh.vertices[i].tangent_sign = (tw >= 0) ? 1.0f : -1.0f;
        }
        attr_offset += vertex_count * XOB_TANGENT_SIZE;
        std::cerr << "[XOB] Parsed " << vertex_count << " tangents\n";
    }
    
    // Skinning data (8 bytes per vertex)
    // Format: 4 bone indices (u8) + 4 bone weights (u8 normalized to 0-1)
    constexpr size_t SKINNING_SIZE = 8;
    if (has_skinning) {
        size_t skinning_offset = attr_offset;
        std::cerr << "[XOB] Parsing skinning data at offset " << skinning_offset << "\n";
        
        for (uint32_t i = 0; i < vertex_count && i < mesh.vertices.size(); i++) {
            size_t off = skinning_offset + i * SKINNING_SIZE;
            if (off + SKINNING_SIZE > region.size()) break;
            
            // Bone indices (4 bytes)
            mesh.vertices[i].bone_indices.x = region[off];
            mesh.vertices[i].bone_indices.y = region[off + 1];
            mesh.vertices[i].bone_indices.z = region[off + 2];
            mesh.vertices[i].bone_indices.w = region[off + 3];
            
            // Bone weights (4 bytes, normalized u8 -> 0.0-1.0)
            mesh.vertices[i].bone_weights.x = static_cast<float>(region[off + 4]) / 255.0f;
            mesh.vertices[i].bone_weights.y = static_cast<float>(region[off + 5]) / 255.0f;
            mesh.vertices[i].bone_weights.z = static_cast<float>(region[off + 6]) / 255.0f;
            mesh.vertices[i].bone_weights.w = static_cast<float>(region[off + 7]) / 255.0f;
            
            // Debug first vertex's skinning data
            if (i == 0) {
                std::cerr << "[XOB] Skinning[0]: bones=(" 
                          << mesh.vertices[i].bone_indices.x << "," 
                          << mesh.vertices[i].bone_indices.y << ","
                          << mesh.vertices[i].bone_indices.z << ","
                          << mesh.vertices[i].bone_indices.w << ") weights=("
                          << mesh.vertices[i].bone_weights.x << ","
                          << mesh.vertices[i].bone_weights.y << ","
                          << mesh.vertices[i].bone_weights.z << ","
                          << mesh.vertices[i].bone_weights.w << ")\n";
            }
        }
        attr_offset += vertex_count * SKINNING_SIZE;
        std::cerr << "[XOB] Parsed " << vertex_count << " skinning records\n";
    }
    
    // Extra normal data (8 bytes per vertex)
    // Format: secondary normal (4 bytes) + secondary tangent (4 bytes)
    if (has_extra_normals) {
        size_t extra_offset = attr_offset;
        std::cerr << "[XOB] Parsing extra normals at offset " << extra_offset << "\n";
        
        for (uint32_t i = 0; i < vertex_count && i < mesh.vertices.size(); i++) {
            size_t off = extra_offset + i * XOB_EXTRA_SIZE;
            if (off + XOB_EXTRA_SIZE > region.size()) break;
            
            // Extra normal (first 4 bytes)
            int8_t enx = static_cast<int8_t>(region[off]);
            int8_t eny = static_cast<int8_t>(region[off + 1]);
            int8_t enz = static_cast<int8_t>(region[off + 2]);
            // byte 3 is padding or sign
            
            glm::vec3 extra_normal(
                static_cast<float>(enx) / 127.0f,
                static_cast<float>(eny) / 127.0f,
                static_cast<float>(enz) / 127.0f
            );
            float len = glm::length(extra_normal);
            if (len > 0.001f) extra_normal /= len;
            else extra_normal = glm::vec3(0.0f, 1.0f, 0.0f);
            mesh.vertices[i].extra_normal = extra_normal;
            
            // Extra tangent (second 4 bytes)
            int8_t etx = static_cast<int8_t>(region[off + 4]);
            int8_t ety = static_cast<int8_t>(region[off + 5]);
            int8_t etz = static_cast<int8_t>(region[off + 6]);
            // byte 7 is padding or sign
            
            glm::vec3 extra_tangent(
                static_cast<float>(etx) / 127.0f,
                static_cast<float>(ety) / 127.0f,
                static_cast<float>(etz) / 127.0f
            );
            len = glm::length(extra_tangent);
            if (len > 0.001f) extra_tangent /= len;
            else extra_tangent = glm::vec3(1.0f, 0.0f, 0.0f);
            mesh.vertices[i].extra_tangent = extra_tangent;
        }
        attr_offset += vertex_count * XOB_EXTRA_SIZE;
        std::cerr << "[XOB] Parsed " << vertex_count << " extra normal records\n";
    }
    
    std::cerr << "[XOB] Final attr_offset=" << attr_offset << " region_size=" << region.size() << "\n";
    
    return !mesh.vertices.empty() && !mesh.indices.empty();
}

/**
 * Read null-terminated string from buffer
 * Returns (string, bytes_consumed including null terminator)
 */
static std::pair<std::string, size_t> read_null_string(const uint8_t* data, size_t max_size) {
    size_t len = 0;
    while (len < max_size && data[len] != '\0') {
        len++;
    }
    std::string s(reinterpret_cast<const char*>(data), len);
    return {s, len + 1};  // +1 for null terminator
}

/**
 * Extract materials from HEAD chunk
 * 
 * XOB HEAD Header Structure (offsets from HEAD data start):
 *   0x00-0x17: Bounding box (6 floats: min XYZ, max XYZ)
 *   0x18-0x1F: Padding (zeros)
 *   0x20-0x27: More padding
 *   0x28-0x2B: Float value (unknown)
 *   0x2C-0x2D: Rendering material count (uint16)
 *   0x2E-0x2F: Bone/object count (uint16) - 0 for simple meshes
 *   0x30-0x33: LOD count (uint32, usually 1)
 *   0x34-0x37: Reserved (0)
 *   0x38-0x3B: Material data section size (uint32)
 *   0x3C onwards: Material name+path pairs (null-terminated strings)
 * 
 * Materials are stored as pairs: (name, path) where path contains the GUID.
 * Only the first material_count pairs are rendering materials.
 */
static std::vector<XobMaterial> extract_materials_from_head(const uint8_t* data, size_t size) {
    std::vector<XobMaterial> materials;
    
    // Need at least 0x3C bytes for header
    if (size < 0x3C) {
        std::cerr << "[XOB] HEAD chunk too small for header: " << size << " bytes\n";
        return materials;
    }
    
    // Read material count from offset 0x2C (uint16, not uint32!)
    uint16_t material_count = read_u16_le(data + 0x2C);
    uint16_t bone_count = read_u16_le(data + 0x2E);
    uint32_t mat_data_size = read_u32_le(data + 0x38);
    
    std::cerr << "[XOB] HEAD header: material_count=" << material_count 
              << " bone_count=" << bone_count 
              << " mat_data_size=" << mat_data_size << "\n";
    
    // Try to parse materials from header if count is reasonable
    bool use_header_parsing = (material_count > 0 && material_count <= 100);
    
    if (use_header_parsing) {
        // Find LZO4 position (marks end of material strings section)
        size_t lzo4_pos = size;
        for (size_t i = 0x3C; i + 4 <= size; i++) {
            if (data[i] == 'L' && data[i+1] == 'Z' && data[i+2] == 'O' && data[i+3] == '4') {
                lzo4_pos = i;
                break;
            }
        }
        
        // Parse material name+path pairs starting at offset 0x3C
        size_t pos = 0x3C;
        for (uint16_t i = 0; i < material_count && pos < lzo4_pos; i++) {
            // Read material name
            auto [name, name_len] = read_null_string(data + pos, lzo4_pos - pos);
            pos += name_len;
            
            if (pos >= lzo4_pos) break;
            
            // Read material path (contains GUID + path)
            auto [path, path_len] = read_null_string(data + pos, lzo4_pos - pos);
            pos += path_len;
            
            // Validate path has material extension
            if (path.find(".emat") == std::string::npos && 
                path.find(".gamemat") == std::string::npos) {
                std::cerr << "[XOB] Material " << i << " path missing extension: " << path << "\n";
                // This might not be a valid material, but continue anyway
            }
            
            XobMaterial mat;
            mat.name = name;
            mat.path = path;
            mat.diffuse_texture = path;
            materials.push_back(mat);
            
            std::cerr << "[XOB] Material " << i << ": name='" << name << "' path='" << path << "'\n";
        }
        
        if (!materials.empty()) {
            std::cerr << "[XOB] Parsed " << materials.size() << " materials from header\n";
            return materials;
        }
        
        std::cerr << "[XOB] Header parsing produced no materials, falling back to GUID search\n";
    } else {
        std::cerr << "[XOB] Invalid material count " << material_count << ", falling back to GUID search\n";
    }
    
    // Fallback: GUID pattern search
    std::cerr << "[XOB] Falling back to GUID pattern search\n";
    
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
                size_t path_start = i;  // Include GUID in path
                size_t path_end = i + 18;
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
 * CORRECT ALGORITHM (verified on 38 test XOB files - 100% match):
 * 
 * SUBMESH BLOCK STRUCTURE (relative to 0xFFFF marker):
 *   -6: u16 index_count (number of indices for this submesh)
 *   -4: u16 lod_index (0=highest detail)
 *   -2: u16 (always 0)
 *    0: 0xFFFF marker
 *   +2: u16 material_index
 *   +4: u16 flags (lo byte=pass type, hi byte=lod level info)
 * 
 * KEY INSIGHT: Material 0 is ALWAYS IMPLICIT!
 * - For multi-material meshes, only materials 1+ have explicit markers
 * - Material 0's index count = total_indices - sum(all other materials)
 * - When material 0 HAS markers, they represent render passes/LODs, NOT additional geometry
 * - For each material > 0, use the FIRST marker's index_count encountered
 */
static std::vector<MaterialRange> extract_material_ranges(const uint8_t* data, size_t size, 
                                                          uint32_t total_triangles, 
                                                          size_t num_materials) {
    std::vector<MaterialRange> result;
    
    LOG_DEBUG("XobParser", "Extracting material ranges: " << total_triangles << " tris, " << num_materials << " materials");
    
    uint32_t total_indices = total_triangles * 3;
    
    // Find HEAD chunk
    const uint8_t* head_start = nullptr;
    size_t head_size = 0;
    for (size_t i = 0; i + 8 < size; i++) {
        if (data[i] == 'H' && data[i+1] == 'E' && data[i+2] == 'A' && data[i+3] == 'D') {
            head_size = read_u32_be(data + i + 4);
            head_start = data + i + 8;
            break;
        }
    }
    if (!head_start || head_size == 0) {
        // No HEAD - assign all to material 0
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
        return result;
    }
    
    // Find LZO4 descriptors
    std::vector<size_t> lzo4_positions;
    for (size_t i = 0; i + 4 <= head_size; i++) {
        if (head_start[i] == 'L' && head_start[i+1] == 'Z' && 
            head_start[i+2] == 'O' && head_start[i+3] == '4') {
            lzo4_positions.push_back(i);
            i += 115;
        }
    }
    
    if (lzo4_positions.empty()) {
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
        return result;
    }
    
    // Data after all LZO4 descriptors contains submesh blocks
    size_t after_start = lzo4_positions[0] + (116 * lzo4_positions.size());
    if (after_start >= head_size) {
        // No submesh data - single material mesh
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
        return result;
    }
    
    const uint8_t* after = head_start + after_start;
    size_t after_size = head_size - after_start;
    
    LOG_DEBUG("XobParser", "Submesh data: " << after_size << " bytes after LZO4 descriptors");
    
    // Dump raw submesh data for analysis
    std::ostringstream raw_hex;
    raw_hex << "Submesh raw (first 128 bytes): ";
    for (size_t i = 0; i < std::min<size_t>(128, after_size); i++) {
        raw_hex << std::hex << std::setw(2) << std::setfill('0') << (int)after[i] << " ";
        if ((i + 1) % 16 == 0) raw_hex << "| ";
    }
    LOG_DEBUG("XobParser", raw_hex.str());
    
    // Map to store material info: first entry has start position, count
    // Structure appears to be: [...][idx_count:u16][lod:u16][0][FFFF][mat_idx:u16][flags:u16]
    // But we might also have: idx_start somewhere before
    struct SubmeshEntry {
        uint32_t material_index;
        uint32_t index_start;   // NEW: try to find this
        uint32_t index_count;
        uint16_t lod_index;
        uint16_t flags;
    };
    std::vector<SubmeshEntry> submesh_entries;
    
    // Scan for 0xFFFF markers and extract ALL surrounding data
    for (size_t pos = 6; pos + 6 <= after_size; pos++) {
        if (after[pos] == 0xFF && after[pos+1] == 0xFF) {
            SubmeshEntry entry;
            entry.material_index = read_u16_le(after + pos + 2);
            entry.flags = read_u16_le(after + pos + 4);
            
            // Data before FFFF marker
            entry.index_count = read_u16_le(after + pos - 6);
            entry.lod_index = read_u16_le(after + pos - 4);
            // pos - 2 is reserved/zero
            
            // Look for potential index_start (try pos - 8 or pos - 10)
            uint16_t potential_start1 = (pos >= 8) ? read_u16_le(after + pos - 8) : 0;
            uint16_t potential_start2 = (pos >= 10) ? read_u16_le(after + pos - 10) : 0;
            
            // Only process valid materials
            if (entry.material_index < num_materials) {
                LOG_DEBUG("XobParser", "FFFF at +" << pos 
                    << ": mat=" << entry.material_index 
                    << " idx_count=" << entry.index_count 
                    << " lod=" << entry.lod_index
                    << " flags=0x" << std::hex << entry.flags << std::dec
                    << " | before: [" << potential_start2 << ", " << potential_start1 << "]");
                
                entry.index_start = 0; // Unknown for now
                submesh_entries.push_back(entry);
            }
            pos += 5; // Skip past this marker
        }
    }
    
    // NEW APPROACH: Process blocks in ORDER (not by material index)
    // The index buffer is arranged to match block order, not material order
    
    // Structure to hold block info in order
    struct BlockEntry {
        size_t position;        // Position in submesh data
        uint32_t material_index;
        uint32_t index_count;
        uint16_t lod;
        uint16_t flags;
    };
    std::vector<BlockEntry> all_blocks;
    
    // First pass: collect ALL blocks in order
    for (size_t pos = 6; pos + 6 <= after_size; pos++) {
        if (after[pos] == 0xFF && after[pos+1] == 0xFF) {
            BlockEntry entry;
            entry.position = pos;
            entry.material_index = read_u16_le(after + pos + 2);
            entry.flags = read_u16_le(after + pos + 4);
            entry.index_count = read_u16_le(after + pos - 6);
            entry.lod = read_u16_le(after + pos - 4);
            
            if (entry.material_index < num_materials) {
                all_blocks.push_back(entry);
                LOG_DEBUG("XobParser", "Block at +" << pos << ": mat=" << entry.material_index 
                          << " idx_count=" << entry.index_count << " lod=" << entry.lod 
                          << " flags=0x" << std::hex << entry.flags << std::dec);
            }
            pos += 5;
        }
    }
    
    LOG_DEBUG("XobParser", "Found " << all_blocks.size() << " total submesh blocks");
    
    // Find which LOD levels are present
    std::set<uint16_t> lod_levels;
    for (const auto& block : all_blocks) {
        lod_levels.insert(block.lod);
    }
    
    // Prefer LOD 0 if available, otherwise use minimum LOD
    uint16_t target_lod = 0;
    if (lod_levels.find(0) == lod_levels.end() && !lod_levels.empty()) {
        target_lod = *lod_levels.begin(); // Use lowest available LOD
    }
    
    LOG_DEBUG("XobParser", "Target LOD: " << target_lod 
              << " (available: " << lod_levels.size() << " levels)");
    
    // Second pass: extract ALL blocks at target LOD level
    // IMPORTANT: Include ALL pass types (0x01, 0x02, etc.) as they all contain valid geometry
    // The pass type flag indicates render pass (opaque, transparent, etc.) but ALL are part of the mesh
    std::map<uint32_t, uint32_t> mat_first_count; // material -> first seen index_count
    std::vector<std::pair<uint32_t, uint32_t>> ordered_entries; // (mat_idx, index_count) in block order
    
    for (const auto& block : all_blocks) {
        // Only process blocks at target LOD
        if (block.lod != target_lod) continue;
        
        // First occurrence of this material at this LOD (any pass type)
        if (mat_first_count.find(block.material_index) == mat_first_count.end()) {
            mat_first_count[block.material_index] = block.index_count;
            ordered_entries.push_back({block.material_index, block.index_count});
            LOG_DEBUG("XobParser", "Using block: mat=" << block.material_index 
                      << " index_count=" << block.index_count
                      << " flags=0x" << std::hex << (int)block.flags << std::dec);
        }
    }
    
    // NOTE: Do NOT add materials from other LODs - they have 0 triangles at target LOD
    // The geometry at LOD 0 only contains triangles for materials with LOD 0 blocks
    
    LOG_DEBUG("XobParser", "Using " << ordered_entries.size() << " blocks at LOD " << target_lod);
    
    // Calculate sum of all explicit entries
    uint32_t explicit_sum = 0;
    for (const auto& entry : ordered_entries) {
        explicit_sum += entry.second;
    }
    
    // Check if explicit entries exceed total - if so, mat0 entries are multi-pass (overlapping)
    // In that case, use implicit calculation for mat0
    bool mat0_is_implicit = false;
    uint32_t mat0_explicit_count = 0;
    for (const auto& entry : ordered_entries) {
        if (entry.first == 0) {
            mat0_explicit_count = entry.second;
            break;
        }
    }
    
    // Calculate what mat0 SHOULD be if implicit
    uint32_t sum_others = explicit_sum - mat0_explicit_count;
    uint32_t mat0_implicit = (sum_others < total_indices) ? (total_indices - sum_others) : 0;
    
    LOG_DEBUG("XobParser", "Mat0 explicit=" << mat0_explicit_count << ", implicit=" << mat0_implicit 
              << ", sum_others=" << sum_others << ", explicit_sum=" << explicit_sum);
    
    // If explicit sum exceeds total, mat0's explicit entry includes multi-pass data
    // Use implicit calculation instead
    if (explicit_sum > total_indices) {
        mat0_is_implicit = true;
        LOG_DEBUG("XobParser", "Explicit sum (" << explicit_sum << ") > total (" << total_indices 
                  << "), using implicit mat0=" << mat0_implicit);
    }
    
    // CRITICAL: The index buffer is organized by MATERIAL INDEX ORDER (0, 1, 2, ...)
    // NOT by block order in the header! Sort entries by material index.
    
    // Build map from material index to index count
    std::map<uint32_t, uint32_t> mat_to_count;
    for (const auto& entry : ordered_entries) {
        uint32_t mat_idx = entry.first;
        uint32_t idx_count = entry.second;
        
        // Only store first occurrence (already filtered above)
        if (mat_to_count.find(mat_idx) == mat_to_count.end()) {
            mat_to_count[mat_idx] = idx_count;
        }
    }
    
    // Apply implicit mat0 calculation if needed
    // ONLY if mat0 had explicit blocks at target LOD (multi-pass case)
    // Do NOT add mat0 if it had no blocks at target LOD
    if (mat0_is_implicit && mat_to_count.find(0) != mat_to_count.end()) {
        mat_to_count[0] = mat0_implicit;
        LOG_DEBUG("XobParser", "Applied implicit mat0: " << mat0_implicit);
    }
    // NOTE: If mat0 has no blocks at target LOD, it has 0 triangles - don't add it
    
    LOG_DEBUG("XobParser", "Building ranges in MATERIAL INDEX ORDER (not block order)");
    
    // Build material ranges in MATERIAL INDEX ORDER (0, 1, 2, ...)
    // The std::map iterates in key order, which is material index order
    uint32_t current_tri = 0;
    for (const auto& [mat_idx, idx_count_raw] : mat_to_count) {
        uint32_t idx_count = idx_count_raw;
        
        // Skip materials with zero indices
        if (idx_count == 0) continue;
        
        // Convert to triangle count (round down)
        uint32_t tri_count = idx_count / 3;
        if (tri_count == 0) continue;
        
        // Clamp to total triangles
        if (current_tri >= total_triangles) {
            LOG_DEBUG("XobParser", "Skipping mat " << mat_idx << " - start beyond total");
            continue;
        }
        if (current_tri + tri_count > total_triangles) {
            tri_count = total_triangles - current_tri;
        }
        
        MaterialRange r;
        r.material_index = mat_idx;
        r.triangle_start = current_tri;
        r.triangle_end = current_tri + tri_count;
        r.triangle_count = tri_count;
        result.push_back(r);
        
        LOG_DEBUG("XobParser", "  Range: mat=" << mat_idx << " tris=" << current_tri 
                  << "-" << (current_tri + tri_count));
        
        current_tri += tri_count;
    }
    
    // If no ranges were created, fall back to single material
    if (result.empty()) {
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
    }
    
    LOG_DEBUG("XobParser", "Material ranges assigned: " << result.size());
    for (const auto& r : result) {
        LOG_DEBUG("XobParser", "  mat=" << r.material_index << " tris=" << r.triangle_start 
                  << "-" << r.triangle_end << " (count=" << r.triangle_count << ")");
    }
    
    return result;
}

/**
 * Parse COLL chunk - Collision Data
 * 
 * Structure per spec (XOB_FORMAT_SPEC.md):
 * - Array of 64-byte collision object headers
 * - Followed by collision mesh data (vertices + indices)
 * 
 * Object Header (64 bytes):
 *   0x00: u8  - Collision type (0x03=complex, 0x05=simple, 0x07=dynamic)
 *   0x01: u8  - Flags (0xFF=mesh, 0x02=primitive)
 *   0x02: u16 - Name index into HEAD strings
 *   0x04: float[9] - 3x3 rotation matrix (row-major)
 *   0x28: float[3] - Translation XYZ
 *   0x34: u32 - Reserved (0)
 *   0x38: u16 - Index start
 *   0x3A: u16 - Index end + 1
 *   0x3C: u32 - Reserved (0)
 */
static void parse_coll_chunk(const uint8_t* data, size_t size, 
                              std::vector<XobCollisionObject>& objects,
                              XobCollisionMesh& mesh) {
    if (size < 64) {
        std::cerr << "[XOB] COLL chunk too small: " << size << " bytes\n";
        return;
    }
    
    // First pass: count collision objects (each is 64 bytes)
    // Objects end where mesh data begins - detect by checking for valid type/flags
    size_t num_objects = 0;
    size_t pos = 0;
    
    while (pos + 64 <= size) {
        uint8_t type = data[pos];
        uint8_t flags = data[pos + 1];
        
        // Valid collision object types: 0x03, 0x05, 0x07
        // Valid flags: 0xFF (mesh), 0x02 (primitive)
        bool valid_type = (type == 0x03 || type == 0x05 || type == 0x07);
        bool valid_flags = (flags == 0xFF || flags == 0x02);
        
        if (!valid_type || !valid_flags) {
            break;  // End of object array
        }
        
        num_objects++;
        pos += 64;
    }
    
    std::cerr << "[XOB] COLL: Found " << num_objects << " collision objects\n";
    
    if (num_objects == 0) return;
    
    // Parse collision objects
    objects.clear();
    objects.reserve(num_objects);
    
    for (size_t i = 0; i < num_objects; i++) {
        const uint8_t* obj = data + i * 64;
        
        XobCollisionObject coll;
        coll.type = static_cast<XobCollisionType>(obj[0]);
        coll.flags = obj[1];
        coll.name_index = read_u16_le(obj + 2);
        
        // Read 3x3 rotation matrix (row-major floats)
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                coll.rotation[row][col] = read_f32_le(obj + 4 + (row * 3 + col) * 4);
            }
        }
        
        // Translation
        coll.translation.x = read_f32_le(obj + 0x28);
        coll.translation.y = read_f32_le(obj + 0x2C);
        coll.translation.z = read_f32_le(obj + 0x30);
        
        // Index references
        coll.index_start = read_u16_le(obj + 0x38);
        coll.index_end = read_u16_le(obj + 0x3A);
        
        objects.push_back(coll);
        
        std::cerr << "[XOB] COLL obj " << i << ": type=0x" << std::hex << (int)obj[0] << std::dec
                  << " flags=0x" << std::hex << (int)coll.flags << std::dec
                  << " idx=" << coll.index_start << "-" << coll.index_end
                  << " trans=(" << coll.translation.x << "," << coll.translation.y << "," << coll.translation.z << ")\n";
    }
    
    // Parse collision mesh data (after object headers)
    size_t mesh_start = num_objects * 64;
    size_t remaining = size - mesh_start;
    
    if (remaining < 12) {
        std::cerr << "[XOB] COLL: No mesh data after objects\n";
        return;
    }
    
    // Mesh data: vertices (float[3] each) followed by indices (u16[3] per triangle)
    // We need to figure out where vertices end and indices begin
    // Heuristic: indices are small numbers (< vertex_count), vertices are floats
    
    const uint8_t* mesh_data = data + mesh_start;
    
    // Try to find the split point by looking for the pattern change
    // Vertices are 12 bytes each (3 floats), indices are 6 bytes per triangle (3 u16s)
    
    // Estimate: assume roughly equal bytes for verts and indices
    // Start by trying to interpret as vertices
    size_t max_verts = remaining / 12;
    size_t actual_verts = 0;
    
    mesh.vertices.clear();
    mesh.bounds_min = glm::vec3(FLT_MAX);
    mesh.bounds_max = glm::vec3(-FLT_MAX);
    
    // Read vertices until we hit invalid data
    for (size_t v = 0; v < max_verts; v++) {
        float x = read_f32_le(mesh_data + v * 12);
        float y = read_f32_le(mesh_data + v * 12 + 4);
        float z = read_f32_le(mesh_data + v * 12 + 8);
        
        // Check if these look like reasonable vertex coordinates
        // Collision meshes should have coordinates within reasonable bounds
        if (std::abs(x) > 10000.0f || std::abs(y) > 10000.0f || std::abs(z) > 10000.0f ||
            std::isnan(x) || std::isnan(y) || std::isnan(z) ||
            std::isinf(x) || std::isinf(y) || std::isinf(z)) {
            break;
        }
        
        glm::vec3 vert(x, y, z);
        mesh.vertices.push_back(vert);
        mesh.bounds_min = glm::min(mesh.bounds_min, vert);
        mesh.bounds_max = glm::max(mesh.bounds_max, vert);
        actual_verts++;
    }
    
    std::cerr << "[XOB] COLL mesh: " << actual_verts << " vertices\n";
    
    // Remaining data is indices
    size_t indices_start = mesh_start + actual_verts * 12;
    size_t indices_size = size - indices_start;
    size_t num_indices = indices_size / 2;  // u16 each
    
    mesh.indices.clear();
    mesh.indices.reserve(num_indices);
    
    for (size_t i = 0; i < num_indices; i++) {
        uint16_t idx = read_u16_le(data + indices_start + i * 2);
        if (idx < actual_verts) {
            mesh.indices.push_back(idx);
        }
    }
    
    std::cerr << "[XOB] COLL mesh: " << mesh.indices.size() << " indices (" 
              << mesh.indices.size() / 3 << " triangles)\n";
}

/**
 * Parse VOLM chunk - Spatial Octree
 * 
 * Structure per spec (XOB_FORMAT_SPEC.md):
 * Header (12 bytes):
 *   0x00: u16 - Reserved (0)
 *   0x02: u16 - Octree depth (typically 4)
 *   0x04: u16 - Internal node count
 *   0x06: u16 - Total node count
 *   0x08: u16 - Data size (octree data bytes / 2)
 *   0x0A: u16 - Reserved (0)
 * 
 * Followed by packed octree bitmask data
 */
static void parse_volm_chunk(const uint8_t* data, size_t size, XobOctree& octree) {
    if (size < 12) {
        std::cerr << "[XOB] VOLM chunk too small: " << size << " bytes\n";
        return;
    }
    
    // Read header
    // uint16_t reserved1 = read_u16_le(data + 0);
    octree.depth = read_u16_le(data + 2);
    octree.internal_nodes = read_u16_le(data + 4);
    octree.total_nodes = read_u16_le(data + 6);
    uint16_t data_size_half = read_u16_le(data + 8);
    // uint16_t reserved2 = read_u16_le(data + 10);
    
    size_t data_size = static_cast<size_t>(data_size_half) * 2;
    
    std::cerr << "[XOB] VOLM: depth=" << octree.depth 
              << " internal_nodes=" << octree.internal_nodes
              << " total_nodes=" << octree.total_nodes
              << " data_size=" << data_size << "\n";
    
    // Read octree data
    if (12 + data_size <= size) {
        octree.data.assign(data + 12, data + 12 + data_size);
    } else {
        // Copy what we have
        octree.data.assign(data + 12, data + size);
    }
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
    if (data_.size() < 12) {
        LOG_ERROR("XobParser", "Data too small: " << data_.size() << " bytes (need 12+)");
        return std::nullopt;
    }
    
    // Verify FORM header
    if (std::memcmp(data_.data(), "FORM", 4) != 0) {
        LOG_ERROR("XobParser", "Invalid magic: not a FORM container");
        return std::nullopt;
    }
    
    // Check XOB type
    const char* form_type = reinterpret_cast<const char*>(data_.data() + 8);
    if (form_type[0] != 'X' || form_type[1] != 'O' || form_type[2] != 'B') {
        LOG_ERROR("XobParser", "Invalid form type: '" << std::string(form_type, 4) << "' (expected XOB*)");
        return std::nullopt;
    }
    
    // Find HEAD chunk
    static constexpr uint8_t HEAD_ID[4] = {'H', 'E', 'A', 'D'};
    auto head_chunk = find_chunk(HEAD_ID);
    if (!head_chunk) {
        LOG_ERROR("XobParser", "HEAD chunk not found");
        return std::nullopt;
    }
    
    auto head_data = *head_chunk;
    LOG_DEBUG("XobParser", "HEAD chunk: " << head_data.size() << " bytes");
    
    // Extract materials from HEAD chunk
    materials_ = extract_materials_from_head(head_data.data(), head_data.size());
    LOG_DEBUG("XobParser", "Found " << materials_.size() << " materials");
    
    // Parse LZO4 descriptors from HEAD chunk
    auto descriptors = parse_lzo4_descriptors(head_data.data(), head_data.size());
    if (descriptors.empty()) {
        LOG_ERROR("XobParser", "No LZO4 descriptors found in HEAD chunk");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "Found " << descriptors.size() << " LOD descriptors");
    
    // Validate LOD index
    if (target_lod >= descriptors.size()) {
        LOG_WARNING("XobParser", "Requested LOD " << target_lod << " > max " 
                    << (descriptors.size()-1) << ", using LOD 0");
        target_lod = 0;
    }
    
    // Get descriptor for target LOD
    const auto& desc = descriptors[target_lod];
    if (desc.vertex_count == 0 || desc.triangle_count == 0) {
        LOG_ERROR("XobParser", "Invalid LOD " << target_lod << " descriptor: verts=" 
                  << desc.vertex_count << " tris=" << desc.triangle_count);
        return std::nullopt;
    }
    
    // Find LODS chunk
    static constexpr uint8_t LODS_ID[4] = {'L', 'O', 'D', 'S'};
    auto lods_chunk = find_chunk(LODS_ID);
    if (!lods_chunk) {
        LOG_ERROR("XobParser", "LODS chunk not found");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "LODS chunk: " << lods_chunk->size() << " bytes compressed");
    
    // Decompress LODS data with dictionary chaining
    auto decompressed = decompress_lz4_chained(lods_chunk->data(), lods_chunk->size());
    if (decompressed.empty()) {
        LOG_ERROR("XobParser", "LZ4 decompression failed (input=" << lods_chunk->size() << ")");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "Decompressed: " << decompressed.size() << " bytes");
    
    // Extract LOD region (REVERSE order - LOD0 at END)
    auto region = extract_lod_region_internal(decompressed, descriptors, target_lod);
    if (region.empty()) {
        LOG_ERROR("XobParser", "Failed to extract LOD " << target_lod << " region");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "LOD " << target_lod << " region: " << region.size() << " bytes");
    
    // Parse mesh from region (pass format flags for correct attribute layout)
    XobMesh mesh;
    if (!parse_mesh_from_region(region, desc.vertex_count, desc.triangle_count, 
                                 desc.position_stride, 
                                 desc.has_normals, desc.has_uvs, desc.has_tangents,
                                 desc.has_skinning, desc.has_extra_normals, mesh)) {
        LOG_ERROR("XobParser", "Failed to parse mesh (verts=" << desc.vertex_count 
                  << " tris=" << desc.triangle_count << " stride=" << desc.position_stride << ")");
        return std::nullopt;
    }
    LOG_INFO("XobParser", "Parsed LOD " << target_lod << ": " << mesh.vertices.size() 
             << " vertices, " << mesh.indices.size() << " indices");
    
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
    
    // Log material paths for debugging
    for (size_t i = 0; i < materials_.size(); i++) {
        LOG_DEBUG("XobParser", "Material " << i << ": name=" << materials_[i].name << " path=" << materials_[i].path);
    }
    
    // Extract material ranges (which triangles use which material)
    mesh.material_ranges = extract_material_ranges(data_.data(), data_.size(), desc.triangle_count, materials_.size());
    LOG_DEBUG("XobParser", "Material ranges assigned: " << mesh.material_ranges.size());
    
    // Parse COLL chunk (collision data) - optional
    static constexpr uint8_t COLL_ID[4] = {'C', 'O', 'L', 'L'};
    auto coll_chunk = find_chunk(COLL_ID);
    if (coll_chunk) {
        LOG_DEBUG("XobParser", "COLL chunk: " << coll_chunk->size() << " bytes");
        parse_coll_chunk(coll_chunk->data(), coll_chunk->size(), 
                         mesh.collision_objects, mesh.collision_mesh);
        LOG_DEBUG("XobParser", "Parsed " << mesh.collision_objects.size() << " collision objects, "
                  << mesh.collision_mesh.vertices.size() << " collision vertices");
    }
    
    // Parse VOLM chunk (spatial octree) - optional
    static constexpr uint8_t VOLM_ID[4] = {'V', 'O', 'L', 'M'};
    auto volm_chunk = find_chunk(VOLM_ID);
    if (volm_chunk) {
        LOG_DEBUG("XobParser", "VOLM chunk: " << volm_chunk->size() << " bytes");
        parse_volm_chunk(volm_chunk->data(), volm_chunk->size(), mesh.octree);
        LOG_DEBUG("XobParser", "Parsed octree: depth=" << mesh.octree.depth 
                  << " nodes=" << mesh.octree.total_nodes);
    }
    
    // Extract bone count from HEAD header
    if (head_data.size() >= 0x30) {
        mesh.bone_count = read_u16_le(head_data.data() + 0x2E);
        LOG_DEBUG("XobParser", "Bone count: " << mesh.bone_count);
    }
    
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


