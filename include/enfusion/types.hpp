/**
 * Enfusion Unpacker - Common types and definitions
 */

#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <memory>
#include <filesystem>
#include <functional>

namespace enfusion {

namespace fs = std::filesystem;

// Forward declarations
class PakReader;
class RdbParser;
class ManifestLoader;
class AddonExtractor;

/**
 * Entry from resource database.
 */
struct RdbEntry {
    std::array<uint8_t, 16> resource_id{};
    std::string path;
    uint64_t fragment_offset = 0;
    uint32_t fragment_count = 0;
    uint32_t total_size = 0;
};

/**
 * Fragment location in PAK file.
 */
struct Fragment {
    uint32_t compressed_size = 0;
    uint32_t decompressed_size = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
};

/**
 * LZO compression descriptor for XOB.
 */
struct LzoDescriptor {
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t compressed_size = 0;
    uint32_t decompressed_size = 0;
};

/**
 * Material range for mesh rendering.
 * Defines which triangles use a specific material.
 */
struct MaterialRange {
    uint32_t material_index = 0;    // Index into materials array
    uint32_t triangle_start = 0;    // First triangle using this material
    uint32_t triangle_end = 0;      // One past last triangle
    uint32_t triangle_count = 0;    // Number of triangles
    
    // For rendering: convert to index buffer range
    uint32_t index_start() const { return triangle_start * 3; }
    uint32_t index_count() const { return triangle_count * 3; }
};

/**
 * XOB vertex structure.
 */
struct XobVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    
    // Convenience accessors for individual components
    float x() const { return position.x; }
    float y() const { return position.y; }
    float z() const { return position.z; }
};

/**
 * XOB LOD level.
 */
struct XobLod {
    float distance = 0.0f;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    std::vector<uint32_t> indices;
};

/**
 * Material reference from XOB.
 */
struct XobMaterial {
    std::string name;               // Material name (extracted from path)
    std::string path;               // Full path to .emat or .gamemat file
    std::string diffuse_texture;    // Diffuse/albedo texture path
    std::string normal_texture;     // Normal map path
    std::string specular_texture;   // Specular/roughness map path
    std::string emissive_texture;   // Emissive texture path
    uint32_t gl_diffuse = 0;        // OpenGL texture handle for diffuse
    uint32_t gl_normal = 0;         // OpenGL texture handle for normal
};

/**
 * Parsed mesh data from XOB.
 */
struct XobMesh {
    std::vector<XobVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<XobLod> lods;
    std::vector<XobMaterial> materials;
    std::vector<MaterialRange> material_ranges;  // Which triangles use which material
    uint32_t version = 0;
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
};

/**
 * Loaded texture data.
 */
struct TextureData {
    std::vector<uint8_t> pixels;  // RGBA
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    uint32_t mip_count = 1;
    std::string format;
    uint32_t gl_texture = 0;  // OpenGL texture handle
};

/**
 * Resource info from manifest.
 */
struct ResourceInfo {
    std::string path;
    std::string guid;
    std::string type;
    size_t size = 0;
};

/**
 * Addon metadata.
 */
struct AddonInfo {
    std::string name;
    fs::path path;
    std::vector<fs::path> pak_files;
    size_t total_size = 0;
    bool is_loaded = false;
};

/**
 * Application settings.
 */
struct AppSettings {
    // Paths
    fs::path last_addon_path;
    fs::path last_export_path;
    fs::path arma_addons_path;
    fs::path game_install_path;    // Game installation path (contains Addons folder)
    fs::path mods_install_path;    // Mods folder path (Workshop or custom mods)

    // Export settings
    bool convert_textures_to_png = true;
    bool convert_meshes_to_obj = true;

    // UI settings
    float ui_scale = 1.0f;
    int theme = 0;  // 0=Dark, 1=Light, 2=DarkBlue, 3=Purple
};

/**
 * Progress callback for long operations.
 */
using ProgressCallback = std::function<void(float progress, const std::string& status)>;

} // namespace enfusion
