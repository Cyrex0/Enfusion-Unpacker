/**
 * Enfusion Unpacker - Texture Viewer Panel
 */

#pragma once

#include "enfusion/types.hpp"
#include <memory>
#include <string>
#include <filesystem>
#include <vector>
#include <cstdint>

namespace enfusion {

/**
 * Panel for viewing DDS textures.
 */
class TextureViewer {
public:
    TextureViewer();
    ~TextureViewer();

    void render();

    /**
     * Load texture from raw data (DDS/EDDS).
     */
    void load_texture_data(const std::vector<uint8_t>& data, const std::string& name);

    /**
     * Load texture from file path.
     */
    void load_texture(const std::filesystem::path& path);

    /**
     * Clear current texture.
     */
    void clear();

    bool has_texture() const { return texture_loaded_; }

private:
    void render_toolbar();
    void render_texture_view();
    void render_info_panel();
    void render_channel_selector();
    void render_info_bar();

    bool parse_dds(const std::vector<uint8_t>& data);
    void create_gl_texture();
    void destroy_gl_texture();
    void fit_to_view(float view_width, float view_height);

    std::vector<uint8_t> pixel_data_;
    std::string texture_name_;
    std::filesystem::path current_path_;

    // Texture state
    bool texture_loaded_ = false;
    bool loading_ = false;
    std::string error_message_;
    uint32_t texture_id_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t channels_ = 4;
    uint32_t mip_levels_ = 1;
    std::string format_;

    // View settings
    float zoom_ = 1.0f;
    float pan_x_ = 0.0f;
    float pan_y_ = 0.0f;
    bool show_alpha_ = true;
    bool show_red_ = true;
    bool show_green_ = true;
    bool show_blue_ = true;
    bool tile_preview_ = false;
    int current_mip_ = 0;

    // Drag state
    bool is_dragging_ = false;
    float drag_start_x_ = 0.0f;
    float drag_start_y_ = 0.0f;
};

} // namespace enfusion
