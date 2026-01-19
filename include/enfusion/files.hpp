/**
 * Enfusion Unpacker - File utilities
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>

namespace enfusion {

namespace fs = std::filesystem;

/**
 * Read entire file into memory.
 */
std::vector<uint8_t> read_file(const std::filesystem::path& path);

/**
 * Write data to file.
 */
bool write_file(const std::filesystem::path& path, const uint8_t* data, size_t size);
bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data);

/**
 * Create directories recursively.
 */
bool create_directories(const std::filesystem::path& path);

/**
 * Check if file exists.
 */
bool file_exists(const std::filesystem::path& path);

/**
 * Get file size.
 */
size_t file_size(const std::filesystem::path& path);

/**
 * Get file extension (lowercase).
 */
std::string get_extension(const std::filesystem::path& path);

/**
 * Change file extension.
 */
std::filesystem::path change_extension(const std::filesystem::path& path, const std::string& new_ext);

/**
 * Detect file type from data.
 */
std::string detect_file_type(const uint8_t* data, size_t size);

/**
 * Ensure unique path.
 */
std::filesystem::path ensure_unique_path(const std::filesystem::path& path);

/**
 * Format file size for display.
 */
std::string format_file_size(size_t bytes);

/**
 * Get file icon for extension.
 */
std::string get_file_icon(const std::string& extension);

} // namespace enfusion
