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
 * Compression type for PAK entries.
 */
enum class PakCompression : uint32_t {
    None = 0,
    Zlib = 0x106
};

/**
 * Entry in PAK file.
 */
struct PakEntry {
    std::string path;
    uint32_t offset = 0;         // Absolute offset in file
    uint32_t size = 0;           // Compressed size (or original if uncompressed)
    uint32_t original_size = 0;  // Uncompressed size
    PakCompression compression = PakCompression::None;
    
    bool is_compressed() const { return compression != PakCompression::None; }
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
    bool parse_form_header();
    bool parse_chunks();
    void parse_file_entries(std::istream& stream, uint32_t chunk_size);
    void parse_entry_recursive(std::istream& stream, const std::string& parent_path, size_t end_pos);
    bool matches_pattern(const std::string& text, const std::string& pattern) const;
    
    // Helper for big-endian reads
    static uint32_t read_uint32_be(std::istream& stream);
    static uint32_t read_uint32_le(std::istream& stream);

    std::filesystem::path pak_path_;
    std::ifstream file_;
    std::vector<PakEntry> entries_;
    uint32_t data_offset_ = 0;   // Start of DATA chunk content
    uint32_t data_size_ = 0;     // Size of DATA chunk
};

} // namespace enfusion
