/**
 * Enfusion Unpacker - Path Utilities
 * 
 * Common path manipulation functions to reduce code duplication.
 */

#pragma once

#include <string>
#include <algorithm>
#include <filesystem>

namespace enfusion {

/**
 * Normalize path separators and case for consistent comparisons.
 * Converts backslashes to forward slashes and lowercases the path.
 */
inline std::string normalize_path(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

/**
 * Get lowercase file extension including the dot.
 */
inline std::string get_extension_lower(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

/**
 * Check if path ends with given suffix (case-insensitive).
 */
inline bool path_ends_with(const std::string& path, const std::string& suffix) {
    std::string path_lower = normalize_path(path);
    std::string suffix_lower = suffix;
    std::transform(suffix_lower.begin(), suffix_lower.end(), suffix_lower.begin(), ::tolower);
    
    if (suffix_lower.size() > path_lower.size()) return false;
    return path_lower.compare(path_lower.size() - suffix_lower.size(), 
                               suffix_lower.size(), suffix_lower) == 0;
}

/**
 * Extract parent directory from a path string.
 */
inline std::string get_parent_path(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    
    size_t pos = normalized.rfind('/');
    if (pos == std::string::npos) return "";
    return normalized.substr(0, pos);
}

} // namespace enfusion
