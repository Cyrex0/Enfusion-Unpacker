/**
 * Enfusion Unpacker - PAK Manager
 * 
 * Manages multiple PAK files and provides unified access for
 * dependency resolution across all loaded PAKs.
 */

#pragma once

#include "enfusion/pak_reader.hpp"
#include "enfusion/addon_extractor.hpp"
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <set>

namespace enfusion {

/**
 * Dependency information for a file
 */
struct FileDependency {
    std::string path;           // Path to the dependency
    std::string source_pak;     // Which PAK contains this file
    bool resolved = false;      // Whether the dependency was found
    std::string type;           // Type: "texture", "material", "mesh", etc.
};

/**
 * Dependency graph for an asset
 */
struct DependencyGraph {
    std::string root_file;      // The file being analyzed
    std::string root_pak;       // PAK containing the root file
    std::vector<FileDependency> dependencies;
    std::set<std::string> missing_paks;  // PAKs that need to be loaded
    
    size_t resolved_count() const {
        size_t count = 0;
        for (const auto& dep : dependencies) {
            if (dep.resolved) count++;
        }
        return count;
    }
};

/**
 * Manages multiple PAK files for cross-PAK dependency resolution
 */
class PakManager {
public:
    static PakManager& instance();
    
    PakManager();
    ~PakManager();
    
    // PAK management
    bool load_pak(const std::filesystem::path& pak_path);
    void unload_pak(const std::filesystem::path& pak_path);
    void unload_all();
    
    bool is_loaded(const std::filesystem::path& pak_path) const;
    std::vector<std::filesystem::path> loaded_paks() const;
    
    // File access across all PAKs
    std::vector<uint8_t> read_file(const std::string& virtual_path);
    bool file_exists(const std::string& virtual_path) const;
    std::string find_file_pak(const std::string& virtual_path) const;
    
    // Texture/Material lookup across PAKs
    std::vector<uint8_t> find_texture(const std::string& material_name, 
                                       const std::string& base_path);
    
    // Dependency resolution
    DependencyGraph resolve_dependencies(const std::string& file_path,
                                         const std::string& source_pak);
    
    // Get all texture paths across all loaded PAKs
    std::vector<std::string> get_all_texture_paths() const;
    std::vector<std::string> get_all_texture_paths(const std::string& filter) const;
    
    // Auto-load PAKs from a game folder
    void scan_game_folder(const std::filesystem::path& game_path);
    void load_common_paks();  // Load commonly needed PAKs like core.pak
    
    // Callbacks
    using LoadCallback = std::function<void(const std::string& pak_name, bool success)>;
    void set_load_callback(LoadCallback callback) { load_callback_ = callback; }
    
private:
    struct LoadedPak {
        std::filesystem::path path;
        std::unique_ptr<AddonExtractor> extractor;
        std::vector<std::string> file_list;  // Cached file list
    };
    
    std::vector<FileDependency> find_material_dependencies(
        const std::string& emat_path,
        const std::string& source_pak);
    
    std::vector<FileDependency> find_mesh_dependencies(
        const std::string& xob_path,
        const std::string& source_pak);
    
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<LoadedPak>> paks_;
    std::unordered_map<std::string, size_t> pak_index_;  // path -> index
    std::filesystem::path game_folder_;
    LoadCallback load_callback_;
    
    // Singleton
    PakManager(const PakManager&) = delete;
    PakManager& operator=(const PakManager&) = delete;
};

} // namespace enfusion
