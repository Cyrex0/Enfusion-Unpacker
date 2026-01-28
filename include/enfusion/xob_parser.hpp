/**
 * Enfusion Unpacker - XOB Parser
 */

#pragma once

#include "types.hpp"
#include <vector>
#include <span>
#include <optional>

namespace enfusion {

class XobParser {
public:
    static constexpr uint8_t MAGIC[4] = {'F', 'O', 'R', 'M'};
    static constexpr uint8_t FORM_TYPE[4] = {'X', 'O', 'B', '9'};
    
    explicit XobParser(std::span<const uint8_t> data);
    
    std::optional<XobMesh> parse(uint32_t lod = 0);
    const std::vector<LzoDescriptor>& descriptors() const { return descriptors_; }
    const std::vector<XobMaterial>& materials() const { return materials_; }
    uint32_t lod_count() const { return static_cast<uint32_t>(descriptors_.size()); }
    
private:
    std::optional<std::span<const uint8_t>> find_chunk(const uint8_t* chunk_id) const;
    std::vector<LzoDescriptor> parse_descriptors(std::span<const uint8_t> head_data);
    std::vector<XobMaterial> parse_materials(std::span<const uint8_t> head_data);
    std::vector<MaterialRange> parse_material_ranges(std::span<const uint8_t> head_data, uint32_t triangle_count);
    std::span<const uint8_t> extract_lod_region(std::span<const uint8_t> decompressed, uint32_t lod);
    std::optional<XobMesh> parse_mesh_region(std::span<const uint8_t> region, const LzoDescriptor& desc);
    void calculate_bounds(XobMesh& mesh);
    
    std::span<const uint8_t> data_;
    std::vector<LzoDescriptor> descriptors_;
    std::vector<XobMaterial> materials_;
};

} // namespace enfusion
