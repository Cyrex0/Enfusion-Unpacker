/**
 * XOB Vertex Layout Detection Implementation
 */

#include "enfusion/xob_vertex_layout.hpp"
#include "enfusion/logging.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace enfusion {
namespace xob {

VertexLayoutDetector::VertexLayoutDetector(
    const std::vector<uint8_t>& region,
    uint16_t vertex_count,
    uint16_t triangle_count,
    uint8_t mesh_type,
    uint8_t bone_streams)
    : region_(region)
    , vertex_count_(vertex_count)
    , triangle_count_(triangle_count)
    , mesh_type_(mesh_type)
    , bone_streams_(bone_streams)
{
    uint32_t index_count = static_cast<uint32_t>(triangle_count) * 3;
    idx_array_size_ = index_count * INDEX_SIZE;
    dual_idx_array_size_ = idx_array_size_ * 2;
}

bool VertexLayoutDetector::probe_uvs_f16(size_t offset) const {
    if (offset + 8 > region_.size()) return false;
    
    // Sample vertices across the stream
    std::vector<uint32_t> samples = {0, 1, 2, 3, 4, 5, 7, 9};
    if (vertex_count_ > 4) {
        samples.push_back(vertex_count_ / 4);
        samples.push_back(vertex_count_ / 2);
        samples.push_back((vertex_count_ * 3) / 4);
        samples.push_back(vertex_count_ - 1);
    }
    
    int valid_count = 0;
    int nonzero_count = 0;
    float max_abs = 0.0f;
    int checked = 0;
    
    for (uint32_t idx : samples) {
        if (idx >= vertex_count_) continue;
        size_t off = offset + static_cast<size_t>(idx) * 4;
        if (off + 4 > region_.size()) continue;
        
        float u = half_to_float(read_u16_le(region_.data() + off));
        float v = half_to_float(read_u16_le(region_.data() + off + 2));
        
        if (std::isfinite(u) && std::isfinite(v)) {
            max_abs = std::max(max_abs, std::abs(u));
            max_abs = std::max(max_abs, std::abs(v));
            if (std::abs(u) > 1e-4f || std::abs(v) > 1e-4f) nonzero_count++;
            if (u >= -8.0f && u <= 8.0f && v >= -8.0f && v <= 8.0f) valid_count++;
        }
        checked++;
    }
    
    return checked > 0 && valid_count >= std::max(3, checked * 6 / 10) 
           && nonzero_count >= 2 && max_abs <= 16.0f;
}

bool VertexLayoutDetector::probe_positions(size_t offset) const {
    if (offset + 48 > region_.size()) return false;
    
    size_t stride = (mesh_type_ == MESH_SKINNED || mesh_type_ == MESH_SKINNED_EMISSIVE) ? 16 : 12;
    int valid_count = 0;
    
    for (int i = 0; i < 4; i++) {
        float x = read_f32_le(region_.data() + offset + i * stride);
        float y = read_f32_le(region_.data() + offset + i * stride + 4);
        float z = read_f32_le(region_.data() + offset + i * stride + 8);
        
        bool valid = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                     std::abs(x) < 1000.0f && std::abs(y) < 1000.0f && std::abs(z) < 1000.0f &&
                     (std::abs(x) > 1e-6f || std::abs(y) > 1e-6f || std::abs(z) > 1e-6f);
        if (valid) valid_count++;
    }
    
    return valid_count >= 3;
}

void VertexLayoutDetector::scan_for_uv_stream(VertexLayout& layout) {
    size_t scan_start = layout.tangent_offset + static_cast<size_t>(vertex_count_) * TANGENT_SIZE;
    size_t scan_limit = std::min(region_.size(), scan_start + static_cast<size_t>(vertex_count_) * 64);
    
    struct UvCandidate {
        size_t offset = 0;
        bool is_f32 = false;
        int valid = 0;
        int nonzero = 0;
        float max_abs = 0.0f;
    };
    
    auto score = [&](size_t offset, bool is_f32) -> UvCandidate {
        size_t elem_size = is_f32 ? 8 : 4;
        if (offset + static_cast<size_t>(vertex_count_) * elem_size > region_.size()) {
            return {offset, is_f32, 0, 0, 0.0f};
        }
        
        UvCandidate c{offset, is_f32, 0, 0, 0.0f};
        const uint32_t samples = 64;
        
        for (uint32_t s = 0; s < samples; s++) {
            uint32_t idx = (vertex_count_ > 1) ? (s * (vertex_count_ - 1) / (samples - 1)) : 0;
            size_t off = offset + static_cast<size_t>(idx) * elem_size;
            if (off + elem_size > region_.size()) continue;
            
            float u, v;
            if (is_f32) {
                u = read_f32_le(region_.data() + off);
                v = read_f32_le(region_.data() + off + 4);
            } else {
                u = half_to_float(read_u16_le(region_.data() + off));
                v = half_to_float(read_u16_le(region_.data() + off + 2));
            }
            
            if (std::isfinite(u) && std::isfinite(v)) {
                c.max_abs = std::max(c.max_abs, std::max(std::abs(u), std::abs(v)));
                if (std::abs(u) > 1e-4f || std::abs(v) > 1e-4f) c.nonzero++;
                if (u >= -4.0f && u <= 4.0f && v >= -4.0f && v <= 4.0f) c.valid++;
            }
        }
        return c;
    };
    
    UvCandidate best;
    
    // Scan f16 UVs
    for (size_t scan = scan_start; scan + 4 <= scan_limit; scan += 4) {
        auto c = score(scan, false);
        if (c.valid > best.valid || (c.valid == best.valid && c.nonzero > best.nonzero)) {
            best = c;
        }
    }
    
    // Scan f32 UVs
    for (size_t scan = scan_start; scan + 8 <= scan_limit; scan += 8) {
        auto c = score(scan, true);
        if (c.valid > best.valid || (c.valid == best.valid && c.nonzero > best.nonzero)) {
            best = c;
        }
    }
    
    if (best.offset > 0 && best.valid >= 12 && best.nonzero >= 6 && best.max_abs <= 4.0f) {
        layout.found_uv_offset = best.offset;
        layout.uv_is_f32 = best.is_f32;
        layout.uv_probes_valid = true;
        LOG_INFO("XobParser", "UV scan found stream at " << best.offset 
                 << " format=" << (best.is_f32 ? "f32" : "f16"));
    }
}

VertexLayout VertexLayoutDetector::detect() {
    VertexLayout layout;
    layout.vertex_data_offset = dual_idx_array_size_;
    
    if (vertex_count_ == 0 || region_.size() <= layout.vertex_data_offset) {
        return layout;
    }
    
    // Stream sizes
    size_t pos_stream = static_cast<size_t>(vertex_count_) * 12;
    size_t norm_stream = static_cast<size_t>(vertex_count_) * NORMAL_SIZE;
    size_t tan_stream = static_cast<size_t>(vertex_count_) * TANGENT_SIZE;
    size_t color_stream = static_cast<size_t>(vertex_count_) * VERTEX_COLOR_SIZE;
    size_t uv_stream = static_cast<size_t>(vertex_count_) * UV_SIZE_HALF;
    
    // Calculate test offsets for dual index layout
    size_t base_dual = layout.vertex_data_offset + pos_stream + norm_stream + tan_stream;
    size_t uv_offset_dual = base_dual;
    size_t uv_offset_dual_color = base_dual + color_stream;
    
    // Single index layout
    size_t single_vertex_offset = idx_array_size_;
    size_t base_single = single_vertex_offset + pos_stream + norm_stream + tan_stream;
    size_t uv_offset_single = base_single;
    size_t uv_offset_single_color = base_single + color_stream;
    
    // Try probing UV locations
    if (probe_uvs_f16(uv_offset_dual)) {
        layout.use_separated_streams = true;
        layout.uv_probes_valid = true;
        layout.found_uv_offset = uv_offset_dual;
    }
    else if (probe_uvs_f16(uv_offset_dual_color)) {
        layout.use_separated_streams = true;
        layout.has_color_before_uv = true;
        layout.uv_probes_valid = true;
        layout.found_uv_offset = uv_offset_dual_color;
    }
    else if (probe_uvs_f16(uv_offset_single) && probe_positions(single_vertex_offset)) {
        layout.use_separated_streams = true;
        layout.use_single_index = true;
        layout.uv_probes_valid = true;
        layout.found_uv_offset = uv_offset_single;
        layout.vertex_data_offset = single_vertex_offset;
    }
    else if (probe_uvs_f16(uv_offset_single_color) && probe_positions(single_vertex_offset)) {
        layout.use_separated_streams = true;
        layout.use_single_index = true;
        layout.has_color_before_uv = true;
        layout.uv_probes_valid = true;
        layout.found_uv_offset = uv_offset_single_color;
        layout.vertex_data_offset = single_vertex_offset;
    }
    // Fallback: check if positions look valid
    else if (probe_positions(layout.vertex_data_offset)) {
        layout.use_separated_streams = true;
    }
    else if (probe_positions(single_vertex_offset)) {
        layout.use_separated_streams = true;
        layout.use_single_index = true;
        layout.vertex_data_offset = single_vertex_offset;
    }
    
    // Calculate stream offsets
    bool is_skinned = (mesh_type_ == MESH_SKINNED || mesh_type_ == MESH_SKINNED_EMISSIVE);
    layout.position_stride = is_skinned ? POSITION_STRIDE_16 : POSITION_STRIDE_12;
    
    size_t actual_pos_stream = static_cast<size_t>(vertex_count_) * layout.position_stride;
    layout.pos_offset = layout.vertex_data_offset;
    layout.norm_offset = layout.pos_offset + actual_pos_stream;
    layout.tangent_offset = layout.norm_offset + norm_stream;
    
    // If UV probes failed but we have separated streams, try scanning
    if (layout.use_separated_streams && !layout.uv_probes_valid) {
        scan_for_uv_stream(layout);
    }
    
    // Calculate UV offsets
    size_t extra_data = layout.has_color_before_uv ? color_stream : 0;
    if (layout.uv_probes_valid && layout.found_uv_offset > 0) {
        layout.uv0_offset = layout.found_uv_offset;
    } else {
        layout.uv0_offset = layout.tangent_offset + tan_stream + extra_data;
    }
    layout.uv1_offset = layout.uv0_offset + uv_stream;
    layout.color_offset = layout.uv1_offset + uv_stream;
    
    return layout;
}

} // namespace xob
} // namespace enfusion
