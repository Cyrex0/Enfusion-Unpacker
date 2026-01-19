/**
 * Enfusion Unpacker - EDDS Converter Implementation
 * Based on Python edds.py from enfusion_toolkit
 *
 * EDDS files are DDS textures with LZ4-compressed mip levels
 * 
 * EDDS Structure:
 * - Standard DDS header (128 bytes)
 * - DX10 extended header (20 bytes) if FourCC == "DX10"
 * - Mip Table: N entries x 8 bytes (Tag + Size per entry)
 *   - Tags: "COPY" = raw data, "LZ4 " = compressed
 *   - Mips are ordered smallest to largest in the table
 * - Mip Data: follows the table in same order
 * - Output: DDS header + mips ordered largest to smallest
 */

#include "enfusion/edds_converter.hpp"
#include <lz4.h>
#include <cstring>
#include <algorithm>
#include <fstream>

namespace enfusion {

static constexpr uint8_t DDS_MAGIC[4] = {'D', 'D', 'S', ' '};
static constexpr uint8_t DX10_FOURCC[4] = {'D', 'X', '1', '0'};

static inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/**
 * LZ4 stream decompression with dictionary chaining
 * 
 * Format (from Python decompress_lz4_stream):
 * - First 4 bytes: total decompressed size
 * - Block-based LZ4 with dictionary chaining:
 *   - 4-byte header: bit31 = is_final, bits0-30 = compressed_block_size
 *   - Compressed block data (uses previous block as dictionary)
 * - Max decompressed block size: 0x10000 (64KB)
 * 
 * CRITICAL: Dictionary MUST persist across ALL blocks
 */
static std::vector<uint8_t> decompress_lz4_stream(const uint8_t* data, size_t size, size_t expected_size) {
    if (size < 4) return {};
    
    std::vector<uint8_t> result;
    result.reserve(expected_size);
    
    size_t pos = 0;
    
    // First 4 bytes: total decompressed size
    uint32_t total_size = read_u32_le(data + pos);
    pos += 4;
    
    size_t remaining = total_size;
    std::vector<uint8_t> prev_dict;
    
    while (pos < size && remaining > 0) {
        if (pos + 4 > size) break;
        
        uint32_t header = read_u32_le(data + pos);
        pos += 4;
        
        uint32_t block_size = header & 0x7FFFFFFF;
        // Note: is_final flag is NOT used for decompression control
        // We continue until we've decompressed total_size bytes
        
        if (block_size == 0) break;
        if (block_size > 0x20000) break;
        if (pos + block_size > size) break;
        
        size_t expected_block = std::min(remaining, size_t(0x10000));
        std::vector<uint8_t> decompressed(expected_block);
        
        int dec_size;
        if (!prev_dict.empty()) {
            dec_size = LZ4_decompress_safe_usingDict(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                static_cast<int>(expected_block),
                reinterpret_cast<const char*>(prev_dict.data()),
                static_cast<int>(prev_dict.size())
            );
        } else {
            dec_size = LZ4_decompress_safe(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                static_cast<int>(expected_block)
            );
        }
        
        pos += block_size;
        
        if (dec_size <= 0) break;
        
        decompressed.resize(dec_size);
        result.insert(result.end(), decompressed.begin(), decompressed.end());
        remaining -= dec_size;
        
        // Use this block as dictionary for next
        if (decompressed.size() <= 0x10000) {
            prev_dict = std::move(decompressed);
        } else {
            prev_dict.assign(decompressed.end() - 0x10000, decompressed.end());
        }
        
        // Do NOT break on is_final - continue until remaining == 0
    }
    
    return result;
}

/**
 * Alternative: Simple LZ4 block decompression (no header size, single block)
 * Used when the data is a simple compressed block without stream format
 */
static std::vector<uint8_t> decompress_lz4_block(const uint8_t* data, size_t size, size_t expected_size) {
    std::vector<uint8_t> result(expected_size);
    
    int dec_size = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data),
        reinterpret_cast<char*>(result.data()),
        static_cast<int>(size),
        static_cast<int>(expected_size)
    );
    
    if (dec_size > 0) {
        result.resize(dec_size);
        return result;
    }
    
    return {};
}

EddsConverter::EddsConverter(std::span<const uint8_t> data) : data_(data) {
    if (!data_.empty()) {
        parse_header();
    }
}

bool EddsConverter::is_edds() const {
    if (data_.size() < 128) return false;
    
    if (std::memcmp(data_.data(), DDS_MAGIC, 4) != 0) return false;
    
    size_t data_offset = 128;
    if (data_.size() >= 88 && std::memcmp(data_.data() + 84, DX10_FOURCC, 4) == 0) {
        data_offset = 148;
    }
    
    if (data_offset + 4 > data_.size()) return false;
    
    const uint8_t* tag = data_.data() + data_offset;
    return (std::memcmp(tag, "COPY", 4) == 0 || std::memcmp(tag, "LZ4 ", 4) == 0);
}

std::vector<uint8_t> EddsConverter::convert() {
    if (!is_edds()) {
        return std::vector<uint8_t>(data_.begin(), data_.end());
    }
    
    size_t header_size = 128;
    if (data_.size() >= 88 && std::memcmp(data_.data() + 84, DX10_FOURCC, 4) == 0) {
        header_size = 148;
    }
    
    parse_mip_table(header_size);
    if (mip_table_.empty()) {
        return std::vector<uint8_t>(data_.begin(), data_.end());
    }
    
    std::vector<uint8_t> output(data_.begin(), data_.begin() + header_size);
    
    std::vector<std::vector<uint8_t>> mip_data;
    size_t data_pos = header_size + mip_table_.size() * 8;
    
    for (size_t i = 0; i < mip_table_.size(); i++) {
        const auto& entry = mip_table_[i];
        const auto& tag = entry.first;
        uint32_t compressed_size = entry.second;
        
        uint32_t mip_level = static_cast<uint32_t>(mip_count_ - 1 - i);
        size_t expected_size = calc_mip_size(mip_level);
        
        if (data_pos + compressed_size > data_.size()) break;
        
        const uint8_t* chunk = data_.data() + data_pos;
        data_pos += compressed_size;
        
        if (std::memcmp(tag.data(), "COPY", 4) == 0) {
            mip_data.emplace_back(chunk, chunk + compressed_size);
        } else if (std::memcmp(tag.data(), "LZ4 ", 4) == 0) {
            // Try stream decompression first (with header size)
            auto decompressed = decompress_lz4_stream(chunk, compressed_size, expected_size);
            
            // If stream failed, try simple block decompression
            if (decompressed.empty() || decompressed.size() < expected_size / 2) {
                decompressed = decompress_lz4_block(chunk, compressed_size, expected_size);
            }
            
            // Pad if still too small
            if (decompressed.size() < expected_size) {
                decompressed.resize(expected_size, 0);
            }
            mip_data.push_back(std::move(decompressed));
        } else {
            mip_data.emplace_back(chunk, chunk + compressed_size);
        }
    }
    
    for (auto it = mip_data.rbegin(); it != mip_data.rend(); ++it) {
        output.insert(output.end(), it->begin(), it->end());
    }
    
    return output;
}

bool EddsConverter::convert_to_dds(const fs::path& input, std::vector<uint8_t>& output) {
    std::ifstream file(input, std::ios::binary);
    if (!file) return false;
    
    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    
    EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
    output = converter.convert();
    return !output.empty();
}

bool EddsConverter::convert_file(const fs::path& input, const fs::path& output_path) {
    std::vector<uint8_t> dds_data;
    if (!convert_to_dds(input, dds_data)) return false;
    
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;
    
    out.write(reinterpret_cast<const char*>(dds_data.data()), dds_data.size());
    return true;
}

void EddsConverter::parse_header() {
    if (data_.size() < 32) return;
    
    height_ = read_u32_le(data_.data() + 12);
    width_ = read_u32_le(data_.data() + 16);
    mip_count_ = read_u32_le(data_.data() + 28);
    
    if (mip_count_ == 0) mip_count_ = 1;
    
    if (data_.size() >= 88) {
        const uint8_t* fourcc = data_.data() + 84;
        has_dx10_ = (std::memcmp(fourcc, DX10_FOURCC, 4) == 0);
        
        if (has_dx10_ && data_.size() >= 132) {
            uint32_t dxgi_format = read_u32_le(data_.data() + 128);
            switch (dxgi_format) {
                case 70: case 71: case 72:
                    bytes_per_block_ = 8;
                    format_ = "BC1";
                    break;
                case 73: case 74: case 75: case 76: case 77:
                case 83: case 84:
                case 95: case 96: case 97: case 98: case 99:
                    bytes_per_block_ = 16;
                    format_ = "BC7";
                    break;
                default:
                    bytes_per_block_ = 16;
                    format_ = "UNKNOWN";
                    break;
            }
        } else {
            if (std::memcmp(fourcc, "DXT1", 4) == 0) {
                bytes_per_block_ = 8;
                format_ = "DXT1";
            } else {
                bytes_per_block_ = 16;
                format_ = "DXT5";
            }
        }
    }
}

std::vector<std::pair<uint32_t, uint32_t>> EddsConverter::parse_mip_table(size_t data_offset) {
    std::vector<std::pair<uint32_t, uint32_t>> table;
    mip_table_.clear();
    
    size_t pos = data_offset;
    for (uint32_t i = 0; i < mip_count_; i++) {
        if (pos + 8 > data_.size()) break;
        
        std::array<uint8_t, 4> tag;
        std::memcpy(tag.data(), data_.data() + pos, 4);
        uint32_t size = read_u32_le(data_.data() + pos + 4);
        
        mip_table_.push_back({tag, size});
        table.push_back({0, size});
        
        pos += 8;
    }
    
    return table;
}

size_t EddsConverter::calc_mip_size(uint32_t mip_level) const {
    uint32_t mip_w = std::max(1u, width_ >> mip_level);
    uint32_t mip_h = std::max(1u, height_ >> mip_level);
    uint32_t blocks_x = std::max(1u, (mip_w + 3) / 4);
    uint32_t blocks_y = std::max(1u, (mip_h + 3) / 4);
    return static_cast<size_t>(blocks_x) * blocks_y * bytes_per_block_;
}

std::string EddsConverter::get_format_name(uint32_t format) {
    switch (format) {
        case 70: case 71: case 72: return "BC1";
        case 73: case 74: case 75: return "BC2";
        case 76: case 77: return "BC3";
        case 83: return "BC4";
        case 84: return "BC5";
        case 95: case 96: case 97: case 98: case 99: return "BC7";
        default: return "UNKNOWN";
    }
}

std::string EddsConverter::format_name() const {
    return format_;
}

} // namespace enfusion
