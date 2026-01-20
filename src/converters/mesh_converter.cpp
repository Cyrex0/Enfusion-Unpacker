/**
 * Enfusion Unpacker - Mesh Converter Implementation
 * 
 * Full OBJ and FBX export with materials and material ranges.
 */

#include "enfusion/mesh_converter.hpp"
#include "enfusion/xob_parser.hpp"
#include "enfusion/files.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <set>

namespace enfusion {

MeshConverter::MeshConverter(std::span<const uint8_t> xob_data, const std::string& name)
    : xob_data_(xob_data), name_(name) {
    
    XobParser parser(xob_data_);
    mesh_ = parser.parse(0);
}

MeshConverter::MeshConverter(const XobMesh& mesh, const std::string& name)
    : name_(name) {
    mesh_ = mesh;
}

std::optional<MeshConverter::Result> MeshConverter::convert(const ExportOptions& options) {
    if (!mesh_) return std::nullopt;
    
    Result result;
    
    if (options.format == ExportFormat::OBJ) {
        result.primary = generate_obj(options);
        result.material = generate_mtl(options);
    } else if (options.format == ExportFormat::FBX) {
        result.primary = generate_fbx(options);
        result.material = ""; // FBX embeds materials
    }
    
    result.stats.vertices = static_cast<uint32_t>(mesh_->vertices.size());
    result.stats.faces = static_cast<uint32_t>(mesh_->indices.size() / 3);
    result.stats.materials = static_cast<uint32_t>(mesh_->materials.size());
    result.stats.material_ranges = static_cast<uint32_t>(mesh_->material_ranges.size());
    
    return result;
}

bool MeshConverter::save(const fs::path& output_dir, const ExportOptions& options) {
    auto result = convert(options);
    if (!result) return false;
    
    std::filesystem::create_directories(output_dir);
    
    if (options.format == ExportFormat::OBJ) {
        std::ofstream obj_file(output_dir / (name_ + ".obj"));
        if (!obj_file) return false;
        obj_file << result->primary;
        
        std::ofstream mtl_file(output_dir / (name_ + ".mtl"));
        if (!mtl_file) return false;
        mtl_file << result->material;
    } else if (options.format == ExportFormat::FBX) {
        std::ofstream fbx_file(output_dir / (name_ + ".fbx"));
        if (!fbx_file) return false;
        fbx_file << result->primary;
    }
    
    return true;
}

std::string MeshConverter::generate_obj(const ExportOptions& options) {
    if (!mesh_) return "";
    
    std::ostringstream obj;
    obj << std::fixed << std::setprecision(6);
    
    // Header
    obj << "# Enfusion Unpacker - OBJ Export\n";
    obj << "# Vertices: " << mesh_->vertices.size() << "\n";
    obj << "# Triangles: " << mesh_->indices.size() / 3 << "\n";
    obj << "# Materials: " << mesh_->materials.size() << "\n";
    obj << "# Material Ranges: " << mesh_->material_ranges.size() << "\n";
    obj << "\n";
    
    // MTL reference
    obj << "mtllib " << name_ << ".mtl\n\n";
    
    // Write all vertices
    for (const auto& v : mesh_->vertices) {
        obj << "v " << v.position.x << " " << v.position.y << " " << v.position.z << "\n";
    }
    obj << "\n";
    
    // Write all normals
    if (options.export_normals) {
        for (const auto& v : mesh_->vertices) {
            obj << "vn " << v.normal.x << " " << v.normal.y << " " << v.normal.z << "\n";
        }
        obj << "\n";
    }
    
    // Write all UVs
    if (options.export_uvs) {
        for (const auto& v : mesh_->vertices) {
            obj << "vt " << v.uv.x << " " << v.uv.y << "\n";
        }
        obj << "\n";
    }
    
    // Write faces grouped by material
    if (!mesh_->material_ranges.empty() && options.export_materials) {
        // Sort ranges by triangle start
        auto sorted_ranges = mesh_->material_ranges;
        std::sort(sorted_ranges.begin(), sorted_ranges.end(),
                  [](const MaterialRange& a, const MaterialRange& b) {
                      return a.triangle_start < b.triangle_start;
                  });
        
        for (size_t range_idx = 0; range_idx < sorted_ranges.size(); range_idx++) {
            const auto& range = sorted_ranges[range_idx];
            
            // Get material name
            std::string mat_name = "material_" + std::to_string(range.material_index);
            if (range.material_index < mesh_->materials.size()) {
                mat_name = mesh_->materials[range.material_index].name;
                // Sanitize name for OBJ
                std::replace(mat_name.begin(), mat_name.end(), ' ', '_');
                std::replace(mat_name.begin(), mat_name.end(), '/', '_');
            }
            
            obj << "\n# Material range " << range_idx << ": " << range.triangle_count << " triangles\n";
            obj << "g submesh_" << range_idx << "_" << mat_name << "\n";
            obj << "usemtl " << mat_name << "\n";
            
            // Write faces for this material range (OBJ indices are 1-based)
            for (uint32_t tri = range.triangle_start; tri < range.triangle_end; tri++) {
                uint32_t i = tri * 3;
                if (i + 2 >= mesh_->indices.size()) break;
                
                uint32_t v1 = mesh_->indices[i] + 1;
                uint32_t v2 = mesh_->indices[i + 1] + 1;
                uint32_t v3 = mesh_->indices[i + 2] + 1;
                
                if (options.export_normals && options.export_uvs) {
                    obj << "f " << v1 << "/" << v1 << "/" << v1 
                        << " " << v2 << "/" << v2 << "/" << v2 
                        << " " << v3 << "/" << v3 << "/" << v3 << "\n";
                } else if (options.export_normals) {
                    obj << "f " << v1 << "//" << v1 
                        << " " << v2 << "//" << v2 
                        << " " << v3 << "//" << v3 << "\n";
                } else if (options.export_uvs) {
                    obj << "f " << v1 << "/" << v1 
                        << " " << v2 << "/" << v2 
                        << " " << v3 << "/" << v3 << "\n";
                } else {
                    obj << "f " << v1 << " " << v2 << " " << v3 << "\n";
                }
            }
        }
    } else {
        // No material ranges - write all faces with default material
        if (!mesh_->materials.empty()) {
            std::string mat_name = mesh_->materials[0].name;
            std::replace(mat_name.begin(), mat_name.end(), ' ', '_');
            std::replace(mat_name.begin(), mat_name.end(), '/', '_');
            obj << "usemtl " << mat_name << "\n";
        } else {
            obj << "usemtl default_material\n";
        }
        obj << "g mesh\n";
        
        for (size_t i = 0; i < mesh_->indices.size(); i += 3) {
            uint32_t v1 = mesh_->indices[i] + 1;
            uint32_t v2 = mesh_->indices[i + 1] + 1;
            uint32_t v3 = mesh_->indices[i + 2] + 1;
            
            if (options.export_normals && options.export_uvs) {
                obj << "f " << v1 << "/" << v1 << "/" << v1 
                    << " " << v2 << "/" << v2 << "/" << v2 
                    << " " << v3 << "/" << v3 << "/" << v3 << "\n";
            } else if (options.export_normals) {
                obj << "f " << v1 << "//" << v1 
                    << " " << v2 << "//" << v2 
                    << " " << v3 << "//" << v3 << "\n";
            } else if (options.export_uvs) {
                obj << "f " << v1 << "/" << v1 
                    << " " << v2 << "/" << v2 
                    << " " << v3 << "/" << v3 << "\n";
            } else {
                obj << "f " << v1 << " " << v2 << " " << v3 << "\n";
            }
        }
    }
    
    return obj.str();
}

std::string MeshConverter::generate_mtl(const ExportOptions& options) {
    if (!mesh_) return "";
    
    std::ostringstream mtl;
    mtl << "# Enfusion Unpacker - MTL Export\n\n";
    
    // Add default material if no materials exist
    if (mesh_->materials.empty()) {
        mtl << "newmtl default_material\n";
        mtl << "Ns 225.000000\n";
        mtl << "Ka 1.000000 1.000000 1.000000\n";
        mtl << "Kd 0.800000 0.800000 0.800000\n";
        mtl << "Ks 0.500000 0.500000 0.500000\n";
        mtl << "Ke 0.000000 0.000000 0.000000\n";
        mtl << "Ni 1.450000\n";
        mtl << "d 1.000000\n";
        mtl << "illum 2\n\n";
    }
    
    // Export each material
    for (size_t i = 0; i < mesh_->materials.size(); i++) {
        const auto& mat = mesh_->materials[i];
        
        std::string mat_name = mat.name;
        std::replace(mat_name.begin(), mat_name.end(), ' ', '_');
        std::replace(mat_name.begin(), mat_name.end(), '/', '_');
        
        mtl << "newmtl " << mat_name << "\n";
        mtl << "Ns 225.000000\n";
        mtl << "Ka 1.000000 1.000000 1.000000\n";
        mtl << "Kd 0.800000 0.800000 0.800000\n";
        mtl << "Ks 0.500000 0.500000 0.500000\n";
        mtl << "Ke 0.000000 0.000000 0.000000\n";
        mtl << "Ni 1.450000\n";
        mtl << "d 1.000000\n";
        mtl << "illum 2\n";
        
        // Add texture references as comments (paths are Enfusion-specific)
        if (!mat.path.empty()) {
            mtl << "# Original material: " << mat.path << "\n";
            
            // Derive texture path hints
            std::string base = mat.path;
            size_t ext_pos = base.rfind('.');
            if (ext_pos != std::string::npos) {
                base = base.substr(0, ext_pos);
            }
            mtl << "# Suggested diffuse: " << base << "_BCR.edds\n";
            mtl << "# Suggested normal: " << base << "_NMO.edds\n";
        }
        mtl << "\n";
    }
    
    return mtl.str();
}

std::string MeshConverter::generate_fbx(const ExportOptions& options) {
    if (!mesh_) return "";
    
    std::ostringstream fbx;
    fbx << std::fixed << std::setprecision(6);
    
    // Get current time for FBX header
    auto now = std::time(nullptr);
    auto* tm = std::localtime(&now);
    
    // FBX ASCII Header
    fbx << "; FBX 7.4.0 project file\n";
    fbx << "; Enfusion Unpacker Export\n";
    fbx << "; ----------------------------------------------------\n\n";
    
    fbx << "FBXHeaderExtension:  {\n";
    fbx << "    FBXHeaderVersion: 1003\n";
    fbx << "    FBXVersion: 7400\n";
    fbx << "    CreationTimeStamp:  {\n";
    fbx << "        Version: 1000\n";
    fbx << "        Year: " << (1900 + tm->tm_year) << "\n";
    fbx << "        Month: " << (tm->tm_mon + 1) << "\n";
    fbx << "        Day: " << tm->tm_mday << "\n";
    fbx << "        Hour: " << tm->tm_hour << "\n";
    fbx << "        Minute: " << tm->tm_min << "\n";
    fbx << "        Second: " << tm->tm_sec << "\n";
    fbx << "        Millisecond: 0\n";
    fbx << "    }\n";
    fbx << "    Creator: \"Enfusion Unpacker\"\n";
    fbx << "}\n\n";
    
    // Global Settings
    fbx << "GlobalSettings:  {\n";
    fbx << "    Version: 1000\n";
    fbx << "    Properties70:  {\n";
    fbx << "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n";
    fbx << "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n";
    fbx << "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n";
    fbx << "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n";
    fbx << "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n";
    fbx << "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
    fbx << "        P: \"OriginalUpAxis\", \"int\", \"Integer\", \"\",-1\n";
    fbx << "        P: \"OriginalUpAxisSign\", \"int\", \"Integer\", \"\",1\n";
    fbx << "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n";
    fbx << "    }\n";
    fbx << "}\n\n";
    
    // Documents
    fbx << "Documents:  {\n";
    fbx << "    Count: 1\n";
    fbx << "    Document: 1000000000, \"\", \"Scene\" {\n";
    fbx << "        Properties70:  {\n";
    fbx << "            P: \"SourceObject\", \"object\", \"\", \"\"\n";
    fbx << "            P: \"ActiveAnimStackName\", \"KString\", \"\", \"\", \"\"\n";
    fbx << "        }\n";
    fbx << "        RootNode: 0\n";
    fbx << "    }\n";
    fbx << "}\n\n";
    
    // References
    fbx << "References:  {\n}\n\n";
    
    // Definitions
    size_t mat_count = mesh_->materials.empty() ? 1 : mesh_->materials.size();
    fbx << "Definitions:  {\n";
    fbx << "    Version: 100\n";
    fbx << "    Count: " << (3 + mat_count) << "\n";
    fbx << "    ObjectType: \"GlobalSettings\" {\n";
    fbx << "        Count: 1\n";
    fbx << "    }\n";
    fbx << "    ObjectType: \"Model\" {\n";
    fbx << "        Count: 1\n";
    fbx << "    }\n";
    fbx << "    ObjectType: \"Geometry\" {\n";
    fbx << "        Count: 1\n";
    fbx << "    }\n";
    fbx << "    ObjectType: \"Material\" {\n";
    fbx << "        Count: " << mat_count << "\n";
    fbx << "    }\n";
    fbx << "}\n\n";
    
    // Objects
    fbx << "Objects:  {\n";
    
    // Geometry (Mesh)
    int64_t geom_id = 2000000000;
    fbx << "    Geometry: " << geom_id << ", \"Geometry::" << name_ << "\", \"Mesh\" {\n";
    
    // Vertices
    fbx << "        Vertices: *" << (mesh_->vertices.size() * 3) << " {\n";
    fbx << "            a: ";
    for (size_t i = 0; i < mesh_->vertices.size(); i++) {
        if (i > 0) fbx << ",";
        const auto& v = mesh_->vertices[i];
        fbx << v.position.x << "," << v.position.y << "," << v.position.z;
    }
    fbx << "\n        }\n";
    
    // Polygon Vertex Indices (negative for end of polygon)
    fbx << "        PolygonVertexIndex: *" << mesh_->indices.size() << " {\n";
    fbx << "            a: ";
    for (size_t i = 0; i < mesh_->indices.size(); i += 3) {
        if (i > 0) fbx << ",";
        fbx << mesh_->indices[i] << "," << mesh_->indices[i+1] << "," << (-(int64_t)mesh_->indices[i+2] - 1);
    }
    fbx << "\n        }\n";
    
    // Normals
    if (options.export_normals) {
        fbx << "        LayerElementNormal: 0 {\n";
        fbx << "            Version: 101\n";
        fbx << "            Name: \"\"\n";
        fbx << "            MappingInformationType: \"ByVertice\"\n";
        fbx << "            ReferenceInformationType: \"Direct\"\n";
        fbx << "            Normals: *" << (mesh_->vertices.size() * 3) << " {\n";
        fbx << "                a: ";
        for (size_t i = 0; i < mesh_->vertices.size(); i++) {
            if (i > 0) fbx << ",";
            const auto& v = mesh_->vertices[i];
            fbx << v.normal.x << "," << v.normal.y << "," << v.normal.z;
        }
        fbx << "\n            }\n";
        fbx << "        }\n";
    }
    
    // UVs
    if (options.export_uvs) {
        fbx << "        LayerElementUV: 0 {\n";
        fbx << "            Version: 101\n";
        fbx << "            Name: \"UVMap\"\n";
        fbx << "            MappingInformationType: \"ByVertice\"\n";
        fbx << "            ReferenceInformationType: \"Direct\"\n";
        fbx << "            UV: *" << (mesh_->vertices.size() * 2) << " {\n";
        fbx << "                a: ";
        for (size_t i = 0; i < mesh_->vertices.size(); i++) {
            if (i > 0) fbx << ",";
            const auto& v = mesh_->vertices[i];
            fbx << v.uv.x << "," << v.uv.y;
        }
        fbx << "\n            }\n";
        fbx << "        }\n";
    }
    
    // Material layer
    if (options.export_materials && !mesh_->material_ranges.empty()) {
        fbx << "        LayerElementMaterial: 0 {\n";
        fbx << "            Version: 101\n";
        fbx << "            Name: \"\"\n";
        fbx << "            MappingInformationType: \"ByPolygon\"\n";
        fbx << "            ReferenceInformationType: \"IndexToDirect\"\n";
        fbx << "            Materials: *" << (mesh_->indices.size() / 3) << " {\n";
        fbx << "                a: ";
        
        // Build per-triangle material indices
        std::vector<uint32_t> tri_materials(mesh_->indices.size() / 3, 0);
        for (const auto& range : mesh_->material_ranges) {
            for (uint32_t t = range.triangle_start; t < range.triangle_end && t < tri_materials.size(); t++) {
                tri_materials[t] = range.material_index;
            }
        }
        
        for (size_t i = 0; i < tri_materials.size(); i++) {
            if (i > 0) fbx << ",";
            fbx << tri_materials[i];
        }
        fbx << "\n            }\n";
        fbx << "        }\n";
    }
    
    // Layer
    fbx << "        Layer: 0 {\n";
    fbx << "            Version: 100\n";
    if (options.export_normals) {
        fbx << "            LayerElement:  {\n";
        fbx << "                Type: \"LayerElementNormal\"\n";
        fbx << "                TypedIndex: 0\n";
        fbx << "            }\n";
    }
    if (options.export_uvs) {
        fbx << "            LayerElement:  {\n";
        fbx << "                Type: \"LayerElementUV\"\n";
        fbx << "                TypedIndex: 0\n";
        fbx << "            }\n";
    }
    if (options.export_materials && !mesh_->material_ranges.empty()) {
        fbx << "            LayerElement:  {\n";
        fbx << "                Type: \"LayerElementMaterial\"\n";
        fbx << "                TypedIndex: 0\n";
        fbx << "            }\n";
    }
    fbx << "        }\n";
    fbx << "    }\n";
    
    // Model
    int64_t model_id = 3000000000;
    fbx << "    Model: " << model_id << ", \"Model::" << name_ << "\", \"Mesh\" {\n";
    fbx << "        Version: 232\n";
    fbx << "        Properties70:  {\n";
    fbx << "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0\n";
    fbx << "            P: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n";
    fbx << "            P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
    fbx << "        }\n";
    fbx << "        Shading: T\n";
    fbx << "        Culling: \"CullingOff\"\n";
    fbx << "    }\n";
    
    // Materials
    int64_t mat_base_id = 4000000000;
    if (mesh_->materials.empty()) {
        fbx << "    Material: " << mat_base_id << ", \"Material::default_material\", \"\" {\n";
        fbx << "        Version: 102\n";
        fbx << "        ShadingModel: \"phong\"\n";
        fbx << "        Properties70:  {\n";
        fbx << "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.8,0.8,0.8\n";
        fbx << "        }\n";
        fbx << "    }\n";
    } else {
        for (size_t i = 0; i < mesh_->materials.size(); i++) {
            const auto& mat = mesh_->materials[i];
            std::string mat_name = mat.name;
            std::replace(mat_name.begin(), mat_name.end(), ' ', '_');
            
            fbx << "    Material: " << (mat_base_id + static_cast<int64_t>(i)) << ", \"Material::" << mat_name << "\", \"\" {\n";
            fbx << "        Version: 102\n";
            fbx << "        ShadingModel: \"phong\"\n";
            fbx << "        Properties70:  {\n";
            fbx << "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.8,0.8,0.8\n";
            fbx << "        }\n";
            fbx << "    }\n";
        }
    }
    
    fbx << "}\n\n";
    
    // Connections
    fbx << "Connections:  {\n";
    fbx << "    C: \"OO\"," << model_id << ",0\n";
    fbx << "    C: \"OO\"," << geom_id << "," << model_id << "\n";
    
    // Connect materials
    if (mesh_->materials.empty()) {
        fbx << "    C: \"OO\"," << mat_base_id << "," << model_id << "\n";
    } else {
        for (size_t i = 0; i < mesh_->materials.size(); i++) {
            fbx << "    C: \"OO\"," << (mat_base_id + static_cast<int64_t>(i)) << "," << model_id << "\n";
        }
    }
    
    fbx << "}\n";
    
    return fbx.str();
}

} // namespace enfusion
