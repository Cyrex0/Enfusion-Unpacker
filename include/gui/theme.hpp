/**
 * Enfusion Unpacker - Theme & Styling
 */

#pragma once

namespace enfusion {

/**
 * Available themes.
 */
enum class Theme {
    Dark,
    Light,
    DarkBlue,
    Purple
};

/**
 * Apply ImGui theme.
 */
void apply_theme(Theme theme);

/**
 * Setup custom fonts.
 */
void setup_fonts(float scale = 1.0f);

/**
 * Get accent color for current theme.
 */
void get_accent_color(float& r, float& g, float& b);

} // namespace enfusion
