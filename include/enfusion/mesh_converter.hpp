/**
 * Enfusion Unpacker - Mesh Converter
 */

#pragma once

#include "types.hpp"
#include <filesystem>
#include <string>
#include <span>
#include <optional>

namespace enfusion {

class MeshConverter {
public:
    MeshConverter(std::span<const uint8_t> xob_data, const std::string& name = "mesh");
    
    struct Result {
        std::string obj;
        std::string mtl;
        struct {
            uint32_t vertices = 0;
            uint32_t faces = 0;
            uint32_t materials = 0;
        } stats;
    };
    
    std::optional<Result> convert(uint32_t lod = 0, 
                                   const fs::path& output_dir = {},
                                   const fs::path& texture_search_dir = {});
    
    bool save(const fs::path& output_dir, uint32_t lod = 0,
              const fs::path& texture_search_dir = {});
    
    const XobMesh* mesh() const { return mesh_ ? &*mesh_ : nullptr; }
    
private:
    std::string generate_obj();
    std::string generate_mtl(const fs::path& output_dir, const fs::path& texture_search_dir);
    
    std::span<const uint8_t> xob_data_;
    std::string name_;
    std::optional<XobMesh> mesh_;
};

} // namespace enfusion
