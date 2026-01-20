/**
 * Enfusion Unpacker - Addon Extractor Implementation
 * 
 * Uses PakReader for correct file data access.
 * RDB provides the list of files, PakReader provides correct data mapping.
 */

#include "enfusion/addon_extractor.hpp"
#include "enfusion/compression.hpp"
#include "enfusion/files.hpp"

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
    
    if (!std::filesystem::exists(pak_path_)) return false;
    
    // Initialize and open PAK reader
    pak_reader_ = std::make_unique<PakReader>();
    if (!pak_reader_->open(pak_path_)) {
        std::cerr << "Failed to open PAK file with PakReader" << std::endl;
        return false;
    }
    
    std::cout << "PAK file loaded: " << pak_reader_->file_count() << " entries" << std::endl;
    
    // Build file list from PAK entries (the authoritative source)
    files_.clear();
    auto pak_entries = pak_reader_->list_files();
    for (size_t i = 0; i < pak_entries.size(); ++i) {
        RdbFile file;
        file.path = pak_entries[i].path;
        file.size = pak_entries[i].original_size;
        file.index = static_cast<int>(i);
        files_.push_back(file);
    }
    
    std::cout << "Files indexed: " << files_.size() << " files" << std::endl;
    
    loaded_ = true;
    return true;
}

bool AddonExtractor::parse_rdb() {
    // RDB parsing is no longer needed - we use PAK FILE chunk directly
    return true;
}

std::vector<RdbFile> AddonExtractor::list_files() const {
    return files_;
}

std::vector<uint8_t> AddonExtractor::read_file(const RdbFile& file) {
    // Use PakReader to read the file data correctly
    if (!pak_reader_ || !pak_reader_->is_open()) {
        std::cerr << "PAK reader not available" << std::endl;
        return {};
    }
    
    auto data = pak_reader_->read_file(file.path);
    if (data.empty()) {
        std::cerr << "Failed to read file from PAK: " << file.path << std::endl;
    }
    return data;
}

std::vector<uint8_t> AddonExtractor::read_file(const std::string& path) {
    // Use PakReader directly - it has the correct path->offset mapping
    if (!pak_reader_ || !pak_reader_->is_open()) {
        std::cerr << "PAK reader not available" << std::endl;
        return {};
    }
    
    auto data = pak_reader_->read_file(path);
    if (data.empty()) {
        std::cerr << "Failed to read file from PAK: " << path << std::endl;
    }
    return data;
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
