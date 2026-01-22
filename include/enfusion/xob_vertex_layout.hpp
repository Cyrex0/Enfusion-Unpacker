/**
 * XOB Vertex Layout Detection
 * 
 * Detects vertex stream layout (separated vs interleaved) by probing
 * the decompressed mesh data for valid UV and position patterns.
 */

#pragma once

#include "xob_types.hpp"
#include <vector>
#include <cstdint>

namespace enfusion {
namespace xob {

/**
 * Detects vertex data layout from decompressed mesh region.
 * 
 * XOB meshes can use either:
 * - Separated streams: [positions][normals][tangents][UVs]...
 * - Interleaved: [pos+norm+tan+uv per vertex]...
 * 
 * This class probes the data to determine which layout is used
 * and locates the UV stream offset.
 */
class VertexLayoutDetector {
public:
    VertexLayoutDetector(const std::vector<uint8_t>& region,
                         uint16_t vertex_count,
                         uint16_t triangle_count,
                         uint8_t mesh_type,
                         uint8_t bone_streams);
    
    VertexLayout detect();
    
private:
    // Probe for valid half-float UVs at offset
    bool probe_uvs_f16(size_t offset) const;
    
    // Probe for valid positions at offset
    bool probe_positions(size_t offset) const;
    
    // Scan for UV stream when probes fail
    void scan_for_uv_stream(VertexLayout& layout);
    
    const std::vector<uint8_t>& region_;
    uint16_t vertex_count_;
    uint16_t triangle_count_;
    uint8_t mesh_type_;
    uint8_t bone_streams_;
    
    size_t idx_array_size_;
    size_t dual_idx_array_size_;
};

} // namespace xob
} // namespace enfusion
