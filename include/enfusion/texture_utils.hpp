/**
 * Enfusion Unpacker - Texture Utilities
 * 
 * Common texture-related constants and utilities.
 */

#pragma once

#include <string>
#include <vector>
#include <algorithm>

namespace enfusion {

// Texture file suffixes for different types
namespace texture_suffixes {

// Color/Diffuse texture suffixes (primary textures)
inline const std::vector<std::string> COLOR = {
    "_BCR.edds", "_MCR.edds", "_co.edds", ".edds",
    "_BCR.dds", "_MCR.dds", "_co.dds", ".dds"
};

// Normal map suffixes (should be skipped for color lookups)
inline const std::vector<std::string> NORMAL = {
    "_nmo", "_normal", "_nm", "_n"
};

// Specular/Material property suffixes
inline const std::vector<std::string> MATERIAL = {
    "_smdi", "_specular", "_spec", "_roughness", "_metallic"
};

// Mask/Utility texture suffixes
inline const std::vector<std::string> MASK = {
    "_global_mask", "_mask", "_ao", "_occlusion"
};

// Other texture suffixes to skip for base color detection
inline const std::vector<std::string> SKIP_FOR_COLOR = {
    "_global_mask", "_mask", "_nmo", "_normal", "_nm", "_n",
    "_smdi", "_specular", "_spec", "_ao", "_occlusion",
    "_roughness", "_metallic", "_height", "_emissive", 
    "_opacity", "_alpha", "_vfx"
};

} // namespace texture_suffixes

/**
 * Check if a texture path should be skipped for base color detection.
 */
inline bool is_non_color_texture(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    for (const auto& suffix : texture_suffixes::SKIP_FOR_COLOR) {
        if (lower.find(suffix) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/**
 * Try to find base color texture from material name.
 * Returns possible texture paths to try.
 */
inline std::vector<std::string> get_color_texture_paths(
    const std::string& material_name, 
    const std::string& base_dir) 
{
    std::vector<std::string> paths;
    
    std::string dir = base_dir;
    if (!dir.empty() && dir.back() != '/') dir += '/';
    
    // Try Textures subfolder first
    for (const auto& suffix : texture_suffixes::COLOR) {
        paths.push_back(dir + "Textures/" + material_name + suffix);
    }
    
    // Then same directory
    for (const auto& suffix : texture_suffixes::COLOR) {
        paths.push_back(dir + material_name + suffix);
    }
    
    return paths;
}

} // namespace enfusion
