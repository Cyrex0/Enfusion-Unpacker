/**
 * Enfusion Unpacker - PAK Reader Implementation
 */

#include "enfusion/pak_reader.hpp"
#include "enfusion/compression.hpp"
#include "enfusion/files.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>

namespace enfusion {

// PAK file format constants
constexpr uint32_t PAK_MAGIC = 0x01000003;  // "PAK" magic number
constexpr size_t PAK_HEADER_SIZE = 24;

struct PakHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint64_t toc_offset;
    uint32_t toc_size;
};

PakReader::PakReader() = default;
PakReader::~PakReader() = default;

bool PakReader::open(const std::filesystem::path& path) {
    file_.open(path, std::ios::binary);
    if (!file_) {
        return false;
    }
    
    pak_path_ = path;
    
    // Read header
    PakHeader header;
    file_.read(reinterpret_cast<char*>(&header), sizeof(header));
    
    if (header.magic != PAK_MAGIC) {
        file_.close();
        return false;
    }
    
    // Read table of contents
    file_.seekg(header.toc_offset);
    
    std::vector<uint8_t> toc_data(header.toc_size);
    file_.read(reinterpret_cast<char*>(toc_data.data()), header.toc_size);
    
    // Parse TOC entries
    parse_toc(toc_data, header.file_count);
    
    return true;
}

void PakReader::close() {
    file_.close();
    entries_.clear();
    pak_path_.clear();
}

void PakReader::parse_toc(const std::vector<uint8_t>& toc_data, uint32_t file_count) {
    entries_.clear();
    entries_.reserve(file_count);
    
    size_t offset = 0;
    
    for (uint32_t i = 0; i < file_count && offset < toc_data.size(); ++i) {
        PakEntry entry;
        
        // Read path length
        if (offset + 2 > toc_data.size()) break;
        uint16_t path_len = *reinterpret_cast<const uint16_t*>(&toc_data[offset]);
        offset += 2;
        
        // Read path
        if (offset + path_len > toc_data.size()) break;
        entry.path = std::string(reinterpret_cast<const char*>(&toc_data[offset]), path_len);
        offset += path_len;
        
        // Read file info
        if (offset + 24 > toc_data.size()) break;
        entry.offset = *reinterpret_cast<const uint64_t*>(&toc_data[offset]);
        entry.size = *reinterpret_cast<const uint32_t*>(&toc_data[offset + 8]);
        entry.compressed_size = *reinterpret_cast<const uint32_t*>(&toc_data[offset + 12]);
        entry.flags = *reinterpret_cast<const uint32_t*>(&toc_data[offset + 16]);
        entry.crc = *reinterpret_cast<const uint32_t*>(&toc_data[offset + 20]);
        offset += 24;
        
        entry.is_compressed = (entry.compressed_size != entry.size);
        
        entries_.push_back(entry);
    }
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
    
    // Seek to file data
    file_.seekg(entry.offset);
    
    // Read compressed or raw data
    size_t read_size = entry.is_compressed ? entry.compressed_size : entry.size;
    std::vector<uint8_t> data(read_size);
    file_.read(reinterpret_cast<char*>(data.data()), read_size);
    
    // Decompress if needed
    if (entry.is_compressed) {
        try {
            // Detect compression type
            CompressionType type = detect_compression(data.data(), data.size());
            if (type == CompressionType::None) {
                // Try LZ4 as default for unknown
                type = CompressionType::LZ4;
            }
            return decompress_auto(data.data(), data.size(), entry.size, type);
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
        total += entry.size;
    }
    return total;
}

size_t PakReader::compressed_size() const {
    size_t total = 0;
    for (const auto& entry : entries_) {
        total += entry.is_compressed ? entry.compressed_size : entry.size;
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


