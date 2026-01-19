/**
 * Enfusion Unpacker - Text Viewer Implementation
 */

#include "gui/text_viewer.hpp"
#include <imgui.h>
#include <algorithm>

namespace enfusion {

TextViewer::TextViewer() {
}

void TextViewer::load_text_data(const std::vector<uint8_t>& data, const std::string& filename) {
    // Clear previous content first
    clear();
    
    filename_ = filename;
    
    // Convert binary data to string, handling different line endings
    text_.clear();
    text_.reserve(data.size());
    
    for (size_t i = 0; i < data.size(); i++) {
        char c = static_cast<char>(data[i]);
        
        // Handle CRLF -> LF
        if (c == '\r') {
            if (i + 1 < data.size() && data[i + 1] == '\n') {
                continue; // Skip \r, next iteration will add \n
            }
            c = '\n'; // Treat lone \r as \n
        }
        
        // Filter out non-printable characters except newline and tab
        if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) {
            text_ += c;
        }
    }
    
    // Reset scroll position for new file
    scroll_to_top_ = true;
}

void TextViewer::render() {
    render_toolbar();
    ImGui::Separator();
    
    if (text_.empty()) {
        ImGui::TextDisabled("No text file loaded");
        ImGui::TextDisabled("Select a .c, .et, .conf, .xml, .json, or .txt file");
        return;
    }
    
    // Text display area with scrolling
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= 24; // Leave room for status
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_HorizontalScrollbar;
    
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
    
    if (ImGui::BeginChild("TextContent", size, true, flags)) {
        // Scroll to top when new file loaded
        if (scroll_to_top_) {
            ImGui::SetScrollY(0.0f);
            scroll_to_top_ = false;
        }
        
        if (show_line_numbers_) {
            // Split into lines and display with line numbers
            size_t line = 1;
            size_t start = 0;
            
            for (size_t i = 0; i <= text_.size(); i++) {
                if (i == text_.size() || text_[i] == '\n') {
                    // Line number
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%4zu ", line);
                    ImGui::SameLine();
                    
                    // Line content
                    std::string line_text = text_.substr(start, i - start);
                    
                    // Syntax highlighting for common patterns
                    bool has_keyword = false;
                    const char* keywords[] = {"void", "int", "float", "bool", "string", "class",
                                              "return", "if", "else", "for", "while", "switch",
                                              "case", "break", "continue", "true", "false", "null"};
                    
                    for (const char* kw : keywords) {
                        if (line_text.find(kw) != std::string::npos) {
                            has_keyword = true;
                            break;
                        }
                    }
                    
                    if (line_text.find("//") != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.4f, 0.6f, 0.4f, 1.0f), "%s", line_text.c_str());
                    } else if (has_keyword) {
                        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%s", line_text.c_str());
                    } else {
                        ImGui::TextUnformatted(line_text.c_str());
                    }
                    
                    start = i + 1;
                    line++;
                }
            }
        } else {
            // Simple text display
            if (word_wrap_) {
                ImGui::TextWrapped("%s", text_.c_str());
            } else {
                ImGui::TextUnformatted(text_.c_str());
            }
        }
    }
    ImGui::EndChild();
    
    ImGui::PopStyleColor();
    
    // Status bar
    size_t line_count = std::count(text_.begin(), text_.end(), '\n') + 1;
    ImGui::Text("File: %s | Lines: %zu | Size: %zu bytes", 
                filename_.c_str(), line_count, text_.size());
}

void TextViewer::render_toolbar() {
    ImGui::Checkbox("Line Numbers", &show_line_numbers_);
    ImGui::SameLine();
    ImGui::Checkbox("Word Wrap", &word_wrap_);
}

void TextViewer::clear() {
    text_.clear();
    filename_.clear();
    scroll_to_top_ = true;
}

} // namespace enfusion
