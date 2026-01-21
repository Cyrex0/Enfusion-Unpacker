/**
 * Enfusion Unpacker - Texture Viewer Implementation
 */

#include "gui/texture_viewer.hpp"
#include "enfusion/dds_loader.hpp"
#include "enfusion/edds_converter.hpp"
#include "enfusion/files.hpp"

#include <imgui.h>
#include <glad/glad.h>
#include <fstream>
#include <algorithm>

namespace enfusion {

TextureViewer::TextureViewer() = default;

TextureViewer::~TextureViewer() {
    if (texture_id_ != 0) {
        glDeleteTextures(1, &texture_id_);
    }
}

void TextureViewer::load_texture_data(const std::vector<uint8_t>& data, const std::string& name) {
    // ALWAYS clear previous texture first
    clear();
    
    texture_name_ = name;
    loading_ = true;
    error_message_.clear();

    try {
        if (data.empty()) {
            error_message_ = "Empty data";
            loading_ = false;
            return;
        }

        std::vector<uint8_t> dds_data;

        // Check if EDDS (starts with "DDS " but has COPY/LZ4 mip table)
        EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
        if (converter.is_edds()) {
            dds_data = converter.convert();
            if (dds_data.empty()) {
                // Conversion failed, try original data
                dds_data = data;
            }
        } else {
            dds_data = data;
        }

        auto result = DdsLoader::load(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
        if (!result) {
            error_message_ = "Failed to parse DDS data";
            loading_ = false;
            return;
        }

        pixel_data_ = std::move(result->pixels);
        width_ = result->width;
        height_ = result->height;
        channels_ = result->channels;
        format_ = result->format;
        mip_levels_ = result->mip_count;

        // Create OpenGL texture
        create_gl_texture();

        texture_loaded_ = true;
        loading_ = false;

        // Reset view for new texture
        zoom_ = 1.0f;
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;

    } catch (const std::exception& e) {
        error_message_ = std::string("Error: ") + e.what();
        loading_ = false;
    }
}

void TextureViewer::load_texture(const std::filesystem::path& path) {
    current_path_ = path;
    auto data = read_file(path);
    load_texture_data(data, path.filename().string());
}

void TextureViewer::clear() {
    // Delete existing OpenGL texture
    if (texture_id_ != 0) {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }
    
    // Clear all state
    texture_loaded_ = false;
    loading_ = false;
    error_message_.clear();
    pixel_data_.clear();
    texture_name_.clear();
    width_ = 0;
    height_ = 0;
    channels_ = 4;
    mip_levels_ = 1;
    format_.clear();
    
    // Reset view
    zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
}

void TextureViewer::create_gl_texture() {
    // Delete any existing texture
    if (texture_id_ != 0) {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }

    if (pixel_data_.empty() || width_ == 0 || height_ == 0) {
        return;
    }

    glGenTextures(1, &texture_id_);
    glBindTexture(GL_TEXTURE_2D, texture_id_);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    GLenum gl_format = (channels_ == 4) ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, gl_format, width_, height_, 0, gl_format,
                 GL_UNSIGNED_BYTE, pixel_data_.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    
    glBindTexture(GL_TEXTURE_2D, 0);
}

void TextureViewer::destroy_gl_texture() {
    if (texture_id_ != 0) {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }
}

bool TextureViewer::parse_dds(const std::vector<uint8_t>& data) {
    // Implemented by DdsLoader
    return true;
}

void TextureViewer::render() {
    render_toolbar();

    ImGui::Separator();

    ImVec2 content_size = ImGui::GetContentRegionAvail();

    if (loading_) {
        ImGui::Text("Loading texture...");
        return;
    }

    if (!error_message_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_message_.c_str());
        return;
    }

    if (!texture_loaded_ || texture_id_ == 0) {
        ImGui::TextDisabled("No texture loaded.");
        ImGui::TextDisabled("Select a .edds or .dds file.");
        return;
    }

    ImGui::BeginChild("TextureView", ImVec2(0, -30), true, ImGuiWindowFlags_HorizontalScrollbar);

    float display_width = width_ * zoom_;
    float display_height = height_ * zoom_;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (display_width < avail.x) {
        ImGui::SetCursorPosX((avail.x - display_width) / 2.0f + pan_x_);
    }
    if (display_height < avail.y) {
        ImGui::SetCursorPosY((avail.y - display_height) / 2.0f + pan_y_);
    }

    ImVec4 tint = ImVec4(
        show_red_ ? 1.0f : 0.0f,
        show_green_ ? 1.0f : 0.0f,
        show_blue_ ? 1.0f : 0.0f,
        1.0f
    );

    if (!show_red_ && !show_green_ && !show_blue_) {
        tint = ImVec4(1, 1, 1, 1);
    }

    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(texture_id_)),
                 ImVec2(display_width, display_height),
                 ImVec2(0, 0), ImVec2(1, 1), tint, ImVec4(0, 0, 0, 0));

    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            zoom_ *= (wheel > 0) ? 1.1f : 0.9f;
            zoom_ = std::clamp(zoom_, 0.1f, 10.0f);
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            pan_x_ += delta.x;
            pan_y_ += delta.y;
        }
    }

    ImGui::EndChild();

    render_info_bar();
}

void TextureViewer::render_toolbar() {
    if (ImGui::Button("-")) {
        zoom_ *= 0.8f;
        zoom_ = std::max(zoom_, 0.1f);
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("##Zoom", &zoom_, 0.1f, 10.0f, "%.1fx");
    ImGui::SameLine();

    if (ImGui::Button("+")) {
        zoom_ *= 1.2f;
        zoom_ = std::min(zoom_, 10.0f);
    }
    ImGui::SameLine();

    if (ImGui::Button("Fit")) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        fit_to_view(avail.x, avail.y);
    }
    ImGui::SameLine();

    if (ImGui::Button("1:1")) {
        zoom_ = 1.0f;
        pan_x_ = 0;
        pan_y_ = 0;
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    ImGui::Text("Channels:");
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, show_red_ ? ImVec4(0.8f, 0.2f, 0.2f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
    if (ImGui::SmallButton("R")) show_red_ = !show_red_;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, show_green_ ? ImVec4(0.2f, 0.8f, 0.2f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
    if (ImGui::SmallButton("G")) show_green_ = !show_green_;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, show_blue_ ? ImVec4(0.2f, 0.2f, 0.8f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
    if (ImGui::SmallButton("B")) show_blue_ = !show_blue_;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    if (channels_ == 4) {
        ImGui::PushStyleColor(ImGuiCol_Button, show_alpha_ ? ImVec4(0.6f, 0.6f, 0.6f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
        if (ImGui::SmallButton("A")) show_alpha_ = !show_alpha_;
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    if (ImGui::Button("Export PNG")) {
        // TODO: Implement PNG export
    }
}

void TextureViewer::render_texture_view() {
    // Handled in render()
}

void TextureViewer::render_info_panel() {
    // Handled in render_info_bar()
}

void TextureViewer::render_channel_selector() {
    // Handled in render_toolbar()
}

void TextureViewer::render_info_bar() {
    ImGui::TextDisabled("%dx%d | %s | %d mips | Zoom: %.0f%%",
                        width_, height_, format_.c_str(), mip_levels_, zoom_ * 100.0f);
    ImGui::SameLine(ImGui::GetWindowWidth() - 200);
    ImGui::TextDisabled("%s", texture_name_.c_str());
}

void TextureViewer::fit_to_view(float view_width, float view_height) {
    if (width_ == 0 || height_ == 0) return;
    
    float scale_x = view_width / static_cast<float>(width_);
    float scale_y = view_height / static_cast<float>(height_);
    zoom_ = std::min(scale_x, scale_y) * 0.9f;
    pan_x_ = 0;
    pan_y_ = 0;
}

} // namespace enfusion
