/**
 * Enfusion Unpacker - Theme Implementation
 */

#include "gui/theme.hpp"
#include <imgui.h>

namespace enfusion {

void apply_theme(Theme theme) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    // Rounding
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    
    // Sizing
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;
    
    // Borders
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    
    switch (theme) {
        case Theme::Dark:
        default: {
            // Modern dark theme
            ImVec4 bg = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
            ImVec4 bgLight = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
            ImVec4 bgDark = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
            ImVec4 accent = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
            ImVec4 accentHover = ImVec4(0.36f, 0.69f, 1.00f, 1.00f);
            ImVec4 accentActive = ImVec4(0.20f, 0.50f, 0.85f, 1.00f);
            ImVec4 text = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
            ImVec4 textDim = ImVec4(0.60f, 0.60f, 0.65f, 1.00f);
            
            colors[ImGuiCol_WindowBg] = bg;
            colors[ImGuiCol_ChildBg] = bgDark;
            colors[ImGuiCol_PopupBg] = bgLight;
            colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
            colors[ImGuiCol_TitleBg] = bgDark;
            colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
            colors[ImGuiCol_TitleBgCollapsed] = bgDark;
            colors[ImGuiCol_MenuBarBg] = bgDark;
            colors[ImGuiCol_ScrollbarBg] = bgDark;
            colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
            colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.43f, 1.00f);
            colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.50f, 0.53f, 1.00f);
            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = accentActive;
            colors[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
            colors[ImGuiCol_ButtonActive] = accent;
            colors[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.50f);
            colors[ImGuiCol_HeaderActive] = accent;
            colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
            colors[ImGuiCol_SeparatorHovered] = accentHover;
            colors[ImGuiCol_SeparatorActive] = accent;
            colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
            colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
            colors[ImGuiCol_ResizeGripActive] = accent;
            colors[ImGuiCol_Tab] = bgLight;
            colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
            colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.45f, 0.75f, 1.00f);
            colors[ImGuiCol_TabUnfocused] = bgDark;
            colors[ImGuiCol_TabUnfocusedActive] = bgLight;
            colors[ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
            colors[ImGuiCol_DockingEmptyBg] = bgDark;
            colors[ImGuiCol_PlotLines] = accent;
            colors[ImGuiCol_PlotLinesHovered] = accentHover;
            colors[ImGuiCol_PlotHistogram] = accent;
            colors[ImGuiCol_PlotHistogramHovered] = accentHover;
            colors[ImGuiCol_TableHeaderBg] = bgDark;
            colors[ImGuiCol_TableBorderStrong] = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
            colors[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
            colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
            colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
            colors[ImGuiCol_DragDropTarget] = accent;
            colors[ImGuiCol_NavHighlight] = accent;
            colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
            colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
            colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
            colors[ImGuiCol_Text] = text;
            colors[ImGuiCol_TextDisabled] = textDim;
            break;
        }
        
        case Theme::Light: {
            ImGui::StyleColorsLight();
            break;
        }
        
        case Theme::DarkBlue: {
            // Dark blue theme
            ImVec4 bg = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
            ImVec4 accent = ImVec4(0.20f, 0.55f, 0.90f, 1.00f);
            
            colors[ImGuiCol_WindowBg] = bg;
            colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
            colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
            colors[ImGuiCol_Border] = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.18f, 0.24f, 1.00f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
            colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
            colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
            colors[ImGuiCol_Button] = ImVec4(0.15f, 0.20f, 0.28f, 1.00f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.28f, 0.40f, 1.00f);
            colors[ImGuiCol_ButtonActive] = accent;
            colors[ImGuiCol_Header] = ImVec4(0.15f, 0.20f, 0.28f, 1.00f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.30f, 0.45f, 1.00f);
            colors[ImGuiCol_HeaderActive] = accent;
            colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
            colors[ImGuiCol_TabHovered] = accent;
            colors[ImGuiCol_TabActive] = ImVec4(0.15f, 0.35f, 0.60f, 1.00f);
            colors[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
            colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            break;
        }
        
        case Theme::Purple: {
            // Purple accent theme
            ImVec4 bg = ImVec4(0.11f, 0.10f, 0.14f, 1.00f);
            ImVec4 accent = ImVec4(0.60f, 0.40f, 0.90f, 1.00f);
            
            colors[ImGuiCol_WindowBg] = bg;
            colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.08f, 0.11f, 1.00f);
            colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.12f, 0.18f, 1.00f);
            colors[ImGuiCol_Border] = ImVec4(0.25f, 0.22f, 0.32f, 1.00f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.14f, 0.22f, 1.00f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.18f, 0.28f, 1.00f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.22f, 0.32f, 1.00f);
            colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.08f, 0.11f, 1.00f);
            colors[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.10f, 0.14f, 1.00f);
            colors[ImGuiCol_Button] = ImVec4(0.22f, 0.18f, 0.30f, 1.00f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.24f, 0.42f, 1.00f);
            colors[ImGuiCol_ButtonActive] = accent;
            colors[ImGuiCol_Header] = ImVec4(0.22f, 0.18f, 0.30f, 1.00f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.30f, 0.55f, 1.00f);
            colors[ImGuiCol_HeaderActive] = accent;
            colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.12f, 0.18f, 1.00f);
            colors[ImGuiCol_TabHovered] = accent;
            colors[ImGuiCol_TabActive] = ImVec4(0.40f, 0.28f, 0.60f, 1.00f);
            colors[ImGuiCol_Text] = ImVec4(0.92f, 0.90f, 0.95f, 1.00f);
            colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.60f, 1.00f);
            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            break;
        }
    }
}

void setup_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    
    // Use default font for now, can add custom fonts later
    io.FontGlobalScale = scale;
    
    // Add icons font here if needed
    // io.Fonts->AddFontFromFileTTF("fonts/fa-solid-900.ttf", 14.0f * scale, ...);
}

void get_accent_color(float& r, float& g, float& b) {
    ImVec4 col = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
    r = col.x;
    g = col.y;
    b = col.z;
}

} // namespace enfusion
