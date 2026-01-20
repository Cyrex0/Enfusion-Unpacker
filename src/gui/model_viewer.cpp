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

namespace enfusion {


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
    
    // Delete per-material textures
    for (auto& [idx, tex_id] : material_diffuse_textures_) {
        if (tex_id != 0) {
            glDeleteTextures(1, &tex_id);
        }
    }
    material_diffuse_textures_.clear();
    
    renderer_->set_texture(0);
    renderer_->clear_material_textures();
}

void ModelViewer::clear() {
    // Clear mesh data
    current_mesh_.reset();
    renderer_->set_mesh(nullptr);
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
    
    model_name_ = name;
    loading_ = true;
    error_message_.clear();

    try {
        if (data.empty()) {
            error_message_ = "Empty data";
            loading_ = false;
            return;
        }

        XobParser parser(std::span<const uint8_t>(data.data(), data.size()));
        auto mesh = parser.parse(0);

        if (!mesh) {
            error_message_ = "Failed to parse XOB file";
            loading_ = false;
            return;
        }

        // Validate mesh data
        if (mesh->vertices.empty()) {
            error_message_ = "XOB file has no vertices";
            loading_ = false;
            return;
        }

        if (mesh->lods.empty() || mesh->lods[0].indices.empty()) {
            error_message_ = "XOB file has no indices";
            loading_ = false;
            return;
        }

        current_mesh_ = std::make_unique<XobMesh>(std::move(*mesh));
        renderer_->set_mesh(current_mesh_.get());

        vertex_count_ = current_mesh_->vertices.size();
        face_count_ = current_mesh_->lods[0].indices.size() / 3;
        lod_count_ = static_cast<int>(current_mesh_->lods.size());
        if (lod_count_ == 0) lod_count_ = 1;

        calculate_bounds();
        
        // Load material textures
        load_material_textures();

        // Center camera on model
        glm::vec3 center = (bounds_min_ + bounds_max_) * 0.5f;
        camera_->set_target(center);

        float model_size = glm::length(bounds_max_ - bounds_min_);
        if (model_size < 0.001f) model_size = 1.0f;
        camera_->set_distance(model_size * 1.5f);

        model_loaded_ = true;
        loading_ = false;

    } catch (const std::exception& e) {
        error_message_ = std::string("Error: ") + e.what();
        loading_ = false;
    }
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
        ImGui::Text("Loading model...");
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
        std::cerr << "[ModelViewer] No materials to load\n";
        return;
    }
    
    if (!texture_loader_) {
        std::cerr << "[ModelViewer] No texture loader set\n";
        return;
    }
    
    std::cerr << "[ModelViewer] Auto-loading textures for " << current_mesh_->materials.size() << " materials\n";
    std::cerr << "[ModelViewer] Available textures count: " << available_textures_.size() << "\n";
    std::cerr << "[ModelViewer] Model path: " << model_name_ << "\n";
    
    // Extract model directory for relative texture lookups
    std::string model_dir;
    size_t last_slash = model_name_.rfind('/');
    if (last_slash == std::string::npos) last_slash = model_name_.rfind('\\');
    if (last_slash != std::string::npos) {
        model_dir = model_name_.substr(0, last_slash);
    }
    std::cerr << "[ModelViewer] Model directory: " << model_dir << "\n";
    
    // Debug: print first 10 available textures
    int debug_count = 0;
    for (const auto& tex : available_textures_) {
        if (debug_count++ < 10) {
            std::cerr << "[ModelViewer]   Available: " << tex << "\n";
        }
    }
    
    // Build a map of texture base names to full paths for quick lookup
    // Key: lowercase base name (without path and extension), Value: full path
    std::map<std::string, std::vector<std::string>> texture_map;
    
    for (const auto& tex : available_textures_) {
        std::string tex_lower = tex;
        std::transform(tex_lower.begin(), tex_lower.end(), tex_lower.begin(), ::tolower);
        
        // Only consider .edds files
        if (tex_lower.find(".edds") == std::string::npos) continue;
        
        // Extract filename without path
        std::string filename = tex;
        size_t last_slash = tex.rfind('/');
        if (last_slash != std::string::npos) {
            filename = tex.substr(last_slash + 1);
        }
        
        // Store with lowercase filename as key
        std::string key = filename;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        texture_map[key].push_back(tex);
    }
    
    // For each material, find the best matching diffuse texture
    for (size_t mat_idx = 0; mat_idx < current_mesh_->materials.size(); mat_idx++) {
        const auto& mat = current_mesh_->materials[mat_idx];
        std::cerr << "[ModelViewer] Processing material " << mat_idx << ": " << mat.name << "\n";
        
        // Extract base name from material (remove extension)
        std::string mat_base = mat.name;
        size_t ext_pos = mat_base.rfind('.');
        if (ext_pos != std::string::npos) {
            mat_base = mat_base.substr(0, ext_pos);
        }
        std::string mat_base_lower = mat_base;
        std::transform(mat_base_lower.begin(), mat_base_lower.end(), mat_base_lower.begin(), ::tolower);
        
        // Diffuse texture suffixes in priority order
        // Enfusion: _BCR (Base Color Roughness), _MCR (Metal Color Roughness - also contains color!)
        std::vector<std::string> diffuse_suffixes = {
            "_bcr", "_mcr", "_co", "_diffuse", "_diff", "_d", "_albedo", "_color", "_basecolor", ""
        };
        
        // Suffixes that indicate NON-diffuse textures (skip these)
        std::vector<std::string> skip_suffixes = {
            "_global_mask", "_mask", "_nmo", "_normal", "_nm", "_n",
            "_smdi", "_specular", "_spec", "_ao", "_occlusion",
            "_roughness", "_metallic", "_height",
            "_emissive", "_opacity", "_alpha", "_vfx"
        };
        
        std::string best_match;
        int best_priority = 999;
        
        // Search all available textures
        for (const auto& [tex_key, tex_paths] : texture_map) {
            // Remove .edds for comparison
            std::string tex_base = tex_key;
            size_t edds_pos = tex_base.rfind(".edds");
            if (edds_pos != std::string::npos) {
                tex_base = tex_base.substr(0, edds_pos);
            }
            
            // Check if this texture's base name starts with or contains the material name
            bool name_matches = false;
            
            // Exact match: texture is materialname_suffix or just materialname
            if (tex_base.find(mat_base_lower) == 0) {
                name_matches = true;
            }
            // Also try if material name is contained in texture name
            else if (tex_base.find(mat_base_lower) != std::string::npos) {
                name_matches = true;
            }
            
            if (!name_matches) continue;
            
            // Check if this is a skip texture (normal map, mask, etc.)
            bool should_skip = false;
            for (const auto& skip : skip_suffixes) {
                if (tex_base.length() >= skip.length()) {
                    std::string ending = tex_base.substr(tex_base.length() - skip.length());
                    if (ending == skip) {
                        should_skip = true;
                        break;
                    }
                }
            }
            if (should_skip) continue;
            
            // Find the priority of this texture based on its suffix
            for (size_t i = 0; i < diffuse_suffixes.size(); i++) {
                const auto& suffix = diffuse_suffixes[i];
                bool matches = false;
                
                if (suffix.empty()) {
                    // Empty suffix means exact match (materialname.edds)
                    matches = (tex_base == mat_base_lower);
                } else {
                    // Check if texture ends with this suffix
                    if (tex_base.length() >= suffix.length()) {
                        std::string ending = tex_base.substr(tex_base.length() - suffix.length());
                        matches = (ending == suffix);
                    }
                }
                
                if (matches && static_cast<int>(i) < best_priority) {
                    best_priority = static_cast<int>(i);
                    best_match = tex_paths[0];  // Use first path if multiple
                    break;
                }
            }
        }
        
        // Apply the best match if found
        if (!best_match.empty()) {
            std::cerr << "[ModelViewer] Matched texture for material " << mat_idx 
                      << ": " << best_match << " (priority: " << best_priority << ")\n";
            apply_texture_to_material(mat_idx, best_match);
        } else {
            std::cerr << "[ModelViewer] No texture match in available_textures for: " << mat.name << "\n";
            
            // Fallback: try to load from PAK (current or via PakManager for cross-PAK)
            bool found = false;
            
            std::string path_base = mat.path.empty() ? mat.name : mat.path;
            size_t ext_pos2 = path_base.rfind('.');
            if (ext_pos2 != std::string::npos) {
                path_base = path_base.substr(0, ext_pos2);
            }
            
            // Get directory from path
            std::string dir_path;
            size_t last_slash = path_base.rfind('/');
            if (last_slash != std::string::npos) {
                dir_path = path_base.substr(0, last_slash);
                path_base = path_base.substr(last_slash + 1);
            }
            
            // Try various texture path patterns
            std::vector<std::string> paths_to_try = {
                dir_path + "/Textures/" + path_base + "_BCR.edds",
                dir_path + "/Textures/" + path_base + "_MCR.edds",
                dir_path + "/Textures/" + path_base + "_co.edds",
                dir_path + "/Textures/" + path_base + ".edds",
                dir_path + "/" + path_base + "_BCR.edds",
                dir_path + "/" + path_base + "_MCR.edds",
                dir_path + "/" + path_base + "_co.edds",
                dir_path + "/" + path_base + ".edds"
            };
            
            // First try with current PAK's texture_loader_
            if (texture_loader_) {
                for (const auto& path : paths_to_try) {
                    auto data = texture_loader_(path);
                    if (!data.empty()) {
                        found = try_load_texture_data(mat_idx, data, path);
                        if (found) break;
                    }
                }
            }
            
            // If not found, try PakManager for cross-PAK lookup
            if (!found) {
                auto& pak_mgr = PakManager::instance();
                for (const auto& path : paths_to_try) {
                    auto data = pak_mgr.read_file(path);
                    if (!data.empty()) {
                        std::cerr << "[ModelViewer] Found texture via PakManager: " << path << "\n";
                        found = try_load_texture_data(mat_idx, data, path);
                        if (found) break;
                    }
                }
            }
            
            if (!found) {
                std::cerr << "Failed to find texture for material: " << mat.name << "\n";
            }
        }
    }
    
    std::cerr << "[ModelViewer] Auto-texture loading complete. " 
              << material_diffuse_textures_.size() << " textures applied.\n";
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
    
    std::cerr << "[ModelViewer] Applying texture: " << path << "\n";
    
    // Destroy old texture
    destroy_textures();
    
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
    std::cerr << "[ModelViewer] Texture applied: " << texture->width << "x" << texture->height << "\n";
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
    
    // Convert EDDS to DDS
    EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
    auto dds_data = converter.convert();
    if (dds_data.empty()) return false;
    
    // Load DDS
    auto texture = DdsLoader::load(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
    if (!texture || texture->pixels.empty()) return false;
    
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
    
    std::cerr << "[ModelViewer] Material " << material_index << " texture applied from " << path 
              << ": " << texture->width << "x" << texture->height << "\n";
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
