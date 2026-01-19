/**
 * Enfusion Unpacker - 3D Model Viewer Panel
 */

#pragma once

#include "enfusion/types.hpp"
#include "renderer/mesh_renderer.hpp"
#include "renderer/camera.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace enfusion {

class AddonExtractor;

/**
 * Panel for viewing XOB 3D models.
 */
class ModelViewer {
public:
    ModelViewer();
    ~ModelViewer();

    void render();

    /**
     * Load model from raw XOB data.
     */
    void load_model_data(const std::vector<uint8_t>& data, const std::string& name);

    /**
     * Load model from file path.
     */
    void load_model(const std::filesystem::path& path);

    /**
     * Clear current model.
     */
    void clear();

    bool has_mesh() const { return model_loaded_; }
    
    /**
     * Set texture loader callback - takes a relative path, returns texture data
     */
    void set_texture_loader(std::function<std::vector<uint8_t>(const std::string&)> loader) {
        texture_loader_ = loader;
    }
    
    /**
     * Set available texture list for texture browser
     */
    void set_available_textures(const std::vector<std::string>& textures) {
        available_textures_ = textures;
        filtered_textures_ = textures;
    }

private:
    void render_toolbar();
    void render_info_bar();
    void ensure_framebuffer(int width, int height);
    void reset_camera();
    void set_view(float yaw, float pitch);
    void calculate_bounds();

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<MeshRenderer> renderer_;
    std::unique_ptr<XobMesh> current_mesh_;
    std::filesystem::path current_path_;
    std::string model_name_;

    // Model info
    size_t vertex_count_ = 0;
    size_t face_count_ = 0;
    int lod_count_ = 1;

    // Bounding box
    glm::vec3 bounds_min_{0.0f};
    glm::vec3 bounds_max_{0.0f};

    // View settings
    bool show_wireframe_ = false;
    bool show_normals_ = false;
    bool show_grid_ = true;
    int current_lod_ = 0;
    glm::vec3 bg_color_{0.15f, 0.15f, 0.18f};

    // State
    bool model_loaded_ = false;
    bool loading_ = false;
    std::string error_message_;

    // Framebuffer
    uint32_t fbo_ = 0;
    uint32_t fb_texture_ = 0;
    uint32_t fb_depth_ = 0;
    int fb_width_ = 0;
    int fb_height_ = 0;
    
    // Texture loading
    std::function<std::vector<uint8_t>(const std::string&)> texture_loader_;
    uint32_t diffuse_texture_ = 0;
    
    // Texture browser
    std::vector<std::string> available_textures_;
    std::vector<std::string> filtered_textures_;
    std::string current_texture_path_;
    char texture_filter_[256] = "";
    int selected_texture_idx_ = -1;
    bool show_texture_browser_ = false;
    
    void load_material_textures();
    void destroy_textures();
    void apply_texture(const std::string& path);
    void render_texture_browser();
    void filter_textures();
};

} // namespace enfusion
