/**
 * Enfusion Unpacker - Main Window Implementation
 */

#include "gui/main_window.hpp"
#include "gui/app.hpp"
#include "gui/theme.hpp"
#include "gui/text_viewer.hpp"
#include "enfusion/addon_extractor.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <Windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace enfusion {

MainWindow::MainWindow() {
    addon_browser_ = std::make_unique<AddonBrowser>();
    file_browser_ = std::make_unique<FileBrowser>();
    texture_viewer_ = std::make_unique<TextureViewer>();
    model_viewer_ = std::make_unique<ModelViewer>();
    text_viewer_ = std::make_unique<TextViewer>();
    export_dialog_ = std::make_unique<ExportDialog>();
    settings_dialog_ = std::make_unique<SettingsDialog>();

    // Initialize addon browser with saved paths
    auto& settings = App::instance().settings();
    
    // First try mods path (workshop addons), then game install path, then arma_addons_path
    if (!settings.mods_install_path.empty() && std::filesystem::exists(settings.mods_install_path)) {
        addon_browser_->set_addons_path(settings.mods_install_path);
    } else if (!settings.game_install_path.empty()) {
        std::filesystem::path addons_path = settings.game_install_path / "Addons";
        if (std::filesystem::exists(addons_path)) {
            addon_browser_->set_addons_path(addons_path);
        }
    } else if (!settings.arma_addons_path.empty() && std::filesystem::exists(settings.arma_addons_path)) {
        addon_browser_->set_addons_path(settings.arma_addons_path);
    }

    addon_browser_->on_addon_selected = [this](const std::filesystem::path& path) {
        current_addon_path_ = path;
        file_browser_->load(path);
        App::instance().set_status("Loaded addon: " + path.filename().string());
    };

    file_browser_->on_file_selected = [this](const std::string& file_path) {
        selected_file_path_ = file_path;

        auto ext = std::filesystem::path(file_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto extractor = file_browser_->extractor();
        if (!extractor) {
            App::instance().set_status("Error: No extractor available");
            return;
        }

        auto data = extractor->read_file(file_path);
        if (data.empty()) {
            App::instance().set_status("Error: Could not read file: " + file_path);
            return;
        }

        App::instance().set_status("Loaded: " + file_path + " (" + std::to_string(data.size()) + " bytes)");

        if (ext == ".edds" || ext == ".dds") {
            texture_viewer_->load_texture_data(data, file_path);
            show_texture_viewer_ = true;
        } else if (ext == ".xob") {
            // Set up texture loader for the model viewer
            model_viewer_->set_texture_loader([extractor](const std::string& path) -> std::vector<uint8_t> {
                return extractor->read_file(path);
            });
            // Provide list of available textures for texture browser
            model_viewer_->set_available_textures(file_browser_->get_texture_paths());
            model_viewer_->load_model_data(data, file_path);
            show_model_viewer_ = true;
        } else if (ext == ".c" || ext == ".et" || ext == ".conf" || ext == ".layout" ||
                   ext == ".xml" || ext == ".json" || ext == ".txt" || ext == ".cfg" ||
                   ext == ".meta" || ext == ".script") {
            text_viewer_->load_text_data(data, file_path);
            show_text_viewer_ = true;
        }
    };
    
    // Hook up export callback from context menu - exports the current mod pack
    file_browser_->on_export_requested = [this](const std::string& file_path) {
        if (!current_addon_path_.empty()) {
            export_dialog_->set_source(current_addon_path_);
            export_dialog_->init_from_settings();
            show_export_dialog_ = true;
        }
    };
}

MainWindow::~MainWindow() = default;

void MainWindow::render() {
    render_title_bar();
    render_menu_bar();
    render_dockspace();
    render_panels();
    render_dialogs();
    render_status_bar();
}

void MainWindow::render_title_bar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    GLFWwindow* window = App::instance().window();
    
    const float title_bar_height = 30.0f;
    const float button_width = 45.0f;
    const float menu_start_x = 150.0f;
    const float menu_end_x = 350.0f; // Approximate end of Help menu
    float buttons_x = viewport->Size.x - (button_width * 3);
    
    // Draw title bar background
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(viewport->Pos, 
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + title_bar_height),
        IM_COL32(26, 26, 30, 255));

    // Use standard main menu bar for menus - it handles popups correctly
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 7));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0, 0, 0, 0)); // Transparent - we draw our own bg
    
    if (ImGui::BeginMainMenuBar()) {
        // App title as non-interactive text
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "E");
        ImGui::SameLine(0, 4);
        ImGui::TextUnformatted("Enfusion Unpacker");
        ImGui::SameLine(0, 30);
        
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Addon...", "Ctrl+O")) open_addon_dialog();
            if (ImGui::MenuItem("Open Addons Folder...", "Ctrl+Shift+O")) open_addons_folder_dialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Export Selected...", "Ctrl+E", false, !current_addon_path_.empty())) {
                // Export the currently selected mod pack (entire addon)
                export_dialog_->set_source(current_addon_path_);
                export_dialog_->init_from_settings();
                show_export_dialog_ = true;
            }
            if (ImGui::MenuItem("Batch Export...", "Ctrl+Shift+E", false, !addon_browser_->get_addons().empty())) {
                // Batch export - show dialog with addon selection
                export_dialog_->set_batch_mode(addon_browser_->get_addons());
                export_dialog_->init_from_settings();
                show_export_dialog_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) App::instance().quit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Addon Browser", nullptr, &show_addon_browser_);
            ImGui::MenuItem("File Browser", nullptr, &show_file_browser_);
            ImGui::MenuItem("Texture Viewer", nullptr, &show_texture_viewer_);
            ImGui::MenuItem("Model Viewer", nullptr, &show_model_viewer_);
            ImGui::MenuItem("Text Viewer", nullptr, &show_text_viewer_);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) reset_layout_ = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Batch Extract...", nullptr, false, !addon_browser_->get_addons().empty())) {
                export_dialog_->set_batch_mode(addon_browser_->get_addons());
                export_dialog_->init_from_settings();
                show_export_dialog_ = true;
            }
            if (ImGui::MenuItem("Convert Textures...", nullptr, false, false)) {
                // TODO: Implement standalone texture converter dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Settings...", "Ctrl+,")) show_settings_ = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) show_about_ = true;
            ImGui::EndMenu();
        }
        
        // Window control buttons - position at right side
        float menu_bar_height = ImGui::GetFrameHeight();
        ImGui::SetCursorPosX(viewport->Size.x - 135); // 3 buttons * 45px
        
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        
        // Minimize button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        
        if (ImGui::Button("  _  ##min", ImVec2(45, menu_bar_height))) {
            glfwIconifyWindow(window);
        }
        ImGui::SameLine(0, 0);
        
        // Maximize/Restore button
        if (ImGui::Button(glfwGetWindowAttrib(window, GLFW_MAXIMIZED) ? "  \xE2\x96\xA1  ##max" : "  \xE2\x96\xA1  ##max", ImVec2(45, menu_bar_height))) {
            if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) 
                glfwRestoreWindow(window);
            else 
                glfwMaximizeWindow(window);
        }
        ImGui::PopStyleColor(4);
        ImGui::SameLine(0, 0);
        
        // Close button - red on hover
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        if (ImGui::Button("  X  ##close", ImVec2(45, menu_bar_height))) {
            App::instance().quit();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    
    // Window dragging - anywhere on title bar except interactive elements
    ImGuiIO& io = ImGui::GetIO();
    bool in_title_bar = io.MousePos.y >= viewport->Pos.y && 
                        io.MousePos.y < viewport->Pos.y + title_bar_height &&
                        io.MousePos.x >= viewport->Pos.x &&
                        io.MousePos.x < viewport->Pos.x + viewport->Size.x;
    
    // Check if we're hovering over any menu or button
    bool over_interactive = ImGui::IsAnyItemHovered();
    
    if (in_title_bar && !over_interactive && ImGui::IsMouseClicked(0)) {
        static double last_click = 0;
        double now = glfwGetTime();
        if (now - last_click < 0.3) {
            if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) glfwRestoreWindow(window);
            else glfwMaximizeWindow(window);
            last_click = 0;
        } else {
#ifdef _WIN32
            HWND hwnd = glfwGetWin32Window(window);
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
#endif
            last_click = now;
        }
    }
}

void MainWindow::render_menu_bar() {
    // Menu is now integrated into title bar
}

void MainWindow::render_dockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    float title_bar_height = 32.0f;
    float status_height = 24.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + title_bar_height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - title_bar_height - status_height));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpaceWindow", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    dockspace_id_ = ImGui::GetID("MainDockSpace");

    static bool first_frame = true;
    if (first_frame || reset_layout_) {
        setup_default_layout();
        reset_layout_ = false;
        first_frame = false;
    }

    ImGui::DockSpace(dockspace_id_, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    ImGui::End();
}

void MainWindow::setup_default_layout() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(dockspace_id_);
    ImGui::DockBuilderAddNode(dockspace_id_, ImGuiDockNodeFlags_DockSpace);

    float title_bar_height = 32.0f;
    float status_height = 24.0f;
    ImVec2 dock_size(viewport->Size.x, viewport->Size.y - title_bar_height - status_height);
    ImGui::DockBuilderSetNodeSize(dockspace_id_, dock_size);

    ImGuiID dock_left, dock_right;
    ImGui::DockBuilderSplitNode(dockspace_id_, ImGuiDir_Left, 0.25f, &dock_left, &dock_right);

    ImGuiID dock_left_top, dock_left_bottom;
    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.4f, &dock_left_top, &dock_left_bottom);

    ImGui::DockBuilderDockWindow("Addon Browser", dock_left_top);
    ImGui::DockBuilderDockWindow("File Browser", dock_left_bottom);
    ImGui::DockBuilderDockWindow("Texture Viewer", dock_right);
    ImGui::DockBuilderDockWindow("Model Viewer", dock_right);
    ImGui::DockBuilderDockWindow("Text Viewer", dock_right);

    ImGui::DockBuilderFinish(dockspace_id_);
}

void MainWindow::render_panels() {
    if (show_addon_browser_) {
        if (ImGui::Begin("Addon Browser", &show_addon_browser_)) {
            addon_browser_->render();
        }
        ImGui::End();
    }

    if (show_file_browser_) {
        if (ImGui::Begin("File Browser", &show_file_browser_)) {
            file_browser_->render();
        }
        ImGui::End();
    }

    if (show_texture_viewer_) {
        if (ImGui::Begin("Texture Viewer", &show_texture_viewer_)) {
            texture_viewer_->render();
        }
        ImGui::End();
    }

    if (show_model_viewer_) {
        if (ImGui::Begin("Model Viewer", &show_model_viewer_)) {
            model_viewer_->render();
        }
        ImGui::End();
    }

    if (show_text_viewer_) {
        if (ImGui::Begin("Text Viewer", &show_text_viewer_)) {
            text_viewer_->render();
        }
        ImGui::End();
    }
}

void MainWindow::render_dialogs() {
    if (show_export_dialog_) {
        export_dialog_->render(&show_export_dialog_);
    }

    if (show_settings_) {
        settings_dialog_->render(&show_settings_);
    }

    if (show_about_) {
        render_about_dialog();
    }
    
    // Loading overlay
    if (App::instance().is_loading()) {
        ImGui::OpenPopup("Loading...");
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        
        if (ImGui::BeginPopupModal("Loading...", nullptr, 
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            
            ImGui::Text("%s", App::instance().loading_message().c_str());
            ImGui::Spacing();
            
            // Animated spinner
            float time = static_cast<float>(ImGui::GetTime());
            int dots = static_cast<int>(time * 2.0f) % 4;
            std::string spinner = std::string(dots, '.') + std::string(3 - dots, ' ');
            ImGui::Text("Please wait%s", spinner.c_str());
            
            ImGui::EndPopup();
        }
    }
}

void MainWindow::render_about_dialog() {
    ImGui::OpenPopup("About Enfusion Unpacker");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 250));

    if (ImGui::BeginPopupModal("About Enfusion Unpacker", &show_about_, ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Enfusion Unpacker");
        ImGui::Text("Version 1.0.0");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("A professional tool for extracting and viewing Enfusion game assets.");
        ImGui::Spacing();

        ImGui::Text("Features:");
        ImGui::BulletText("PAK archive extraction");
        ImGui::BulletText("RDB database parsing");
        ImGui::BulletText("XOB 3D model viewing");
        ImGui::BulletText("EDDS texture viewing");
        ImGui::BulletText("OBJ/PNG export");

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button("Close", ImVec2(120, 0))) {
            show_about_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void MainWindow::render_status_bar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - 24.0f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 24.0f));
    ImGui::SetNextWindowViewport(viewport->ID);  // Keep attached to main viewport

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));

    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        ImGui::Text("%s", App::instance().status().c_str());

        if (!selected_file_path_.empty()) {
            float text_width = ImGui::CalcTextSize(selected_file_path_.c_str()).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - text_width - 16.0f);
            ImGui::TextDisabled("%s", selected_file_path_.c_str());
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void MainWindow::open_addon_dialog() {
#ifdef _WIN32
    char filename[MAX_PATH] = {0};

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "PAK Files\0*.pak\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Open Addon";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        current_addon_path_ = filename;
        file_browser_->load(std::filesystem::path(filename).parent_path());
        App::instance().set_status("Loaded: " + current_addon_path_.filename().string());
    }
#endif
}

void MainWindow::open_addons_folder_dialog() {
#ifdef _WIN32
    char path[MAX_PATH] = {0};

    BROWSEINFOA bi = {};
    bi.lpszTitle = "Select Addons Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl && SHGetPathFromIDListA(pidl, path)) {
        addon_browser_->set_addons_path(path);
        App::instance().settings().arma_addons_path = path;
        CoTaskMemFree(pidl);
    }
#endif
}

} // namespace enfusion
