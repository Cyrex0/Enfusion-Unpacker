/**
 * Enfusion Unpacker - 3D Model Viewer Implementation
 */

#include "gui/model_viewer.hpp"
#include "enfusion/xob_parser.hpp"
#include "enfusion/edds_converter.hpp"
#include "enfusion/dds_loader.hpp"
#include "enfusion/files.hpp"
#include "renderer/mesh_renderer.hpp"
#include "renderer/camera.hpp"

#include <imgui.h>
#include <glad/glad.h>
#include <cfloat>
#include <algorithm>
#include <iostream>

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
    renderer_->set_texture(0);
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
    
    // Try to load diffuse texture from first material
    const auto& mat = current_mesh_->materials[0];
    std::cerr << "[ModelViewer] Loading texture for material: " << mat.name << "\n";
    
    // The material path is usually an .emat file, we need to find the corresponding .edds
    // Try various patterns
    std::string tex_path = mat.diffuse_texture;
    
    // Convert .emat/.gamemat to potential .edds paths
    std::vector<std::string> paths_to_try;
    
    // Remove material extension and try common texture suffixes
    std::string base_path = tex_path;
    size_t ext_pos = base_path.rfind('.');
    if (ext_pos != std::string::npos) {
        base_path = base_path.substr(0, ext_pos);
    }
    
    // Try different texture naming conventions
    paths_to_try.push_back(base_path + "_co.edds");  // color/diffuse
    paths_to_try.push_back(base_path + "_diff.edds");
    paths_to_try.push_back(base_path + "_d.edds");
    paths_to_try.push_back(base_path + ".edds");
    
    // Also try in Textures folder with same name
    size_t last_slash = base_path.rfind('/');
    if (last_slash != std::string::npos) {
        std::string filename = base_path.substr(last_slash + 1);
        paths_to_try.push_back("Assets/Textures/" + filename + "_co.edds");
        paths_to_try.push_back("Assets/Textures/" + filename + ".edds");
    }
    
    for (const auto& path : paths_to_try) {
        std::cerr << "[ModelViewer] Trying texture: " << path << "\n";
        auto data = texture_loader_(path);
        if (!data.empty()) {
            std::cerr << "[ModelViewer] Found texture: " << path << " (" << data.size() << " bytes)\n";
            
            // Convert EDDS to DDS
            EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
            auto dds_data = converter.convert();
            if (dds_data.empty()) {
                std::cerr << "[ModelViewer] EDDS conversion failed\n";
                continue;
            }
            
            // Load DDS
            auto texture = DdsLoader::load(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
            if (!texture) {
                std::cerr << "[ModelViewer] DDS loading failed\n";
                continue;
            }
            
            if (texture->pixels.empty()) {
                std::cerr << "[ModelViewer] DDS decode failed - no pixels\n";
                continue;
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
            std::cerr << "[ModelViewer] Texture loaded successfully: " << texture->width 
                      << "x" << texture->height << "\n";
            return;
        }
    }
    
    std::cerr << "[ModelViewer] Could not find any texture\n";
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

} // namespace enfusion
