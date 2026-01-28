/**
 * Enfusion Unpacker - Mesh Converter Implementation (Stub)
 */

#include "enfusion/mesh_converter.hpp"
#include "enfusion/xob_parser.hpp"
#include <fstream>

namespace enfusion {

MeshConverter::MeshConverter(std::span<const uint8_t> xob_data, const std::string& name)
    : xob_data_(xob_data), name_(name) {
    
    XobParser parser(xob_data_);
    mesh_ = parser.parse(0);
}

std::optional<MeshConverter::Result> MeshConverter::convert(uint32_t lod,
                                                             const fs::path& output_dir,
                                                             const fs::path& texture_search_dir) {
    if (!mesh_) return std::nullopt;
    
    Result result;
    result.obj = generate_obj();
    result.mtl = generate_mtl(output_dir, texture_search_dir);
    result.stats.vertices = static_cast<uint32_t>(mesh_->vertices.size());
    result.stats.faces = static_cast<uint32_t>(mesh_->lods.empty() ? 0 : mesh_->lods[0].indices.size() / 3);
    result.stats.materials = static_cast<uint32_t>(mesh_->materials.size());
    return result;
}

bool MeshConverter::save(const fs::path& output_dir, uint32_t lod, const fs::path& texture_search_dir) {
    auto result = convert(lod, output_dir, texture_search_dir);
    if (!result) return false;
    
    std::filesystem::create_directories(output_dir);
    
    std::ofstream obj_file(output_dir / (name_ + ".obj"));
    if (!obj_file) return false;
    obj_file << result->obj;
    
    std::ofstream mtl_file(output_dir / (name_ + ".mtl"));
    if (!mtl_file) return false;
    mtl_file << result->mtl;
    
    return true;
}

std::string MeshConverter::generate_obj() {
    if (!mesh_) return "";
    
    std::string obj;
    obj += "# Enfusion Unpacker OBJ Export\n";
    obj += "mtllib " + name_ + ".mtl\n\n";
    
    for (const auto& v : mesh_->vertices) {
        obj += "v " + std::to_string(v.position.x) + " " + std::to_string(v.position.y) + " " + std::to_string(v.position.z) + "\n";
    }
    
    return obj;
}

std::string MeshConverter::generate_mtl(const fs::path& output_dir, const fs::path& texture_search_dir) {
    std::string mtl;
    mtl += "# Enfusion Unpacker MTL Export\n\n";
    
    for (const auto& mat : mesh_->materials) {
        mtl += "newmtl " + mat.name + "\n";
        mtl += "Kd 0.8 0.8 0.8\n\n";
    }
    
    return mtl;
}

} // namespace enfusion
