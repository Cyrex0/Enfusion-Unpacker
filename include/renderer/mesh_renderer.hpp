/**
 * Enfusion Unpacker - Mesh Renderer
 */

#pragma once

#include "enfusion/types.hpp"
#include <memory>

namespace enfusion {

class Shader;

/**
 * OpenGL mesh renderer.
 */
class MeshRenderer {
public:
    MeshRenderer();
    ~MeshRenderer();

    void init();
    void cleanup();

    void set_mesh(const XobMesh* mesh);
    void render(const glm::mat4& view, const glm::mat4& projection);

    // Render option setters
    void set_wireframe(bool enable) { wireframe_ = enable; }
    void set_show_normals(bool enable) { show_normals_ = enable; }
    void set_show_grid(bool enable) { show_grid_ = enable; }
    void set_current_lod(int lod) { current_lod_ = lod; }

    // Render option getters
    bool wireframe() const { return wireframe_; }
    bool show_normals() const { return show_normals_; }
    bool show_grid() const { return show_grid_; }
    int current_lod() const { return current_lod_; }
    float grid_size() const { return grid_size_; }

private:
    void upload_mesh();
    void create_grid();
    void render_mesh(const glm::mat4& view, const glm::mat4& projection);
    void render_grid(const glm::mat4& view, const glm::mat4& projection);
    void render_normals(const glm::mat4& view, const glm::mat4& projection);

    // Mesh buffers
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;
    uint32_t ebo_ = 0;

    // Grid buffers
    uint32_t grid_vao_ = 0;
    uint32_t grid_vbo_ = 0;

    // Mesh data
    const XobMesh* mesh_ = nullptr;
    size_t vertex_count_ = 0;
    size_t index_count_ = 0;

    // Render options
    bool show_grid_ = true;
    bool show_normals_ = false;
    bool wireframe_ = false;
    float grid_size_ = 10.0f;
    int current_lod_ = 0;

    // Shaders
    std::unique_ptr<Shader> mesh_shader_;
    std::unique_ptr<Shader> grid_shader_;
};

} // namespace enfusion
