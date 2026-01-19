/**
 * Enfusion Unpacker - 3D Model Viewer Implementation
 */

#include "gui/model_viewer.hpp"
#include "enfusion/xob_parser.hpp"
#include "enfusion/files.hpp"
#include "renderer/mesh_renderer.hpp"
#include "renderer/camera.hpp"

#include <imgui.h>
#include <glad/glad.h>
#include <cfloat>
#include <algorithm>

namespace enfusion {

ModelViewer::ModelViewer()
    : camera_(std::make_unique<Camera>())
    , renderer_(std::make_unique<MeshRenderer>()) {

    camera_->set_distance(5.0f);
    camera_->set_angles(45.0f, 30.0f);

    renderer_->init();
}

ModelViewer::~ModelViewer() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteTextures(1, &fb_texture_);
        glDeleteRenderbuffers(1, &fb_depth_);
    }
}

void ModelViewer::clear() {
    // Clear mesh data
    current_mesh_.reset();
    renderer_->set_mesh(nullptr);
    
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
}

void ModelViewer::render_info_bar() {
    if (model_loaded_) {
        ImGui::Text("Vertices: %zu | Faces: %zu | LODs: %d | File: %s",
                    vertex_count_, face_count_, lod_count_,
                    model_name_.c_str());
    }
}

} // namespace enfusion
