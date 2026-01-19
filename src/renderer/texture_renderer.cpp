/**
 * Enfusion Unpacker - Texture Renderer Implementation
 */

#include "renderer/texture_renderer.hpp"
#include "renderer/shader.hpp"
#include <glad/glad.h>

namespace enfusion {

const char* TEXTURE_VERTEX_SHADER = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

uniform mat4 transform;

void main() {
    gl_Position = transform * vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

const char* TEXTURE_FRAGMENT_SHADER = R"(
#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D texture0;
uniform vec4 channelMask;
uniform float alpha;

void main() {
    vec4 texColor = texture(texture0, TexCoord);
    vec3 rgb = texColor.rgb * channelMask.rgb;
    
    // If only one channel is visible, show as grayscale
    if (channelMask.r + channelMask.g + channelMask.b == 1.0) {
        float val = dot(texColor.rgb, channelMask.rgb);
        rgb = vec3(val);
    }
    
    // Show alpha channel as grayscale if alpha mask is set
    if (channelMask.a > 0.0 && channelMask.r + channelMask.g + channelMask.b == 0.0) {
        rgb = vec3(texColor.a);
    }
    
    FragColor = vec4(rgb, alpha);
}
)";

const char* CHECKERBOARD_FRAGMENT_SHADER = R"(
#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

uniform vec2 textureSize;
uniform float checkSize;

void main() {
    vec2 pos = TexCoord * textureSize / checkSize;
    float pattern = mod(floor(pos.x) + floor(pos.y), 2.0);
    vec3 color = mix(vec3(0.4), vec3(0.6), pattern);
    FragColor = vec4(color, 1.0);
}
)";

TextureRenderer::TextureRenderer() = default;

TextureRenderer::~TextureRenderer() {
    cleanup();
}

void TextureRenderer::init() {
    // Create texture shader
    texture_shader_ = std::make_unique<Shader>();
    texture_shader_->load(TEXTURE_VERTEX_SHADER, TEXTURE_FRAGMENT_SHADER);
    
    // Create checkerboard shader
    checkerboard_shader_ = std::make_unique<Shader>();
    checkerboard_shader_->load(TEXTURE_VERTEX_SHADER, CHECKERBOARD_FRAGMENT_SHADER);
    
    // Create quad VAO
    create_quad();
}

void TextureRenderer::cleanup() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
        vao_ = vbo_ = 0;
    }
}

void TextureRenderer::create_quad() {
    float vertices[] = {
        // Position     // TexCoord
        -1.0f, -1.0f,   0.0f, 0.0f,
         1.0f, -1.0f,   1.0f, 0.0f,
         1.0f,  1.0f,   1.0f, 1.0f,
        -1.0f, -1.0f,   0.0f, 0.0f,
         1.0f,  1.0f,   1.0f, 1.0f,
        -1.0f,  1.0f,   0.0f, 1.0f
    };
    
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    
    glBindVertexArray(0);
}

void TextureRenderer::render(GLuint texture_id, const glm::mat4& transform,
                              const ChannelMask& channels, float alpha) {
    if (vao_ == 0) {
        init();
    }
    
    texture_shader_->use();
    texture_shader_->set_mat4("transform", transform);
    texture_shader_->set_vec4("channelMask", glm::vec4(
        channels.r ? 1.0f : 0.0f,
        channels.g ? 1.0f : 0.0f,
        channels.b ? 1.0f : 0.0f,
        channels.a ? 1.0f : 0.0f
    ));
    texture_shader_->set_float("alpha", alpha);
    texture_shader_->set_int("texture0", 0);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void TextureRenderer::render_checkerboard(const glm::mat4& transform,
                                           const glm::vec2& texture_size,
                                           float check_size) {
    if (vao_ == 0) {
        init();
    }
    
    checkerboard_shader_->use();
    checkerboard_shader_->set_mat4("transform", transform);
    checkerboard_shader_->set_vec2("textureSize", texture_size);
    checkerboard_shader_->set_float("checkSize", check_size);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

} // namespace enfusion
