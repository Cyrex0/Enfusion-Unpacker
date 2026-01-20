/**
 * Enfusion Unpacker - 3D Model Viewer Implementation
 */

#include "gui/model_viewer.hpp"
#include "enfusion/xob_parser.hpp"
#include "enfusion/edds_converter.hpp"
#include "enfusion/dds_loader.hpp"
#include "enfusion/files.hpp"
#include "enfusion/pak_manager.hpp"
#include "renderer/mesh_renderer.hpp"
#include "renderer/camera.hpp"

#include <imgui.h>
#include <glad/glad.h>
#include <cfloat>
#include <algorithm>
#include <iostream>
#include <map>
#include <regex>

namespace enfusion {

/**
 * Parse an .emat file to extract texture paths.
 * Returns a map of texture type (e.g., "Diffuse", "Normal") to texture path.
 */
static std::map<std::string, std::string> parse_emat_textures(const std::vector<uint8_t>& data) {
    std::map<std::string, std::string> textures;
    int diffuse_priority = 999;  // Lower = better
    
    if (data.empty()) return textures;
    
    // Convert to string for searching
    std::string content(data.begin(), data.end());
    
    // Look for .edds paths in the emat file
    // Pattern: find paths ending in .edds
    size_t pos = 0;
    while ((pos = content.find(".edds", pos)) != std::string::npos) {
        // Search backwards for start of path
        size_t start = pos;
        while (start > 0 && content[start - 1] != '\0' && content[start - 1] != '"' && 
               content[start - 1] != ' ' && content[start - 1] != '\n' && content[start - 1] != '\r' &&
               static_cast<unsigned char>(content[start - 1]) >= 32) {
            start--;
        }
        
        std::string path = content.substr(start, pos + 5 - start);
        
        // Strip GUID prefix if present (format: {XXXXXXXXXXXXXXXX}path)
        // GUIDs are 16 hex chars inside curly braces
        if (path.length() > 18 && path[0] == '{') {
            size_t close_brace = path.find('}');
            if (close_brace != std::string::npos && close_brace == 17) {
                path = path.substr(18);  // Skip {16hexchars}
            }
        }
        
        // Skip empty paths
        if (path.empty()) {
            pos += 5;
            continue;
        }
        
        // Determine texture type based on suffix
        std::string path_lower = path;
        std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
        
        // Skip non-diffuse textures entirely
        if (path_lower.find("_global_mask") != std::string::npos ||
            path_lower.find("_mask") != std::string::npos ||
            path_lower.find("_vfx") != std::string::npos ||
            path_lower.find("_ao") != std::string::npos ||
            path_lower.find("_emissive") != std::string::npos ||
            path_lower.find("_opacity") != std::string::npos ||
            path_lower.find("_alpha") != std::string::npos) {
            pos += 5;
            continue;
        }
        
        // Normal maps
        if (path_lower.find("_nmo") != std::string::npos ||
            path_lower.find("_normal") != std::string::npos ||
            path_lower.find("_nm") != std::string::npos) {
            textures["Normal"] = path;
            pos += 5;
            continue;
        }
        
        // Specular maps
        if (path_lower.find("_smdi") != std::string::npos ||
            path_lower.find("_specular") != std::string::npos) {
            textures["Specular"] = path;
            pos += 5;
            continue;
        }
        
        // Diffuse textures with priority
        int priority = 999;
        if (path_lower.find("_bcr") != std::string::npos) priority = 0;
        else if (path_lower.find("_mcr") != std::string::npos) priority = 1;
        else if (path_lower.find("_co") != std::string::npos) priority = 2;
        else if (path_lower.find("_diffuse") != std::string::npos) priority = 3;
        else if (path_lower.find("_albedo") != std::string::npos) priority = 4;
        else if (path_lower.find("_color") != std::string::npos) priority = 5;
        else priority = 10;  // Unknown suffix, low priority
        
        if (priority < diffuse_priority) {
            diffuse_priority = priority;
            textures["Diffuse"] = path;
        }
        
        pos += 5;
    }
    
    return textures;
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
        
        // IMPROVED MATCHING: Check multiple patterns
        bool name_matches = false;
        
        // 1. Exact match (texture base == material base)
        if (tex_base_lower == mat_base_lower) {
            name_matches = true;
        }
        // 2. Texture starts with material name (e.g., mat="helmet" matches "helmet_bcr")
        else if (tex_base_lower.find(mat_base_lower) == 0) {
            name_matches = true;
        }
        // 3. Material starts with texture base (e.g., mat="helmet_mat" matches "helmet_bcr")
        else if (mat_base_lower.find(tex_base_lower) == 0 && tex_base_lower.length() >= 3) {
            name_matches = true;
        }
        // 4. Common prefix matching (both share significant prefix)
        else {
            size_t common_len = 0;
            size_t min_len = std::min(tex_base_lower.length(), mat_base_lower.length());
            while (common_len < min_len && tex_base_lower[common_len] == mat_base_lower[common_len]) {
                common_len++;
            }
            // Require at least 4 chars or 50% of material name to match
            if (common_len >= 4 || (mat_base_lower.length() > 0 && common_len >= mat_base_lower.length() / 2)) {
                name_matches = true;
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
        
        // Prefer textures in same directory (add bonus)
        bool is_better = false;
        if (priority < best_priority) {
            is_better = true;
        } else if (priority == best_priority && in_same_dir && !best_in_same_dir) {
            is_better = true;
        }
        
        if (is_better) {
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

            camera_->set_aspect(static_cast<float>(view_width) / static_cast<float>(view_height));
            glm::mat4 view = camera_->view_matrix();
            glm::mat4 projection = camera_->projection_matrix();
            renderer_->render(view, projection);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        if (fb_texture_ != 0) {
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(fb_texture_)),
                         ImVec2(static_cast<float>(view_width), static_cast<float>(view_height)),
                         ImVec2(0, 1), ImVec2(1, 0));

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
        return;
    }
    
    if (!texture_loader_) {
        return;
    }
    
    auto& pak_mgr = PakManager::instance();
    
    std::cerr << "[ModelViewer] Auto-loading textures for " << current_mesh_->materials.size() << " materials\n";
    std::cerr << "[ModelViewer] Available textures in PAK: " << available_textures_.size() << "\n";
    std::cerr << "[ModelViewer] Texture cache size: " << texture_cache_.size() << "\n";
    
    int textures_loaded = 0;
    int textures_from_cache = 0;
    
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
    
    // For each material, find the best texture
    for (size_t mat_idx = 0; mat_idx < current_mesh_->materials.size(); mat_idx++) {
        auto& mat = current_mesh_->materials[mat_idx];
        bool texture_found = false;
        
        std::cerr << "[ModelViewer] Processing material " << mat_idx << ": " << mat.name << "\n";
        
        // Get material directory for locality matching
        std::string mat_dir;
        if (!mat.path.empty()) {
            size_t dir_slash = mat.path.rfind('/');
            if (dir_slash != std::string::npos) {
                mat_dir = mat.path.substr(0, dir_slash);
            }
        }
        
        // PRIORITY 1: Use improved texture matching with fuzzy name matching
        std::string local_match = find_best_texture_match(mat.name, mat_dir);
        if (!local_match.empty()) {
            std::cerr << "[ModelViewer] Found local texture match: " << local_match << "\n";
            
            // Check texture cache first
            uint32_t cached_tex = get_cached_texture(local_match);
            if (cached_tex != 0) {
                material_diffuse_textures_[mat_idx] = cached_tex;
                renderer_->set_material_texture(mat_idx, cached_tex);
                textures_loaded++;
                textures_from_cache++;
                std::cerr << "[ModelViewer] Using cached texture: " << local_match << "\n";
                continue;
            }
            
            // Load texture data
            if (texture_loader_) {
                auto tex_data = texture_loader_(local_match);
                if (!tex_data.empty()) {
                    texture_found = try_load_texture_data(mat_idx, tex_data, local_match);
                    if (texture_found) {
                        textures_loaded++;
                        std::cerr << "[ModelViewer] Loaded from local PAK: " << local_match << "\n";
                        continue;
                    }
                }
            }
        }
        
        // PRIORITY 2: Try to load emat and get texture path from it
        std::string emat_texture_path;
        if (!mat.path.empty()) {
            std::cerr << "[ModelViewer] Loading emat: " << mat.path << "\n";
            
            // Try local PAK first
            auto emat_data = texture_loader_ ? texture_loader_(mat.path) : std::vector<uint8_t>{};
            if (emat_data.empty()) emat_data = pak_mgr.read_file(mat.path);
            
            // Try index for emat
            if (emat_data.empty() && pak_mgr.is_index_ready()) {
                pak_mgr.try_load_pak_for_file(mat.path);
                emat_data = pak_mgr.read_file(mat.path);
            }
            
            if (!emat_data.empty()) {
                auto tex_paths = parse_emat_textures(emat_data);
                if (tex_paths.count("Diffuse")) {
                    emat_texture_path = tex_paths["Diffuse"];
                    std::cerr << "[ModelViewer] Emat specifies diffuse: " << emat_texture_path << "\n";
                    
                    // Check if emat texture is "shared" (contains _SharedData or similar)
                    bool is_shared_texture = emat_texture_path.find("_SharedData") != std::string::npos ||
                                             emat_texture_path.find("_shared") != std::string::npos;
                    
                    // If shared texture, prefer local match if we have one
                    if (is_shared_texture && !local_match.empty()) {
                        std::cerr << "[ModelViewer] Emat uses shared texture, preferring local: " << local_match << "\n";
                        // Already tried local above, so skip emat texture
                    } else {
                        // Try to load emat texture from current PAK
                        auto tex_data = texture_loader_ ? texture_loader_(emat_texture_path) : std::vector<uint8_t>{};
                        if (tex_data.empty()) tex_data = pak_mgr.read_file(emat_texture_path);
                        
                        // Try index for texture (searches all PAKs including game PAKs)
                        if (tex_data.empty()) {
                            std::cerr << "[ModelViewer] Searching index for: " << emat_texture_path << "\n";
                            tex_data = search_index_for_texture(emat_texture_path);
                        }
                        
                        if (!tex_data.empty()) {
                            texture_found = try_load_texture_data(mat_idx, tex_data, emat_texture_path);
                            if (texture_found) {
                                textures_loaded++;
                                std::cerr << "[ModelViewer] Loaded from emat: " << emat_texture_path << "\n";
                                continue;
                            }
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
            
            // Get directory from material path
            std::string mat_dir;
            if (!mat.path.empty()) {
                size_t dir_slash = mat.path.rfind('/');
                if (dir_slash != std::string::npos) {
                    mat_dir = mat.path.substr(0, dir_slash);
                }
            }
            
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
                        std::cerr << "[ModelViewer] Loaded from search: " << search_path << "\n";
                        break;
                    }
                }
            }
        }
        
        // PRIORITY 4: Search by material base name in index
        if (!texture_found && pak_mgr.is_index_ready()) {
            std::string mat_base = mat.name;
            size_t ext_pos = mat_base.rfind('.');
            if (ext_pos != std::string::npos) mat_base = mat_base.substr(0, ext_pos);
            
            auto matches = pak_mgr.search_textures_by_material(mat_base);
            if (!matches.empty()) {
                std::cerr << "[ModelViewer] Index search found " << matches.size() << " matches for " << mat_base << "\n";
                const auto& best = matches[0];
                
                auto tex_data = pak_mgr.read_file(best.path);
                if (tex_data.empty()) tex_data = search_index_for_texture(best.path);
                
                if (!tex_data.empty()) {
                    texture_found = try_load_texture_data(mat_idx, tex_data, best.path);
                    if (texture_found) {
                        textures_loaded++;
                        std::cerr << "[ModelViewer] Loaded from index search: " << best.path << "\n";
                    }
                }
            }
        }
        
        if (!texture_found) {
            std::cerr << "[ModelViewer] No texture found for material " << mat_idx << ": " << mat.name << "\n";
        }
    }
    
    std::cerr << "[ModelViewer] Loaded " << textures_loaded << "/" 
              << current_mesh_->materials.size() << " material textures"
              << " (" << textures_from_cache << " from cache)\n";
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
    renderer_->set_material_texture(material_index, tex_id);
    
    std::cerr << "[ModelViewer] Material " << material_index << " texture applied: " 
              << texture->width << "x" << texture->height << "\n";
}

bool ModelViewer::try_load_texture_data(size_t material_index, const std::vector<uint8_t>& data, const std::string& path) {
    if (data.empty()) return false;
    
    // Check if this texture is already in cache
    uint32_t cached_tex = get_cached_texture(path);
    if (cached_tex != 0) {
        material_diffuse_textures_[material_index] = cached_tex;
        renderer_->set_material_texture(material_index, cached_tex);
        std::cerr << "[ModelViewer] Material " << material_index << " using cached texture: " << path << "\n";
        return true;
    }
    
    // Convert EDDS to DDS
    EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
    auto dds_data = converter.convert();
    if (dds_data.empty()) return false;
    
    // Load DDS
    auto texture = DdsLoader::load(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
    if (!texture || texture->pixels.empty()) return false;
    
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
    
    // Add to cache
    add_texture_to_cache(path, tex_id, texture->width, texture->height);
    
    material_diffuse_textures_[material_index] = tex_id;
    renderer_->set_material_texture(material_index, tex_id);
    
    std::cerr << "[ModelViewer] Material " << material_index << " texture loaded from " << path 
              << ": " << texture->width << "x" << texture->height << " (added to cache)\n";
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
