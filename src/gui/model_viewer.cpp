/**
 * Enfusion Unpacker - 3D Model Viewer Implementation
 */

#include "gui/model_viewer.hpp"
#include "enfusion/xob_parser.hpp"
#include "enfusion/edds_converter.hpp"
#include "enfusion/dds_loader.hpp"
#include "enfusion/files.hpp"
#include "enfusion/pak_manager.hpp"
#include "enfusion/pak_index.hpp"
#include "enfusion/logging.hpp"
#include "renderer/mesh_renderer.hpp"
#include "renderer/camera.hpp"

#include <imgui.h>
#include <glad/glad.h>
#include <cfloat>
#include <algorithm>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>

namespace enfusion {

/**
 * Strip GUID prefix from Enfusion paths
 * Paths like "{21323EC9A1A7061A}Assets/..." become "Assets/..."
 */
static std::string strip_guid_prefix(const std::string& path) {
    if (path.length() > 18 && path[0] == '{') {
        size_t close_brace = path.find('}');
        if (close_brace == 17) {  // GUIDs are 16 hex chars
            return path.substr(18);
        }
    }
    return path;
}

// Material info struct to hold parsed data
struct MaterialInfo {
    std::map<std::string, std::string> textures;
    float base_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};  // Default mid-gray
    bool has_color = false;
    bool is_mcr_material = false;  // True for MatPBRMulti with MCR textures
};

/**
 * Parse color value from emat content like "Color_3 0.434 0.301 0.205 1"
 */
static bool parse_color_value(const std::string& content, const std::string& color_name, float* out_color) {
    // Look for pattern like "Color_3 0.434 0.301 0.205 1"
    size_t pos = content.find(color_name);
    if (pos == std::string::npos) return false;
    
    pos += color_name.length();
    // Skip whitespace
    while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
    
    // Find end of line (look for newline, carriage return, or opening brace for next property)
    size_t end_pos = content.find('\n', pos);
    if (end_pos == std::string::npos) end_pos = content.length();
    
    // Also check for carriage return
    size_t cr_pos = content.find('\r', pos);
    if (cr_pos != std::string::npos && cr_pos < end_pos) end_pos = cr_pos;
    
    // Also stop at '..' which indicates newline in our logging (but don't stop at decimal points)
    // We need to be careful not to stop at decimal points in floats
    
    std::string value_str = content.substr(pos, end_pos - pos);
    
    std::istringstream iss(value_str);
    float r, g, b, a;
    if (iss >> r >> g >> b >> a) {
        out_color[0] = r;
        out_color[1] = g;
        out_color[2] = b;
        out_color[3] = a;
        LOG_DEBUG("EmatParser", "  Parsed " << color_name << ": " << r << ", " << g << ", " << b << ", " << a);
        return true;
    }
    return false;
}

/**
 * Parse an .emat file to extract texture paths and material properties.
 * 
 * The emat file IS the source of truth. It tells us exactly which textures the material uses.
 * Materials can (and do) reference textures from other assets, shared data, etc.
 * 
 * For MatPBRMulti materials:
 * - MCR = Metallic-Cavity-Roughness (NOT color in G channel!)
 * - Base color comes from Color_X parameters (Color_1, Color_2, Color_3)
 * - Color_3 is typically the main surface color
 * 
 * PRIORITY for diffuse:
 * 1. Texture matching material name + _BCR (actual color texture)
 * 2. First _BCR texture
 * 3. For MCR-only materials, we use the Color parameter
 */
static MaterialInfo parse_emat_material(const std::vector<uint8_t>& data, 
                                        const std::string& material_name = "") {
    MaterialInfo info;
    
    if (data.empty()) return info;
    
    std::string content(data.begin(), data.end());
    
    LOG_DEBUG("EmatParser", "Parsing emat for: " << material_name);
    LOG_DEBUG("EmatParser", "Emat size: " << data.size() << " bytes");
    
    // Dump full emat content for analysis (truncate if too long)
    std::string content_preview = content;
    // Replace non-printable chars with dots for logging
    for (char& c : content_preview) {
        if (c < 32 || c > 126) c = '.';
    }
    if (content_preview.length() > 2000) {
        LOG_DEBUG("EmatParser", "Emat content (first 2000 chars): " << content_preview.substr(0, 2000));
    } else {
        LOG_DEBUG("EmatParser", "Emat content: " << content_preview);
    }
    
    // Get material base name for matching
    std::string mat_base = material_name;
    std::transform(mat_base.begin(), mat_base.end(), mat_base.begin(), ::tolower);
    
    // Collect ALL texture paths in order
    std::vector<std::string> all_textures;
    size_t pos = 0;
    while ((pos = content.find(".edds", pos)) != std::string::npos) {
        size_t start = pos;
        while (start > 0 && content[start - 1] != '\0' && content[start - 1] != '"' && 
               content[start - 1] != ' ' && content[start - 1] != '\n' && content[start - 1] != '\r' &&
               static_cast<unsigned char>(content[start - 1]) >= 32) {
            start--;
        }
        
        std::string path = content.substr(start, pos + 5 - start);
        
        // Strip GUID prefix
        if (path.length() > 18 && path[0] == '{') {
            size_t close_brace = path.find('}');
            if (close_brace == 17) {
                path = path.substr(18);
            }
        }
        
        if (!path.empty()) {
            all_textures.push_back(path);
        }
        pos += 5;
    }
    
    LOG_DEBUG("EmatParser", "Found " << all_textures.size() << " textures in emat:");
    for (size_t i = 0; i < all_textures.size(); i++) {
        LOG_DEBUG("EmatParser", "  [" << i << "] " << all_textures[i]);
    }
    
    // Collect candidates for diffuse texture with priority scoring
    struct TextureCandidate {
        std::string path;
        int priority;  // Lower = better
    };
    std::vector<TextureCandidate> diffuse_candidates;
    
    // Go through textures IN ORDER
    for (const auto& path : all_textures) {
        std::string filename = path;
        size_t last_slash = filename.rfind('/');
        if (last_slash != std::string::npos) filename = filename.substr(last_slash + 1);
        
        std::string filename_lower = filename;
        std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);
        
        // Skip non-texture types
        if (filename_lower.find("_mask") != std::string::npos ||
            filename_lower.find("_vfx") != std::string::npos ||
            filename_lower.find("_ao") != std::string::npos ||
            filename_lower.find("_emissive") != std::string::npos ||
            filename_lower.find("_opacity") != std::string::npos ||
            filename_lower.find("_alpha") != std::string::npos ||
            filename_lower.find("_a.edds") != std::string::npos) {
            LOG_DEBUG("EmatParser", "  Skipping (non-diffuse type): " << path);
            continue;
        }
        
        // Normal maps
        if (filename_lower.find("_nmo") != std::string::npos ||
            filename_lower.find("_normal") != std::string::npos ||
            filename_lower.find("_nm.") != std::string::npos) {
            if (!info.textures.count("Normal")) {
                info.textures["Normal"] = path;
                LOG_DEBUG("EmatParser", "  -> Normal map: " << path);
            }
            continue;
        }
        
        // SMDI (specular/metallic)
        if (filename_lower.find("_smdi") != std::string::npos ||
            filename_lower.find("_specular") != std::string::npos) {
            if (!info.textures.count("Specular")) {
                info.textures["Specular"] = path;
                LOG_DEBUG("EmatParser", "  -> Specular: " << path);
            }
            continue;
        }
        
        // MCR texture - save for PBR data (NOT diffuse color!)
        bool is_mcr = filename_lower.find("_mcr") != std::string::npos;
        if (is_mcr) {
            // Check if it matches material name
            std::string tex_base = filename_lower;
            size_t suffix_pos = tex_base.rfind("_mcr");
            if (suffix_pos != std::string::npos) tex_base = tex_base.substr(0, suffix_pos);
            suffix_pos = tex_base.rfind(".edds");
            if (suffix_pos != std::string::npos) tex_base = tex_base.substr(0, suffix_pos);
            
            bool name_match = !mat_base.empty() && tex_base.find(mat_base) != std::string::npos;
            if (name_match || !info.textures.count("MCR")) {
                info.textures["MCR"] = path;
                info.is_mcr_material = true;
                LOG_DEBUG("EmatParser", "  -> MCR texture: " << path << (name_match ? " (name match)" : ""));
            }
            continue;
        }
        
        // Color/diffuse textures (BCR = Base Color + Roughness, has actual color!)
        bool is_bcr = filename_lower.find("_bcr") != std::string::npos;
        bool is_diffuse = filename_lower.find("_diffuse") != std::string::npos ||
                          filename_lower.find("_albedo") != std::string::npos ||
                          filename_lower.find("_color") != std::string::npos ||
                          filename_lower.find("_co.") != std::string::npos ||
                          filename_lower.find("_co_") != std::string::npos ||
                          filename_lower.find("_basecolor") != std::string::npos;
        
        if (is_bcr || is_diffuse) {
            int priority = 1000;  // Default low priority
            
            // Check if filename matches material name (highest priority)
            std::string tex_base = filename_lower;
            // Remove suffix for comparison
            size_t suffix_pos = tex_base.rfind("_bcr");
            if (suffix_pos != std::string::npos) tex_base = tex_base.substr(0, suffix_pos);
            suffix_pos = tex_base.rfind(".edds");
            if (suffix_pos != std::string::npos) tex_base = tex_base.substr(0, suffix_pos);
            
            // Check for overlay textures by their names (not by path!)
            // These are layer textures for multi-material blending, not primary diffuse
            bool is_overlay = filename_lower.find("st_mud") != std::string::npos ||
                              filename_lower.find("st_dirt") != std::string::npos ||
                              filename_lower.find("st_dust") != std::string::npos ||
                              filename_lower.find("st_plaster") != std::string::npos ||
                              filename_lower.find("st_rust") != std::string::npos ||
                              filename_lower.find("st_decal") != std::string::npos ||
                              filename_lower.find("_detail") != std::string::npos ||
                              filename_lower.find("_overlay") != std::string::npos ||
                              filename_lower.find("_weathering") != std::string::npos;
            
            // Note: ST_Leather_01_BCR.edds, ST_Metal_01_BCR.edds etc. in _SharedData are 
            // PRIMARY diffuse textures, not overlays! Don't filter by path.
            
            if (is_overlay) {
                priority = 900;  // Overlay textures - lowest priority
                LOG_DEBUG("EmatParser", "  Overlay/detail BCR (low priority): " << path);
            } else if (!mat_base.empty() && tex_base.find(mat_base) != std::string::npos) {
                // Texture name contains material name - high priority
                priority = is_bcr ? 10 : 20;
                LOG_DEBUG("EmatParser", "  BCR name match (priority=" << priority << "): " << path);
            } else if (!mat_base.empty() && mat_base.find(tex_base) != std::string::npos && tex_base.length() > 5) {
                // Material name contains texture base - good match
                priority = is_bcr ? 50 : 60;
                LOG_DEBUG("EmatParser", "  Partial match (priority=" << priority << "): " << path);
            } else {
                // No name match but still BCR texture - give reasonable priority
                // BCR in _SharedData folders are legitimate textures
                priority = is_bcr ? 100 : 300;
                LOG_DEBUG("EmatParser", "  No name match (priority=" << priority << "): " << path);
            }
            
            diffuse_candidates.push_back({path, priority});
        }
    }
    
    // Sort candidates by priority (lower = better) and pick the best
    LOG_DEBUG("EmatParser", "Diffuse (BCR) candidates: " << diffuse_candidates.size());
    for (const auto& c : diffuse_candidates) {
        LOG_DEBUG("EmatParser", "  priority=" << c.priority << " path=" << c.path);
    }
    
    if (!diffuse_candidates.empty()) {
        std::sort(diffuse_candidates.begin(), diffuse_candidates.end(),
                  [](const TextureCandidate& a, const TextureCandidate& b) {
                      return a.priority < b.priority;
                  });
        
        // Use best candidate - for MCR-only materials, overlay BCR is still useful
        // Prioritize non-overlay (< 500), but use overlay if nothing better available
        if (diffuse_candidates[0].priority < 500) {
            info.textures["Diffuse"] = diffuse_candidates[0].path;
            LOG_INFO("EmatParser", "Selected diffuse BCR: " << diffuse_candidates[0].path 
                      << " (priority=" << diffuse_candidates[0].priority << ")");
        } else if (info.is_mcr_material) {
            // For MCR materials, use the MCR texture as visual reference if no BCR
            // The shader will use base color from Color_3 parameter
            LOG_INFO("EmatParser", "MCR material - using base color, no BCR texture");
        } else {
            // No good BCR, but we have overlay candidates - use best one as fallback
            info.textures["Diffuse"] = diffuse_candidates[0].path;
            LOG_INFO("EmatParser", "Using fallback diffuse BCR: " << diffuse_candidates[0].path 
                      << " (priority=" << diffuse_candidates[0].priority << ")");
        }
    }
    
    // Parse color parameters from the emat content
    // MatPBRMulti uses Color_1, Color_2, Color_3 etc.
    // Color_3 is typically the main surface color for vehicles
    LOG_DEBUG("EmatParser", "Parsing color parameters...");
    
    // Try Color_3 first (main color), then Color_2, then Color_1, then Color
    if (parse_color_value(content, "Color_3 ", info.base_color)) {
        info.has_color = true;
        LOG_INFO("EmatParser", "Using Color_3 as base color: " << info.base_color[0] << ", " 
                 << info.base_color[1] << ", " << info.base_color[2]);
    } else if (parse_color_value(content, "Color_2 ", info.base_color)) {
        info.has_color = true;
        LOG_INFO("EmatParser", "Using Color_2 as base color");
    } else if (parse_color_value(content, "Color_1 ", info.base_color)) {
        info.has_color = true;
        LOG_INFO("EmatParser", "Using Color_1 as base color");
    } else if (parse_color_value(content, "Color ", info.base_color)) {
        info.has_color = true;
        LOG_INFO("EmatParser", "Using Color as base color");
    }
    
    // If no diffuse BCR texture but we have MCR + color, log that we'll use solid color
    if (!info.textures.count("Diffuse") && info.is_mcr_material && info.has_color) {
        LOG_INFO("EmatParser", "MCR material without BCR - will use solid color rendering");
    } else if (!info.textures.count("Diffuse") && !info.has_color) {
        LOG_WARN("EmatParser", "No diffuse texture and no color found for: " << material_name);
    }
    
    return info;
}


ModelViewer::ModelViewer()
    : camera_(std::make_unique<Camera>())
    , renderer_(std::make_unique<MeshRenderer>()) {

    camera_->set_distance(5.0f);
    camera_->set_angles(45.0f, 30.0f);

    renderer_->init();
}

ModelViewer::~ModelViewer() {
    destroy_textures();
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteTextures(1, &fb_texture_);
        glDeleteRenderbuffers(1, &fb_depth_);
    }
}

void ModelViewer::destroy_textures() {
    if (diffuse_texture_ != 0) {
        glDeleteTextures(1, &diffuse_texture_);
        diffuse_texture_ = 0;
    }
    
    // Delete per-material textures (these reference the cache, so just clear the map)
    material_diffuse_textures_.clear();
    
    // Clear the texture cache (deletes all cached OpenGL textures)
    clear_texture_cache();
    
    renderer_->set_texture(0);
    renderer_->clear_material_textures();
}

void ModelViewer::clear_texture_cache() {
    for (auto& [path, cached] : texture_cache_) {
        if (cached.gl_texture_id != 0) {
            glDeleteTextures(1, &cached.gl_texture_id);
        }
    }
    texture_cache_.clear();
}

uint32_t ModelViewer::get_cached_texture(const std::string& path) const {
    std::string key = path;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    
    auto it = texture_cache_.find(key);
    if (it != texture_cache_.end()) {
        return it->second.gl_texture_id;
    }
    return 0;
}

void ModelViewer::add_texture_to_cache(const std::string& path, uint32_t texture_id, int width, int height) {
    std::string key = path;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    
    CachedTexture cached;
    cached.gl_texture_id = texture_id;
    cached.width = width;
    cached.height = height;
    cached.ref_count = 1;
    
    texture_cache_[key] = cached;
}

std::string ModelViewer::find_best_texture_match(const std::string& material_name,
                                                  const std::string& material_dir) const {
    // Extract base name from material (remove extension)
    std::string mat_base = material_name;
    size_t ext_pos = mat_base.rfind('.');
    if (ext_pos != std::string::npos) {
        mat_base = mat_base.substr(0, ext_pos);
    }
    std::string mat_base_lower = mat_base;
    std::transform(mat_base_lower.begin(), mat_base_lower.end(), mat_base_lower.begin(), ::tolower);
    
    // Diffuse texture suffixes in priority order
    static const std::vector<std::string> diffuse_suffixes = {
        "_bcr", "_mcr", "_co", "_diffuse", "_diff", "_d", "_albedo", "_color", "_basecolor", ""
    };
    
    // Suffixes to skip (non-diffuse)
    static const std::vector<std::string> skip_suffixes = {
        "_global_mask", "_mask", "_nmo", "_normal", "_nm", "_n",
        "_smdi", "_specular", "_spec", "_ao", "_occlusion",
        "_roughness", "_metallic", "_height",
        "_emissive", "_opacity", "_alpha", "_vfx"
    };
    
    std::string best_match;
    int best_priority = 999;
    bool best_in_same_dir = false;  // Prefer textures in same/nearby directory
    
    // Get material directory for locality matching
    std::string mat_dir_lower = material_dir;
    std::transform(mat_dir_lower.begin(), mat_dir_lower.end(), mat_dir_lower.begin(), ::tolower);
    
    for (const auto& tex : available_textures_) {
        std::string tex_lower = tex;
        std::transform(tex_lower.begin(), tex_lower.end(), tex_lower.begin(), ::tolower);
        
        // Only consider .edds files
        if (tex_lower.find(".edds") == std::string::npos) continue;
        
        // Extract filename without path
        std::string tex_filename = tex;
        size_t slash_pos = tex.rfind('/');
        if (slash_pos != std::string::npos) {
            tex_filename = tex.substr(slash_pos + 1);
        }
        
        // Extract base name (remove extension)
        std::string tex_base = tex_filename;
        size_t edds_pos = tex_base.rfind(".edds");
        if (edds_pos != std::string::npos) {
            tex_base = tex_base.substr(0, edds_pos);
        }
        std::string tex_base_lower = tex_base;
        std::transform(tex_base_lower.begin(), tex_base_lower.end(), tex_base_lower.begin(), ::tolower);
        
        // Remove common suffixes from texture name for comparison
        std::string tex_core = tex_base_lower;
        for (const auto& suffix : diffuse_suffixes) {
            if (!suffix.empty() && tex_core.length() > suffix.length()) {
                if (tex_core.substr(tex_core.length() - suffix.length()) == suffix) {
                    tex_core = tex_core.substr(0, tex_core.length() - suffix.length());
                    break;
                }
            }
        }
        
        // STRICT MATCHING: Only match if core names are very similar
        bool name_matches = false;
        int match_quality = 0;  // Higher = better match
        
        // 1. Exact match (texture core == material base) - BEST
        if (tex_core == mat_base_lower) {
            name_matches = true;
            match_quality = 100;
        }
        // 2. Texture core starts with full material name
        else if (tex_core.find(mat_base_lower) == 0 && 
                 (tex_core.length() == mat_base_lower.length() || tex_core[mat_base_lower.length()] == '_')) {
            name_matches = true;
            match_quality = 90;
        }
        // 3. Material name starts with texture core (texture is base for material variant)
        else if (mat_base_lower.find(tex_core) == 0 && tex_core.length() >= 6 &&
                 (mat_base_lower.length() == tex_core.length() || mat_base_lower[tex_core.length()] == '_')) {
            name_matches = true;
            match_quality = 80;
        }
        // 4. Check if they share the same "base" before any underscore suffix
        else {
            // Extract base name before first underscore
            size_t mat_underscore = mat_base_lower.find('_');
            size_t tex_underscore = tex_core.find('_');
            
            std::string mat_first_part = (mat_underscore != std::string::npos) ? 
                mat_base_lower.substr(0, mat_underscore) : mat_base_lower;
            std::string tex_first_part = (tex_underscore != std::string::npos) ? 
                tex_core.substr(0, tex_underscore) : tex_core;
            
            // Only match if first parts are identical AND both are meaningful (>= 4 chars)
            // AND the second parts also have some similarity
            if (mat_first_part == tex_first_part && mat_first_part.length() >= 4) {
                // Check if second parts are also related
                std::string mat_rest = (mat_underscore != std::string::npos) ?
                    mat_base_lower.substr(mat_underscore + 1) : "";
                std::string tex_rest = (tex_underscore != std::string::npos) ?
                    tex_core.substr(tex_underscore + 1) : "";
                
                // Match if: same first part AND (no second parts OR second parts share prefix)
                if (mat_rest.empty() || tex_rest.empty() || 
                    mat_rest.find(tex_rest) == 0 || tex_rest.find(mat_rest) == 0) {
                    name_matches = true;
                    match_quality = 70;
                }
            }
        }
        
        if (!name_matches) continue;
        
        // Skip non-diffuse textures
        bool should_skip = false;
        for (const auto& skip : skip_suffixes) {
            if (tex_base_lower.length() >= skip.length()) {
                std::string ending = tex_base_lower.substr(tex_base_lower.length() - skip.length());
                if (ending == skip) { should_skip = true; break; }
            }
        }
        if (should_skip) continue;
        
        // Check if texture is in same/similar directory
        bool in_same_dir = false;
        if (!mat_dir_lower.empty()) {
            in_same_dir = tex_lower.find(mat_dir_lower) != std::string::npos;
        }
        
        // Find priority based on suffix
        int priority = 10;  // Default
        for (size_t i = 0; i < diffuse_suffixes.size(); i++) {
            const auto& suffix = diffuse_suffixes[i];
            bool matches = suffix.empty() ? 
                (tex_base_lower == mat_base_lower) :
                (tex_base_lower.length() >= suffix.length() && 
                 tex_base_lower.substr(tex_base_lower.length() - suffix.length()) == suffix);
            
            if (matches) {
                priority = static_cast<int>(i);
                break;
            }
        }
        
        // Combine match quality, priority, and directory bonus
        int score = match_quality * 1000 - priority * 10 + (in_same_dir ? 5 : 0);
        int best_score = (best_match.empty()) ? -1 : 
            (best_priority < 100 ? best_priority : 0) * 1000 - best_priority * 10 + (best_in_same_dir ? 5 : 0);
        
        if (score > best_score) {
            best_priority = priority;
            best_match = tex;
            best_in_same_dir = in_same_dir;
        }
    }
    
    return best_match;
}

void ModelViewer::clear() {
    // Clear mesh data
    current_mesh_.reset();
    renderer_->set_mesh(nullptr);  // shared_ptr<const XobMesh> accepts nullptr
    destroy_textures();
    
    // Reset state
    model_loaded_ = false;
    loading_ = false;
    error_message_.clear();
    model_name_.clear();
    vertex_count_ = 0;
    face_count_ = 0;
    lod_count_ = 0;
    
    // Reset material editor state
    selected_material_idx_ = -1;
    highlighted_material_idx_ = -1;
    
    // Reset bounds
    bounds_min_ = glm::vec3(0.0f);
    bounds_max_ = glm::vec3(0.0f);
    
    // Reset camera to default
    camera_->set_distance(5.0f);
    camera_->set_angles(45.0f, 30.0f);
    camera_->set_target(glm::vec3(0.0f));
}

void ModelViewer::load_model_data(const std::vector<uint8_t>& data, const std::string& name) {
    // ALWAYS clear previous model first
    clear();
    
    // Reset lazy load counter for new model
    auto& pak_mgr = PakManager::instance();
    pak_mgr.reset_lazy_load_counter();
    
    model_name_ = name;
    loading_ = true;
    load_cancelled_ = false;
    load_progress_ = 0.0f;
    error_message_.clear();

    if (data.empty()) {
        error_message_ = "Empty data";
        loading_ = false;
        return;
    }

    // Copy data for async processing (data may go out of scope)
    auto data_copy = std::make_shared<std::vector<uint8_t>>(data);
    
    // Parse mesh asynchronously to keep UI responsive
    load_future_ = std::async(std::launch::async, [this, data_copy]() -> std::shared_ptr<XobMesh> {
        try {
            load_progress_ = 0.1f;
            
            XobParser parser(std::span<const uint8_t>(data_copy->data(), data_copy->size()));
            
            if (load_cancelled_) return nullptr;
            load_progress_ = 0.3f;
            
            auto mesh = parser.parse(0);
            
            if (load_cancelled_) return nullptr;
            load_progress_ = 0.7f;
            
            if (!mesh) {
                return nullptr;
            }
            
            // Validate mesh data
            if (mesh->vertices.empty() || mesh->lods.empty() || mesh->lods[0].indices.empty()) {
                return nullptr;
            }
            
            load_progress_ = 0.9f;
            
            return std::make_shared<XobMesh>(std::move(*mesh));
            
        } catch (const std::exception&) {
            return nullptr;
        }
    });
}

void ModelViewer::load_model(const std::filesystem::path& path) {
    current_path_ = path;
    auto data = read_file(path);
    load_model_data(data, path.filename().string());
}

void ModelViewer::calculate_bounds() {
    if (!current_mesh_ || current_mesh_->vertices.empty()) {
        bounds_min_ = glm::vec3(0.0f);
        bounds_max_ = glm::vec3(0.0f);
        return;
    }

    bounds_min_ = glm::vec3(FLT_MAX);
    bounds_max_ = glm::vec3(-FLT_MAX);

    for (const auto& v : current_mesh_->vertices) {
        bounds_min_.x = (std::min)(bounds_min_.x, v.position.x);
        bounds_min_.y = (std::min)(bounds_min_.y, v.position.y);
        bounds_min_.z = (std::min)(bounds_min_.z, v.position.z);
        bounds_max_.x = (std::max)(bounds_max_.x, v.position.x);
        bounds_max_.y = (std::max)(bounds_max_.y, v.position.y);
        bounds_max_.z = (std::max)(bounds_max_.z, v.position.z);
    }
}

void ModelViewer::reset_camera() {
    camera_->reset();
    if (current_mesh_) {
        glm::vec3 center = (bounds_min_ + bounds_max_) * 0.5f;
        camera_->set_target(center);
        float size = glm::length(bounds_max_ - bounds_min_);
        if (size < 0.001f) size = 1.0f;
        camera_->set_distance(size * 1.5f);
    }
}

void ModelViewer::set_view(float yaw, float pitch) {
    camera_->set_angles(yaw, pitch);
}

void ModelViewer::ensure_framebuffer(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (fbo_ != 0 && fb_width_ == width && fb_height_ == height) return;

    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteTextures(1, &fb_texture_);
        glDeleteRenderbuffers(1, &fb_depth_);
    }

    fb_width_ = width;
    fb_height_ = height;

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    glGenTextures(1, &fb_texture_);
    glBindTexture(GL_TEXTURE_2D, fb_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_texture_, 0);

    glGenRenderbuffers(1, &fb_depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, fb_depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb_depth_);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ModelViewer::render() {
    // Check if async loading has completed
    if (loading_ && load_future_.valid()) {
        if (load_future_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                auto mesh = load_future_.get();
                if (mesh) {
                    current_mesh_ = mesh;
                    renderer_->set_mesh(current_mesh_);
                    
                    vertex_count_ = current_mesh_->vertices.size();
                    face_count_ = current_mesh_->lods[0].indices.size() / 3;
                    lod_count_ = static_cast<int>(current_mesh_->lods.size());
                    if (lod_count_ == 0) lod_count_ = 1;
                    
                    calculate_bounds();
                    load_material_textures();
                    
                    glm::vec3 center = (bounds_min_ + bounds_max_) * 0.5f;
                    camera_->set_target(center);
                    float model_size = glm::length(bounds_max_ - bounds_min_);
                    camera_->set_distance(model_size * 1.5f);
                    
                    model_loaded_ = true;
                } else {
                    error_message_ = "Failed to parse XOB file";
                }
            } catch (const std::exception& e) {
                error_message_ = std::string("Load error: ") + e.what();
            }
            loading_ = false;
            load_progress_ = 0.0f;
        }
    }
    
    // Render toolbar
    render_toolbar();
    ImGui::Separator();
    
    // Render texture browser window if open
    if (show_texture_browser_) {
        render_texture_browser();
    }
    
    // Render material editor window if open
    if (show_material_editor_ && model_loaded_ && current_mesh_) {
        render_material_editor();
    }

    auto content_region = ImGui::GetContentRegionAvail();
    int view_width = static_cast<int>(content_region.x);
    int view_height = static_cast<int>(content_region.y - 30);

    if (loading_) {
        float progress = load_progress_.load();
        ImGui::Text("Loading model...");
        ImGui::ProgressBar(progress, ImVec2(-1, 0), 
            progress > 0 ? nullptr : "Parsing...");
        if (ImGui::Button("Cancel")) {
            load_cancelled_ = true;
        }
        return;
    }

    if (!error_message_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_message_.c_str());
        return;
    }

    if (!model_loaded_ || !current_mesh_) {
        ImGui::TextDisabled("No model loaded.");
        ImGui::TextDisabled("Select a .xob file to view.");
        return;
    }

    if (view_width > 0 && view_height > 0) {
        ensure_framebuffer(view_width, view_height);

        if (fbo_ != 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
            glViewport(0, 0, view_width, view_height);
            glClearColor(bg_color_.r, bg_color_.g, bg_color_.b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);

            renderer_->set_wireframe(show_wireframe_);
            renderer_->set_show_grid(show_grid_);
            renderer_->set_debug_material_colors(show_material_debug_);

            camera_->set_aspect(static_cast<float>(view_width) / static_cast<float>(view_height));
            glm::mat4 view = camera_->view_matrix();
            glm::mat4 projection = camera_->projection_matrix();
            renderer_->render(view, projection);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        if (fb_texture_ != 0) {
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(fb_texture_)),
                         ImVec2(static_cast<float>(view_width), static_cast<float>(view_height)),
                         ImVec2(0, 1), ImVec2(1, 0), ImVec4(1, 1, 1, 1), ImVec4(0, 0, 0, 0));

            if (ImGui::IsItemHovered()) {
                auto& io = ImGui::GetIO();
                if (io.MouseWheel != 0) {
                    camera_->zoom(io.MouseWheel * 0.5f);
                }
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    camera_->orbit(io.MouseDelta.x * 0.5f, io.MouseDelta.y * 0.5f);
                }
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                    camera_->pan(io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
                }
            }
        }
    }

    render_info_bar();
}

void ModelViewer::render_toolbar() {
    if (ImGui::Button("Reset View")) reset_camera();
    ImGui::SameLine();
    if (ImGui::Button("Front")) set_view(0, 0);
    ImGui::SameLine();
    if (ImGui::Button("Back")) set_view(180, 0);
    ImGui::SameLine();
    if (ImGui::Button("Left")) set_view(-90, 0);
    ImGui::SameLine();
    if (ImGui::Button("Right")) set_view(90, 0);
    ImGui::SameLine();
    if (ImGui::Button("Top")) set_view(0, 90);
    ImGui::SameLine();
    if (ImGui::Button("Bottom")) set_view(0, -90);
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &show_wireframe_);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &show_grid_);
    ImGui::SameLine();
    ImGui::Checkbox("Mat Debug", &show_material_debug_);
    ImGui::SameLine();
    ImGui::ColorEdit3("BG", &bg_color_.x, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    if (ImGui::Button("Materials")) {
        show_material_editor_ = !show_material_editor_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Textures")) {
        show_texture_browser_ = true;
    }
}

void ModelViewer::render_info_bar() {
    if (model_loaded_) {
        ImGui::Text("Vertices: %zu | Faces: %zu | LODs: %d | File: %s",
                    vertex_count_, face_count_, lod_count_,
                    model_name_.c_str());
    }
}

void ModelViewer::load_material_textures() {
    if (!current_mesh_ || current_mesh_->materials.empty()) {
        LOG_DEBUG("ModelViewer", "load_material_textures: No mesh or no materials");
        return;
    }
    
    if (!texture_loader_) {
        LOG_DEBUG("ModelViewer", "load_material_textures: No texture loader set");
        return;
    }
    
    auto& pak_mgr = PakManager::instance();
    
    LOG_INFO("ModelViewer", "========== MATERIAL TEXTURE LOADING ==========");
    LOG_INFO("ModelViewer", "Total materials: " << current_mesh_->materials.size());
    LOG_INFO("ModelViewer", "Total material ranges: " << current_mesh_->material_ranges.size());
    LOG_INFO("ModelViewer", "Available textures in PAK: " << available_textures_.size());
    LOG_INFO("ModelViewer", "Texture cache size: " << texture_cache_.size());
    
    // Dump all materials for analysis
    LOG_DEBUG("ModelViewer", "--- ALL MATERIALS ---");
    for (size_t i = 0; i < current_mesh_->materials.size(); i++) {
        const auto& m = current_mesh_->materials[i];
        LOG_DEBUG("ModelViewer", "  [" << i << "] name=\"" << m.name << "\" path=\"" << m.path << "\"");
    }
    
    // Dump all material ranges for analysis
    LOG_DEBUG("ModelViewer", "--- ALL MATERIAL RANGES ---");
    uint32_t total_range_tris = 0;
    for (size_t i = 0; i < current_mesh_->material_ranges.size(); i++) {
        const auto& r = current_mesh_->material_ranges[i];
        LOG_DEBUG("ModelViewer", "  Range[" << i << "] mat_idx=" << r.material_index 
                  << " tri_start=" << r.triangle_start << " tri_end=" << r.triangle_end 
                  << " tri_count=" << r.triangle_count);
        total_range_tris += r.triangle_count;
    }
    uint32_t total_tris = static_cast<uint32_t>(current_mesh_->indices.size() / 3);
    LOG_INFO("ModelViewer", "Material ranges cover " << total_range_tris << " of " << total_tris << " triangles (" 
              << (total_tris > 0 ? (100 * total_range_tris / total_tris) : 0) << "%)");
    
    int textures_loaded = 0;
    int textures_from_cache = 0;
    int textures_skipped = 0;
    
    // Helper lambda to search index for a texture
    auto search_index_for_texture = [&](const std::string& texture_path) -> std::vector<uint8_t> {
        if (!pak_mgr.is_index_ready()) return {};
        
        // Try to load the PAK containing this texture
        if (pak_mgr.try_load_pak_for_file(texture_path)) {
            auto data = pak_mgr.read_file(texture_path);
            if (!data.empty()) return data;
        }
        return {};
    };
    
    LOG_DEBUG("ModelViewer", "--- LOADING TEXTURES ---");
    
    // For each material, find the best texture
    for (size_t mat_idx = 0; mat_idx < current_mesh_->materials.size(); mat_idx++) {
        auto& mat = current_mesh_->materials[mat_idx];
        bool texture_found = false;
        
        LOG_DEBUG("ModelViewer", "");
        LOG_DEBUG("ModelViewer", "=== Material " << mat_idx << ": " << mat.name << " ===");
        LOG_DEBUG("ModelViewer", "  Path: " << mat.path);
        
        // Skip .gamemat files - these are physics/game materials without textures
        if (mat.path.find(".gamemat") != std::string::npos) {
            LOG_DEBUG("ModelViewer", "  -> SKIP: gamemat (physics material, no texture)");
            textures_skipped++;
            continue;
        }
        
        // Skip known procedural/color-only materials
        std::string mat_lower = mat.name;
        std::transform(mat_lower.begin(), mat_lower.end(), mat_lower.begin(), ::tolower);
        if (mat_lower == "chrome" || mat_lower == "black_matte" || mat_lower == "mirror_generic" ||
            mat_lower.find("_generic") != std::string::npos) {
            LOG_DEBUG("ModelViewer", "  -> SKIP: procedural material");
            textures_skipped++;
            continue;
        }
        
        // Get material directory for locality matching
        // Strip GUID prefix from material path for file loading
        std::string mat_path_clean = strip_guid_prefix(mat.path);
        std::string mat_dir;
        if (!mat_path_clean.empty()) {
            size_t dir_slash = mat_path_clean.rfind('/');
            if (dir_slash != std::string::npos) {
                mat_dir = mat_path_clean.substr(0, dir_slash);
            }
        }
        
        // =========================================================================
        // PRIORITY 1: Load .emat file and get EXACT texture path from it
        // =========================================================================
        std::string emat_texture_path;
        if (!mat_path_clean.empty()) {
            LOG_DEBUG("ModelViewer", "  PRIORITY 1: Loading emat file: " << mat_path_clean);
            
            // Try local PAK first
            auto emat_data = texture_loader_ ? texture_loader_(mat_path_clean) : std::vector<uint8_t>{};
            if (emat_data.empty()) {
                LOG_DEBUG("ModelViewer", "    Emat not in local PAK, trying loaded PAKs");
                emat_data = pak_mgr.read_file(mat_path_clean);
            }
            
            // Try index for emat (searches ALL PAKs)
            if (emat_data.empty() && pak_mgr.is_index_ready()) {
                LOG_DEBUG("ModelViewer", "    Emat not in loaded PAKs, searching ALL PAKs");
                pak_mgr.try_load_pak_for_file(mat_path_clean);
                emat_data = pak_mgr.read_file(mat_path_clean);
            }
            
            if (!emat_data.empty()) {
                auto mat_info = parse_emat_material(emat_data, mat.name);
                
                // Store the base color from the emat
                if (mat_info.has_color) {
                    material_base_colors_[mat_idx] = glm::vec3(
                        mat_info.base_color[0],
                        mat_info.base_color[1],
                        mat_info.base_color[2]
                    );
                    renderer_->set_material_color(mat_idx, material_base_colors_[mat_idx]);
                    LOG_INFO("ModelViewer", "Set material " << mat_idx << " base color: " 
                             << mat_info.base_color[0] << ", " << mat_info.base_color[1] << ", " << mat_info.base_color[2]);
                }
                
                if (mat_info.textures.count("Diffuse")) {
                    emat_texture_path = mat_info.textures["Diffuse"];
                    LOG_INFO("ModelViewer", "Emat " << mat.name << " specifies diffuse: " << emat_texture_path);
                    
                    // Detect if this is actually an MCR texture (by filename)
                    std::string path_lower = emat_texture_path;
                    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
                    bool is_mcr = path_lower.find("_mcr") != std::string::npos;
                    
                    // Check cache first
                    uint32_t cached_tex = get_cached_texture(emat_texture_path);
                    if (cached_tex != 0) {
                        material_diffuse_textures_[mat_idx] = cached_tex;
                        renderer_->set_material_texture(mat_idx, cached_tex, is_mcr);
                        textures_loaded++;
                        textures_from_cache++;
                        LOG_DEBUG("ModelViewer", "Using cached emat texture: " << emat_texture_path << " (MCR=" << is_mcr << ")");
                        continue;
                    }
                    
                    // Try to load the exact texture from emat
                    auto tex_data = texture_loader_ ? texture_loader_(emat_texture_path) : std::vector<uint8_t>{};
                    if (tex_data.empty()) tex_data = pak_mgr.read_file(emat_texture_path);
                    
                    // Try index for texture (searches ALL PAKs)
                    if (tex_data.empty()) {
                        LOG_DEBUG("ModelViewer", "Searching ALL PAKs for emat texture: " << emat_texture_path);
                        tex_data = search_index_for_texture(emat_texture_path);
                    }
                    
                    if (!tex_data.empty()) {
                        texture_found = try_load_texture_data(mat_idx, tex_data, emat_texture_path);
                        if (texture_found) {
                            textures_loaded++;
                            LOG_INFO("ModelViewer", "Loaded from emat path: " << emat_texture_path);
                            continue;
                        }
                    } else {
                        LOG_WARN("ModelViewer", "Could not find emat texture: " << emat_texture_path);
                    }
                } else {
                    LOG_DEBUG("ModelViewer", "Emat has no Diffuse (BCR) texture");
                    
                    // For MCR-only materials: load MCR texture + use base color
                    // MCR provides metallic/cavity/roughness, base color provides surface color
                    if (mat_info.textures.count("MCR")) {
                        std::string mcr_path = mat_info.textures["MCR"];
                        LOG_INFO("ModelViewer", "Loading MCR texture for PBR details: " << mcr_path);
                        
                        auto mcr_data = texture_loader_ ? texture_loader_(mcr_path) : std::vector<uint8_t>{};
                        if (mcr_data.empty()) mcr_data = pak_mgr.read_file(mcr_path);
                        if (mcr_data.empty()) mcr_data = search_index_for_texture(mcr_path);
                        
                        if (!mcr_data.empty()) {
                            texture_found = try_load_texture_data(mat_idx, mcr_data, mcr_path);
                            if (texture_found) {
                                textures_loaded++;
                                LOG_INFO("ModelViewer", "Loaded MCR texture: " << mcr_path);
                            }
                        }
                    }
                    
                    // If we have base color, that's sufficient even without texture
                    if (!texture_found && mat_info.has_color) {
                        texture_found = true;  // Don't try other methods, use the solid color
                        LOG_INFO("ModelViewer", "Using solid base color for material (no texture)");
                    }
                }
            } else {
                LOG_DEBUG("ModelViewer", "Could not load emat: " << mat_path_clean);
            }
        }
        
        // =========================================================================
        // PRIORITY 2: Local PAK fuzzy matching (only if emat didn't work)
        // This is a fallback when emat can't be found or doesn't specify textures
        // =========================================================================
        if (!texture_found) {
            std::string local_match = find_best_texture_match(mat.name, mat_dir);
            if (!local_match.empty()) {
                LOG_DEBUG("ModelViewer", "PRIORITY 2: Local fuzzy match: " << local_match);
                
                // Detect MCR from filename
                std::string match_lower = local_match;
                std::transform(match_lower.begin(), match_lower.end(), match_lower.begin(), ::tolower);
                bool is_mcr_match = match_lower.find("_mcr") != std::string::npos;
                
                // Check texture cache first
                uint32_t cached_tex = get_cached_texture(local_match);
                if (cached_tex != 0) {
                    material_diffuse_textures_[mat_idx] = cached_tex;
                    renderer_->set_material_texture(mat_idx, cached_tex, is_mcr_match);
                    textures_loaded++;
                    textures_from_cache++;
                    LOG_DEBUG("ModelViewer", "Using cached texture: " << local_match << " (MCR=" << is_mcr_match << ")");
                    continue;
                }
                
                // Load texture data
                if (texture_loader_) {
                    auto tex_data = texture_loader_(local_match);
                    if (!tex_data.empty()) {
                        texture_found = try_load_texture_data(mat_idx, tex_data, local_match);
                        if (texture_found) {
                            textures_loaded++;
                            LOG_DEBUG("ModelViewer", "Loaded from local PAK: " << local_match);
                            continue;
                        }
                    }
                }
            }
        }
        
        // PRIORITY 3: Search index for texture by material name patterns
        if (!texture_found) {
            // Diffuse texture suffixes in priority order
            static const std::vector<std::string> diffuse_suffixes = {
                "_bcr", "_mcr", "_co", "_diffuse", "_diff", "_d", "_albedo", "_color", "_basecolor", ""
            };
            
            std::string mat_base = mat.name;
            size_t ext_pos = mat_base.rfind('.');
            if (ext_pos != std::string::npos) mat_base = mat_base.substr(0, ext_pos);
            
            // Get directory from material path (use already cleaned path)
            // mat_dir was already computed from mat_path_clean above
            
            // Build search paths
            std::vector<std::string> search_paths;
            for (const auto& suffix : diffuse_suffixes) {
                if (!mat_dir.empty()) {
                    search_paths.push_back(mat_dir + "/Textures/" + mat_base + suffix + ".edds");
                    search_paths.push_back(mat_dir + "/" + mat_base + suffix + ".edds");
                }
                // Generic asset paths
                search_paths.push_back("Assets/Textures/" + mat_base + suffix + ".edds");
            }
            
            for (const auto& search_path : search_paths) {
                // Try current PAK
                auto tex_data = texture_loader_ ? texture_loader_(search_path) : std::vector<uint8_t>{};
                if (tex_data.empty()) tex_data = pak_mgr.read_file(search_path);
                
                // Try index (all PAKs)
                if (tex_data.empty()) {
                    tex_data = search_index_for_texture(search_path);
                }
                
                if (!tex_data.empty()) {
                    texture_found = try_load_texture_data(mat_idx, tex_data, search_path);
                    if (texture_found) {
                        textures_loaded++;
                        LOG_DEBUG("ModelViewer", "Loaded from search: " << search_path);
                        break;
                    }
                }
            }
        }
        
        // PRIORITY 4: Search ENTIRE INDEX by material base name (searches ALL 400+ PAKs!)
        if (!texture_found) {
            std::string mat_base = mat.name;
            size_t ext_pos = mat_base.rfind('.');
            if (ext_pos != std::string::npos) mat_base = mat_base.substr(0, ext_pos);
            
            // Use the new PakIndex search that searches ALL indexed PAKs
            auto& index = PakIndex::instance();
            if (index.is_ready()) {
                auto matches = index.search_textures_for_material(mat_base);
                if (!matches.empty()) {
                    LOG_DEBUG("ModelViewer", "Global index found " << matches.size() 
                              << " textures for " << mat_base << " in " << matches[0].pak_path.filename().string());
                    
                    // Try each match in priority order
                    for (const auto& match : matches) {
                        // Detect MCR from filename
                        std::string match_path_lower = match.file_path;
                        std::transform(match_path_lower.begin(), match_path_lower.end(), match_path_lower.begin(), ::tolower);
                        bool is_mcr_match = match_path_lower.find("_mcr") != std::string::npos;
                        
                        // Check cache first
                        uint32_t cached_tex = get_cached_texture(match.file_path);
                        if (cached_tex != 0) {
                            material_diffuse_textures_[mat_idx] = cached_tex;
                            renderer_->set_material_texture(mat_idx, cached_tex, is_mcr_match);
                            textures_loaded++;
                            textures_from_cache++;
                            texture_found = true;
                            LOG_DEBUG("ModelViewer", "Using cached texture: " << match.file_path << " (MCR=" << is_mcr_match << ")");
                            break;
                        }
                        
                        // Load the PAK if needed and get texture data
                        if (pak_mgr.try_load_pak_for_file(match.file_path)) {
                            auto tex_data = pak_mgr.read_file(match.file_path);
                            if (!tex_data.empty()) {
                                texture_found = try_load_texture_data(mat_idx, tex_data, match.file_path);
                                if (texture_found) {
                                    textures_loaded++;
                                    LOG_INFO("ModelViewer", "Loaded from global index: " << match.file_path 
                                              << " (from " << match.pak_path.filename().string() << ")");
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        if (!texture_found) {
            LOG_WARN("ModelViewer", "No texture found for material " << mat_idx << ": " << mat.name 
                     << " (path: " << mat.path << ")");
        }
    }
    
    LOG_INFO("ModelViewer", "Texture loading complete: " << textures_loaded << " loaded, " 
              << textures_from_cache << " from cache, " << textures_skipped << " skipped, " 
              << (current_mesh_->materials.size() - textures_loaded - textures_skipped) << " failed");
}

void ModelViewer::filter_textures() {
    filtered_textures_.clear();
    std::string filter_lower = texture_filter_;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
    
    for (const auto& tex : available_textures_) {
        if (filter_lower.empty()) {
            filtered_textures_.push_back(tex);
        } else {
            std::string tex_lower = tex;
            std::transform(tex_lower.begin(), tex_lower.end(), tex_lower.begin(), ::tolower);
            if (tex_lower.find(filter_lower) != std::string::npos) {
                filtered_textures_.push_back(tex);
            }
        }
    }
    
    // Reset selection if out of range
    if (selected_texture_idx_ >= static_cast<int>(filtered_textures_.size())) {
        selected_texture_idx_ = filtered_textures_.empty() ? -1 : 0;
    }
}

void ModelViewer::apply_texture(const std::string& path) {
    if (!texture_loader_) {
        std::cerr << "[ModelViewer] No texture loader\n";
        return;
    }
    
    // Destroy old texture
    destroy_textures();
    
    auto data = texture_loader_(path);
    if (data.empty()) {
        return;
    }
    
    // Convert EDDS to DDS
    EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
    auto dds_data = converter.convert();
    if (dds_data.empty()) {
        return;
    }
    
    // Load DDS
    auto texture = DdsLoader::load(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
    if (!texture || texture->pixels.empty()) {
        return;
    }
    
    // Create GL texture
    glGenTextures(1, &diffuse_texture_);
    glBindTexture(GL_TEXTURE_2D, diffuse_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 
                 0, GL_RGBA, GL_UNSIGNED_BYTE, texture->pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    renderer_->set_texture(diffuse_texture_);
    current_texture_path_ = path;
}

void ModelViewer::render_texture_browser() {
    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Texture Browser", &show_texture_browser_)) {
        // Filter input
        if (ImGui::InputText("Filter", texture_filter_, sizeof(texture_filter_))) {
            filter_textures();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            texture_filter_[0] = '\0';
            filter_textures();
        }
        
        ImGui::Text("%zu textures (showing %zu)", available_textures_.size(), filtered_textures_.size());
        
        // Current texture
        if (!current_texture_path_.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Current: %s", current_texture_path_.c_str());
        }
        
        ImGui::Separator();
        
        // Prev/Next buttons for cycling
        if (ImGui::Button("<< Prev")) {
            if (!filtered_textures_.empty()) {
                selected_texture_idx_--;
                if (selected_texture_idx_ < 0) selected_texture_idx_ = static_cast<int>(filtered_textures_.size()) - 1;
                apply_texture(filtered_textures_[selected_texture_idx_]);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >>")) {
            if (!filtered_textures_.empty()) {
                selected_texture_idx_++;
                if (selected_texture_idx_ >= static_cast<int>(filtered_textures_.size())) selected_texture_idx_ = 0;
                apply_texture(filtered_textures_[selected_texture_idx_]);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove Texture")) {
            destroy_textures();
            current_texture_path_.clear();
        }
        
        ImGui::Separator();
        
        // Texture list
        ImGui::BeginChild("TextureList", ImVec2(0, 0), true);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filtered_textures_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& tex = filtered_textures_[i];
                bool is_selected = (i == selected_texture_idx_);
                
                // Show just filename for cleaner display
                std::string display_name = tex;
                size_t last_slash = tex.rfind('/');
                if (last_slash != std::string::npos) {
                    display_name = tex.substr(last_slash + 1);
                }
                
                if (ImGui::Selectable(display_name.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected_texture_idx_ = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        apply_texture(tex);
                    }
                }
                
                // Tooltip with full path
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tex.c_str());
                }
            }
        }
        clipper.End();
        ImGui::EndChild();
    }
    ImGui::End();
}

void ModelViewer::apply_texture_to_material(size_t material_index, const std::string& path) {
    if (!texture_loader_) {
        std::cerr << "[ModelViewer] No texture loader\n";
        return;
    }
    
    std::cerr << "[ModelViewer] Applying texture to material " << material_index << ": " << path << "\n";
    
    auto data = texture_loader_(path);
    if (data.empty()) {
        std::cerr << "[ModelViewer] Failed to load texture data\n";
        return;
    }
    
    // Convert EDDS to DDS
    EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
    auto dds_data = converter.convert();
    if (dds_data.empty()) {
        std::cerr << "[ModelViewer] EDDS conversion failed\n";
        return;
    }
    
    // Load DDS
    auto texture = DdsLoader::load(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
    if (!texture || texture->pixels.empty()) {
        std::cerr << "[ModelViewer] DDS loading failed\n";
        return;
    }
    
    // Delete old texture if exists
    auto it = material_diffuse_textures_.find(material_index);
    if (it != material_diffuse_textures_.end() && it->second != 0) {
        glDeleteTextures(1, &it->second);
    }
    
    // Detect MCR from path
    std::string path_lower = path;
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
    bool is_mcr = path_lower.find("_mcr") != std::string::npos;
    
    // Create GL texture
    uint32_t tex_id;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 
                 0, GL_RGBA, GL_UNSIGNED_BYTE, texture->pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    material_diffuse_textures_[material_index] = tex_id;
    renderer_->set_material_texture(material_index, tex_id, is_mcr);
    
    std::cerr << "[ModelViewer] Material " << material_index << " texture applied: " 
              << texture->width << "x" << texture->height << " (MCR=" << is_mcr << ")\n";
}

bool ModelViewer::try_load_texture_data(size_t material_index, const std::vector<uint8_t>& data, const std::string& path) {
    if (data.empty()) return false;
    
    // Detect if this is an MCR texture (Metallic-Color-Roughness where G=albedo)
    std::string path_lower = path;
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
    bool is_mcr = path_lower.find("_mcr") != std::string::npos;
    
    // Check if this texture is already in cache
    uint32_t cached_tex = get_cached_texture(path);
    if (cached_tex != 0) {
        material_diffuse_textures_[material_index] = cached_tex;
        renderer_->set_material_texture(material_index, cached_tex, is_mcr);
        LOG_DEBUG("ModelViewer", "Material " << material_index << " using cached texture: " << path << " (MCR=" << is_mcr << ")");
        return true;
    }
    
    // Convert EDDS to DDS
    EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
    auto dds_data = converter.convert();
    if (dds_data.empty()) {
        LOG_WARN("ModelViewer", "Failed to convert EDDS: " << path);
        return false;
    }
    
    // Try GPU-compressed texture upload first (bypasses broken BC7 software decoder)
    auto gpu_texture = DdsLoader::load_for_gpu(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
    if (!gpu_texture || gpu_texture->pixels.empty()) {
        LOG_WARN("ModelViewer", "Failed to load DDS for GPU: " << path);
        return false;
    }
    
    // Create GL texture
    uint32_t tex_id;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    if (gpu_texture->is_compressed) {
        // Upload compressed texture directly - GPU will decompress
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, gpu_texture->gl_internal_format,
                               gpu_texture->width, gpu_texture->height, 0,
                               gpu_texture->compressed_size, gpu_texture->pixels.data());
        LOG_DEBUG("ModelViewer", "Uploaded compressed texture " << path 
                  << " format=" << gpu_texture->format << " GL=0x" << std::hex << gpu_texture->gl_internal_format);
    } else {
        // Uncompressed texture - upload as RGBA
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gpu_texture->width, gpu_texture->height, 
                     0, GL_RGBA, GL_UNSIGNED_BYTE, gpu_texture->pixels.data());
        LOG_DEBUG("ModelViewer", "Uploaded uncompressed texture " << path << " format=" << gpu_texture->format);
    }
    
    // Check for GL errors
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOG_WARN("ModelViewer", "GL error uploading texture " << path << ": 0x" << std::hex << err);
        glDeleteTextures(1, &tex_id);
        return false;
    }
    
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Add to cache
    add_texture_to_cache(path, tex_id, gpu_texture->width, gpu_texture->height);
    
    material_diffuse_textures_[material_index] = tex_id;
    renderer_->set_material_texture(material_index, tex_id, is_mcr);
    
    LOG_INFO("ModelViewer", "Material " << material_index << " texture loaded from " << path 
              << ": " << gpu_texture->width << "x" << gpu_texture->height 
              << " format=" << gpu_texture->format << " MCR=" << is_mcr);
    return true;
}

void ModelViewer::render_material_editor() {
    if (!current_mesh_ || !show_material_editor_) return;
    
    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Material Editor", &show_material_editor_)) {
        // Summary
        ImGui::Text("Materials: %zu", current_mesh_->materials.size());
        ImGui::Text("Material Ranges: %zu", current_mesh_->material_ranges.size());
        ImGui::Separator();
        
        // Material list
        ImGui::BeginChild("MaterialList", ImVec2(0, 200), true);
        
        for (size_t i = 0; i < current_mesh_->materials.size(); i++) {
            const auto& mat = current_mesh_->materials[i];
            
            ImGui::PushID(static_cast<int>(i));
            
            bool is_selected = (static_cast<int>(i) == selected_material_idx_);
            
            // Count triangles for this material
            uint32_t tri_count = 0;
            for (const auto& range : current_mesh_->material_ranges) {
                if (range.material_index == i) {
                    tri_count += range.triangle_count;
                }
            }
            
            // Material row
            char label[256];
            snprintf(label, sizeof(label), "[%zu] %s (%u tris)", i, mat.name.c_str(), tri_count);
            
            if (ImGui::Selectable(label, is_selected)) {
                selected_material_idx_ = static_cast<int>(i);
                // Highlight this material in the 3D view
                renderer_->set_highlight_material(static_cast<int>(i));
            }
            
            // Hover highlight
            if (ImGui::IsItemHovered()) {
                renderer_->set_highlight_material(static_cast<int>(i));
            }
            
            ImGui::PopID();
        }
        
        // Clear highlight when not hovering any material
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            renderer_->set_highlight_material(-1);
        }
        
        ImGui::EndChild();
        
        ImGui::Separator();
        
        // Selected material details
        if (selected_material_idx_ >= 0 && selected_material_idx_ < static_cast<int>(current_mesh_->materials.size())) {
            const auto& mat = current_mesh_->materials[selected_material_idx_];
            
            ImGui::Text("Selected: %s", mat.name.c_str());
            ImGui::TextWrapped("Path: %s", mat.path.c_str());
            
            // Check if has texture assigned
            auto it = material_diffuse_textures_.find(selected_material_idx_);
            if (it != material_diffuse_textures_.end() && it->second != 0) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Texture: Assigned");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Texture: None");
            }
            
            ImGui::Separator();
            
            // Texture assignment section
            ImGui::Text("Assign Texture:");
            
            // Show texture filter and list
            static char mat_tex_filter[256] = "";
            ImGui::InputText("##MatTexFilter", mat_tex_filter, sizeof(mat_tex_filter));
            
            ImGui::BeginChild("MatTexList", ImVec2(0, 120), true);
            
            std::string filter_lower = mat_tex_filter;
            std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
            
            for (const auto& tex : available_textures_) {
                std::string tex_lower = tex;
                std::transform(tex_lower.begin(), tex_lower.end(), tex_lower.begin(), ::tolower);
                
                if (!filter_lower.empty() && tex_lower.find(filter_lower) == std::string::npos) {
                    continue;
                }
                
                // Show just filename
                std::string display_name = tex;
                size_t last_slash = tex.rfind('/');
                if (last_slash != std::string::npos) {
                    display_name = tex.substr(last_slash + 1);
                }
                
                if (ImGui::Selectable(display_name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        apply_texture_to_material(selected_material_idx_, tex);
                    }
                }
                
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tex.c_str());
                }
            }
            
            ImGui::EndChild();
            
            if (ImGui::Button("Clear Texture")) {
                auto tex_it = material_diffuse_textures_.find(selected_material_idx_);
                if (tex_it != material_diffuse_textures_.end() && tex_it->second != 0) {
                    glDeleteTextures(1, &tex_it->second);
                    material_diffuse_textures_.erase(tex_it);
                    renderer_->set_material_texture(selected_material_idx_, 0);
                }
            }
        }
    }
    ImGui::End();
}

} // namespace enfusion
