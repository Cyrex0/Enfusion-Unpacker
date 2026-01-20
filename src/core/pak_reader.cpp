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

#include <fstream>
#include <cstring>
#include <algorithm>
#include <array>

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
    file_.open(path, std::ios::binary);
    if (!file_) {
        return false;
    }
    
    pak_path_ = path;
    
    // Parse FORM header
    if (!parse_form_header()) {
        file_.close();
        return false;
    }
    
    // Parse all chunks
    if (!parse_chunks()) {
        file_.close();
        return false;
    }
    
    return true;
}

bool PakReader::parse_form_header() {
    // Read FORM signature
    char sig[4];
    file_.read(sig, 4);
    if (std::memcmp(sig, FORM_SIGNATURE.data(), 4) != 0) {
        return false;
    }
    
    // Read form size (big-endian)
    uint32_t form_size = read_uint32_be(file_);
    (void)form_size; // We don't need this for parsing
    
    // Read form type (should be PAC1)
    char form_type[4];
    file_.read(form_type, 4);
    if (std::memcmp(form_type, PAC1_TYPE.data(), 4) != 0) {
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
    
    // Parse entries recursively
    parse_entry_recursive(stream, "", end_pos);
}

void PakReader::parse_entry_recursive(std::istream& stream, const std::string& parent_path, size_t end_pos) {
    while (static_cast<size_t>(stream.tellg()) < end_pos) {
        // Read entry type
        uint8_t entry_type;
        stream.read(reinterpret_cast<char*>(&entry_type), 1);
        if (stream.gcount() < 1) break;
        
        // Read name length
        uint8_t name_len;
        stream.read(reinterpret_cast<char*>(&name_len), 1);
        if (stream.gcount() < 1 || name_len == 0 || name_len > 255) break;
        
        // Read name
        std::string name(name_len, '\0');
        stream.read(name.data(), name_len);
        if (stream.gcount() < name_len) break;
        
        // Build full path
        std::string full_path = parent_path.empty() ? name : parent_path + "/" + name;
        
        if (entry_type == ENTRY_TYPE_DIRECTORY) {
            // Directory entry
            uint32_t child_count = read_uint32_le(stream);
            
            // Recursively parse children
            for (uint32_t i = 0; i < child_count && static_cast<size_t>(stream.tellg()) < end_pos; ++i) {
                // Read child entry type
                uint8_t child_type;
                stream.read(reinterpret_cast<char*>(&child_type), 1);
                if (stream.gcount() < 1) break;
                
                // Read child name length
                uint8_t child_name_len;
                stream.read(reinterpret_cast<char*>(&child_name_len), 1);
                if (stream.gcount() < 1 || child_name_len == 0) break;
                
                // Read child name
                std::string child_name(child_name_len, '\0');
                stream.read(child_name.data(), child_name_len);
                if (stream.gcount() < child_name_len) break;
                
                std::string child_full_path = full_path + "/" + child_name;
                
                if (child_type == ENTRY_TYPE_DIRECTORY) {
                    // Nested directory - recurse
                    uint32_t grandchild_count = read_uint32_le(stream);
                    
                    // Parse grandchildren inline
                    for (uint32_t j = 0; j < grandchild_count && static_cast<size_t>(stream.tellg()) < end_pos; ++j) {
                        // Use helper to parse single entry
                        size_t pos_before = stream.tellg();
                        parse_entry_recursive(stream, child_full_path, end_pos);
                        // Break after one entry since parse_entry_recursive handles one at a time
                        if (stream.tellg() == pos_before) break;
                    }
                }
                else if (child_type == ENTRY_TYPE_FILE) {
                    // File entry
                    PakEntry entry;
                    entry.path = child_full_path;
                    entry.offset = read_uint32_le(stream);
                    entry.size = read_uint32_le(stream);
                    entry.original_size = read_uint32_le(stream);
                    read_uint32_le(stream); // unknown1
                    uint32_t compression = read_uint32_be(stream);
                    entry.compression = static_cast<PakCompression>(compression);
                    stream.seekg(4, std::ios::cur); // unknown2
                    
                    entries_.push_back(entry);
                }
            }
        }
        else if (entry_type == ENTRY_TYPE_FILE) {
            // File entry at root level
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
        return {};
    }
    return read_file(*entry);
}

std::vector<uint8_t> PakReader::read_file(const PakEntry& entry) {
    if (!file_.is_open()) {
        return {};
    }
    
    // Seek to file data (offset is absolute in file)
    file_.seekg(entry.offset);
    
    // Read data
    std::vector<uint8_t> data(entry.size);
    file_.read(reinterpret_cast<char*>(data.data()), entry.size);
    
    // Decompress if needed
    if (entry.is_compressed()) {
        try {
            if (entry.compression == PakCompression::Zlib) {
                return decompress_zlib(data.data(), data.size(), entry.original_size);
            }
            // Add other compression types here if discovered
        } catch (...) {
            // Decompression failed, return empty
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
