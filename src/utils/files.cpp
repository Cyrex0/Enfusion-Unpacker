/**
 * Enfusion Unpacker - File Utilities Implementation (Stub)
 */

#include "enfusion/files.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace enfusion {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

bool write_file(const std::filesystem::path& path, const uint8_t* data, size_t size) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data), size);
    return true;
}

bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    return write_file(path, data.data(), data.size());
}

bool create_directories(const std::filesystem::path& path) {
    return std::filesystem::create_directories(path);
}

bool file_exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}

size_t file_size(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return 0;
    return static_cast<size_t>(std::filesystem::file_size(path));
}

std::string get_extension(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::filesystem::path change_extension(const std::filesystem::path& path, const std::string& new_ext) {
    auto result = path;
    result.replace_extension(new_ext);
    return result;
}

std::string detect_file_type(const uint8_t* data, size_t size) {
    if (size < 4) return "unknown";
    
    // DDS
    if (data[0] == 'D' && data[1] == 'D' && data[2] == 'S' && data[3] == ' ')
        return "dds";
    
    // FORM/XOB
    if (data[0] == 'F' && data[1] == 'O' && data[2] == 'R' && data[3] == 'M')
        return "xob";
    
    // PNG
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return "png";
    
    return "unknown";
}

std::filesystem::path ensure_unique_path(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return path;
    
    auto stem = path.stem().string();
    auto ext = path.extension().string();
    auto parent = path.parent_path();
    
    // Limit iterations to prevent infinite loop (e.g., filesystem issues)
    constexpr int MAX_ATTEMPTS = 10000;
    for (int counter = 1; counter <= MAX_ATTEMPTS; ++counter) {
        auto new_path = parent / (stem + "_" + std::to_string(counter) + ext);
        if (!std::filesystem::exists(new_path)) return new_path;
    }
    
    // If we exhausted attempts, return path with timestamp
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return parent / (stem + "_" + std::to_string(now) + ext);
}

std::string format_file_size(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit < 3) {
        size /= 1024.0;
        unit++;
    }
    
    char buf[64];
    if (unit == 0) {
        snprintf(buf, sizeof(buf), "%zu B", bytes);
    } else {
        snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    }
    return buf;
}

std::string get_file_icon(const std::string& extension) {
    if (extension == ".edds" || extension == ".dds" || extension == ".png") return "[TEX]";
    if (extension == ".xob") return "[MDL]";
    if (extension == ".et" || extension == ".emat") return "[MAT]";
    if (extension == ".rdb") return "[RDB]";
    return "[FILE]";
}

} // namespace enfusion
