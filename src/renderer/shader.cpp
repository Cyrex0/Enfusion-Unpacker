/**
 * Enfusion Unpacker - Shader Implementation
 */

#include "renderer/shader.hpp"
#include <glad/glad.h>
#include <fstream>
#include <sstream>
#include <iostream>

namespace enfusion {

Shader::Shader() : program_id_(0) {}

Shader::~Shader() {
    if (program_id_ != 0) {
        glDeleteProgram(program_id_);
    }
}

bool Shader::load(const std::string& vertex_source, const std::string& fragment_source) {
    // Compile vertex shader
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    const char* vs_src = vertex_source.c_str();
    glShaderSource(vertex_shader, 1, &vs_src, nullptr);
    glCompileShader(vertex_shader);
    
    if (!check_compile_errors(vertex_shader, "VERTEX")) {
        glDeleteShader(vertex_shader);
        return false;
    }
    
    // Compile fragment shader
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    const char* fs_src = fragment_source.c_str();
    glShaderSource(fragment_shader, 1, &fs_src, nullptr);
    glCompileShader(fragment_shader);
    
    if (!check_compile_errors(fragment_shader, "FRAGMENT")) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }
    
    // Link program
    program_id_ = glCreateProgram();
    glAttachShader(program_id_, vertex_shader);
    glAttachShader(program_id_, fragment_shader);
    glLinkProgram(program_id_);
    
    if (!check_compile_errors(program_id_, "PROGRAM")) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        glDeleteProgram(program_id_);
        program_id_ = 0;
        return false;
    }
    
    // Clean up shaders
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return true;
}

bool Shader::load_from_file(const std::filesystem::path& vertex_path,
                             const std::filesystem::path& fragment_path) {
    std::string vertex_source = read_file(vertex_path);
    std::string fragment_source = read_file(fragment_path);
    
    if (vertex_source.empty() || fragment_source.empty()) {
        return false;
    }
    
    return load(vertex_source, fragment_source);
}

void Shader::use() const {
    glUseProgram(program_id_);
}

void Shader::set_bool(const std::string& name, bool value) const {
    glUniform1i(get_uniform_location(name), static_cast<int>(value));
}

void Shader::set_int(const std::string& name, int value) const {
    glUniform1i(get_uniform_location(name), value);
}

void Shader::set_float(const std::string& name, float value) const {
    glUniform1f(get_uniform_location(name), value);
}

void Shader::set_vec2(const std::string& name, const glm::vec2& value) const {
    glUniform2fv(get_uniform_location(name), 1, &value[0]);
}

void Shader::set_vec3(const std::string& name, const glm::vec3& value) const {
    glUniform3fv(get_uniform_location(name), 1, &value[0]);
}

void Shader::set_vec4(const std::string& name, const glm::vec4& value) const {
    glUniform4fv(get_uniform_location(name), 1, &value[0]);
}

void Shader::set_mat3(const std::string& name, const glm::mat3& value) const {
    glUniformMatrix3fv(get_uniform_location(name), 1, GL_FALSE, &value[0][0]);
}

void Shader::set_mat4(const std::string& name, const glm::mat4& value) const {
    glUniformMatrix4fv(get_uniform_location(name), 1, GL_FALSE, &value[0][0]);
}

GLint Shader::get_uniform_location(const std::string& name) const {
    auto it = uniform_cache_.find(name);
    if (it != uniform_cache_.end()) {
        return it->second;
    }
    
    GLint location = glGetUniformLocation(program_id_, name.c_str());
    uniform_cache_[name] = location;
    return location;
}

bool Shader::check_compile_errors(GLuint shader, const std::string& type) {
    GLint success;
    char info_log[1024];
    
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 1024, nullptr, info_log);
            std::cerr << "Shader compilation error (" << type << "): " << info_log << std::endl;
            return false;
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, nullptr, info_log);
            std::cerr << "Program linking error: " << info_log << std::endl;
            return false;
        }
    }
    
    return true;
}

std::string Shader::read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Built-in shaders
const char* Shader::MESH_VERTEX_SHADER = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoord = aTexCoord;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";

const char* Shader::MESH_FRAGMENT_SHADER = R"(
#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 objectColor;
uniform bool useTexture;
uniform sampler2D diffuseMap;

void main() {
    // Ambient - high for visibility
    float ambientStrength = 0.5;
    vec3 ambient = ambientStrength * lightColor;
    
    // Diffuse lighting
    vec3 norm = normalize(Normal);
    float diff = max(dot(norm, -lightDir), 0.0);
    vec3 diffuse = diff * lightColor * 0.7;
    
    vec3 color = objectColor;
    if (useTexture) {
        vec4 texColor = texture(diffuseMap, TexCoord);
        // For MCR textures: R=Metallic, G=Color(albedo), B=Roughness
        // The G channel contains the grayscale albedo
        // Use G channel as the base color, significantly boosted
        float albedo = texColor.g;
        
        // Create color from albedo - boost it significantly
        // MCR albedo values are typically in 0.1-0.4 range
        color = vec3(albedo * 3.0);
        
        // Add slight color variation from R/B for visual interest  
        color.r += texColor.r * 0.1;
        color.b += texColor.b * 0.1;
        
        color = clamp(color, 0.0, 1.0);
    }
    
    vec3 result = (ambient + diffuse) * color;
    FragColor = vec4(result, 1.0);
}
)";

const char* Shader::GRID_VERTEX_SHADER = R"(
#version 330 core
layout (location = 0) in vec3 aPos;

uniform mat4 view;
uniform mat4 projection;

out vec3 nearPoint;
out vec3 farPoint;

vec3 unprojectPoint(float x, float y, float z, mat4 viewInv, mat4 projInv) {
    vec4 unprojectedPoint = viewInv * projInv * vec4(x, y, z, 1.0);
    return unprojectedPoint.xyz / unprojectedPoint.w;
}

void main() {
    mat4 viewInv = inverse(view);
    mat4 projInv = inverse(projection);
    nearPoint = unprojectPoint(aPos.x, aPos.y, 0.0, viewInv, projInv);
    farPoint = unprojectPoint(aPos.x, aPos.y, 1.0, viewInv, projInv);
    gl_Position = vec4(aPos, 1.0);
}
)";

const char* Shader::GRID_FRAGMENT_SHADER = R"(
#version 330 core
in vec3 nearPoint;
in vec3 farPoint;
out vec4 FragColor;

uniform float gridSize;
uniform vec3 gridColor;

float computeDepth(vec3 pos, mat4 view, mat4 projection) {
    vec4 clipPos = projection * view * vec4(pos, 1.0);
    return clipPos.z / clipPos.w;
}

float grid(vec3 fragPos, float scale) {
    vec2 coord = fragPos.xz * scale;
    vec2 derivative = fwidth(coord);
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / derivative;
    float line = min(grid.x, grid.y);
    return 1.0 - min(line, 1.0);
}

void main() {
    float t = -nearPoint.y / (farPoint.y - nearPoint.y);
    vec3 fragPos = nearPoint + t * (farPoint - nearPoint);
    
    float gridVal = grid(fragPos, 1.0 / gridSize) * 0.5;
    gridVal += grid(fragPos, 0.1 / gridSize) * 0.3;
    
    FragColor = vec4(gridColor, gridVal * float(t > 0));
}
)";

} // namespace enfusion
