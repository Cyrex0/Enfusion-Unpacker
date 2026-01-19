/**
 * Enfusion Unpacker - Text Viewer Header
 */

#pragma once

#include "enfusion/types.hpp"
#include <string>
#include <vector>

namespace enfusion {

class TextViewer {
public:
    TextViewer();
    ~TextViewer() = default;

    void load_text_data(const std::vector<uint8_t>& data, const std::string& filename);
    void render();
    void clear();

    bool has_content() const { return !text_.empty(); }
    const std::string& filename() const { return filename_; }

private:
    void render_toolbar();

    std::string text_;
    std::string filename_;
    bool word_wrap_ = true;
    bool show_line_numbers_ = true;
    bool scroll_to_top_ = false;
    float font_size_ = 14.0f;
};

} // namespace enfusion
