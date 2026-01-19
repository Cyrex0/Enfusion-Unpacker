/**
 * Enfusion Unpacker - Settings Dialog
 */

#pragma once

#include "enfusion/types.hpp"
#include <filesystem>

namespace enfusion {

/**
 * Application settings dialog.
 */
class SettingsDialog {
public:
    SettingsDialog() = default;
    ~SettingsDialog() = default;

    void render(bool* open);

private:
    void render_content();
    void render_general_tab(AppSettings& settings);
    void render_appearance_tab(AppSettings& settings);
    void render_export_tab(AppSettings& settings);
    void render_paths_tab(AppSettings& settings);
    
    void apply_settings();
    void browse_folder(std::filesystem::path& path);
};

} // namespace enfusion
