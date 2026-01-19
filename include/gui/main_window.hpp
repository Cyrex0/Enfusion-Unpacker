/**
 * Enfusion Unpacker - Main Window
 */

#pragma once

#include "enfusion/types.hpp"
#include "gui/addon_browser.hpp"
#include "gui/file_browser.hpp"
#include "gui/texture_viewer.hpp"
#include "gui/model_viewer.hpp"
#include "gui/export_dialog.hpp"
#include "gui/settings_dialog.hpp"
#include <imgui.h>
#include <memory>
#include <vector>
#include <string>

namespace enfusion {

class AddonExtractor;

/**
 * Main window with dockable panels.
 */
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    void render();

private:
    void render_menu_bar();
    void render_status_bar();
    void render_dockspace();
    void render_panels();
    void render_dialogs();
    void render_about_dialog();
    void setup_default_layout();

    void open_addon_dialog();
    void open_addons_folder_dialog();

    // Panels
    std::unique_ptr<AddonBrowser> addon_browser_;
    std::unique_ptr<FileBrowser> file_browser_;
    std::unique_ptr<TextureViewer> texture_viewer_;
    std::unique_ptr<ModelViewer> model_viewer_;
    std::unique_ptr<ExportDialog> export_dialog_;
    std::unique_ptr<SettingsDialog> settings_dialog_;

    // Current addon
    fs::path current_addon_path_;
    std::string selected_file_path_;  // Path within PAK

    // UI state
    bool show_addon_browser_ = true;
    bool show_file_browser_ = true;
    bool show_texture_viewer_ = true;
    bool show_model_viewer_ = true;
    bool show_export_dialog_ = false;
    bool show_settings_ = false;
    bool show_about_ = false;
    bool reset_layout_ = false;
    ImGuiID dockspace_id_ = 0;
};

} // namespace enfusion
