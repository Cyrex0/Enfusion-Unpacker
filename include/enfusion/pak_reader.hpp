/**
 * Enfusion Unpacker - PAK Reader
 */

#pragma once

#include "types.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>

namespace enfusion {

/**
 * Entry in PAK file.
 */
struct PakEntry {
    std::string path;
    uint64_t offset = 0;
    uint32_t size = 0;
    uint32_t compressed_size = 0;
    uint32_t flags = 0;
    uint32_t crc = 0;
    bool is_compressed = false;
};

/**
 * PAK file reader.
 */
class PakReader {
public:
    using ProgressCallback = std::function<bool(const std::string& file, size_t current, size_t total)>;

    PakReader();
    ~PakReader();

    bool open(const std::filesystem::path& path);
    void close();

    std::vector<PakEntry> list_files() const;
    std::vector<PakEntry> list_files(const std::string& pattern) const;
    
    const PakEntry* find_entry(const std::string& path) const;
    
    std::vector<uint8_t> read_file(const std::string& path);
    std::vector<uint8_t> read_file(const PakEntry& entry);
    
    bool extract_file(const std::string& path, const std::filesystem::path& output_path);
    bool extract_file(const PakEntry& entry, const std::filesystem::path& output_path);
    
    bool extract_all(const std::filesystem::path& output_dir, ProgressCallback callback = nullptr);
    
    bool is_open() const { return file_.is_open(); }
    size_t file_count() const;
    size_t total_size() const;
    size_t compressed_size() const;
    const std::filesystem::path& path() const { return pak_path_; }

private:
    void parse_toc(const std::vector<uint8_t>& toc_data, uint32_t file_count);
    bool matches_pattern(const std::string& text, const std::string& pattern) const;

    std::filesystem::path pak_path_;
    std::ifstream file_;
    std::vector<PakEntry> entries_;
};

} // namespace enfusion
