/**
 * XOB Material Range Extractor Implementation
 */

#include "enfusion/xob_material_ranges.hpp"
#include "enfusion/logging.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace enfusion {
namespace xob {

MaterialRangeExtractor::MaterialRangeExtractor(
    std::span<const uint8_t> data,
    uint32_t total_triangles,
    size_t num_materials,
    uint8_t mesh_type)
    : data_(data)
    , total_triangles_(total_triangles)
    , num_materials_(num_materials)
    , mesh_type_(mesh_type)
    , is_skinned_(mesh_type == MESH_SKINNED || mesh_type == MESH_SKINNED_EMISSIVE)
{
}

std::vector<MaterialRange> MaterialRangeExtractor::extract_from_descriptors(
    const std::vector<LzoDescriptor>& descriptors,
    uint32_t target_lod)
{
    std::vector<MaterialRange> result;
    
    if (num_materials_ <= 1 || descriptors.empty()) {
        MaterialRange r;
        r.material_index = 0;
        r.triangle_start = 0;
        r.triangle_end = total_triangles_;
        r.triangle_count = total_triangles_;
        result.push_back(r);
        return result;
    }
    
    // Group descriptors by LOD
    std::map<uint32_t, std::vector<size_t>> lod_map;
    for (size_t i = 0; i < descriptors.size(); i++) {
        lod_map[descriptors[i].quality_tier].push_back(i);
    }
    
    std::vector<size_t> target_descs;
    if (lod_map.count(target_lod)) {
        target_descs = lod_map[target_lod];
    } else if (!lod_map.empty()) {
        target_descs = lod_map.begin()->second;
    }
    
    if (target_descs.size() <= 1) {
        uint32_t mat_idx = !descriptors.empty() ? descriptors[0].submesh_index : 0;
        if (mat_idx >= num_materials_) mat_idx = 0;
        
        MaterialRange r;
        r.material_index = mat_idx;
        r.triangle_start = 0;
        r.triangle_end = total_triangles_;
        r.triangle_count = total_triangles_;
        result.push_back(r);
        return result;
    }
    
    // Build ranges from multiple descriptors
    std::vector<std::pair<uint32_t, uint32_t>> submesh_ranges;
    for (size_t idx : target_descs) {
        const auto& desc = descriptors[idx];
        uint32_t mat_idx = desc.submesh_index;
        if (mat_idx >= num_materials_) mat_idx = 0;
        submesh_ranges.push_back({mat_idx, desc.triangle_count});
    }
    
    std::sort(submesh_ranges.begin(), submesh_ranges.end());
    
    uint32_t current_tri = 0;
    for (const auto& [mat_idx, tri_count] : submesh_ranges) {
        if (tri_count == 0 || current_tri >= total_triangles_) continue;
        
        uint32_t actual = std::min(tri_count, total_triangles_ - current_tri);
        
        MaterialRange r;
        r.material_index = mat_idx;
        r.triangle_start = current_tri;
        r.triangle_end = current_tri + actual;
        r.triangle_count = actual;
        result.push_back(r);
        
        current_tri += actual;
    }
    
    // Handle remaining triangles
    if (current_tri < total_triangles_) {
        uint32_t remaining = total_triangles_ - current_tri;
        bool found = false;
        for (auto& r : result) {
            if (r.material_index == 0) {
                r.triangle_end = total_triangles_;
                r.triangle_count += remaining;
                found = true;
                break;
            }
        }
        if (!found) {
            MaterialRange r;
            r.material_index = 0;
            r.triangle_start = current_tri;
            r.triangle_end = total_triangles_;
            r.triangle_count = remaining;
            result.push_back(r);
        }
    }
    
    if (result.empty()) {
        MaterialRange r;
        r.material_index = 0;
        r.triangle_start = 0;
        r.triangle_end = total_triangles_;
        r.triangle_count = total_triangles_;
        result.push_back(r);
    }
    
    return result;
}

std::vector<MaterialRangeExtractor::SubmeshBlock> MaterialRangeExtractor::find_submesh_blocks() {
    std::vector<SubmeshBlock> blocks;
    
    // Find HEAD chunk
    const uint8_t* head_start = nullptr;
    size_t head_size = 0;
    
    for (size_t i = 0; i + 8 < data_.size(); i++) {
        if (data_[i] == 'H' && data_[i+1] == 'E' && data_[i+2] == 'A' && data_[i+3] == 'D') {
            head_size = read_u32_be(data_.data() + i + 4);
            head_start = data_.data() + i + 8;
            break;
        }
    }
    
    if (!head_start || head_size == 0) return blocks;
    
    // Find LZO4 descriptors to locate submesh data
    size_t after_lzo4 = 0;
    for (size_t i = 0; i + 4 <= head_size; i++) {
        if (head_start[i] == 'L' && head_start[i+1] == 'Z' && 
            head_start[i+2] == 'O' && head_start[i+3] == '4') {
            size_t desc_end = i + LZO4_DESCRIPTOR_SIZE;
            if (desc_end > after_lzo4) after_lzo4 = desc_end;
            i += LZO4_DESCRIPTOR_SIZE - 1;
        }
    }
    
    if (after_lzo4 >= head_size) return blocks;
    
    const uint8_t* after = head_start + after_lzo4;
    size_t after_size = head_size - after_lzo4;
    
    // Scan for 0xFFFF markers (submesh block terminators)
    for (size_t pos = 8; pos + 6 <= after_size; pos++) {
        if (after[pos] == 0xFF && after[pos + 1] == 0xFF) {
            SubmeshBlock block;
            block.position = pos;
            block.material_index = read_u16_le(after + pos + 2);
            block.flags = read_u16_le(after + pos + 4);
            block.index_count = read_u16_le(after + pos - 6);
            block.lod = read_u16_le(after + pos - 4);
            block.order_key = (pos >= 10) ? read_u16_le(after + pos - 10) : 0;
            
            if (block.material_index < num_materials_) {
                // Fix LOD for skinned meshes
                if (is_skinned_) {
                    uint16_t base = block.flags & 0xFF;
                    uint16_t upper = (block.flags >> 8) & 0xFF;
                    
                    if (base == 0x02 || (base == 0x01 && upper == 0)) {
                        block.lod = 0;
                    } else if (base == 0x01 && upper > 0) {
                        block.lod = upper;
                    } else if (block.lod > 10) {
                        block.lod = 0;
                    }
                }
                
                blocks.push_back(block);
            }
            pos += 5;
        }
    }
    
    return blocks;
}

std::vector<MaterialRange> MaterialRangeExtractor::build_ranges(
    const std::vector<SubmeshBlock>& all_blocks)
{
    std::vector<MaterialRange> result;
    uint32_t total_indices = total_triangles_ * 3;
    
    if (all_blocks.empty()) {
        MaterialRange r;
        r.material_index = 0;
        r.triangle_start = 0;
        r.triangle_end = total_triangles_;
        r.triangle_count = total_triangles_;
        result.push_back(r);
        return result;
    }
    
    // Find target LOD with best coverage
    std::map<uint16_t, uint32_t> lod_sums;
    for (const auto& block : all_blocks) {
        if (block.lod <= 10) {
            lod_sums[block.lod] += block.index_count;
        }
    }
    
    uint16_t target_lod = 0;
    uint32_t best_sum = 0;
    for (const auto& [lod, sum] : lod_sums) {
        if (sum > best_sum) {
            target_lod = lod;
            best_sum = sum;
        }
    }
    
    // Count passes per material
    std::map<uint32_t, uint32_t> pass_counts;
    std::map<uint32_t, uint32_t> total_idx;
    
    for (const auto& block : all_blocks) {
        if (block.lod != target_lod) continue;
        pass_counts[block.material_index]++;
        total_idx[block.material_index] += block.index_count;
    }
    
    // Collect unique materials in file order
    std::set<uint32_t> seen;
    std::vector<SubmeshBlock> ordered;
    
    for (const auto& block : all_blocks) {
        if (block.lod > 10) continue;
        
        if (!seen.count(block.material_index)) {
            seen.insert(block.material_index);
            
            SubmeshBlock corrected = block;
            
            // Correct for multi-pass
            if (block.lod == target_lod) {
                uint32_t passes = pass_counts[block.material_index];
                if (passes > 1 && !is_skinned_ && (block.flags & 0x2)) {
                    corrected.index_count = total_idx[block.material_index] / passes;
                }
            }
            
            ordered.push_back(corrected);
        }
    }
    
    // Sort by order key
    std::sort(ordered.begin(), ordered.end(),
              [](const SubmeshBlock& a, const SubmeshBlock& b) {
                  if (a.order_key == 0 && b.order_key == 0)
                      return a.material_index < b.material_index;
                  if (a.order_key == 0) return false;
                  if (b.order_key == 0) return true;
                  return a.order_key < b.order_key;
              });
    
    // Build ranges with proportional allocation
    uint32_t block_total = 0;
    for (const auto& block : ordered) {
        block_total += block.index_count / 3;
    }
    
    const uint32_t SMALL_THRESHOLD = 10000;
    uint32_t small_total = 0, large_count = 0;
    
    for (const auto& block : ordered) {
        uint32_t tris = block.index_count / 3;
        if (tris < SMALL_THRESHOLD) small_total += tris;
        else large_count++;
    }
    
    uint32_t large_budget = (total_triangles_ > small_total) ? (total_triangles_ - small_total) : 0;
    uint32_t large_claimed = 0;
    for (const auto& block : ordered) {
        uint32_t tris = block.index_count / 3;
        if (tris >= SMALL_THRESHOLD) large_claimed += tris;
    }
    
    uint32_t current = 0;
    for (size_t i = 0; i < ordered.size(); i++) {
        const auto& block = ordered[i];
        uint32_t block_tris = block.index_count / 3;
        uint32_t tri_count;
        
        if (block_tris < SMALL_THRESHOLD) {
            tri_count = block_tris;
        } else if (large_claimed > 0) {
            float ratio = static_cast<float>(block_tris) / static_cast<float>(large_claimed);
            tri_count = static_cast<uint32_t>(large_budget * ratio + 0.5f);
        } else {
            tri_count = large_budget / std::max(1u, large_count);
        }
        
        if (current + tri_count > total_triangles_)
            tri_count = total_triangles_ - current;
        
        if (i == ordered.size() - 1)
            tri_count = total_triangles_ - current;
        
        if (tri_count == 0) continue;
        
        MaterialRange r;
        r.material_index = block.material_index;
        r.triangle_start = current;
        r.triangle_end = current + tri_count;
        r.triangle_count = tri_count;
        result.push_back(r);
        
        current += tri_count;
    }
    
    if (result.empty()) {
        MaterialRange r;
        r.material_index = 0;
        r.triangle_start = 0;
        r.triangle_end = total_triangles_;
        r.triangle_count = total_triangles_;
        result.push_back(r);
    }
    
    return result;
}

std::vector<MaterialRange> MaterialRangeExtractor::extract() {
    if (num_materials_ <= 1) {
        std::vector<MaterialRange> result;
        MaterialRange r;
        r.material_index = 0;
        r.triangle_start = 0;
        r.triangle_end = total_triangles_;
        r.triangle_count = total_triangles_;
        result.push_back(r);
        return result;
    }
    
    auto blocks = find_submesh_blocks();
    return build_ranges(blocks);
}

} // namespace xob
} // namespace enfusion
