#pragma once

/**
 * Enfusion Unpacker - Addon Extractor
 * 
 * Extracts files from Enfusion addon packages by combining:
 * - RDB (resource database) for file paths
 * - PAK file for actual data (uses PakReader for correct file mapping)
 */

#include "enfusion/types.hpp"
#include "enfusion/pak_reader.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <memory>

namespace enfusion {

/**
 * File entry from RDB database
 */
struct RdbFile {
    std::string path;
    uint32_t size = 0;
    int index = 0;
};

/**
 * Fragment entry from manifest (for internal use)
 */
struct ManifestFragment {
    int index = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
    std::string sha512;
};

/**
 * Extracts files from Enfusion addon packages.
 */
class AddonExtractor {
public:
    AddonExtractor();
    ~AddonExtractor();

    /**
     * Load an addon directory.
     * @param addon_dir Path to addon folder containing data.pak
     * @return true if loaded successfully
     */
    bool load(const std::filesystem::path& addon_dir);

    /**
     * Check if addon is loaded
     */
    bool is_loaded() const { return loaded_; }

    /**
     * Get list of files in the addon
     */
    std::vector<RdbFile> list_files() const;

    /**
     * Read a file from the addon (decompressed)
     */
    std::vector<uint8_t> read_file(const RdbFile& file);
    std::vector<uint8_t> read_file(const std::string& path);

    /**
     * Extract a single file to disk
     */
    bool extract_file(const RdbFile& file, const std::filesystem::path& output_path);

    /**
     * Extract all files
     */
    bool extract_all(const std::filesystem::path& output_dir,
                     std::function<bool(const std::string&, size_t, size_t)> callback = nullptr);

    /**
     * Get addon directory path
     */
    const std::filesystem::path& addon_dir() const { return addon_dir_; }
    
    /**
     * Get last error message for debugging
     */
    const std::string& last_error() const { return last_error_; }

private:
    bool parse_rdb();
    void set_error(const std::string& msg) { last_error_ = msg; }

    std::filesystem::path addon_dir_;
    std::filesystem::path pak_path_;
    std::filesystem::path rdb_path_;

    // PakReader for correct file data access
    std::unique_ptr<PakReader> pak_reader_;
    
    std::vector<RdbFile> files_;

    bool loaded_ = false;
    std::string last_error_;
};

} // namespace enfusion
