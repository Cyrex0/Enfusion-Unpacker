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
#include <map>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include <future>
#include <atomic>
#include <mutex>

namespace enfusion {

class AddonExtractor;

/**
 * Cached texture entry to avoid reloading same texture for multiple materials
 */
struct CachedTexture {
    uint32_t gl_texture_id = 0;  // OpenGL texture handle
    int width = 0;
    int height = 0;
    size_t ref_count = 0;        // Number of materials using this texture
};

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
    std::shared_ptr<XobMesh> current_mesh_;
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
    bool show_material_debug_ = false;  // Debug: show material colors instead of textures
    int current_lod_ = 0;
    glm::vec3 bg_color_{0.15f, 0.15f, 0.18f};

    // State
    bool model_loaded_ = false;
    bool loading_ = false;
    std::string error_message_;
    
    // Async loading
    std::future<std::shared_ptr<XobMesh>> load_future_;
    std::atomic<float> load_progress_{0.0f};
    std::atomic<bool> load_cancelled_{false};
    mutable std::mutex load_mutex_;

    // Framebuffer
    uint32_t fbo_ = 0;
    uint32_t fb_texture_ = 0;
    uint32_t fb_depth_ = 0;
    int fb_width_ = 0;
    int fb_height_ = 0;
    
    // Texture loading
    std::function<std::vector<uint8_t>(const std::string&)> texture_loader_;
    uint32_t diffuse_texture_ = 0;
    
    // Per-material textures (material_index -> texture_id)
    std::map<size_t, uint32_t> material_diffuse_textures_;
    
    // Per-material base colors (from emat Color_3 parameter)
    std::map<size_t, glm::vec3> material_base_colors_;
    
    // Texture cache: path (lowercase) -> cached texture info
    // Prevents loading same texture multiple times for different materials
    std::unordered_map<std::string, CachedTexture> texture_cache_;
    
    // Texture browser
    std::vector<std::string> available_textures_;
    std::vector<std::string> filtered_textures_;
    std::string current_texture_path_;
    char texture_filter_[256] = "";
    int selected_texture_idx_ = -1;
    bool show_texture_browser_ = false;
    
    // Material editor
    bool show_material_editor_ = true;
    int selected_material_idx_ = -1;
    int highlighted_material_idx_ = -1;
    
    void load_material_textures();
    void destroy_textures();
    void clear_texture_cache();  // Clean up texture cache
    void apply_texture(const std::string& path);
    void apply_texture_to_material(size_t material_index, const std::string& path);
    bool try_load_texture_data(size_t material_index, const std::vector<uint8_t>& data, const std::string& path);
    
    // Texture cache helpers
    uint32_t get_cached_texture(const std::string& path) const;
    void add_texture_to_cache(const std::string& path, uint32_t texture_id, int width, int height);
    
    void render_texture_browser();
    void render_material_editor();
    void filter_textures();
    
    // Improved texture matching - finds textures by fuzzy name matching
    std::string find_best_texture_match(const std::string& material_name, 
                                        const std::string& material_dir) const;
};

} // namespace enfusion
