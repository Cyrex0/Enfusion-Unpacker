/**
 * Enfusion Unpacker - Mesh Renderer Implementation
 */

#include "renderer/mesh_renderer.hpp"
#include "renderer/shader.hpp"
#include <glad/glad.h>

namespace enfusion {

MeshRenderer::MeshRenderer() = default;

MeshRenderer::~MeshRenderer() {
    cleanup();
}

void MeshRenderer::init() {
    // Create shaders
    mesh_shader_ = std::make_unique<Shader>();
    mesh_shader_->load(Shader::MESH_VERTEX_SHADER, Shader::MESH_FRAGMENT_SHADER);
    
    grid_shader_ = std::make_unique<Shader>();
    grid_shader_->load(Shader::GRID_VERTEX_SHADER, Shader::GRID_FRAGMENT_SHADER);
    
    // Create grid VAO
    create_grid();
}

void MeshRenderer::cleanup() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
        glDeleteBuffers(1, &ebo_);
        vao_ = vbo_ = ebo_ = 0;
    }
    
    if (grid_vao_ != 0) {
        glDeleteVertexArrays(1, &grid_vao_);
        glDeleteBuffers(1, &grid_vbo_);
        grid_vao_ = grid_vbo_ = 0;
    }
}

void MeshRenderer::set_mesh(const XobMesh* mesh) {
    mesh_ = mesh;
    if (mesh_) {
        upload_mesh();
    }
}

void MeshRenderer::upload_mesh() {
    if (!mesh_) return;
    
    // Clean up old buffers
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
        glDeleteBuffers(1, &ebo_);
    }
    
    // Create VAO
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);
    
    glBindVertexArray(vao_);
    
    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, 
                 mesh_->vertices.size() * sizeof(XobVertex),
                 mesh_->vertices.data(),
                 GL_STATIC_DRAW);
    
    // Upload index data
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 mesh_->indices.size() * sizeof(uint32_t),
                 mesh_->indices.data(),
                 GL_STATIC_DRAW);
    
    // Set vertex attributes
    // Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(XobVertex),
                          reinterpret_cast<void*>(offsetof(XobVertex, position)));
    
    // Normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(XobVertex),
                          reinterpret_cast<void*>(offsetof(XobVertex, normal)));
    
    // UV
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(XobVertex),
                          reinterpret_cast<void*>(offsetof(XobVertex, uv)));
    
    glBindVertexArray(0);
    
    vertex_count_ = mesh_->vertices.size();
    index_count_ = mesh_->indices.size();
}

void MeshRenderer::create_grid() {
    // Fullscreen quad for infinite grid
    float vertices[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f
    };
    
    glGenVertexArrays(1, &grid_vao_);
    glGenBuffers(1, &grid_vbo_);
    
    glBindVertexArray(grid_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, grid_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    
    glBindVertexArray(0);
}

void MeshRenderer::render(const glm::mat4& view, const glm::mat4& projection) {
    // Render grid first (behind mesh)
    if (show_grid_) {
        render_grid(view, projection);
    }
    
    // Render mesh
    if (mesh_ && vao_ != 0) {
        render_mesh(view, projection);
    }
    
    // Render normals overlay
    if (show_normals_ && mesh_) {
        render_normals(view, projection);
    }
}

void MeshRenderer::render_mesh(const glm::mat4& view, const glm::mat4& projection) {
    mesh_shader_->use();
    mesh_shader_->set_mat4("model", glm::mat4(1.0f));
    mesh_shader_->set_mat4("view", view);
    mesh_shader_->set_mat4("projection", projection);
    mesh_shader_->set_vec3("lightDir", glm::normalize(glm::vec3(0.5f, -1.0f, 0.3f)));
    mesh_shader_->set_vec3("lightColor", glm::vec3(1.0f));
    mesh_shader_->set_vec3("objectColor", glm::vec3(0.8f, 0.8f, 0.85f));
    mesh_shader_->set_bool("useTexture", false);
    
    glBindVertexArray(vao_);
    
    if (wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    
    // Determine index range based on LOD
    size_t index_offset = 0;
    size_t index_count = index_count_;
    
    if (!mesh_->lods.empty() && current_lod_ < static_cast<int>(mesh_->lods.size())) {
        index_offset = mesh_->lods[current_lod_].index_offset;
        index_count = mesh_->lods[current_lod_].index_count;
    }
    
    glDrawElements(GL_TRIANGLES, 
                   static_cast<GLsizei>(index_count),
                   GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(index_offset * sizeof(uint32_t)));
    
    if (wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    
    glBindVertexArray(0);
}

void MeshRenderer::render_grid(const glm::mat4& view, const glm::mat4& projection) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    grid_shader_->use();
    grid_shader_->set_mat4("view", view);
    grid_shader_->set_mat4("projection", projection);
    grid_shader_->set_float("gridSize", grid_size_);
    grid_shader_->set_vec3("gridColor", glm::vec3(0.5f));
    
    glBindVertexArray(grid_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    
    glDisable(GL_BLEND);
}

void MeshRenderer::render_normals(const glm::mat4& view, const glm::mat4& projection) {
    // Would need separate normal visualization shader
    // For now, skip this
}

} // namespace enfusion
