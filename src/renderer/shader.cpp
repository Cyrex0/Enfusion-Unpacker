/**
 * Enfusion Unpacker - Shader Implementation
 */

#include "renderer/shader.hpp"
#include "enfusion/logging.hpp"
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
            LOG_ERROR("Shader", "Shader compilation error (" << type << "): " << info_log);
            return false;
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, nullptr, info_log);
            LOG_ERROR("Shader", "Program linking error: " << info_log);
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
uniform vec3 baseColor;     // Base color from emat (for MCR materials)
uniform bool useTexture;
uniform bool isMCRTexture;  // True for _MCR textures (Metallic-Cavity-Roughness)
uniform bool hasBaseColor;  // True if baseColor was set from emat
uniform bool debugMaterialColors;  // Debug: show material colors instead of textures
uniform int debugMaterialIndex;    // Debug: which material (for coloring)
uniform sampler2D diffuseMap;

// Hash function for deterministic material colors
vec3 hashMaterialColor(int matIdx) {
    // Generate unique colors for each material index
    float r = fract(sin(float(matIdx) * 12.9898) * 43758.5453);
    float g = fract(sin(float(matIdx) * 78.233 + 1.0) * 43758.5453);
    float b = fract(sin(float(matIdx) * 39.425 + 2.0) * 43758.5453);
    // Make colors more saturated and distinct
    return vec3(r, g, b) * 0.6 + vec3(0.2);
}

void main() {
    vec3 norm = normalize(Normal);
    
    // Main directional light
    float NdotL = max(dot(norm, -lightDir), 0.0);
    
    // Fill light from opposite side (soft)
    float fillNdotL = max(dot(norm, lightDir), 0.0) * 0.3;
    
    // Sky/ambient from above
    float skyNdotL = dot(norm, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
    
    // Ground bounce (subtle)
    float groundNdotL = max(dot(norm, vec3(0.0, -1.0, 0.0)), 0.0) * 0.1;
    
    // Combine lighting
    float totalLight = 0.35 +           // Ambient base
                       NdotL * 0.55 +    // Main light
                       fillNdotL +       // Fill light
                       skyNdotL * 0.15 + // Sky light
                       groundNdotL;      // Ground bounce
    
    // Debug mode: show material colors instead of textures
    if (debugMaterialColors) {
        vec3 matColor = hashMaterialColor(debugMaterialIndex);
        vec3 result = matColor * totalLight;
        result = pow(result, vec3(1.0/2.2));
        FragColor = vec4(result, 1.0);
        return;
    }
    
    vec3 color = objectColor;
    if (useTexture) {
        vec4 texColor = texture(diffuseMap, TexCoord);
        
        if (isMCRTexture) {
            // MCR texture: Metallic-Cavity-Roughness format
            // R = Metallic (0 = dielectric, 1 = metal)
            // G = Cavity/AO (ambient occlusion detail)
            // B = Roughness (0 = smooth, 1 = rough)
            //
            // The actual base color comes from the emat's Color_3 parameter.
            
            float metallic = texColor.r;
            float cavity = texColor.g;  // AO/cavity for detail
            float roughness = texColor.b;
            
            // Use base color from emat if available, otherwise derive a grayscale
            // signal from MCR to avoid flat-color previews.
            vec3 surfaceColor = hasBaseColor ? baseColor : vec3(0.5, 0.5, 0.5);
            vec3 mcrGray = vec3(metallic * 0.4 + cavity * 0.4 + (1.0 - roughness) * 0.2);
            if (!hasBaseColor) {
                surfaceColor = mcrGray;
            }
            
            // Stronger cavity/roughness modulation to reveal detail in preview
            float cavityMult = mix(0.4, 1.2, cavity);
            float roughMult = mix(1.15, 0.7, roughness);
            float metallicBoost = mix(1.0, 1.2, metallic);
            
            color = surfaceColor * cavityMult * roughMult * metallicBoost;
            
        } else {
            // BCR/standard diffuse texture: RGB = base color
            // If we have a color parameter from emat, use it as a tint/multiplier
            // (For MatPBRBasic materials, Color is often a tint for the BCR texture)
            if (hasBaseColor) {
                // Apply color tint, but check if it's dark (could be unused/default)
                float colorBrightness = (baseColor.r + baseColor.g + baseColor.b) / 3.0;
                if (colorBrightness > 0.1) {
                    // Non-trivial color - multiply (tint)
                    color = texColor.rgb * baseColor * 2.0;  // 2x because colors are often 0.5ish
                } else {
                    // Very dark color - ignore it, use texture only
                    color = texColor.rgb;
                }
            } else {
                color = texColor.rgb;
            }
        }
    } else if (hasBaseColor) {
        // No texture but we have a base color from emat
        color = baseColor;
    }
    
    // Apply lighting
    vec3 result = color * totalLight * lightColor;
    
    // Slight exposure boost
    result *= 1.15;
    
    // Gamma correction (linear to sRGB)
    result = pow(result, vec3(1.0/2.2));
    
    // Clamp
    result = clamp(result, 0.0, 1.0);
    
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
