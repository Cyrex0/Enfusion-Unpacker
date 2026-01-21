/**
 * Enfusion Unpacker - Export Dialog Implementation
 */

#include "gui/export_dialog.hpp"
#include "gui/app.hpp"
#include "gui/widgets.hpp"
#include "enfusion/addon_extractor.hpp"

#include <imgui.h>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#include <shlobj.h>
#endif

namespace enfusion {

void ExportDialog::render(bool* open) {
    if (!*open) return;

    ImGui::OpenPopup("Export");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 400));

    if (ImGui::BeginPopupModal("Export", open, ImGuiWindowFlags_NoResize)) {
        render_content();
        ImGui::EndPopup();
    }
}

void ExportDialog::render_content() {
    if (exporting_) {
        render_progress();
        return;
    }

    // Source info
    ImGui::Text("Source:");
    ImGui::SameLine();
    if (!selected_file_.empty() && !batch_mode_) {
        // Single file export
        ImGui::TextDisabled("%s", selected_file_.c_str());
    } else {
        ImGui::TextDisabled("%s", source_path_.string().c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Output path
    char output_buffer[512] = {0};
    std::string output_str = output_path_.string();
    strncpy(output_buffer, output_str.c_str(), sizeof(output_buffer) - 1);

    ImGui::Text("Output Folder:");
    ImGui::SetNextItemWidth(-80);
    if (ImGui::InputText("##OutputPath", output_buffer, sizeof(output_buffer))) {
        output_path_ = output_buffer;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        browse_output_folder();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Export options
    ImGui::Text("Options:");
    ImGui::Spacing();

    ImGui::Checkbox("Convert textures to PNG", &convert_textures_);
    widgets::HelpMarker("Convert EDDS textures to standard PNG format");

    ImGui::Checkbox("Convert meshes to OBJ/FBX", &convert_meshes_);
    widgets::HelpMarker("Convert XOB meshes to OBJ or FBX format");
    
    if (convert_meshes_) {
        ImGui::Indent();
        
        // Format selection
        const char* format_items[] = { "OBJ (Wavefront)", "FBX (Autodesk)" };
        int current_format = (mesh_format_ == ExportFormat::FBX) ? 1 : 0;
        if (ImGui::Combo("Mesh Format", &current_format, format_items, 2)) {
            mesh_format_ = (current_format == 1) ? ExportFormat::FBX : ExportFormat::OBJ;
        }
        
        ImGui::Checkbox("Export Normals", &export_normals_);
        ImGui::Checkbox("Export UVs", &export_uvs_);
        ImGui::Checkbox("Export Materials", &export_materials_);
        
        ImGui::Unindent();
    }

    ImGui::Checkbox("Keep original files", &keep_originals_);
    widgets::HelpMarker("Keep the original game files alongside converted ones");

    ImGui::Checkbox("Preserve folder structure", &preserve_structure_);
    widgets::HelpMarker("Maintain the original folder hierarchy in output");

    if (batch_mode_) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("File Types to Export:");
        ImGui::Checkbox("Textures (.edds, .dds)", &export_textures_);
        ImGui::Checkbox("Meshes (.xob)", &export_meshes_);
        ImGui::Checkbox("All other files", &export_others_);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Buttons
    float button_width = 120.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_width = button_width * 2 + spacing;

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total_width) / 2.0f);

    if (ImGui::Button("Export", ImVec2(button_width, 0))) {
        start_export();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(button_width, 0))) {
        ImGui::CloseCurrentPopup();
    }
}

void ExportDialog::render_progress() {
    ImGui::Text("Exporting...");
    ImGui::Spacing();

    ImGui::ProgressBar(progress_, ImVec2(-1, 0), "");

    ImGui::Spacing();
    ImGui::TextDisabled("Current: %s", current_file_.c_str());
    ImGui::TextDisabled("Files: %d / %d", files_processed_, total_files_);

    ImGui::Spacing();

    if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
        cancel_requested_ = true;
    }

    // Check if export finished
    if (export_finished_) {
        exporting_ = false;
        export_finished_ = false;

        if (error_message_.empty()) {
            App::instance().set_status("Export completed successfully");
            ImGui::CloseCurrentPopup();
        } else {
            // Show error
            App::instance().set_status("Export failed: " + error_message_);
        }
    }
}

void ExportDialog::start_export() {
    if (output_path_.empty()) {
        error_message_ = "Please select an output folder";
        return;
    }

    exporting_ = true;
    progress_ = 0.0f;
    files_processed_ = 0;
    total_files_ = 0;
    current_file_.clear();
    error_message_.clear();
    cancel_requested_ = false;
    export_finished_ = false;

    // Start export in background thread
    std::thread([this]() {
        try {
            AddonExtractor extractor;
            
            if (extractor.load(source_path_)) {
                extractor.extract_all(output_path_, 
                    [this](const std::string& file, size_t current, size_t total) {
                        current_file_ = file;
                        files_processed_ = static_cast<int>(current);
                        total_files_ = static_cast<int>(total);
                        progress_ = static_cast<float>(current) / static_cast<float>(total);
                        return !cancel_requested_;
                    }
                );
            } else {
                error_message_ = "Failed to load addon";
            }

        } catch (const std::exception& e) {
            error_message_ = e.what();
        }

        export_finished_ = true;
    }).detach();
}

void ExportDialog::browse_output_folder() {
#ifdef _WIN32
    char path[MAX_PATH] = {0};

    BROWSEINFOA bi = {};
    bi.lpszTitle = "Select Output Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl && SHGetPathFromIDListA(pidl, path)) {
        output_path_ = path;
        CoTaskMemFree(pidl);
    }
#endif
}

} // namespace enfusion
