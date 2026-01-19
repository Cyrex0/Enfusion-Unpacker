#pragma once

/**
 * Enfusion Unpacker - Addon Extractor
 * 
 * Extracts files from Enfusion addon packages by combining:
 * - RDB (resource database) for file paths and sizes
 * - Manifest JSON for fragment offsets
 * - PAK file for actual data
 */

#include "enfusion/types.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <tuple>

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

private:
    bool parse_rdb();
    bool load_manifest();
    void build_decompressed_index();
    void index_special_fragments();
    std::optional<std::tuple<uint64_t, uint32_t, bool>> find_file_location(uint32_t file_size, const std::string& path);

    std::filesystem::path addon_dir_;
    std::filesystem::path pak_path_;
    std::filesystem::path rdb_path_;
    std::filesystem::path manifest_path_;

    std::vector<uint8_t> pak_data_;
    std::vector<RdbFile> files_;
    std::vector<ManifestFragment> fragments_;

    std::map<uint32_t, std::vector<int>> size_to_fragments_;
    std::map<size_t, std::vector<std::tuple<int, uint64_t, uint32_t>>> decompressed_sizes_;
    
    // Special fragment indices
    std::vector<int> xob_fragments_;
    std::vector<int> prefab_fragments_;

    bool loaded_ = false;
};

} // namespace enfusion
