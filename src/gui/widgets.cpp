/**
 * Enfusion Unpacker - Custom Widgets Implementation
 */

#include "gui/widgets.hpp"
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>

namespace enfusion {
namespace widgets {

bool IconButton(const char* icon, const char* tooltip, const ImVec2& size) {
    bool clicked = ImGui::Button(icon, size);
    
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", tooltip);
        ImGui::EndTooltip();
    }
    
    return clicked;
}

bool ToggleButton(const char* label, bool* value, const ImVec2& size) {
    ImVec4 active_color = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
    ImVec4 normal_color = ImGui::GetStyle().Colors[ImGuiCol_Button];
    
    if (*value) {
        ImGui::PushStyleColor(ImGuiCol_Button, active_color);
    }
    
    bool clicked = ImGui::Button(label, size);
    
    if (*value) {
        ImGui::PopStyleColor();
    }
    
    if (clicked) {
        *value = !*value;
    }
    
    return clicked;
}

bool SearchInput(const char* id, std::string& text, size_t max_length) {
    char buffer[1024] = {0};
    strncpy(buffer, text.c_str(), std::min(text.size(), sizeof(buffer) - 1));
    
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));
    
    // Search icon
    ImGui::Text("S");
    ImGui::SameLine();
    
    bool changed = ImGui::InputText(id, buffer, max_length, ImGuiInputTextFlags_None);
    
    ImGui::PopStyleVar(2);
    
    if (changed) {
        text = buffer;
    }
    
    // Clear button
    if (!text.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            text.clear();
            changed = true;
        }
    }
    
    return changed;
}

bool PathInput(const char* label, std::filesystem::path& path, const char* filter) {
    char buffer[1024] = {0};
    std::string path_str = path.string();
    strncpy(buffer, path_str.c_str(), std::min(path_str.size(), sizeof(buffer) - 1));
    
    ImGui::Text("%s", label);
    
    ImGui::SetNextItemWidth(-60);
    bool changed = ImGui::InputText("##path", buffer, sizeof(buffer));
    
    if (changed) {
        path = buffer;
    }
    
    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(50, 0))) {
        // TODO: Open file dialog
        return true;
    }
    
    return changed;
}

void Spinner(const char* label, float radius, float thickness, ImU32 color) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size(radius * 2, radius * 2);
    
    ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, 0)) return;
    
    float time = static_cast<float>(ImGui::GetTime());
    int num_segments = 30;
    float start = fabsf(sinf(time * 1.8f) * (num_segments - 5));
    
    const float a_min = IM_PI * 2.0f * start / static_cast<float>(num_segments);
    const float a_max = IM_PI * 2.0f * (static_cast<float>(num_segments) - 3) / static_cast<float>(num_segments);
    
    ImVec2 centre(pos.x + radius, pos.y + radius);
    
    for (int i = 0; i < num_segments; i++) {
        const float a = a_min + (static_cast<float>(i) / static_cast<float>(num_segments)) * (a_max - a_min);
        window->DrawList->PathLineTo(ImVec2(
            centre.x + cosf(a + time * 8.0f) * radius,
            centre.y + sinf(a + time * 8.0f) * radius
        ));
    }
    
    window->DrawList->PathStroke(color, false, thickness);
}

bool ProgressBar(const char* label, float progress, const ImVec2& size) {
    ImGui::Text("%s", label);
    ImGui::ProgressBar(progress, size);
    return progress >= 1.0f;
}

void InfoRow(const char* label, const char* value) {
    ImGui::TextDisabled("%s:", label);
    ImGui::SameLine(120);
    ImGui::Text("%s", value);
}

void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Separator(const char* text) {
    if (text) {
        float width = ImGui::CalcTextSize(text).x;
        float avail = ImGui::GetContentRegionAvail().x;
        float padding = (avail - width) / 2.0f - 10.0f;
        
        if (padding > 0) {
            ImGui::Separator();
            ImGui::SameLine(padding);
            ImGui::TextDisabled("%s", text);
            ImGui::SameLine();
        }
        ImGui::Separator();
    } else {
        ImGui::Separator();
    }
}

void BeginGroupBox(const char* label) {
    ImGui::BeginGroup();
    
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    
    ImGui::GetWindowDrawList()->AddRect(
        cursor,
        ImVec2(cursor.x + avail.x, cursor.y + 200),  // Placeholder height
        ImGui::GetColorU32(ImGuiCol_Border),
        4.0f
    );
    
    if (label) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
        ImGui::Text("%s", label);
    }
    
    ImGui::Indent(8);
}

void EndGroupBox() {
    ImGui::Unindent(8);
    ImGui::EndGroup();
}

} // namespace widgets
} // namespace enfusion

