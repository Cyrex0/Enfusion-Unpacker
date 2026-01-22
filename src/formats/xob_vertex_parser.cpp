/**
 * XOB Vertex Stream Parser Implementation
 */

#include "enfusion/xob_vertex_parser.hpp"
#include "enfusion/logging.hpp"
#include <algorithm>
#include <cmath>

namespace enfusion {
namespace xob {

VertexStreamParser::VertexStreamParser(
    const std::vector<uint8_t>& region,
    uint16_t vertex_count,
    uint8_t mesh_type,
    const VertexLayout& layout)
    : region_(region)
    , vertex_count_(vertex_count)
    , mesh_type_(mesh_type)
    , layout_(layout)
{
    // All mesh types have embedded UVs in separated streams
    has_embedded_uvs_ = true;
    
    // Calculate vertex stride for interleaved layout
    switch (mesh_type_) {
        case MESH_EMISSIVE:
        case MESH_SKINNED_EMISSIVE:
            vertex_stride_ = 32;
            break;
        default:
            vertex_stride_ = 20;
            break;
    }
}

glm::vec3 VertexStreamParser::read_normal(size_t offset) const {
    if (offset + NORMAL_SIZE > region_.size()) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    
    int8_t nx = static_cast<int8_t>(region_[offset]);
    int8_t ny = static_cast<int8_t>(region_[offset + 1]);
    int8_t nz = static_cast<int8_t>(region_[offset + 2]);
    
    glm::vec3 n(
        static_cast<float>(nx) / 127.0f,
        static_cast<float>(ny) / 127.0f,
        static_cast<float>(nz) / 127.0f
    );
    
    float len = glm::length(n);
    return len > 0.001f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

glm::vec3 VertexStreamParser::read_tangent(size_t offset, float& sign) const {
    if (offset + TANGENT_SIZE > region_.size()) {
        sign = 1.0f;
        return glm::vec3(1.0f, 0.0f, 0.0f);
    }
    
    int8_t tx = static_cast<int8_t>(region_[offset]);
    int8_t ty = static_cast<int8_t>(region_[offset + 1]);
    int8_t tz = static_cast<int8_t>(region_[offset + 2]);
    int8_t tw = static_cast<int8_t>(region_[offset + 3]);
    
    glm::vec3 t(
        static_cast<float>(tx) / 127.0f,
        static_cast<float>(ty) / 127.0f,
        static_cast<float>(tz) / 127.0f
    );
    
    float len = glm::length(t);
    sign = (tw >= 0) ? 1.0f : -1.0f;
    return len > 0.001f ? t / len : glm::vec3(1.0f, 0.0f, 0.0f);
}

glm::vec2 VertexStreamParser::read_uv(size_t offset) const {
    if (layout_.uv_is_f32) {
        if (offset + 8 > region_.size()) return glm::vec2(0.0f);
        float u = read_f32_le(region_.data() + offset);
        float v = read_f32_le(region_.data() + offset + 4);
        return glm::vec2(u, 1.0f - v);  // Flip V for OpenGL
    } else {
        if (offset + 4 > region_.size()) return glm::vec2(0.0f);
        float u = half_to_float(read_u16_le(region_.data() + offset));
        float v = half_to_float(read_u16_le(region_.data() + offset + 2));
        return glm::vec2(u, 1.0f - v);  // Flip V for OpenGL
    }
}

void VertexStreamParser::parse_vertex_separated(size_t idx, XobVertex& vert) {
    const uint8_t* data = region_.data();
    
    // Position
    size_t p_off = layout_.pos_offset + idx * layout_.position_stride;
    if (p_off + 12 <= region_.size()) {
        vert.position.x = read_f32_le(data + p_off);
        vert.position.y = read_f32_le(data + p_off + 4);
        vert.position.z = read_f32_le(data + p_off + 8);
    }
    
    // Normal
    size_t n_off = layout_.norm_offset + idx * NORMAL_SIZE;
    vert.normal = read_normal(n_off);
    
    // Tangent
    size_t t_off = layout_.tangent_offset + idx * TANGENT_SIZE;
    vert.tangent = read_tangent(t_off, vert.tangent_sign);
    
    // UVs
    if (has_embedded_uvs_) {
        if (layout_.uv_probes_valid) {
            size_t uv_elem = layout_.uv_is_f32 ? 8 : 4;
            size_t uv_off = layout_.uv0_offset + idx * uv_elem;
            vert.uv = read_uv(uv_off);
        } else {
            // Fallback: planar projection from position
            vert.uv.x = (vert.position.x + 5.0f) * 0.1f;
            vert.uv.y = (vert.position.z + 5.0f) * 0.1f;
        }
    }
}

void VertexStreamParser::parse_vertex_interleaved(size_t idx, XobVertex& vert) {
    size_t off = layout_.vertex_data_offset + idx * vertex_stride_;
    if (off + 12 > region_.size()) {
        vert.position = glm::vec3(0.0f);
        vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        vert.uv = glm::vec2(0.0f);
        return;
    }
    
    const uint8_t* data = region_.data();
    size_t attr_off = off;
    
    // Position (12 bytes)
    vert.position.x = read_f32_le(data + attr_off);
    vert.position.y = read_f32_le(data + attr_off + 4);
    vert.position.z = read_f32_le(data + attr_off + 8);
    attr_off += 12;
    
    // Normal (4 bytes)
    if (attr_off + NORMAL_SIZE <= region_.size()) {
        vert.normal = read_normal(attr_off);
        attr_off += NORMAL_SIZE;
    }
    
    // Tangent (4 bytes)
    if (attr_off + TANGENT_SIZE <= region_.size()) {
        vert.tangent = read_tangent(attr_off, vert.tangent_sign);
        attr_off += TANGENT_SIZE;
    }
    
    // UV (4 bytes as half-floats)
    if (has_embedded_uvs_ && attr_off + 4 <= region_.size()) {
        vert.uv = read_uv(attr_off);
    }
}

bool VertexStreamParser::parse(std::vector<XobVertex>& vertices) {
    vertices.clear();
    vertices.reserve(vertex_count_);
    
    for (uint16_t i = 0; i < vertex_count_; i++) {
        XobVertex vert;
        
        if (layout_.use_separated_streams) {
            parse_vertex_separated(i, vert);
        } else {
            parse_vertex_interleaved(i, vert);
        }
        
        vertices.push_back(vert);
    }
    
    return !vertices.empty();
}

bool VertexStreamParser::parse_indices(uint16_t triangle_count, std::vector<uint32_t>& indices) {
    uint32_t index_count = static_cast<uint32_t>(triangle_count) * 3;
    size_t idx_array_size = index_count * INDEX_SIZE;
    
    indices.clear();
    indices.reserve(index_count);
    
    // Read from first index array
    for (uint32_t i = 0; i < index_count && (i * 2 + 2) <= idx_array_size; i++) {
        uint16_t idx = read_u16_le(region_.data() + i * 2);
        indices.push_back(static_cast<uint32_t>(idx));
    }
    
    // Validate and clamp out-of-range indices
    for (uint32_t& idx : indices) {
        if (idx >= vertex_count_) idx = 0;
    }
    
    return !indices.empty();
}

} // namespace xob
} // namespace enfusion
