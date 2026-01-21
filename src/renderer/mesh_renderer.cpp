/**
 * Enfusion Unpacker - Mesh Renderer Implementation
 */

#include "renderer/mesh_renderer.hpp"
#include "renderer/shader.hpp"
#include "enfusion/logging.hpp"
#include <glad/glad.h>
#include <iostream>
#include <sstream>

namespace enfusion {

// RAII helper for OpenGL state management
class GLStateGuard {
public:
    GLStateGuard(GLenum cap, bool enable) : cap_(cap), was_enabled_(glIsEnabled(cap)) {
        if (enable && !was_enabled_) {
            glEnable(cap_);
        } else if (!enable && was_enabled_) {
            glDisable(cap_);
        }
    }
    ~GLStateGuard() {
        if (was_enabled_) {
            glEnable(cap_);
        } else {
            glDisable(cap_);
        }
    }
    GLStateGuard(const GLStateGuard&) = delete;
    GLStateGuard& operator=(const GLStateGuard&) = delete;
private:
    GLenum cap_;
    GLboolean was_enabled_;
};

// RAII helper for depth mask
class GLDepthMaskGuard {
public:
    explicit GLDepthMaskGuard(GLboolean new_value) {
        glGetBooleanv(GL_DEPTH_WRITEMASK, &old_value_);
        glDepthMask(new_value);
    }
    ~GLDepthMaskGuard() {
        glDepthMask(old_value_);
    }
    GLDepthMaskGuard(const GLDepthMaskGuard&) = delete;
    GLDepthMaskGuard& operator=(const GLDepthMaskGuard&) = delete;
private:
    GLboolean old_value_;
};

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

void MeshRenderer::set_mesh(std::shared_ptr<const XobMesh> mesh) {
    mesh_ = std::move(mesh);
    if (mesh_) {
        upload_mesh();
    }
}

void MeshRenderer::upload_mesh() {
    if (!mesh_) return;
    
    LOG_DEBUG("Renderer", "Uploading mesh: verts=" << mesh_->vertices.size() 
              << " indices=" << mesh_->indices.size());
    
    // Compute bounding box
    bounds_min_ = glm::vec3(FLT_MAX);
    bounds_max_ = glm::vec3(-FLT_MAX);
    for (const auto& v : mesh_->vertices) {
        bounds_min_ = glm::min(bounds_min_, v.position);
        bounds_max_ = glm::max(bounds_max_, v.position);
    }
    
    // Debug: Print first few vertices with UVs
    for (size_t i = 0; i < std::min(size_t(5), mesh_->vertices.size()); i++) {
        const auto& v = mesh_->vertices[i];
        LOG_DEBUG("Renderer", "  Vert " << i << ": pos=(" << v.position.x << ", " 
                  << v.position.y << ", " << v.position.z << ") "
                  << "uv=(" << v.uv.x << ", " << v.uv.y << ") "
                  << "normal=(" << v.normal.x << ", " << v.normal.y << ", " << v.normal.z << ")");
    }
    
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
    
    // Debug: log first 20 indices
    std::ostringstream idx_debug;
    idx_debug << "First 20 indices uploaded to GPU: ";
    for (size_t i = 0; i < std::min(size_t(20), mesh_->indices.size()); i++) {
        idx_debug << mesh_->indices[i] << " ";
    }
    LOG_DEBUG("Renderer", idx_debug.str());
    
    LOG_DEBUG("Renderer", "VAO=" << vao_ << " VBO=" << vbo_ << " EBO=" << ebo_);
    
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

void MeshRenderer::set_material_texture(size_t material_index, uint32_t texture_id, bool is_mcr) {
    material_textures_[material_index].diffuse = texture_id;
    material_textures_[material_index].is_mcr = is_mcr;
}

void MeshRenderer::set_material_color(size_t material_index, const glm::vec3& color) {
    material_textures_[material_index].base_color = color;
    material_textures_[material_index].has_base_color = true;
}

void MeshRenderer::set_material_enabled(size_t material_index, bool enabled) {
    material_textures_[material_index].enabled = enabled;
}

void MeshRenderer::clear_material_textures() {
    material_textures_.clear();
}

void MeshRenderer::render_mesh(const glm::mat4& view, const glm::mat4& projection) {
    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = projection * view * model;
    
    // Frustum culling check
    if (frustum_cull_ && !is_visible_in_frustum(mvp, bounds_min_, bounds_max_)) {
        return;  // Mesh is completely outside frustum
    }
    
    mesh_shader_->use();
    mesh_shader_->set_mat4("model", model);
    mesh_shader_->set_mat4("view", view);
    mesh_shader_->set_mat4("projection", projection);
    mesh_shader_->set_vec3("lightDir", glm::normalize(glm::vec3(0.5f, -1.0f, 0.3f)));
    mesh_shader_->set_vec3("lightColor", glm::vec3(1.0f));
    
    glBindVertexArray(vao_);
    
    if (wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    
    // Determine if we should use per-material rendering
    bool use_material_ranges = false;
    
    if (mesh_ && !mesh_->material_ranges.empty()) {
        // Calculate total triangles covered by ranges
        uint32_t covered_triangles = 0;
        for (const auto& range : mesh_->material_ranges) {
            covered_triangles += range.triangle_count;
        }
        uint32_t total_triangles = static_cast<uint32_t>(index_count_ / 3);
        // Use per-material rendering if ranges cover at least 50% of triangles
        use_material_ranges = (covered_triangles >= total_triangles / 2);
        
        // Log once per mesh
        static std::shared_ptr<const XobMesh> last_logged_mesh = nullptr;
        if (mesh_ != last_logged_mesh) {
            last_logged_mesh = mesh_;
            LOG_INFO("Renderer", "Material ranges: " << mesh_->material_ranges.size() 
                      << " ranges covering " << covered_triangles << "/" << total_triangles << " triangles"
                      << ", use_material_ranges=" << use_material_ranges
                      << ", material_textures=" << material_textures_.size());
            for (size_t i = 0; i < mesh_->material_ranges.size(); i++) {
                const auto& r = mesh_->material_ranges[i];
                auto tex_it = material_textures_.find(r.material_index);
                uint32_t tex = (tex_it != material_textures_.end()) ? tex_it->second.diffuse : 0;
                LOG_DEBUG("Renderer", "  Range[" << i << "]: mat=" << r.material_index 
                          << " tris=" << r.triangle_start << "-" << r.triangle_end 
                          << " tex=" << tex);
            }
        }
    }
    
    // Set debug uniforms
    mesh_shader_->set_bool("debugMaterialColors", debug_material_colors_);
    
    if (use_material_ranges) {
        // Track which triangles have been rendered
        uint32_t last_rendered_end = 0;
        uint32_t total_triangles = static_cast<uint32_t>(index_count_ / 3);
        
        // Render each material range separately
        for (const auto& range : mesh_->material_ranges) {
            // Check if material is enabled
            auto it = material_textures_.find(range.material_index);
            if (it != material_textures_.end() && !it->second.enabled) {
                continue; // Skip disabled materials
            }
            
            // Set debug material index for coloring
            mesh_shader_->set_int("debugMaterialIndex", static_cast<int>(range.material_index));
            
            // Set object color based on whether this material is highlighted
            if (highlighted_material_ >= 0 && range.material_index != static_cast<uint32_t>(highlighted_material_)) {
                // Dim non-highlighted materials
                mesh_shader_->set_vec3("objectColor", glm::vec3(0.4f, 0.4f, 0.45f));
            } else {
                mesh_shader_->set_vec3("objectColor", glm::vec3(1.0f, 1.0f, 1.0f));
            }
            
            // Bind per-material texture if available
            uint32_t tex = 0;
            bool is_mcr = false;
            bool has_base_color = false;
            glm::vec3 base_color(0.5f);
            
            if (it != material_textures_.end()) {
                if (it->second.diffuse != 0) {
                    tex = it->second.diffuse;
                    is_mcr = it->second.is_mcr;
                }
                if (it->second.has_base_color) {
                    has_base_color = true;
                    base_color = it->second.base_color;
                }
            } else if (fallback_texture_ != 0) {
                tex = fallback_texture_;
            }
            
            // Set base color uniform
            mesh_shader_->set_bool("hasBaseColor", has_base_color);
            mesh_shader_->set_vec3("baseColor", base_color);
            
            if (tex != 0) {
                mesh_shader_->set_bool("useTexture", true);
                mesh_shader_->set_bool("isMCRTexture", is_mcr);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                mesh_shader_->set_int("diffuseMap", 0);
            } else {
                mesh_shader_->set_bool("useTexture", false);
                mesh_shader_->set_bool("isMCRTexture", false);
            }
            
            // Draw this material's triangles
            // Debug log on first Mat Debug frame after mesh change
            static std::shared_ptr<const XobMesh> last_debug_mesh = nullptr;
            if (debug_material_colors_ && mesh_ != last_debug_mesh) {
                if (range.material_index == 0 || last_debug_mesh != mesh_) {
                    LOG_DEBUG("Renderer", "Drawing mat " << range.material_index 
                              << " at index_start=" << range.index_start() 
                              << " index_count=" << range.index_count()
                              << " (triangles " << range.triangle_start << "-" << range.triangle_end << ")");
                }
                if (range.material_index == mesh_->materials.size() - 1) {
                    last_debug_mesh = mesh_; // Mark this mesh as logged
                }
            }
            
            glDrawElements(GL_TRIANGLES, 
                          static_cast<GLsizei>(range.index_count()),
                          GL_UNSIGNED_INT,
                          reinterpret_cast<void*>(range.index_start() * sizeof(uint32_t)));
            
            if (range.triangle_end > last_rendered_end) {
                last_rendered_end = range.triangle_end;
            }
        }
        
        // Render any remaining triangles not covered by ranges with default material
        if (last_rendered_end < total_triangles) {
            mesh_shader_->set_vec3("objectColor", glm::vec3(1.0f, 1.0f, 1.0f));
            
            // Try to use the first available texture
            uint32_t tex = fallback_texture_;
            bool is_mcr = false;
            bool has_base_color = false;
            glm::vec3 base_color(0.5f);
            
            if (!material_textures_.empty()) {
                auto& first = material_textures_.begin()->second;
                if (first.diffuse != 0) {
                    tex = first.diffuse;
                    is_mcr = first.is_mcr;
                }
                if (first.has_base_color) {
                    has_base_color = true;
                    base_color = first.base_color;
                }
            }
            
            mesh_shader_->set_bool("hasBaseColor", has_base_color);
            mesh_shader_->set_vec3("baseColor", base_color);
            
            if (tex != 0) {
                mesh_shader_->set_bool("useTexture", true);
                mesh_shader_->set_bool("isMCRTexture", is_mcr);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                mesh_shader_->set_int("diffuseMap", 0);
            } else {
                mesh_shader_->set_bool("useTexture", false);
                mesh_shader_->set_bool("isMCRTexture", false);
            }
            
            uint32_t remaining_start = last_rendered_end * 3;
            uint32_t remaining_count = (total_triangles - last_rendered_end) * 3;
            
            glDrawElements(GL_TRIANGLES,
                          static_cast<GLsizei>(remaining_count),
                          GL_UNSIGNED_INT,
                          reinterpret_cast<void*>(remaining_start * sizeof(uint32_t)));
        }
    } else {
        // Fallback: render entire mesh with best available texture
        mesh_shader_->set_vec3("objectColor", glm::vec3(1.0f, 1.0f, 1.0f));
        
        // Try to use the first available material texture
        uint32_t tex = fallback_texture_;
        bool is_mcr = false;
        bool has_base_color = false;
        glm::vec3 base_color(0.5f);
        
        if (!material_textures_.empty()) {
            auto& first = material_textures_.begin()->second;
            if (first.diffuse != 0) {
                tex = first.diffuse;
                is_mcr = first.is_mcr;
            }
            if (first.has_base_color) {
                has_base_color = true;
                base_color = first.base_color;
            }
        }
        
        mesh_shader_->set_bool("hasBaseColor", has_base_color);
        mesh_shader_->set_vec3("baseColor", base_color);
        
        if (tex != 0) {
            mesh_shader_->set_bool("useTexture", true);
            mesh_shader_->set_bool("isMCRTexture", is_mcr);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            mesh_shader_->set_int("diffuseMap", 0);
        } else {
            mesh_shader_->set_bool("useTexture", false);
            mesh_shader_->set_bool("isMCRTexture", false);
        }
        
        // Determine index range based on LOD
        size_t index_offset = 0;
        size_t index_count = index_count_;
        
        if (mesh_ && !mesh_->lods.empty() && current_lod_ < static_cast<int>(mesh_->lods.size())) {
            index_offset = mesh_->lods[current_lod_].index_offset;
            index_count = mesh_->lods[current_lod_].index_count;
        }
        
        glDrawElements(GL_TRIANGLES, 
                       static_cast<GLsizei>(index_count),
                       GL_UNSIGNED_INT,
                       reinterpret_cast<void*>(index_offset * sizeof(uint32_t)));
    }
    
    if (wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    
    glBindVertexArray(0);
}

void MeshRenderer::render_grid(const glm::mat4& view, const glm::mat4& projection) {
    // Use RAII guards to ensure GL state is restored even on exception
    GLDepthMaskGuard depth_guard(GL_FALSE);
    GLStateGuard blend_guard(GL_BLEND, true);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    grid_shader_->use();
    grid_shader_->set_mat4("view", view);
    grid_shader_->set_mat4("projection", projection);
    grid_shader_->set_float("gridSize", grid_size_);
    grid_shader_->set_vec3("gridColor", glm::vec3(0.5f));
    
    glBindVertexArray(grid_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    // GL state automatically restored by RAII guards
}

void MeshRenderer::render_normals(const glm::mat4& view, const glm::mat4& projection) {
    // Would need separate normal visualization shader
    // For now, skip this
}

bool MeshRenderer::is_visible_in_frustum(const glm::mat4& mvp, const glm::vec3& min, const glm::vec3& max) const {
    // Test all 8 corners of the bounding box against the frustum
    // If all corners are outside any single frustum plane, the box is culled
    glm::vec4 corners[8] = {
        mvp * glm::vec4(min.x, min.y, min.z, 1.0f),
        mvp * glm::vec4(max.x, min.y, min.z, 1.0f),
        mvp * glm::vec4(min.x, max.y, min.z, 1.0f),
        mvp * glm::vec4(max.x, max.y, min.z, 1.0f),
        mvp * glm::vec4(min.x, min.y, max.z, 1.0f),
        mvp * glm::vec4(max.x, min.y, max.z, 1.0f),
        mvp * glm::vec4(min.x, max.y, max.z, 1.0f),
        mvp * glm::vec4(max.x, max.y, max.z, 1.0f)
    };
    
    // Check each frustum plane (in clip space: -w <= x,y,z <= w)
    // Left plane: x >= -w
    bool all_outside = true;
    for (int i = 0; i < 8; ++i) {
        if (corners[i].x >= -corners[i].w) { all_outside = false; break; }
    }
    if (all_outside) return false;
    
    // Right plane: x <= w
    all_outside = true;
    for (int i = 0; i < 8; ++i) {
        if (corners[i].x <= corners[i].w) { all_outside = false; break; }
    }
    if (all_outside) return false;
    
    // Bottom plane: y >= -w
    all_outside = true;
    for (int i = 0; i < 8; ++i) {
        if (corners[i].y >= -corners[i].w) { all_outside = false; break; }
    }
    if (all_outside) return false;
    
    // Top plane: y <= w
    all_outside = true;
    for (int i = 0; i < 8; ++i) {
        if (corners[i].y <= corners[i].w) { all_outside = false; break; }
    }
    if (all_outside) return false;
    
    // Near plane: z >= -w (or z >= 0 depending on depth range)
    all_outside = true;
    for (int i = 0; i < 8; ++i) {
        if (corners[i].z >= -corners[i].w) { all_outside = false; break; }
    }
    if (all_outside) return false;
    
    // Far plane: z <= w
    all_outside = true;
    for (int i = 0; i < 8; ++i) {
        if (corners[i].z <= corners[i].w) { all_outside = false; break; }
    }
    if (all_outside) return false;
    
    return true;  // At least partially visible
}

} // namespace enfusion
