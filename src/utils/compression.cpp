/**
 * Enfusion Unpacker - Compression Implementation
 */

#include "enfusion/compression.hpp"
#include <zlib.h>
#include <lz4.h>
#include <stdexcept>
#include <string>

namespace enfusion {

// Helper to convert zlib error code to string
static const char* zlib_error_string(int err) {
    switch (err) {
        case Z_OK:            return "Z_OK";
        case Z_STREAM_END:    return "Z_STREAM_END";
        case Z_NEED_DICT:     return "Z_NEED_DICT";
        case Z_ERRNO:         return "Z_ERRNO";
        case Z_STREAM_ERROR:  return "Z_STREAM_ERROR";
        case Z_DATA_ERROR:    return "Z_DATA_ERROR";
        case Z_MEM_ERROR:     return "Z_MEM_ERROR";
        case Z_BUF_ERROR:     return "Z_BUF_ERROR";
        case Z_VERSION_ERROR: return "Z_VERSION_ERROR";
        default:              return "UNKNOWN_ERROR";
    }
}

std::vector<uint8_t> decompress_zlib(const uint8_t* data, size_t size, size_t expected_size) {
    std::vector<uint8_t> result(expected_size);
    
    z_stream strm = {};
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(size);
    strm.next_out = result.data();
    strm.avail_out = static_cast<uInt>(expected_size);
    
    int ret = inflateInit(&strm);
    if (ret != Z_OK) {
        throw std::runtime_error(std::string("Failed to initialize zlib decompression: ") + zlib_error_string(ret));
    }
    
    ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    
    if (ret != Z_STREAM_END) {
        throw std::runtime_error(std::string("Zlib decompression failed: ") + zlib_error_string(ret) + 
                                 " (input=" + std::to_string(size) + ", expected=" + std::to_string(expected_size) + ")");
    }
    
    result.resize(strm.total_out);
    return result;
}

std::vector<uint8_t> decompress_zlib(const std::vector<uint8_t>& data, size_t expected_size) {
    return decompress_zlib(data.data(), data.size(), expected_size);
}

std::vector<uint8_t> decompress_lz4(const uint8_t* data, size_t size, size_t expected_size) {
    std::vector<uint8_t> result(expected_size);
    
    int decompressed_size = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data),
        reinterpret_cast<char*>(result.data()),
        static_cast<int>(size),
        static_cast<int>(expected_size)
    );
    
    if (decompressed_size < 0) {
        throw std::runtime_error("LZ4 decompression failed");
    }
    
    result.resize(static_cast<size_t>(decompressed_size));
    return result;
}

std::vector<uint8_t> decompress_lz4(const std::vector<uint8_t>& data, size_t expected_size) {
    return decompress_lz4(data.data(), data.size(), expected_size);
}

std::vector<uint8_t> decompress_auto(const uint8_t* data, size_t size, size_t expected_size, CompressionType type) {
    switch (type) {
        case CompressionType::None:
            return std::vector<uint8_t>(data, data + size);
            
        case CompressionType::Zlib:
            return decompress_zlib(data, size, expected_size);
            
        case CompressionType::LZ4:
            return decompress_lz4(data, size, expected_size);
            
        default:
            throw std::runtime_error("Unknown compression type");
    }
}

CompressionType detect_compression(const uint8_t* data, size_t size) {
    if (size < 2) {
        return CompressionType::None;
    }
    
    // Check for zlib header
    // 78 01 - low compression
    // 78 9C - default compression
    // 78 DA - best compression
    if (data[0] == 0x78 && (data[1] == 0x01 || data[1] == 0x9C || data[1] == 0xDA)) {
        return CompressionType::Zlib;
    }
    
    // LZ4 doesn't have a magic header, so we can't easily detect it
    // Return None as default
    return CompressionType::None;
}

std::vector<uint8_t> compress_zlib(const uint8_t* data, size_t size, int level) {
    uLongf bound = compressBound(static_cast<uLong>(size));
    std::vector<uint8_t> result(bound);
    
    int ret = compress2(
        result.data(), &bound,
        data, static_cast<uLong>(size),
        level
    );
    
    if (ret != Z_OK) {
        throw std::runtime_error("Zlib compression failed");
    }
    
    result.resize(bound);
    return result;
}

std::vector<uint8_t> compress_lz4(const uint8_t* data, size_t size) {
    int bound = LZ4_compressBound(static_cast<int>(size));
    std::vector<uint8_t> result(static_cast<size_t>(bound));
    
    int compressed_size = LZ4_compress_default(
        reinterpret_cast<const char*>(data),
        reinterpret_cast<char*>(result.data()),
        static_cast<int>(size),
        bound
    );
    
    if (compressed_size <= 0) {
        throw std::runtime_error("LZ4 compression failed");
    }
    
    result.resize(static_cast<size_t>(compressed_size));
    return result;
}

} // namespace enfusion
