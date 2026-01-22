/**
 * XOB Material Range Extractor
 * 
 * Determines which triangles belong to which materials by parsing
 * submesh block data from the HEAD chunk.
 */

#pragma once

#include "xob_types.hpp"
#include "types.hpp"
#include <vector>
#include <span>
#include <cstdint>

namespace enfusion {
namespace xob {

/**
 * Extracts material-to-triangle range mappings from XOB HEAD chunk.
 * 
 * Multi-material meshes store submesh blocks after the LZO4 descriptors.
 * Each block defines which material is used for a range of triangles.
 */
class MaterialRangeExtractor {
public:
    MaterialRangeExtractor(std::span<const uint8_t> data, 
                           uint32_t total_triangles,
                           size_t num_materials,
                           uint8_t mesh_type);
    
    /**
     * Extract material ranges from the mesh data.
     * Returns a list of ranges, each mapping triangles to a material index.
     */
    std::vector<MaterialRange> extract();
    
    /**
     * Simplified extraction using LZO4 descriptor submesh indices.
     * Use when submesh block parsing fails.
     */
    std::vector<MaterialRange> extract_from_descriptors(
        const std::vector<LzoDescriptor>& descriptors,
        uint32_t target_lod);
    
private:
    struct SubmeshBlock {
        size_t position;
        uint32_t material_index;
        uint32_t index_count;
        uint16_t lod;
        uint16_t flags;
        uint16_t order_key;
    };
    
    std::vector<SubmeshBlock> find_submesh_blocks();
    std::vector<MaterialRange> build_ranges(const std::vector<SubmeshBlock>& blocks);
    
    std::span<const uint8_t> data_;
    uint32_t total_triangles_;
    size_t num_materials_;
    uint8_t mesh_type_;
    bool is_skinned_;
};

} // namespace xob
} // namespace enfusion
