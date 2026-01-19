/**
 * Enfusion Unpacker - Compression utilities
 */

#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

namespace enfusion {

/**
 * Compression type enum.
 */
enum class CompressionType {
    None,
    Zlib,
    LZ4
};

/**
 * Detect compression type from data.
 */
CompressionType detect_compression(const uint8_t* data, size_t size);

/**
 * Auto decompress based on type.
 */
std::vector<uint8_t> decompress_auto(const uint8_t* data, size_t size, size_t expected_size, CompressionType type);

/**
 * Decompress zlib data.
 */
std::vector<uint8_t> decompress_zlib(const uint8_t* data, size_t size, size_t expected_size);
std::vector<uint8_t> decompress_zlib(const std::vector<uint8_t>& data, size_t expected_size);

/**
 * Decompress LZ4 data.
 */
std::vector<uint8_t> decompress_lz4(const uint8_t* data, size_t size, size_t expected_size);
std::vector<uint8_t> decompress_lz4(const std::vector<uint8_t>& data, size_t expected_size);

/**
 * Compress data with zlib.
 */
std::vector<uint8_t> compress_zlib(const uint8_t* data, size_t size, int level = 6);
std::vector<uint8_t> compress_zlib(const std::vector<uint8_t>& data, int level = 6);

/**
 * Compress data with LZ4.
 */
std::vector<uint8_t> compress_lz4(const uint8_t* data, size_t size);
std::vector<uint8_t> compress_lz4(const std::vector<uint8_t>& data);

} // namespace enfusion
