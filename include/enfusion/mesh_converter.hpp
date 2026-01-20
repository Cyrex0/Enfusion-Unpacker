/**
 * Enfusion Unpacker - Mesh Converter
 * 
 * Exports XOB meshes to OBJ and FBX formats with full material support.
 */

#pragma once

#include "types.hpp"
#include <filesystem>
#include <string>
#include <span>
#include <optional>
#include <functional>

namespace enfusion {

/**
 * Export format options.
 */
enum class ExportFormat {
    OBJ,    // Wavefront OBJ + MTL
    FBX     // Autodesk FBX (ASCII)
};

/**
 * Export options for mesh conversion.
 */
struct ExportOptions {
    ExportFormat format = ExportFormat::OBJ;
    uint32_t lod = 0;
    bool export_normals = true;
    bool export_uvs = true;
    bool export_materials = true;
    bool export_textures = false;  // Copy texture files
    fs::path texture_output_dir;
};

class MeshConverter {
public:
    MeshConverter(std::span<const uint8_t> xob_data, const std::string& name = "mesh");
    MeshConverter(const XobMesh& mesh, const std::string& name = "mesh");
    
    struct Result {
        std::string primary;     // OBJ or FBX content
        std::string material;    // MTL content (for OBJ only)
        struct {
            uint32_t vertices = 0;
            uint32_t faces = 0;
            uint32_t materials = 0;
            uint32_t material_ranges = 0;
        } stats;
    };
    
    /**
     * Convert mesh to specified format.
     */
    std::optional<Result> convert(const ExportOptions& options = {});
    
    /**
     * Save mesh to file(s) in output directory.
     */
    bool save(const fs::path& output_dir, const ExportOptions& options = {});
    
    /**
     * Set texture loader for texture export.
     */
    void set_texture_loader(std::function<std::vector<uint8_t>(const std::string&)> loader) {
        texture_loader_ = loader;
    }
    
    const XobMesh* mesh() const { return mesh_ ? &*mesh_ : nullptr; }
    
private:
    std::string generate_obj(const ExportOptions& options);
    std::string generate_mtl(const ExportOptions& options);
    std::string generate_fbx(const ExportOptions& options);
    
    std::span<const uint8_t> xob_data_;
    std::string name_;
    std::optional<XobMesh> mesh_;
    std::function<std::vector<uint8_t>(const std::string&)> texture_loader_;
};

} // namespace enfusion
