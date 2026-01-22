/**
 * Enfusion Unpacker - PAK Manager Implementation
 */

#include "enfusion/pak_manager.hpp"
#include "enfusion/logging.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>

namespace enfusion {

PakManager& PakManager::instance() {
    static PakManager instance;
    return instance;
}

PakManager::PakManager() = default;
PakManager::~PakManager() = default;

bool PakManager::load_pak(const std::filesystem::path& pak_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string path_str = pak_path.string();
    
    // Check if already loaded
    if (pak_index_.find(path_str) != pak_index_.end()) {
        LOG_DEBUG("PakManager", "Already loaded: " << pak_path.filename().string());
        return true;  // Already loaded
    }
    
    auto pak = std::make_unique<LoadedPak>();
    pak->path = pak_path;
    pak->extractor = std::make_unique<AddonExtractor>();
    
    if (!pak->extractor->load(pak_path)) {
        LOG_ERROR("PakManager", "Failed to load PAK: " << path_str 
                  << " - " << pak->extractor->last_error());
        if (load_callback_) {
            load_callback_(pak_path.filename().string(), false);
        }
        return false;
    }
    
    // Cache file list for fast lookup - extract paths from RdbFile structs
    auto rdb_files = pak->extractor->list_files();
    for (const auto& f : rdb_files) {
        pak->file_list.push_back(f.path);
    }
    
    size_t index = paks_.size();
    pak_index_[path_str] = index;
    paks_.push_back(std::move(pak));
    
    LOG_INFO("PakManager", "Loaded: " << pak_path.filename().string() 
             << " (" << paks_.back()->file_list.size() << " files)");
    
    if (load_callback_) {
        load_callback_(pak_path.filename().string(), true);
    }
    
    return true;
}

void PakManager::unload_pak(const std::filesystem::path& pak_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string path_str = pak_path.string();
    auto it = pak_index_.find(path_str);
    if (it == pak_index_.end()) return;
    
    size_t index = it->second;
    
    // Remove from vector
    paks_.erase(paks_.begin() + index);
    pak_index_.erase(it);
    
    // Rebuild index
    pak_index_.clear();
    for (size_t i = 0; i < paks_.size(); ++i) {
        pak_index_[paks_[i]->path.string()] = i;
    }
    
    std::cerr << "[PakManager] Unloaded: " << pak_path.filename().string() << "\n";
}

void PakManager::unload_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    paks_.clear();
    pak_index_.clear();
    std::cerr << "[PakManager] Unloaded all PAKs" << "\n";
}

bool PakManager::is_loaded(const std::filesystem::path& pak_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pak_index_.find(pak_path.string()) != pak_index_.end();
}

std::vector<std::filesystem::path> PakManager::loaded_paks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::filesystem::path> result;
    for (const auto& pak : paks_) {
        result.push_back(pak->path);
    }
    return result;
}

std::vector<uint8_t> PakManager::read_file(const std::string& virtual_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Normalize path separators
    std::string normalized = virtual_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    LOG_DEBUG("PakManager", "Reading file: " << normalized);
    
    // Search all loaded PAKs
    for (const auto& pak : paks_) {
        for (const auto& file : pak->file_list) {
            std::string file_normalized = file;
            std::replace(file_normalized.begin(), file_normalized.end(), '\\', '/');
            std::transform(file_normalized.begin(), file_normalized.end(), file_normalized.begin(), ::tolower);
            if (file_normalized == normalized) {
                // Read using the stored path to avoid case mismatches
                auto data = pak->extractor->read_file(file);
                if (!data.empty()) {
                    LOG_DEBUG("PakManager", "Found in: " << pak->path.filename().string() 
                              << " (" << data.size() << " bytes)");
                    return data;
                }
                LOG_WARNING("PakManager", "File listed but read failed: " << file 
                            << " in " << pak->path.filename().string());
                return {};
            }
        }
    }
    
    LOG_DEBUG("PakManager", "File not found in loaded PAKs: " << normalized);
    return {};
}

bool PakManager::file_exists(const std::string& virtual_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string normalized = virtual_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    for (const auto& pak : paks_) {
        for (const auto& file : pak->file_list) {
            std::string file_normalized = file;
            std::replace(file_normalized.begin(), file_normalized.end(), '\\', '/');
            std::transform(file_normalized.begin(), file_normalized.end(), file_normalized.begin(), ::tolower);
            if (file_normalized == normalized) {
                return true;
            }
        }
    }
    
    return false;
}

std::string PakManager::find_file_pak(const std::string& virtual_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string normalized = virtual_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    for (const auto& pak : paks_) {
        for (const auto& file : pak->file_list) {
            std::string file_normalized = file;
            std::replace(file_normalized.begin(), file_normalized.end(), '\\', '/');
            std::transform(file_normalized.begin(), file_normalized.end(), file_normalized.begin(), ::tolower);
            if (file_normalized == normalized) {
                return pak->path.string();
            }
        }
    }
    
    return "";
}

std::vector<uint8_t> PakManager::find_texture(const std::string& material_name,
                                               const std::string& base_path) {
    // Try various texture naming conventions
    std::vector<std::string> suffixes = {
        "_BCR.edds", "_MCR.edds", "_co.edds", ".edds",
        "_BCR.dds", "_MCR.dds", "_co.dds", ".dds"
    };
    
    // Extract directory from base_path
    std::filesystem::path base(base_path);
    std::string dir = base.parent_path().string();
    if (!dir.empty() && dir.back() != '/') dir += '/';
    
    // Try Textures subfolder first
    for (const auto& suffix : suffixes) {
        std::string tex_path = dir + "Textures/" + material_name + suffix;
        auto data = read_file(tex_path);
        if (!data.empty()) return data;
    }
    
    // Try same folder
    for (const auto& suffix : suffixes) {
        std::string tex_path = dir + material_name + suffix;
        auto data = read_file(tex_path);
        if (!data.empty()) return data;
    }
    
    // Try Common textures for common materials
    if (material_name.find("chrome") != std::string::npos ||
        material_name.find("glass") != std::string::npos ||
        material_name.find("mirror") != std::string::npos) {
        for (const auto& suffix : suffixes) {
            std::string tex_path = "Common/Materials/Textures/" + material_name + suffix;
            auto data = read_file(tex_path);
            if (!data.empty()) return data;
        }
    }
    
    return {};
}

DependencyGraph PakManager::resolve_dependencies(const std::string& file_path,
                                                  const std::string& source_pak) {
    DependencyGraph graph;
    graph.root_file = file_path;
    graph.root_pak = source_pak;
    
    // Determine file type and extract dependencies
    std::filesystem::path path(file_path);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".xob") {
        graph.dependencies = find_mesh_dependencies(file_path, source_pak);
    } else if (ext == ".emat") {
        graph.dependencies = find_material_dependencies(file_path, source_pak);
    }
    
    // Identify missing PAKs
    for (const auto& dep : graph.dependencies) {
        if (!dep.resolved && !dep.source_pak.empty()) {
            // Parse the path to suggest which PAK might contain it
            if (dep.path.find("Common/") == 0) {
                graph.missing_paks.insert("Core PAK (contains Common/ assets)");
            } else if (dep.path.find("Assets/Vehicles/") == 0) {
                // Extract vehicle name
                size_t start = dep.path.find("Vehicles/") + 9;
                size_t end = dep.path.find('/', start + 1);
                if (end != std::string::npos) {
                    size_t type_end = dep.path.find('/', start);
                    if (type_end != std::string::npos && type_end < end) {
                        std::string vehicle_type = dep.path.substr(start, type_end - start);
                        std::string vehicle_name = dep.path.substr(type_end + 1, end - type_end - 1);
                        graph.missing_paks.insert(vehicle_name + " PAK");
                    }
                }
            }
        }
    }
    
    return graph;
}

std::vector<std::string> PakManager::get_all_texture_paths() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    
    for (const auto& pak : paks_) {
        for (const auto& file : pak->file_list) {
            std::filesystem::path p(file);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".edds" || ext == ".dds") {
                result.push_back(file);
            }
        }
    }
    
    return result;
}

std::vector<std::string> PakManager::get_all_texture_paths(const std::string& filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    
    std::string filter_lower = filter;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
    
    for (const auto& pak : paks_) {
        for (const auto& file : pak->file_list) {
            std::filesystem::path p(file);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            if (ext == ".edds" || ext == ".dds") {
                std::string file_lower = file;
                std::transform(file_lower.begin(), file_lower.end(), file_lower.begin(), ::tolower);
                
                if (file_lower.find(filter_lower) != std::string::npos) {
                    result.push_back(file);
                }
            }
        }
    }
    
    return result;
}

std::vector<PakManager::TextureMatch> PakManager::search_textures_by_material(
    const std::string& material_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TextureMatch> results;
    
    // Lowercase material name for comparison
    std::string mat_lower = material_name;
    std::transform(mat_lower.begin(), mat_lower.end(), mat_lower.begin(), ::tolower);
    
    // Suffixes to skip (non-diffuse textures)
    std::vector<std::string> skip_suffixes = {
        "_global_mask", "_mask", "_nmo", "_normal", "_nm", "_n",
        "_smdi", "_specular", "_spec", "_ao", "_occlusion",
        "_roughness", "_metallic", "_height",
        "_emissive", "_opacity", "_alpha", "_vfx"
    };
    
    // Diffuse suffixes in priority order
    std::vector<std::pair<std::string, int>> diffuse_priorities = {
        {"_bcr", 0}, {"_mcr", 1}, {"_co", 2}, {"_diffuse", 3},
        {"_diff", 4}, {"_d", 5}, {"_albedo", 6}, {"_color", 7}, {"_basecolor", 8}
    };
    
    for (const auto& pak : paks_) {
        for (const auto& file : pak->file_list) {
            std::filesystem::path p(file);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            // Only consider texture files
            if (ext != ".edds" && ext != ".dds") continue;
            
            // Get filename without extension
            std::string filename = p.stem().string();
            std::string filename_lower = filename;
            std::transform(filename_lower.begin(), filename_lower.end(), 
                          filename_lower.begin(), ::tolower);
            
            // Check if filename starts with material name
            if (filename_lower.find(mat_lower) != 0) continue;
            
            // Check if it's a skip texture (normal, mask, etc.)
            bool should_skip = false;
            for (const auto& skip : skip_suffixes) {
                if (filename_lower.length() >= skip.length()) {
                    std::string ending = filename_lower.substr(
                        filename_lower.length() - skip.length());
                    if (ending == skip) {
                        should_skip = true;
                        break;
                    }
                }
            }
            if (should_skip) continue;
            
            // Determine priority based on suffix
            int priority = 100;  // Default low priority
            
            // Check for exact match (no suffix beyond material name)
            if (filename_lower == mat_lower) {
                priority = 9;  // Exact match with no suffix
            } else {
                for (const auto& [suffix, prio] : diffuse_priorities) {
                    if (filename_lower.length() >= suffix.length()) {
                        std::string ending = filename_lower.substr(
                            filename_lower.length() - suffix.length());
                        if (ending == suffix) {
                            priority = prio;
                            break;
                        }
                    }
                }
            }
            
            // Only add if it's a diffuse-type texture
            if (priority <= 9) {
                results.push_back({file, priority});
            }
        }
    }
    
    // Sort by priority (lower = better)
    std::sort(results.begin(), results.end(), 
              [](const TextureMatch& a, const TextureMatch& b) {
                  return a.priority < b.priority;
              });
    
    return results;
}

void PakManager::scan_game_folder(const std::filesystem::path& game_path) {
    game_folder_ = game_path;
    
    // Look for Addons folder
    std::filesystem::path addons_path = game_path / "Addons";
    if (!std::filesystem::exists(addons_path)) {
        addons_path = game_path;  // Maybe user pointed directly to Addons
    }
    
    std::cerr << "[PakManager] Scanning: " << addons_path.string() << "\n";
    
    // Find all PAK files
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(addons_path)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".pak") {
                    load_pak(entry.path());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[PakManager] Scan error: " << e.what() << "\n";
    }
}

void PakManager::load_common_paks() {
    if (game_folder_.empty()) return;
    
    // Common PAK names that contain shared assets
    std::vector<std::string> common_pak_names = {
        "core.pak",
        "Core.pak", 
        "common.pak",
        "Common.pak",
        "base.pak",
        "Base.pak"
    };
    
    std::filesystem::path addons_path = game_folder_ / "Addons";
    if (!std::filesystem::exists(addons_path)) {
        addons_path = game_folder_;
    }
    
    for (const auto& pak_name : common_pak_names) {
        std::filesystem::path pak_path = addons_path / pak_name;
        if (std::filesystem::exists(pak_path)) {
            load_pak(pak_path);
        }
    }
}

void PakManager::set_game_path(const std::filesystem::path& game_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    game_folder_ = game_path;
    std::cerr << "[PakManager] Game path set: " << game_path.string() << "\n";
}

void PakManager::set_mods_path(const std::filesystem::path& mods_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    mods_folder_ = mods_path;
    std::cerr << "[PakManager] Mods path set: " << mods_path.string() << "\n";
}

void PakManager::scan_directory_for_paks(const std::filesystem::path& dir) {
    if (!std::filesystem::exists(dir)) return;
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            if (ext == ".pak") {
                // Add to available list if not already there
                auto it = std::find(available_paks_.begin(), available_paks_.end(), 
                                   entry.path());
                if (it == available_paks_.end()) {
                    available_paks_.push_back(entry.path());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[PakManager] Error scanning " << dir.string() 
                  << ": " << e.what() << "\n";
    }
}

void PakManager::scan_available_paks() {
    std::lock_guard<std::mutex> lock(mutex_);
    available_paks_.clear();
    
    // Scan game Addons folder
    if (!game_folder_.empty()) {
        std::filesystem::path addons_path = game_folder_ / "Addons";
        if (std::filesystem::exists(addons_path)) {
            scan_directory_for_paks(addons_path);
        } else if (std::filesystem::exists(game_folder_)) {
            scan_directory_for_paks(game_folder_);
        }
    }
    
    // Scan mods folder
    if (!mods_folder_.empty() && std::filesystem::exists(mods_folder_)) {
        scan_directory_for_paks(mods_folder_);
    }
    
    std::cerr << "[PakManager] Found " << available_paks_.size() 
              << " available PAKs\n";
}

void PakManager::initialize_index(IndexProgressCallback callback) {
    auto& index = PakIndex::instance();
    
    // Set paths
    index.set_game_path(game_folder_);
    index.set_mods_path(mods_folder_);
    
    // Open database and build/update index
    if (index.open_database()) {
        index.build_index(callback);
        std::cerr << "[PakManager] Index ready: " << index.total_files() << " files in " 
                  << index.total_paks() << " PAKs\n";
    }
}

bool PakManager::is_index_ready() const {
    return PakIndex::instance().is_ready();
}

bool PakManager::try_load_pak_for_file(const std::string& virtual_path) {
    // Don't do lazy loading if disabled
    if (!lazy_loading_) return false;
    
    // First check if any loaded PAK already has the file
    if (file_exists(virtual_path)) return true;
    
    // Use the PakIndex to find the exact PAK
    auto& index = PakIndex::instance();
    if (index.is_ready()) {
        std::filesystem::path pak_path = index.find_pak_for_file(virtual_path);
        
        if (!pak_path.empty()) {
            LOG_DEBUG("PakManager", "Index found PAK for " << virtual_path 
                      << ": " << pak_path.filename().string());
            
            // We know exactly which PAK has this file
            if (!is_loaded(pak_path)) {
                LOG_INFO("PakManager", "Loading PAK: " << pak_path.string());
                if (load_pak(pak_path)) {
                    lazy_load_count_++;
                    return file_exists(virtual_path);
                }
            }
            return file_exists(virtual_path);
        }
        // File not in index - don't log, this is expected for many search attempts
        return false;
    } else {
        LOG_DEBUG("PakManager", "Index not ready, cannot find: " << virtual_path);
    }
    
    // No index ready - limit lazy loading
    if (lazy_load_count_ >= max_lazy_loads_) {
        return false;
    }
    
    // No index and at limit - can't find file
    return false;
}

std::vector<FileDependency> PakManager::find_material_dependencies(
    const std::string& emat_path,
    const std::string& source_pak) {
    
    std::vector<FileDependency> deps;
    
    // Read the emat file
    auto data = read_file(emat_path);
    if (data.empty()) return deps;
    
    // Parse as text to find texture references
    std::string content(data.begin(), data.end());
    
    // Common texture patterns in emat files
    std::vector<std::string> patterns = {
        "DiffuseMap", "NormalMap", "SpecularMap", "DetailMap",
        "EmissiveMap", "OcclusionMap", "HeightMap"
    };
    
    // Simple text search for .edds references
    size_t pos = 0;
    while ((pos = content.find(".edds", pos)) != std::string::npos) {
        // Find the start of the path (look backwards for quote or whitespace)
        size_t start = pos;
        while (start > 0 && content[start - 1] != '"' && content[start - 1] != '\n' &&
               content[start - 1] != ' ' && content[start - 1] != '\t') {
            start--;
        }
        
        std::string tex_path = content.substr(start, pos + 5 - start);
        
        FileDependency dep;
        dep.path = tex_path;
        dep.type = "texture";
        dep.resolved = file_exists(tex_path);
        if (dep.resolved) {
            dep.source_pak = find_file_pak(tex_path);
        }
        deps.push_back(dep);
        
        pos += 5;
    }
    
    return deps;
}

std::vector<FileDependency> PakManager::find_mesh_dependencies(
    const std::string& xob_path,
    const std::string& source_pak) {
    
    std::vector<FileDependency> deps;
    
    // Read the XOB file
    auto data = read_file(xob_path);
    if (data.empty()) return deps;
    
    // Parse XOB to extract material paths
    // Look for .emat strings in the data
    std::string content(data.begin(), data.end());
    
    size_t pos = 0;
    while ((pos = content.find(".emat", pos)) != std::string::npos) {
        // Find the start of the path
        size_t start = pos;
        while (start > 0 && 
               content[start - 1] >= 32 && content[start - 1] < 127 &&
               content[start - 1] != '\0') {
            start--;
        }
        
        std::string mat_path = content.substr(start, pos + 5 - start);
        
        // Validate it looks like a path
        if (mat_path.find('/') != std::string::npos || 
            mat_path.find("Assets") != std::string::npos ||
            mat_path.find("Common") != std::string::npos) {
            
            FileDependency dep;
            dep.path = mat_path;
            dep.type = "material";
            dep.resolved = file_exists(mat_path);
            if (dep.resolved) {
                dep.source_pak = find_file_pak(mat_path);
            }
            deps.push_back(dep);
            
            // Also find texture dependencies for this material
            auto tex_deps = find_material_dependencies(mat_path, source_pak);
            deps.insert(deps.end(), tex_deps.begin(), tex_deps.end());
        }
        
        pos += 5;
    }
    
    // Also look for gamemat references
    pos = 0;
    while ((pos = content.find(".gamemat", pos)) != std::string::npos) {
        size_t start = pos;
        while (start > 0 && 
               content[start - 1] >= 32 && content[start - 1] < 127 &&
               content[start - 1] != '\0') {
            start--;
        }
        
        std::string mat_path = content.substr(start, pos + 8 - start);
        
        if (mat_path.find('/') != std::string::npos) {
            FileDependency dep;
            dep.path = mat_path;
            dep.type = "gamemat";
            dep.resolved = file_exists(mat_path);
            if (dep.resolved) {
                dep.source_pak = find_file_pak(mat_path);
            }
            deps.push_back(dep);
        }
        
        pos += 8;
    }
    
    return deps;
}

} // namespace enfusion
