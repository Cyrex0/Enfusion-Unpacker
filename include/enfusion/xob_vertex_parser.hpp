/**
 * XOB Vertex Stream Parser
 * 
 * Extracts vertex data from decompressed mesh regions using
 * the detected layout (separated or interleaved streams).
 */

#pragma once

#include "xob_types.hpp"
#include "types.hpp"
#include <vector>
#include <cstdint>

namespace enfusion {
namespace xob {

/**
 * Parses vertex data from a mesh region using the detected layout.
 */
class VertexStreamParser {
public:
    VertexStreamParser(const std::vector<uint8_t>& region,
                       uint16_t vertex_count,
                       uint8_t mesh_type,
                       const VertexLayout& layout);
    
    /**
     * Parse all vertices from the region.
     * Returns true if at least some vertices were successfully parsed.
     */
    bool parse(std::vector<XobVertex>& vertices);
    
    /**
     * Parse indices from the region.
     * Uses the first index array (XOB has dual arrays).
     */
    bool parse_indices(uint16_t triangle_count, std::vector<uint32_t>& indices);
    
private:
    void parse_vertex_separated(size_t idx, XobVertex& vert);
    void parse_vertex_interleaved(size_t idx, XobVertex& vert);
    
    glm::vec3 read_normal(size_t offset) const;
    glm::vec3 read_tangent(size_t offset, float& sign) const;
    glm::vec2 read_uv(size_t offset) const;
    
    const std::vector<uint8_t>& region_;
    uint16_t vertex_count_;
    uint8_t mesh_type_;
    VertexLayout layout_;
    size_t vertex_stride_;
    bool has_embedded_uvs_;
};

} // namespace xob
} // namespace enfusion
