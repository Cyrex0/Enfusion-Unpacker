/**
 * Enfusion Unpacker - Main Window Implementation
 */

#include "gui/main_window.hpp"
#include "gui/app.hpp"
#include "gui/theme.hpp"
#include "enfusion/addon_extractor.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#ifdef _WIN32
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
    export_dialog_ = std::make_unique<ExportDialog>();
    settings_dialog_ = std::make_unique<SettingsDialog>();

    // Wire up callbacks
    addon_browser_->on_addon_selected = [this](const std::filesystem::path& path) {
        current_addon_path_ = path;
        file_browser_->load(path);
        App::instance().set_status("Loaded addon: " + path.filename().string());
    };

    file_browser_->on_file_selected = [this](const std::string& file_path) {
        selected_file_path_ = file_path;
        
        auto ext = std::filesystem::path(file_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        // Read file data from PAK via extractor
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
            model_viewer_->load_model_data(data, file_path);
            show_model_viewer_ = true;
        }
    };
}

MainWindow::~MainWindow() = default;

void MainWindow::render() {
    render_menu_bar();
    render_dockspace();
    render_panels();
    render_dialogs();
    render_status_bar();
}

void MainWindow::render_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Addon...", "Ctrl+O")) {
                open_addon_dialog();
            }
            if (ImGui::MenuItem("Open Addons Folder...", "Ctrl+Shift+O")) {
                open_addons_folder_dialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export Selected...", "Ctrl+E", false, !selected_file_path_.empty())) {
                show_export_dialog_ = true;
            }
            if (ImGui::MenuItem("Export All...", "Ctrl+Shift+E", false, !current_addon_path_.empty())) {
                export_dialog_->set_batch_mode(true);
                show_export_dialog_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                App::instance().quit();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Addon Browser", nullptr, &show_addon_browser_);
            ImGui::MenuItem("File Browser", nullptr, &show_file_browser_);
            ImGui::MenuItem("Texture Viewer", nullptr, &show_texture_viewer_);
            ImGui::MenuItem("Model Viewer", nullptr, &show_model_viewer_);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                reset_layout_ = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Batch Extract...")) {
                // TODO: Batch extraction dialog
            }
            if (ImGui::MenuItem("Convert Textures...")) {
                // TODO: Texture conversion dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Settings...", "Ctrl+,")) {
                show_settings_ = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                show_about_ = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void MainWindow::render_dockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    float menu_height = ImGui::GetFrameHeight();
    float status_height = 24.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - menu_height - status_height));
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
    
    // Check if we need to set up the layout
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
    
    float menu_height = ImGui::GetFrameHeight();
    float status_height = 24.0f;
    ImVec2 dock_size(viewport->Size.x, viewport->Size.y - menu_height - status_height);
    ImGui::DockBuilderSetNodeSize(dockspace_id_, dock_size);

    // Split left panel (25%) from right panel (75%)
    ImGuiID dock_left, dock_right;
    ImGui::DockBuilderSplitNode(dockspace_id_, ImGuiDir_Left, 0.25f, &dock_left, &dock_right);

    // Split left into top (Addon Browser) and bottom (File Browser)
    ImGuiID dock_left_top, dock_left_bottom;
    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.4f, &dock_left_top, &dock_left_bottom);

    // Right side is for viewers (tabbed)
    ImGui::DockBuilderDockWindow("Addon Browser", dock_left_top);
    ImGui::DockBuilderDockWindow("File Browser", dock_left_bottom);
    ImGui::DockBuilderDockWindow("Texture Viewer", dock_right);
    ImGui::DockBuilderDockWindow("Model Viewer", dock_right);

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

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));

    if (ImGui::Begin("StatusBar", nullptr, flags)) {
        ImGui::Text("%s", App::instance().status().c_str());

        // Show file info on the right
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
