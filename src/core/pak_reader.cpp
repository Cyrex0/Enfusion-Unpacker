/**
 * Enfusion Unpacker - PAK Reader Implementation
 * 
 * Implements the correct FORM-based IFF format used by Enfusion engine:
 * - FORM header with PAC1 type
 * - HEAD chunk (version/metadata)
 * - DATA chunk (file contents)
 * - FILE chunk (directory/file entries)
 */

#include "enfusion/pak_reader.hpp"
#include "enfusion/compression.hpp"
#include "enfusion/files.hpp"
#include "enfusion/logging.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>
#include <array>
#include <iostream>

namespace enfusion {

// PAK file format constants (FORM/IFF-based)
constexpr std::array<char, 4> FORM_SIGNATURE = {'F', 'O', 'R', 'M'};
constexpr std::array<char, 4> PAC1_TYPE = {'P', 'A', 'C', '1'};
constexpr std::array<char, 4> HEAD_CHUNK = {'H', 'E', 'A', 'D'};
constexpr std::array<char, 4> DATA_CHUNK = {'D', 'A', 'T', 'A'};
constexpr std::array<char, 4> FILE_CHUNK = {'F', 'I', 'L', 'E'};

// Entry types in FILE chunk
constexpr uint8_t ENTRY_TYPE_DIRECTORY = 0;
constexpr uint8_t ENTRY_TYPE_FILE = 1;

PakReader::PakReader() = default;
PakReader::~PakReader() = default;

uint32_t PakReader::read_uint32_be(std::istream& stream) {
    uint8_t bytes[4];
    stream.read(reinterpret_cast<char*>(bytes), 4);
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint32_t PakReader::read_uint32_le(std::istream& stream) {
    uint32_t value;
    stream.read(reinterpret_cast<char*>(&value), 4);
    return value;
}

bool PakReader::open(const std::filesystem::path& path) {
    LOG_DEBUG("PakReader", "Opening: " << path.string());
    
    // Close any previously open file
    close();
    
    file_.open(path, std::ios::binary);
    if (!file_) {
        LOG_ERROR("PakReader", "Failed to open file: " << path.string());
        return false;
    }
    
    pak_path_ = path;
    
    // Use try-catch to ensure file is closed on any exception
    try {
        // Parse FORM header
        if (!parse_form_header()) {
            LOG_ERROR("PakReader", "Failed to parse FORM header in: " << path.filename().string());
            close();
            return false;
        }
        
        // Parse all chunks
        if (!parse_chunks()) {
            LOG_ERROR("PakReader", "Failed to parse chunks in: " << path.filename().string());
            close();
            return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("PakReader", "Exception while parsing: " << e.what());
        close();
        return false;
    }
    
    LOG_INFO("PakReader", "Opened: " << path.filename().string() 
              << " (" << file_count() << " files, DATA@" << data_offset_ << ")");
    
    return true;
}

bool PakReader::parse_form_header() {
    // Read FORM signature
    char sig[4];
    file_.read(sig, 4);
    if (std::memcmp(sig, FORM_SIGNATURE.data(), 4) != 0) {
        LOG_ERROR("PakReader", "Invalid FORM signature: '" 
                  << std::string(sig, 4) << "' (expected 'FORM')");
        return false;
    }
    
    // Read form size (big-endian)
    uint32_t form_size = read_uint32_be(file_);
    (void)form_size; // We don't need this for parsing
    
    // Read form type (should be PAC1)
    char form_type[4];
    file_.read(form_type, 4);
    if (std::memcmp(form_type, PAC1_TYPE.data(), 4) != 0) {
        LOG_ERROR("PakReader", "Invalid form type: '" 
                  << std::string(form_type, 4) << "' (expected 'PAC1')");
        return false;
    }
    
    return true;
}

bool PakReader::parse_chunks() {
    // Get file size for bounds checking
    file_.seekg(0, std::ios::end);
    size_t file_size = file_.tellg();
    file_.seekg(12); // After FORM header
    
    while (static_cast<size_t>(file_.tellg()) < file_size) {
        // Read chunk ID
        char chunk_id[4];
        file_.read(chunk_id, 4);
        if (file_.gcount() < 4) break;
        
        // Read chunk size (big-endian)
        uint32_t chunk_size = read_uint32_be(file_);
        size_t chunk_start = file_.tellg();
        
        if (std::memcmp(chunk_id, HEAD_CHUNK.data(), 4) == 0) {
            // HEAD chunk - skip for now (contains version/GUID info)
            file_.seekg(chunk_start + chunk_size);
        }
        else if (std::memcmp(chunk_id, DATA_CHUNK.data(), 4) == 0) {
            // DATA chunk - record position for file extraction
            data_offset_ = static_cast<uint32_t>(chunk_start);
            data_size_ = chunk_size;
            file_.seekg(chunk_start + chunk_size);
        }
        else if (std::memcmp(chunk_id, FILE_CHUNK.data(), 4) == 0) {
            // FILE chunk - parse directory/file entries
            parse_file_entries(file_, chunk_size);
            file_.seekg(chunk_start + chunk_size);
        }
        else {
            // Unknown chunk, skip
            file_.seekg(chunk_start + chunk_size);
        }
    }
    
    return !entries_.empty();
}

void PakReader::parse_file_entries(std::istream& stream, uint32_t chunk_size) {
    size_t start_pos = stream.tellg();
    size_t end_pos = start_pos + chunk_size;
    
    // Skip 6-byte header (appears to be version/count info)
    stream.seekg(6, std::ios::cur);
    
    // Parse entries recursively - pass entry count for root level
    // Root entries don't have a count prefix, so we parse until end_pos
    parse_directory_contents(stream, "", end_pos, -1);
}

// Parse a single entry (file or directory) and return true if successful
bool PakReader::parse_single_entry(std::istream& stream, const std::string& parent_path, size_t end_pos) {
    if (static_cast<size_t>(stream.tellg()) >= end_pos) return false;
    
    // Read entry type
    uint8_t entry_type;
    stream.read(reinterpret_cast<char*>(&entry_type), 1);
    if (stream.gcount() < 1) return false;
    
    // Read name length  
    uint8_t name_len;
    stream.read(reinterpret_cast<char*>(&name_len), 1);
    if (stream.gcount() < 1 || name_len == 0 || name_len > 255) return false;
    
    // Read name
    std::string name(name_len, '\0');
    stream.read(name.data(), name_len);
    if (stream.gcount() < name_len) return false;
    
    // Build full path
    std::string full_path = parent_path.empty() ? name : parent_path + "/" + name;
    
    if (entry_type == ENTRY_TYPE_DIRECTORY) {
        // Directory entry - read child count and parse children
        uint32_t child_count = read_uint32_le(stream);
        parse_directory_contents(stream, full_path, end_pos, static_cast<int>(child_count));
    }
    else if (entry_type == ENTRY_TYPE_FILE) {
        // File entry
        PakEntry entry;
        entry.path = full_path;
        entry.offset = read_uint32_le(stream);
        entry.size = read_uint32_le(stream);
        entry.original_size = read_uint32_le(stream);
        read_uint32_le(stream); // unknown1
        uint32_t compression = read_uint32_be(stream);
        entry.compression = static_cast<PakCompression>(compression);
        stream.seekg(4, std::ios::cur); // unknown2
        
        entries_.push_back(entry);
    }
    
    return true;
}

// Parse directory contents (children of a directory)
// If child_count is -1, parse until end_pos (for root level)
void PakReader::parse_directory_contents(std::istream& stream, const std::string& dir_path, 
                                         size_t end_pos, int child_count) {
    int parsed = 0;
    while (static_cast<size_t>(stream.tellg()) < end_pos) {
        // If we have a specific count, stop when reached
        if (child_count >= 0 && parsed >= child_count) break;
        
        if (!parse_single_entry(stream, dir_path, end_pos)) break;
        parsed++;
    }
}

void PakReader::close() {
    file_.close();
    entries_.clear();
    pak_path_.clear();
    data_offset_ = 0;
    data_size_ = 0;
}

std::vector<PakEntry> PakReader::list_files() const {
    return entries_;
}

std::vector<PakEntry> PakReader::list_files(const std::string& pattern) const {
    std::vector<PakEntry> result;
    
    for (const auto& entry : entries_) {
        if (matches_pattern(entry.path, pattern)) {
            result.push_back(entry);
        }
    }
    
    return result;
}

const PakEntry* PakReader::find_entry(const std::string& path) const {
    for (const auto& entry : entries_) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<uint8_t> PakReader::read_file(const std::string& path) {
    const PakEntry* entry = find_entry(path);
    if (!entry) {
        LOG_DEBUG("PakReader", "File not found in PAK: " << path);
        return {};
    }
    return read_file(*entry);
}

std::vector<uint8_t> PakReader::read_file(const PakEntry& entry) {
    if (!file_.is_open()) {
        LOG_ERROR("PakReader", "Cannot read file - PAK not open");
        return {};
    }
    
    // Seek to file data (offset is absolute in file)
    file_.seekg(entry.offset);
    if (!file_.good()) {
        LOG_ERROR("PakReader", "Failed to seek to offset " << entry.offset 
                  << " for file: " << entry.path);
        return {};
    }
    
    // Read data
    std::vector<uint8_t> data(entry.size);
    file_.read(reinterpret_cast<char*>(data.data()), entry.size);
    
    if (file_.gcount() != static_cast<std::streamsize>(entry.size)) {
        LOG_ERROR("PakReader", "Failed to read " << entry.size << " bytes for: " 
                  << entry.path << " (got " << file_.gcount() << ")");
        return {};
    }
    
    // Decompress if needed
    if (entry.is_compressed()) {
        try {
            if (entry.compression == PakCompression::Zlib) {
                LOG_DEBUG("PakReader", "Decompressing (zlib): " << entry.path 
                          << " (" << entry.size << " -> " << entry.original_size << ")");
                auto decompressed = decompress_zlib(data.data(), data.size(), entry.original_size);
                if (decompressed.empty()) {
                    LOG_ERROR("PakReader", "Zlib decompression returned empty for: " << entry.path);
                    return {};
                }
                return decompressed;
            }
            LOG_WARNING("PakReader", "Unknown compression type " 
                        << static_cast<uint32_t>(entry.compression) << " for: " << entry.path);
            // Add other compression types here if discovered
        } catch (const std::exception& e) {
            LOG_ERROR("PakReader", "Decompression failed for: " << entry.path << " - " << e.what());
            return {};
        }
    }
    
    return data;
}

bool PakReader::extract_file(const std::string& path, const std::filesystem::path& output_path) {
    auto data = read_file(path);
    if (data.empty()) {
        return false;
    }
    return write_file(output_path, data);
}

bool PakReader::extract_file(const PakEntry& entry, const std::filesystem::path& output_path) {
    auto data = read_file(entry);
    if (data.empty()) {
        return false;
    }
    return write_file(output_path, data);
}

bool PakReader::extract_all(const std::filesystem::path& output_dir, ProgressCallback callback) {
    size_t total = entries_.size();
    size_t current = 0;
    
    for (const auto& entry : entries_) {
        auto output_path = output_dir / entry.path;
        
        if (!extract_file(entry, output_path)) {
            // Log error but continue
        }
        
        ++current;
        if (callback) {
            if (!callback(entry.path, current, total)) {
                return false;  // Cancelled
            }
        }
    }
    
    return true;
}

size_t PakReader::file_count() const {
    return entries_.size();
}

size_t PakReader::total_size() const {
    size_t total = 0;
    for (const auto& entry : entries_) {
        total += entry.original_size;
    }
    return total;
}

size_t PakReader::compressed_size() const {
    size_t total = 0;
    for (const auto& entry : entries_) {
        total += entry.size;
    }
    return total;
}

bool PakReader::matches_pattern(const std::string& text, const std::string& pattern) const {
    // Simple glob pattern matching with * and ?
    size_t ti = 0, pi = 0;
    size_t star_idx = std::string::npos;
    size_t match_idx = 0;
    
    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
            ++ti;
            ++pi;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_idx = pi++;
            match_idx = ti;
        } else if (star_idx != std::string::npos) {
            pi = star_idx + 1;
            ti = ++match_idx;
        } else {
            return false;
        }
    }
    
    while (pi < pattern.size() && pattern[pi] == '*') {
        ++pi;
    }
    
    return pi == pattern.size();
}

} // namespace enfusion
