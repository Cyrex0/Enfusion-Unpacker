/**
 * Enfusion Unpacker - Texture Renderer
 */

#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <cstdint>

typedef unsigned int GLuint;

namespace enfusion {

class Shader;

/**
 * Channel mask for texture viewing.
 */
struct ChannelMask {
    bool r = true;
    bool g = true;
    bool b = true;
    bool a = false;
};

/**
 * OpenGL texture renderer for preview.
 */
class TextureRenderer {
public:
    TextureRenderer();
    ~TextureRenderer();

    void init();
    void cleanup();

    void render(GLuint texture_id, const glm::mat4& transform,
                const ChannelMask& channels = ChannelMask(), float alpha = 1.0f);
    void render_checkerboard(const glm::mat4& transform,
                              const glm::vec2& texture_size,
                              float check_size = 16.0f);

private:
    void create_quad();

    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;

    std::unique_ptr<Shader> texture_shader_;
    std::unique_ptr<Shader> checkerboard_shader_;
};

} // namespace enfusion
