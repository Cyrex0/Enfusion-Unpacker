/**
 * Enfusion Unpacker - Settings Dialog Implementation
 */

#include "gui/settings_dialog.hpp"
#include "gui/app.hpp"
#include "gui/theme.hpp"
#include "gui/widgets.hpp"
#include "enfusion/pak_manager.hpp"
#include "enfusion/pak_index.hpp"

#include <imgui.h>

#ifdef _WIN32
#include <Windows.h>
#include <shlobj.h>
#endif

namespace enfusion {

void SettingsDialog::render(bool* open) {
    if (!*open) return;
    
    ImGui::OpenPopup("Settings");
    
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 450));
    
    if (ImGui::BeginPopupModal("Settings", open, ImGuiWindowFlags_NoResize)) {
        render_content();
        ImGui::EndPopup();
    }
}

void SettingsDialog::render_content() {
    auto& settings = App::instance().settings();
    
    if (ImGui::BeginTabBar("SettingsTabs")) {
        if (ImGui::BeginTabItem("General")) {
            render_general_tab(settings);
            ImGui::EndTabItem();
        }
        
        if (ImGui::BeginTabItem("Appearance")) {
            render_appearance_tab(settings);
            ImGui::EndTabItem();
        }
        
        if (ImGui::BeginTabItem("Export")) {
            render_export_tab(settings);
            ImGui::EndTabItem();
        }
        
        if (ImGui::BeginTabItem("Paths")) {
            render_paths_tab(settings);
            ImGui::EndTabItem();
        }
        
        ImGui::EndTabBar();
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Buttons
    float button_width = 100.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - button_width * 2 - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().WindowPadding.x);
    
    if (ImGui::Button("Apply", ImVec2(button_width, 0))) {
        apply_settings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(button_width, 0))) {
        ImGui::CloseCurrentPopup();
    }
}

void SettingsDialog::render_general_tab(AppSettings& settings) {
    ImGui::Spacing();
    
    ImGui::Text("Default Addons Path:");
    char addons_path_buffer[512] = {0};
    std::string addons_str = settings.arma_addons_path.string();
    strncpy(addons_path_buffer, addons_str.c_str(), sizeof(addons_path_buffer) - 1);
    
    ImGui::SetNextItemWidth(-80);
    if (ImGui::InputText("##AddonsPath", addons_path_buffer, sizeof(addons_path_buffer))) {
        settings.arma_addons_path = addons_path_buffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##Addons")) {
        browse_folder(settings.arma_addons_path);
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Text("Startup Options:");
    static bool load_last_addon = false;
    ImGui::Checkbox("Load last addon on startup", &load_last_addon);
    
    static bool check_updates = true;
    ImGui::Checkbox("Check for updates on startup", &check_updates);
}

void SettingsDialog::render_appearance_tab(AppSettings& settings) {
    ImGui::Spacing();
    
    ImGui::Text("Theme:");
    const char* themes[] = {"Dark", "Light", "Dark Blue", "Purple"};
    
    if (ImGui::Combo("##Theme", &settings.theme, themes, IM_ARRAYSIZE(themes))) {
        apply_theme(static_cast<Theme>(settings.theme));
    }
    
    ImGui::Spacing();
    
    ImGui::Text("UI Scale:");
    if (ImGui::SliderFloat("##Scale", &settings.ui_scale, 0.8f, 2.0f, "%.1f")) {
        setup_fonts(settings.ui_scale);
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Text("Model Viewer:");
    static float grid_size = 10.0f;
    ImGui::SliderFloat("Grid Size", &grid_size, 1.0f, 50.0f);
    
    static bool antialiasing = true;
    ImGui::Checkbox("Antialiasing", &antialiasing);
    
    ImGui::Spacing();
    
    ImGui::Text("Texture Viewer:");
    static bool show_checkerboard = true;
    ImGui::Checkbox("Show checkerboard for transparency", &show_checkerboard);
}

void SettingsDialog::render_export_tab(AppSettings& settings) {
    ImGui::Spacing();
    
    ImGui::Text("Default Export Settings:");
    ImGui::Spacing();
    
    ImGui::Checkbox("Convert textures to PNG by default", &settings.convert_textures_to_png);
    widgets::HelpMarker("Automatically convert EDDS textures to PNG format when exporting");
    
    ImGui::Checkbox("Convert meshes to OBJ by default", &settings.convert_meshes_to_obj);
    widgets::HelpMarker("Automatically convert XOB meshes to Wavefront OBJ format when exporting");
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Text("PNG Export Options:");
    static int png_compression = 6;
    ImGui::SliderInt("Compression Level", &png_compression, 0, 9);
    
    ImGui::Spacing();
    
    ImGui::Text("OBJ Export Options:");
    static bool export_normals = true;
    ImGui::Checkbox("Export normals", &export_normals);
    
    static bool export_uvs = true;
    ImGui::Checkbox("Export UVs", &export_uvs);
    
    static bool export_materials = true;
    ImGui::Checkbox("Export materials (MTL)", &export_materials);
    
    static bool flip_uvs = true;
    ImGui::Checkbox("Flip UVs vertically", &flip_uvs);
}

void SettingsDialog::render_paths_tab(AppSettings& settings) {
    ImGui::Spacing();
    
    // Game Install Path
    ImGui::Text("Game Install Path:");
    widgets::HelpMarker("Path to the game installation folder (e.g., C:\\Program Files\\Steam\\steamapps\\common\\Arma Reforger)\nContains the 'Addons' folder with core game PAKs.");
    
    char game_path_buffer[512] = {0};
    std::string game_str = settings.game_install_path.string();
    strncpy(game_path_buffer, game_str.c_str(), sizeof(game_path_buffer) - 1);
    
    ImGui::SetNextItemWidth(-80);
    if (ImGui::InputText("##GameInstallPath", game_path_buffer, sizeof(game_path_buffer))) {
        settings.game_install_path = game_path_buffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##GameInstall")) {
        browse_folder(settings.game_install_path);
        // Update PakManager
        auto& pak_mgr = PakManager::instance();
        pak_mgr.set_game_path(settings.game_install_path);
        pak_mgr.scan_available_paks();
    }
    
    // Validate game path
    if (!settings.game_install_path.empty()) {
        std::filesystem::path addons = settings.game_install_path / "Addons";
        if (std::filesystem::exists(addons)) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "  Valid (Addons folder found)");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.2f, 1.0f), "  Warning: No Addons folder found");
        }
    }
    
    ImGui::Spacing();
    
    // Mods Install Path
    ImGui::Text("Mods Install Path:");
    widgets::HelpMarker("Path to the mods folder (e.g., Workshop folder or custom mods location)\nContains mod PAK files for lazy loading.");
    
    char mods_path_buffer[512] = {0};
    std::string mods_str = settings.mods_install_path.string();
    strncpy(mods_path_buffer, mods_str.c_str(), sizeof(mods_path_buffer) - 1);
    
    ImGui::SetNextItemWidth(-80);
    if (ImGui::InputText("##ModsInstallPath", mods_path_buffer, sizeof(mods_path_buffer))) {
        settings.mods_install_path = mods_path_buffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##ModsInstall")) {
        browse_folder(settings.mods_install_path);
        // Update PakManager
        auto& pak_mgr = PakManager::instance();
        pak_mgr.set_mods_path(settings.mods_install_path);
        pak_mgr.scan_available_paks();
    }
    
    // Validate mods path
    if (!settings.mods_install_path.empty()) {
        if (std::filesystem::exists(settings.mods_install_path)) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "  Valid");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.2f, 1.0f), "  Path does not exist");
        }
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Show PAK stats
    auto& pak_mgr = PakManager::instance();
    auto& pak_index = PakIndex::instance();
    
    ImGui::Text("PAK Status:");
    ImGui::Text("  Available PAKs: %zu", pak_mgr.available_pak_count());
    ImGui::Text("  Loaded PAKs: %zu", pak_mgr.loaded_pak_count());
    
    ImGui::Spacing();
    ImGui::Text("File Index:");
    if (pak_index.is_ready()) {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), 
                          "  Indexed: %zu files in %zu PAKs", 
                          pak_index.total_files(), pak_index.total_paks());
    } else {
        ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.2f, 1.0f), "  Index not built");
    }
    
    if (ImGui::Button("Scan for PAKs")) {
        pak_mgr.set_game_path(settings.game_install_path);
        pak_mgr.set_mods_path(settings.mods_install_path);
        pak_mgr.scan_available_paks();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rebuild Index")) {
        // Set paths
        pak_index.set_game_path(settings.game_install_path);
        pak_index.set_mods_path(settings.mods_install_path);
        
        // Show that we're rebuilding
        App::instance().set_loading(true, "Rebuilding file index...");
        
        // Build index (this may take a while for many PAKs)
        pak_index.build_index([](const std::string& pak_name, int current, int total) {
            App::instance().set_loading(true, 
                "Indexing: " + pak_name + " (" + std::to_string(current) + "/" + std::to_string(total) + ")");
        });
        
        App::instance().set_loading(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Common PAKs")) {
        pak_mgr.load_common_paks();
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Text("Default Export Path:");
    char export_path_buffer[512] = {0};
    std::string export_str = settings.last_export_path.string();
    strncpy(export_path_buffer, export_str.c_str(), sizeof(export_path_buffer) - 1);
    
    ImGui::SetNextItemWidth(-80);
    if (ImGui::InputText("##ExportPath", export_path_buffer, sizeof(export_path_buffer))) {
        settings.last_export_path = export_path_buffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##Export")) {
        browse_folder(settings.last_export_path);
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Text("Recent Paths:");
    ImGui::BeginChild("RecentPaths", ImVec2(0, 100), true);
    
    // Show recent paths
    static std::vector<std::string> recent_paths;
    if (recent_paths.empty()) {
        ImGui::TextDisabled("No recent paths");
    } else {
        for (size_t i = 0; i < recent_paths.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s", recent_paths[i].c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 60);
            if (ImGui::SmallButton("Remove")) {
                recent_paths.erase(recent_paths.begin() + i);
            }
            ImGui::PopID();
        }
    }
    
    ImGui::EndChild();
    
    if (ImGui::Button("Clear Recent Paths")) {
        recent_paths.clear();
    }
}

void SettingsDialog::apply_settings() {
    auto& settings = App::instance().settings();
    apply_theme(static_cast<Theme>(settings.theme));
    App::instance().set_status("Settings applied");
}

void SettingsDialog::browse_folder(std::filesystem::path& path) {
#ifdef _WIN32
    char folder[MAX_PATH] = {0};
    
    BROWSEINFOA bi = {};
    bi.lpszTitle = "Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl && SHGetPathFromIDListA(pidl, folder)) {
        path = folder;
        CoTaskMemFree(pidl);
    }
#endif
}

} // namespace enfusion
