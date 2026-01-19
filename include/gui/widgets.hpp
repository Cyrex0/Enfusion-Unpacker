/**
 * Enfusion Unpacker - Custom Widgets
 */

#pragma once

#include <string>
#include <filesystem>
#include <functional>
#include <imgui.h>

namespace enfusion {
namespace widgets {

/**
 * Styled button with icon.
 */
bool IconButton(const char* icon, const char* tooltip = nullptr, const ImVec2& size = ImVec2(0, 0));

/**
 * Toggle button.
 */
bool ToggleButton(const char* label, bool* value, const ImVec2& size = ImVec2(0, 0));

/**
 * Search input with clear button.
 */
bool SearchInput(const char* id, std::string& text, size_t max_length = 256);

/**
 * File path input with browse button.
 */
bool PathInput(const char* label, std::filesystem::path& path, const char* filter = nullptr);

/**
 * Spinner for loading states.
 */
void Spinner(const char* label, float radius = 10.0f, float thickness = 3.0f, ImU32 color = 0xFFFFFFFF);

/**
 * Progress bar with label.
 */
bool ProgressBar(const char* label, float progress, const ImVec2& size = ImVec2(-1, 0));

/**
 * Property row for info panels.
 */
void InfoRow(const char* label, const char* value);

/**
 * Tooltip helper.
 */
void HelpMarker(const char* desc);

/**
 * Separator with optional label.
 */
void Separator(const char* text = nullptr);

/**
 * Group box.
 */
void BeginGroupBox(const char* label);
void EndGroupBox();

} // namespace widgets
} // namespace enfusion
