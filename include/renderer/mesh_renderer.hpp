/**
 * Enfusion Unpacker - Mesh Renderer
 */

#pragma once

#include "enfusion/types.hpp"
#include <memory>
#include <vector>
#include <map>

namespace enfusion {

class Shader;

/**
 * Per-material texture bindings.
 */
struct MaterialTextures {
    uint32_t diffuse = 0;   // OpenGL diffuse texture handle
    uint32_t normal = 0;    // OpenGL normal map handle
    bool enabled = true;    // Whether to render this material
};

/**
 * OpenGL mesh renderer with multi-material support.
 */
class MeshRenderer {
public:
    MeshRenderer();
    ~MeshRenderer();

    void init();
    void cleanup();

    void set_mesh(std::shared_ptr<const XobMesh> mesh);
    void set_texture(uint32_t texture_id) { fallback_texture_ = texture_id; }
    void set_material_texture(size_t material_index, uint32_t texture_id);
    void set_material_enabled(size_t material_index, bool enabled);
    void render(const glm::mat4& view, const glm::mat4& projection);

    // Render option setters
    void set_wireframe(bool enable) { wireframe_ = enable; }
    void set_show_normals(bool enable) { show_normals_ = enable; }
    void set_show_grid(bool enable) { show_grid_ = enable; }
    void set_current_lod(int lod) { current_lod_ = lod; }
    void set_highlight_material(int index) { highlighted_material_ = index; }

    // Render option getters
    bool wireframe() const { return wireframe_; }
    bool show_normals() const { return show_normals_; }
    bool show_grid() const { return show_grid_; }
    int current_lod() const { return current_lod_; }
    float grid_size() const { return grid_size_; }
    
    // Material access
    const std::map<size_t, MaterialTextures>& material_textures() const { return material_textures_; }
    void clear_material_textures();

private:
    void upload_mesh();
    void create_grid();
    void render_mesh(const glm::mat4& view, const glm::mat4& projection);
    void render_grid(const glm::mat4& view, const glm::mat4& projection);
    void render_normals(const glm::mat4& view, const glm::mat4& projection);
    
    // Frustum culling helper
    bool is_visible_in_frustum(const glm::mat4& mvp, const glm::vec3& min, const glm::vec3& max) const;

    // Mesh buffers
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;
    uint32_t ebo_ = 0;

    // Grid buffers
    uint32_t grid_vao_ = 0;
    uint32_t grid_vbo_ = 0;

    // Mesh data
    std::shared_ptr<const XobMesh> mesh_;
    size_t vertex_count_ = 0;
    size_t index_count_ = 0;
    
    // Bounding box (computed on mesh upload)
    glm::vec3 bounds_min_{0.0f};
    glm::vec3 bounds_max_{0.0f};

    // Render options
    bool show_grid_ = true;
    bool show_normals_ = false;
    bool wireframe_ = false;
    bool frustum_cull_ = true;  // Enable frustum culling by default
    float grid_size_ = 10.0f;
    int current_lod_ = 0;
    int highlighted_material_ = -1;  // -1 = none

    // Shaders
    std::unique_ptr<Shader> mesh_shader_;
    std::unique_ptr<Shader> grid_shader_;
    
    // Textures - per-material
    std::map<size_t, MaterialTextures> material_textures_;
    uint32_t fallback_texture_ = 0;  // Used when no per-material texture
};

} // namespace enfusion
