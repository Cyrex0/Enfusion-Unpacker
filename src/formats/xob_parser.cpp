/**
 * Enfusion Unpacker - XOB Parser Implementation
 * 
 * XOB9 Format (IFF/FORM container with big-endian chunk sizes):
 * - FORM/XOB9 header (12 bytes)
 * - HEAD chunk: Materials, bones, LOD descriptors
 * - COLL chunk (optional): Collision objects + collision mesh
 * - VOLM chunk (optional): Spatial octree for collision
 * - LODS chunk: LZ4 block-compressed mesh data
 * 
 * See XOB9_FORMAT_SPEC_v8.md for detailed format documentation.
 */

#include "enfusion/xob_parser.hpp"
#include "enfusion/xob_types.hpp"
#include "enfusion/xob_vertex_layout.hpp"
#include "enfusion/xob_vertex_parser.hpp"
#include "enfusion/xob_material_ranges.hpp"
#include "enfusion/compression.hpp"
#include "enfusion/logging.hpp"
#include <lz4.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cfloat>

namespace enfusion {

using namespace xob;

// ============================================================================
// LZ4 Decompression
// ============================================================================

static std::vector<uint8_t> decompress_lz4_chained(const uint8_t* data, size_t size) {
    std::vector<uint8_t> result;
    result.reserve(size * 4);

    size_t pos = 0;
    std::vector<uint8_t> prev_dict;
    prev_dict.reserve(LZ4_DICT_SIZE);

    while (pos < size) {
        if (pos + 4 > size) break;

        uint32_t header = read_u32_le(data + pos);
        pos += 4;

        uint32_t block_size = header & 0x7FFFFFFF;
        if (block_size == 0 || block_size > LZ4_MAX_BLOCK_SIZE || pos + block_size > size)
            break;

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
        if (dec_size <= 0) break;

        decompressed.resize(dec_size);
        result.insert(result.end(), decompressed.begin(), decompressed.end());
        
        // Dictionary from last 64KB of output
        if (result.size() <= LZ4_DICT_SIZE) {
            prev_dict.assign(result.begin(), result.end());
        } else {
            prev_dict.assign(result.end() - LZ4_DICT_SIZE, result.end());
        }
    }

    return result;
}

// ============================================================================
// LZO4 Descriptor Parsing
// ============================================================================

static std::vector<LzoDescriptor> parse_lzo4_descriptors(const uint8_t* data, size_t size) {
    std::vector<LzoDescriptor> descriptors;
    
    size_t pos = 0;
    while (pos + LZO4_MARKER_SIZE <= size) {
        // Find LZO4 marker
        size_t found = SIZE_MAX;
        for (size_t i = pos; i + LZO4_MARKER_SIZE <= size; i++) {
            if (data[i] == 'L' && data[i+1] == 'Z' && data[i+2] == 'O' && data[i+3] == '4') {
                found = i;
                break;
            }
        }
        if (found == SIZE_MAX) break;
        if (found + LZO4_DESCRIPTOR_SIZE > size) break;
        
        const uint8_t* desc = data + found;
        LzoDescriptor d;
        
        d.quality_tier = read_u32_le(desc + LZO4_OFF_QUALITY_TIER);
        d.switch_distance = read_f32_le(desc + LZO4_OFF_SWITCH_DIST);
        d.compressed_size = read_u32_le(desc + LZO4_OFF_COMPRESSED);
        d.decompressed_size = read_u32_le(desc + LZO4_OFF_DECOMPRESSED);
        d.format_flags = read_u32_le(desc + LZO4_OFF_FORMAT_FLAGS);
        d.mesh_type = (d.format_flags >> 24) & 0xFF;
        
        d.bounds_min.x = read_f32_le(desc + LZO4_OFF_BBOX_MIN);
        d.bounds_min.y = read_f32_le(desc + LZO4_OFF_BBOX_MIN + 4);
        d.bounds_min.z = read_f32_le(desc + LZO4_OFF_BBOX_MIN + 8);
        d.bounds_max.x = read_f32_le(desc + LZO4_OFF_BBOX_MAX);
        d.bounds_max.y = read_f32_le(desc + LZO4_OFF_BBOX_MAX + 4);
        d.bounds_max.z = read_f32_le(desc + LZO4_OFF_BBOX_MAX + 8);
        
        d.triangle_count = read_u16_le(desc + LZO4_OFF_TRIANGLE_COUNT);
        d.unique_vertex_count = read_u16_le(desc + LZO4_OFF_UNIQUE_VERTS);
        d.original_vertex_count = read_u16_le(desc + LZO4_OFF_ORIG_VERTS);
        d.submesh_index = read_u16_le(desc + LZO4_OFF_SUBMESH_IDX);
        
        std::memcpy(d.attr_config, desc + LZO4_OFF_ATTR_CONFIG, 8);
        
        d.uv_min_u = read_f32_le(desc + LZO4_OFF_UV_BOUNDS);
        d.uv_max_u = read_f32_le(desc + LZO4_OFF_UV_BOUNDS + 4);
        d.uv_min_v = read_f32_le(desc + LZO4_OFF_UV_BOUNDS + 8);
        d.uv_max_v = read_f32_le(desc + LZO4_OFF_UV_BOUNDS + 12);
        d.surface_scale = read_f32_le(desc + LZO4_OFF_SURFACE_SCALE);
        
        d.position_stride = (d.mesh_type & 0x10) ? POSITION_STRIDE_16 : POSITION_STRIDE_12;
        d.has_normals = true;
        d.has_tangents = true;
        d.has_uvs = true;
        d.has_skinning = (d.mesh_type == MESH_SKINNED || d.mesh_type == MESH_SKINNED_EMISSIVE);
        d.has_second_uv = (d.mesh_type == MESH_EMISSIVE || d.mesh_type == MESH_SKINNED_EMISSIVE);
        d.has_vertex_color = d.has_second_uv;
        
        if (d.attr_config[ATTR_CFG_UV_SETS] > 0) {
            d.has_second_uv = (d.attr_config[ATTR_CFG_UV_SETS] >= 2);
        }
        if (d.attr_config[ATTR_CFG_BONE_STREAMS] > 0) {
            d.has_skinning = true;
        }
        
        descriptors.push_back(d);
        pos = found + LZO4_MARKER_SIZE;
    }
    
    return descriptors;
}

// ============================================================================
// Material Extraction
// ============================================================================

static std::pair<std::string, size_t> read_null_string(const uint8_t* data, size_t max_size) {
    size_t len = 0;
    while (len < max_size && data[len] != '\0') len++;
    return {std::string(reinterpret_cast<const char*>(data), len), len + 1};
}

static std::vector<XobMaterial> extract_materials(const uint8_t* data, size_t size) {
    std::vector<XobMaterial> materials;
    
    if (size < 0x3C) return materials;
    
    uint16_t material_count = read_u16_le(data + 0x2C);
    if (material_count == 0 || material_count > 100) return materials;
    
    // Find LZO4 position (end of material strings)
    size_t lzo4_pos = size;
    for (size_t i = 0x3C; i + 4 <= size; i++) {
        if (data[i] == 'L' && data[i+1] == 'Z' && data[i+2] == 'O' && data[i+3] == '4') {
            lzo4_pos = i;
            break;
        }
    }
    
    // Parse material name+path pairs
    size_t pos = 0x3C;
    for (uint16_t i = 0; i < material_count && pos < lzo4_pos; i++) {
        auto [name, name_len] = read_null_string(data + pos, lzo4_pos - pos);
        pos += name_len;
        if (pos >= lzo4_pos) break;
        
        auto [path, path_len] = read_null_string(data + pos, lzo4_pos - pos);
        pos += path_len;
        
        XobMaterial mat;
        mat.name = name;
        mat.path = path;
        mat.diffuse_texture = path;
        materials.push_back(mat);
    }
    
    return materials;
}

// ============================================================================
// Mesh Parsing
// ============================================================================

static bool parse_mesh_from_region(
    const std::vector<uint8_t>& region,
    uint16_t vertex_count,
    uint16_t triangle_count,
    uint8_t mesh_type,
    uint8_t bone_streams,
    XobMesh& mesh)
{
    if (vertex_count == 0 || triangle_count == 0 || region.empty()) {
        LOG_ERROR("XobParser", "Invalid mesh parameters");
        return false;
    }
    
    // Detect vertex layout
    VertexLayoutDetector detector(region, vertex_count, triangle_count, mesh_type, bone_streams);
    VertexLayout layout = detector.detect();
    
    // Parse vertices and indices
    VertexStreamParser parser(region, vertex_count, mesh_type, layout);
    
    if (!parser.parse_indices(triangle_count, mesh.indices)) {
        LOG_ERROR("XobParser", "Failed to parse indices");
        return false;
    }
    
    if (!parser.parse(mesh.vertices)) {
        LOG_ERROR("XobParser", "Failed to parse vertices");
        return false;
    }
    
    LOG_INFO("XobParser", "Parsed " << mesh.vertices.size() << " vertices, " 
             << mesh.indices.size() << " indices");
    
    return true;
}

// ============================================================================
// Collision Parsing
// ============================================================================

static void parse_coll_chunk(const uint8_t* data, size_t size, 
                              std::vector<XobCollisionObject>& objects,
                              XobCollisionMesh& mesh) {
    if (size < 64) return;
    
    // Count collision objects (64 bytes each)
    size_t num_objects = 0;
    size_t pos = 0;
    
    while (pos + 64 <= size) {
        uint8_t type = data[pos];
        uint8_t flags = data[pos + 1];
        
        bool valid_type = (type == 0x03 || type == 0x05 || type == 0x07);
        bool valid_flags = (flags == 0xFF || flags == 0x02);
        
        if (!valid_type || !valid_flags) break;
        num_objects++;
        pos += 64;
    }
    
    if (num_objects == 0) return;
    
    objects.clear();
    objects.reserve(num_objects);
    
    for (size_t i = 0; i < num_objects; i++) {
        const uint8_t* obj = data + i * 64;
        
        XobCollisionObject coll;
        coll.type = static_cast<XobCollisionType>(obj[0]);
        coll.flags = obj[1];
        coll.name_index = read_u16_le(obj + 2);
        
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                coll.rotation[row][col] = read_f32_le(obj + 4 + (row * 3 + col) * 4);
            }
        }
        
        coll.translation.x = read_f32_le(obj + 0x28);
        coll.translation.y = read_f32_le(obj + 0x2C);
        coll.translation.z = read_f32_le(obj + 0x30);
        coll.index_start = read_u16_le(obj + 0x38);
        coll.index_end = read_u16_le(obj + 0x3A);
        
        objects.push_back(coll);
    }
    
    // Parse collision mesh data
    size_t mesh_start = num_objects * 64;
    size_t remaining = size - mesh_start;
    if (remaining < 12) return;
    
    const uint8_t* mesh_data = data + mesh_start;
    size_t max_verts = remaining / 12;
    
    mesh.vertices.clear();
    mesh.bounds_min = glm::vec3(FLT_MAX);
    mesh.bounds_max = glm::vec3(-FLT_MAX);
    
    size_t actual_verts = 0;
    for (size_t v = 0; v < max_verts; v++) {
        float x = read_f32_le(mesh_data + v * 12);
        float y = read_f32_le(mesh_data + v * 12 + 4);
        float z = read_f32_le(mesh_data + v * 12 + 8);
        
        if (std::abs(x) > 10000.0f || std::abs(y) > 10000.0f || std::abs(z) > 10000.0f ||
            !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            break;
        }
        
        glm::vec3 vert(x, y, z);
        mesh.vertices.push_back(vert);
        mesh.bounds_min = glm::min(mesh.bounds_min, vert);
        mesh.bounds_max = glm::max(mesh.bounds_max, vert);
        actual_verts++;
    }
    
    // Parse indices
    size_t indices_start = mesh_start + actual_verts * 12;
    size_t num_indices = (size - indices_start) / 2;
    
    mesh.indices.clear();
    mesh.indices.reserve(num_indices);
    
    for (size_t i = 0; i < num_indices; i++) {
        uint16_t idx = read_u16_le(data + indices_start + i * 2);
        if (idx < actual_verts) {
            mesh.indices.push_back(idx);
        }
    }
}

// ============================================================================
// Octree Parsing
// ============================================================================

static void parse_volm_chunk(const uint8_t* data, size_t size, XobOctree& octree) {
    if (size < 12) return;
    
    octree.depth = read_u16_le(data + 2);
    octree.internal_nodes = read_u16_le(data + 4);
    octree.total_nodes = read_u16_le(data + 6);
    uint16_t data_size_half = read_u16_le(data + 8);
    
    size_t data_size = static_cast<size_t>(data_size_half) * 2;
    
    if (12 + data_size <= size) {
        octree.data.assign(data + 12, data + 12 + data_size);
    } else {
        octree.data.assign(data + 12, data + size);
    }
}

// ============================================================================
// XobParser Class
// ============================================================================

XobParser::XobParser(std::span<const uint8_t> data) : data_(data) {}

std::optional<std::span<const uint8_t>> XobParser::find_chunk(const uint8_t* chunk_id) const {
    if (data_.size() < 12) return std::nullopt;
    
    for (size_t pos = 12; pos + 8 <= data_.size(); pos++) {
        if (std::memcmp(data_.data() + pos, chunk_id, 4) == 0) {
            uint32_t chunk_size = read_u32_be(data_.data() + pos + 4);
            
            if (chunk_size > 0 && chunk_size < 100000000 && pos + 8 + chunk_size <= data_.size()) {
                return data_.subspan(pos + 8, chunk_size);
            }
        }
    }
    
    return std::nullopt;
}

std::optional<XobMesh> XobParser::parse(uint32_t target_lod) {
    if (data_.size() < 12) {
        LOG_ERROR("XobParser", "Data too small");
        return std::nullopt;
    }
    
    if (std::memcmp(data_.data(), "FORM", 4) != 0) {
        LOG_ERROR("XobParser", "Invalid magic");
        return std::nullopt;
    }
    
    const char* form_type = reinterpret_cast<const char*>(data_.data() + 8);
    if (form_type[0] != 'X' || form_type[1] != 'O' || form_type[2] != 'B') {
        LOG_ERROR("XobParser", "Invalid form type");
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
    
    // Extract materials
    materials_ = extract_materials(head_data.data(), head_data.size());
    
    // Parse LOD descriptors
    descriptors_ = parse_lzo4_descriptors(head_data.data(), head_data.size());
    if (descriptors_.empty()) {
        LOG_ERROR("XobParser", "No LOD descriptors found");
        return std::nullopt;
    }
    
    if (target_lod >= descriptors_.size()) {
        target_lod = 0;
    }
    
    const auto& desc = descriptors_[target_lod];
    if (desc.unique_vertex_count == 0 || desc.triangle_count == 0) {
        LOG_ERROR("XobParser", "Invalid LOD descriptor");
        return std::nullopt;
    }
    
    // Find and decompress LODS chunk
    static constexpr uint8_t LODS_ID[4] = {'L', 'O', 'D', 'S'};
    auto lods_chunk = find_chunk(LODS_ID);
    if (!lods_chunk) {
        LOG_ERROR("XobParser", "LODS chunk not found");
        return std::nullopt;
    }
    
    auto decompressed = decompress_lz4_chained(lods_chunk->data(), lods_chunk->size());
    if (decompressed.empty()) {
        LOG_ERROR("XobParser", "Decompression failed");
        return std::nullopt;
    }
    
    // Extract LOD region
    std::vector<uint8_t> region;
    if (desc.decompressed_size > 0 && desc.decompressed_size <= decompressed.size()) {
        region.assign(decompressed.begin(), decompressed.begin() + desc.decompressed_size);
    } else {
        region = decompressed;
    }
    
    // Parse mesh
    XobMesh mesh;
    if (!parse_mesh_from_region(region, desc.unique_vertex_count, desc.triangle_count,
                                 desc.mesh_type, desc.attr_config[ATTR_CFG_BONE_STREAMS], mesh)) {
        return std::nullopt;
    }
    
    // Calculate bounds
    mesh.bounds_min = glm::vec3(FLT_MAX);
    mesh.bounds_max = glm::vec3(-FLT_MAX);
    for (const auto& vert : mesh.vertices) {
        mesh.bounds_min = glm::min(mesh.bounds_min, vert.position);
        mesh.bounds_max = glm::max(mesh.bounds_max, vert.position);
    }
    
    // Create LOD entry
    XobLod lod_data;
    lod_data.distance = desc.switch_distance;
    lod_data.index_offset = 0;
    lod_data.index_count = static_cast<uint32_t>(mesh.indices.size());
    lod_data.indices = mesh.indices;
    mesh.lods.push_back(lod_data);
    
    // Copy materials
    mesh.materials = materials_;
    
    // Extract material ranges
    if (materials_.size() > 1) {
        MaterialRangeExtractor extractor(data_, desc.triangle_count, materials_.size(), desc.mesh_type);
        mesh.material_ranges = extractor.extract();
    }
    
    if (mesh.material_ranges.empty()) {
        MaterialRange r;
        r.material_index = 0;
        r.triangle_start = 0;
        r.triangle_end = desc.triangle_count;
        r.triangle_count = desc.triangle_count;
        mesh.material_ranges.push_back(r);
    }
    
    // Parse optional chunks
    static constexpr uint8_t COLL_ID[4] = {'C', 'O', 'L', 'L'};
    auto coll_chunk = find_chunk(COLL_ID);
    if (coll_chunk) {
        parse_coll_chunk(coll_chunk->data(), coll_chunk->size(),
                         mesh.collision_objects, mesh.collision_mesh);
    }
    
    static constexpr uint8_t VOLM_ID[4] = {'V', 'O', 'L', 'M'};
    auto volm_chunk = find_chunk(VOLM_ID);
    if (volm_chunk) {
        parse_volm_chunk(volm_chunk->data(), volm_chunk->size(), mesh.octree);
    }
    
    // Extract bone count
    if (head_data.size() >= 0x30) {
        mesh.bone_count = read_u16_le(head_data.data() + 0x2E);
    }
    
    return mesh;
}

} // namespace enfusion
