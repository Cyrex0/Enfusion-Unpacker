/**
 * Enfusion Unpacker - Shader
 */

#pragma once

#include <glm/glm.hpp>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <cstdint>

typedef int GLint;

namespace enfusion {

/**
 * OpenGL shader program wrapper.
 */
class Shader {
public:
    Shader();
    ~Shader();

    bool load(const std::string& vertex_source, const std::string& fragment_source);
    bool load_from_file(const std::filesystem::path& vertex_path,
                         const std::filesystem::path& fragment_path);

    void use() const;

    // Uniform setters
    void set_bool(const std::string& name, bool value) const;
    void set_int(const std::string& name, int value) const;
    void set_float(const std::string& name, float value) const;
    void set_vec2(const std::string& name, const glm::vec2& value) const;
    void set_vec3(const std::string& name, const glm::vec3& value) const;
    void set_vec4(const std::string& name, const glm::vec4& value) const;
    void set_mat3(const std::string& name, const glm::mat3& value) const;
    void set_mat4(const std::string& name, const glm::mat4& value) const;

    uint32_t id() const { return program_id_; }
    bool is_valid() const { return program_id_ != 0; }

    // Built-in shader sources
    static const char* MESH_VERTEX_SHADER;
    static const char* MESH_FRAGMENT_SHADER;
    static const char* GRID_VERTEX_SHADER;
    static const char* GRID_FRAGMENT_SHADER;

private:
    GLint get_uniform_location(const std::string& name) const;
    bool check_compile_errors(uint32_t shader, const std::string& type);
    std::string read_file(const std::filesystem::path& path);

    uint32_t program_id_ = 0;
    mutable std::unordered_map<std::string, GLint> uniform_cache_;
};

} // namespace enfusion
