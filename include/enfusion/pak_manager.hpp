/**
 * Enfusion Unpacker - PAK Manager
 * 
 * Manages multiple PAK files and provides unified access for
 * dependency resolution across all loaded PAKs.
 */

#pragma once

#include "enfusion/pak_reader.hpp"
#include "enfusion/addon_extractor.hpp"
#include "enfusion/pak_index.hpp"
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
 * with lazy loading support.
 */
class PakManager {
public:
    static PakManager& instance();
    
    PakManager();
    ~PakManager();
    
    // Path configuration
    void set_game_path(const std::filesystem::path& game_path);
    void set_mods_path(const std::filesystem::path& mods_path);
    const std::filesystem::path& game_path() const { return game_folder_; }
    const std::filesystem::path& mods_path() const { return mods_folder_; }
    
    // Scan for available PAKs (doesn't load them, just indexes)
    void scan_available_paks();
    size_t available_pak_count() const { return available_paks_.size(); }
    size_t loaded_pak_count() const { return paks_.size(); }
    
    // PAK management
    bool load_pak(const std::filesystem::path& pak_path);
    void unload_pak(const std::filesystem::path& pak_path);
    void unload_all();
    
    bool is_loaded(const std::filesystem::path& pak_path) const;
    std::vector<std::filesystem::path> loaded_paks() const;
    std::vector<std::filesystem::path> available_paks() const { return available_paks_; }
    
    // File access across all PAKs (with lazy loading)
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
    
    // Search for textures matching a material name (returns best matches with priority)
    struct TextureMatch {
        std::string path;
        int priority;  // Lower = better (0 = _BCR, 1 = _MCR, etc.)
    };
    std::vector<TextureMatch> search_textures_by_material(const std::string& material_name) const;
    
    // Auto-load PAKs from a game folder (legacy)
    void scan_game_folder(const std::filesystem::path& game_path);
    void load_common_paks();  // Load commonly needed PAKs like core.pak
    
    // Lazy loading control
    void set_lazy_loading(bool enabled) { lazy_loading_ = enabled; }
    bool lazy_loading() const { return lazy_loading_; }
    
    // Try to find and load a PAK that might contain a file (uses PakIndex)
    bool try_load_pak_for_file(const std::string& virtual_path);
    
    // Reset lazy loading counter (call at start of new model load)
    void reset_lazy_load_counter() { lazy_load_count_ = 0; }
    
    // Initialize the file index (loads from cache or builds)
    // Should be called at startup with a progress callback
    using IndexProgressCallback = std::function<void(const std::string&, int, int)>;
    void initialize_index(IndexProgressCallback callback = nullptr);
    bool is_index_ready() const;
    
    // Callbacks
    using LoadCallback = std::function<void(const std::string& pak_name, bool success)>;
    void set_load_callback(LoadCallback callback) { load_callback_ = callback; }
    
private:
    struct LoadedPak {
        std::filesystem::path path;
        std::unique_ptr<AddonExtractor> extractor;
        std::vector<std::string> file_list;  // Cached file list
    };
    
    // Scan a single directory for PAK files
    void scan_directory_for_paks(const std::filesystem::path& dir);
    
    std::vector<FileDependency> find_material_dependencies(
        const std::string& emat_path,
        const std::string& source_pak);
    
    std::vector<FileDependency> find_mesh_dependencies(
        const std::string& xob_path,
        const std::string& source_pak);
    
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<LoadedPak>> paks_;
    std::unordered_map<std::string, size_t> pak_index_;  // path -> index
    
    // Available but not loaded PAKs
    std::vector<std::filesystem::path> available_paks_;
    
    // Paths
    std::filesystem::path game_folder_;
    std::filesystem::path mods_folder_;
    
    // Settings
    bool lazy_loading_ = true;
    int lazy_load_count_ = 0;
    static constexpr int max_lazy_loads_ = 10;  // Can load more since we use index
    
    LoadCallback load_callback_;
    
    // Singleton
    PakManager(const PakManager&) = delete;
    PakManager& operator=(const PakManager&) = delete;
};

} // namespace enfusion
