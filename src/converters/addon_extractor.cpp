/**
 * Enfusion Unpacker - Addon Extractor Implementation
 * 
 * EXACTLY matches enfusion_unpacker_v2.py logic:
 * - RDB v6 format with entry types 4, 5, 6
 * - Manifest provides fragment offsets 
 * - PAK contains file data
 */

#include "enfusion/addon_extractor.hpp"
#include "enfusion/compression.hpp"
#include "enfusion/files.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace enfusion {

AddonExtractor::AddonExtractor() = default;
AddonExtractor::~AddonExtractor() = default;

bool AddonExtractor::load(const std::filesystem::path& addon_dir) {
    addon_dir_ = addon_dir;
    pak_path_ = addon_dir / "data.pak";
    rdb_path_ = addon_dir / "resourceDatabase.rdb";
    
    // Find manifest file (data.pak_*_manifest.json)
    manifest_path_.clear();
    for (const auto& entry : std::filesystem::directory_iterator(addon_dir)) {
        auto name = entry.path().filename().string();
        if (name.find("data.pak_") != std::string::npos && 
            name.find("_manifest.json") != std::string::npos) {
            manifest_path_ = entry.path();
            break;
        }
    }
    
    if (!std::filesystem::exists(pak_path_)) return false;
    if (!std::filesystem::exists(rdb_path_)) return false;
    if (manifest_path_.empty() || !std::filesystem::exists(manifest_path_)) return false;
    
    // Load PAK data
    pak_data_ = enfusion::read_file(pak_path_);
    if (pak_data_.empty()) return false;
    
    // Load manifest first (needed for decompressed size index)
    if (!load_manifest()) return false;
    
    // Build decompressed size index
    build_decompressed_index();
    
    // Index special fragments
    index_special_fragments();
    
    // Parse RDB
    if (!parse_rdb()) return false;
    
    loaded_ = true;
    return true;
}

bool AddonExtractor::load_manifest() {
    try {
        std::ifstream f(manifest_path_);
        if (!f.is_open()) return false;
        
        nlohmann::json data = nlohmann::json::parse(f);
        
        fragments_.clear();
        size_to_fragments_.clear();
        
        if (!data.contains("fragments")) return false;
        
        auto& frags = data["fragments"];
        for (size_t i = 0; i < frags.size(); ++i) {
            ManifestFragment frag;
            frag.index = static_cast<int>(i);
            frag.size = frags[i]["size"].get<uint32_t>();
            
            // Handle 'offsets' array format (from v2 code)
            if (frags[i].contains("offsets") && !frags[i]["offsets"].empty()) {
                frag.offset = frags[i]["offsets"][0].get<uint64_t>();
            } else if (frags[i].contains("offset")) {
                frag.offset = frags[i]["offset"].get<uint64_t>();
            } else {
                frag.offset = 0;
            }
            
            if (frags[i].contains("sha512")) {
                frag.sha512 = frags[i]["sha512"].get<std::string>();
            }
            
            fragments_.push_back(frag);
            size_to_fragments_[frag.size].push_back(static_cast<int>(fragments_.size() - 1));
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

void AddonExtractor::build_decompressed_index() {
    decompressed_sizes_.clear();
    
    for (const auto& frag : fragments_) {
        if (frag.offset + frag.size > pak_data_.size()) continue;
        
        const uint8_t* data = pak_data_.data() + frag.offset;
        
        // Check for ZLIB header (0x78 0x9c specifically, per v2 code)
        if (frag.size >= 2 && data[0] == 0x78 && data[1] == 0x9c) {
            try {
                auto decompressed = decompress_zlib(data, frag.size, frag.size * 20);
                if (!decompressed.empty()) {
                    decompressed_sizes_[decompressed.size()].push_back({frag.index, frag.offset, frag.size});
                }
            } catch (...) {}
        }
    }
}

void AddonExtractor::index_special_fragments() {
    xob_fragments_.clear();
    prefab_fragments_.clear();
    
    for (const auto& frag : fragments_) {
        if (frag.size < 16) continue;
        if (frag.offset + frag.size > pak_data_.size()) continue;
        
        const uint8_t* data = pak_data_.data() + frag.offset;
        
        // Check for XOB: FORM....XOB9 (uncompressed)
        if (std::memcmp(data, "FORM", 4) == 0 && 
            frag.size >= 12 && std::memcmp(data + 8, "XOB9", 4) == 0) {
            xob_fragments_.push_back(frag.index);
        }
        
        // Check for prefab signatures
        if (frag.size >= 17 && std::memcmp(data, "StaticModelEntity", 17) == 0) {
            prefab_fragments_.push_back(frag.index);
        } else if (frag.size >= 8 && std::memcmp(data, "SubScene", 8) == 0) {
            prefab_fragments_.push_back(frag.index);
        } else if (frag.size >= 13 && std::memcmp(data, "GenericEntity", 13) == 0) {
            prefab_fragments_.push_back(frag.index);
        }
    }
}

bool AddonExtractor::parse_rdb() {
    auto rdb_data = enfusion::read_file(rdb_path_);
    if (rdb_data.size() < 32) return false;
    
    // Verify IFF header
    if (std::memcmp(rdb_data.data(), "FORM", 4) != 0) return false;
    if (std::memcmp(rdb_data.data() + 8, "RDBC", 4) != 0) return false;
    
    uint32_t version = *reinterpret_cast<const uint32_t*>(rdb_data.data() + 12);
    if (version != 6) return false;
    
    uint32_t entry_count = *reinterpret_cast<const uint32_t*>(rdb_data.data() + 28);
    
    files_.clear();
    size_t pos = 32;
    
    // Parse ROOT entry (special format per v2):
    // parent_idx(4) + type_and_pad(4) + hash(8) + padding(3) + timestamp(4) + padding(4) + pathlen(4) + path
    pos += 4;  // parent_idx
    pos += 4;  // type_and_pad
    pos += 8;  // hash
    pos += 3;  // padding
    pos += 4;  // timestamp
    pos += 4;  // padding
    
    if (pos + 4 > rdb_data.size()) return false;
    uint32_t root_pathlen = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
    pos += 4;
    
    if (root_pathlen <= 1000 && pos + root_pathlen <= rdb_data.size()) {
        pos += root_pathlen;  // Skip root path
    }
    
    // Parse remaining entries - format per v2:
    // entry_type(4) + padding(2) + hash(8) + timestamp(4) + type-specific...
    uint32_t entry_idx = 1;
    while (pos < rdb_data.size() - 10 && entry_idx < entry_count) {
        if (pos + 22 > rdb_data.size()) break;
        
        uint32_t entry_type = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
        pos += 4;
        pos += 2;  // padding (2 bytes, not 4!)
        pos += 8;  // hash
        uint32_t timestamp1 = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
        pos += 4;
        
        if (entry_type == 5) {
            // Type 5: Can be directory OR file
            if (pos + 4 > rdb_data.size()) break;
            uint32_t size = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
            pos += 4;
            
            if (pos + 4 > rdb_data.size()) break;
            uint32_t pathlen = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
            pos += 4;
            
            if (pathlen > 1000 || pos + pathlen > rdb_data.size()) break;
            
            std::string path(reinterpret_cast<const char*>(rdb_data.data() + pos), pathlen);
            while (!path.empty() && path.back() == '\0') path.pop_back();
            pos += pathlen;
            
            // Check if this is actually a file (has extension in last component)
            bool is_file = false;
            size_t last_slash = path.rfind('/');
            std::string filename = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
            if (filename.find('.') != std::string::npos) {
                is_file = true;
            }
            
            if (is_file) {
                RdbFile file;
                file.path = path;
                file.size = (size > 0) ? size : 15;  // Use 15 as placeholder for XOB matching
                file.index = static_cast<int>(files_.size());
                files_.push_back(file);
            }
            // Skip directories
            
        } else if (entry_type == 6) {
            // Type 6: File - file_size(4) + timestamp2(4) + block_id(4) + pathlen(4) + path
            if (pos + 4 > rdb_data.size()) break;
            uint32_t file_size = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
            pos += 4;
            pos += 4;  // timestamp2
            pos += 4;  // block_id
            
            if (pos + 4 > rdb_data.size()) break;
            uint32_t pathlen = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
            pos += 4;
            
            if (pathlen > 1000 || pos + pathlen > rdb_data.size()) break;
            
            std::string path(reinterpret_cast<const char*>(rdb_data.data() + pos), pathlen);
            while (!path.empty() && path.back() == '\0') path.pop_back();
            pos += pathlen;
            
            RdbFile file;
            file.path = path;
            file.size = file_size;
            file.index = static_cast<int>(files_.size());
            files_.push_back(file);
            
        } else if (entry_type == 4) {
            // Type 4: Script - size(4) + pathlen(4) + path
            if (pos + 4 > rdb_data.size()) break;
            uint32_t size = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
            pos += 4;
            
            if (pos + 4 > rdb_data.size()) break;
            uint32_t pathlen = *reinterpret_cast<const uint32_t*>(rdb_data.data() + pos);
            pos += 4;
            
            if (pathlen > 1000 || pos + pathlen > rdb_data.size()) break;
            
            std::string path(reinterpret_cast<const char*>(rdb_data.data() + pos), pathlen);
            while (!path.empty() && path.back() == '\0') path.pop_back();
            pos += pathlen;
            
            RdbFile file;
            file.path = path;
            file.size = size;
            file.index = static_cast<int>(files_.size());
            files_.push_back(file);
            
        } else {
            // Unknown type
            break;
        }
        
        entry_idx++;
    }
    
    return !files_.empty();
}

std::vector<RdbFile> AddonExtractor::list_files() const {
    return files_;
}

std::vector<uint8_t> AddonExtractor::read_file(const RdbFile& file) {
    auto location = find_file_location(file.size, file.path);
    if (!location) return {};
    
    auto [offset, size, is_compressed] = *location;
    
    if (offset + size > pak_data_.size()) return {};
    
    if (is_compressed) {
        const uint8_t* data = pak_data_.data() + offset;
        try {
            return decompress_zlib(data, size, file.size * 2);
        } catch (...) {
            return {};
        }
    } else {
        return std::vector<uint8_t>(pak_data_.begin() + offset, pak_data_.begin() + offset + size);
    }
}

std::vector<uint8_t> AddonExtractor::read_file(const std::string& path) {
    for (const auto& file : files_) {
        if (file.path == path) {
            return read_file(file);
        }
    }
    return {};
}

std::optional<std::tuple<uint64_t, uint32_t, bool>> AddonExtractor::find_file_location(
    uint32_t file_size, const std::string& path) {
    
    std::string path_lower = path;
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
    bool is_xob = path_lower.ends_with(".xob");
    bool is_prefab = path_lower.ends_with(".et") || path_lower.ends_with(".ent");
    
    // For XOB files with small placeholder sizes, use XOB fragment matching first
    if (is_xob && file_size < 100 && !xob_fragments_.empty()) {
        int idx = xob_fragments_[0];
        const auto& frag = fragments_[idx];
        return std::make_tuple(frag.offset, frag.size, false);
    }
    
    // Check single fragment match (uncompressed)
    auto it = size_to_fragments_.find(file_size);
    if (it != size_to_fragments_.end() && !it->second.empty()) {
        const auto& frag = fragments_[it->second[0]];
        return std::make_tuple(frag.offset, frag.size, false);
    }
    
    // Check compressed fragment match (decompressed size)
    auto dit = decompressed_sizes_.find(file_size);
    if (dit != decompressed_sizes_.end() && !dit->second.empty()) {
        auto& [idx, offset, size] = dit->second[0];
        return std::make_tuple(offset, size, true);
    }
    
    // For prefab files, use prefab fragment fallback
    if (is_prefab && !prefab_fragments_.empty()) {
        int idx = prefab_fragments_[0];
        const auto& frag = fragments_[idx];
        return std::make_tuple(frag.offset, frag.size, false);
    }
    
    return std::nullopt;
}

bool AddonExtractor::extract_file(const RdbFile& file, const std::filesystem::path& output_path) {
    auto data = read_file(file);
    if (data.empty()) return false;
    
    std::filesystem::create_directories(output_path.parent_path());
    return enfusion::write_file(output_path, data);
}

bool AddonExtractor::extract_all(const std::filesystem::path& output_dir, 
                                  std::function<bool(const std::string&, size_t, size_t)> callback) {
    size_t total = files_.size();
    size_t current = 0;
    
    for (const auto& file : files_) {
        auto output_path = output_dir / file.path;
        extract_file(file, output_path);
        
        ++current;
        if (callback) {
            if (!callback(file.path, current, total)) {
                return false;
            }
        }
    }
    
    return true;
}

} // namespace enfusion
