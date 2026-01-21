/**
 * Enfusion Unpacker - Export Dialog Implementation
 */

#include "gui/export_dialog.hpp"
#include "gui/app.hpp"
#include "gui/widgets.hpp"
#include "enfusion/addon_extractor.hpp"
#include "enfusion/edds_converter.hpp"
#include "enfusion/mesh_converter.hpp"
#include "enfusion/dds_loader.hpp"
#include "enfusion/files.hpp"

#include <imgui.h>
#include <thread>
#include <algorithm>
#include <fstream>
#include <cstring>

#ifdef _WIN32
#include <Windows.h>
#include <shlobj.h>
#endif

namespace enfusion {

void ExportDialog::render(bool* open) {
    if (!*open) return;
    
    dialog_open_ = open;  // Store for use in render_progress

    const char* title = batch_mode_ ? "Batch Export" : "Export Mod Pack";
    ImGui::OpenPopup(title);

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    
    // Larger window for batch mode to show addon list
    ImVec2 size = batch_mode_ ? ImVec2(600, 550) : ImVec2(500, 450);
    ImGui::SetNextWindowSize(size);

    if (ImGui::BeginPopupModal(title, open, ImGuiWindowFlags_NoResize)) {
        render_content(open);
        ImGui::EndPopup();
    }
}

void ExportDialog::render_content(bool* open) {
    if (exporting_) {
        render_progress();
        return;
    }

    if (batch_mode_) {
        // Batch export mode - show addon selection
        ImGui::Text("Select Mod Packs to Export:");
        render_addon_selection();
    } else {
        // Single mod pack export
        ImGui::Text("Export Mod Pack:");
        std::string addon_name = source_path_.filename().string();
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "  %s", addon_name.c_str());
        ImGui::TextDisabled("  %s", source_path_.string().c_str());
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

    // File type filters
    ImGui::Text("File Types to Export:");
    ImGui::Checkbox("Textures (.edds, .dds)", &export_textures_);
    ImGui::Checkbox("Meshes (.xob)", &export_meshes_);
    ImGui::Checkbox("All other files", &export_others_);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Export options
    ImGui::Text("Conversion Options:");
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Buttons
    float button_width = 120.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_width = button_width * 2 + spacing;

    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total_width) / 2.0f);

    // Validate before enabling export button
    bool can_export = !output_path_.empty();
    if (batch_mode_) {
        // Check if at least one addon is selected
        bool any_selected = false;
        for (const auto& addon : batch_addons_) {
            if (addon.selected) {
                any_selected = true;
                break;
            }
        }
        can_export = can_export && any_selected;
    } else {
        can_export = can_export && !source_path_.empty();
    }

    if (!can_export) {
        ImGui::BeginDisabled();
    }
    
    if (ImGui::Button("Export", ImVec2(button_width, 0))) {
        start_export();
    }
    
    if (!can_export) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(button_width, 0))) {
        if (open) *open = false;
        ImGui::CloseCurrentPopup();
    }
}

void ExportDialog::render_addon_selection() {
    // Search filter
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##AddonSearch", "Search mods...", addon_search_, sizeof(addon_search_));
    
    ImGui::Spacing();
    
    // Select All / Deselect All buttons
    if (ImGui::Button("Select All")) {
        for (auto& addon : batch_addons_) {
            addon.selected = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Deselect All")) {
        for (auto& addon : batch_addons_) {
            addon.selected = false;
        }
    }
    ImGui::SameLine();
    
    // Count selected
    int selected_count = 0;
    size_t selected_size = 0;
    for (const auto& addon : batch_addons_) {
        if (addon.selected) {
            selected_count++;
            selected_size += addon.total_size;
        }
    }
    ImGui::TextDisabled("(%d selected, %s)", selected_count, format_size(selected_size).c_str());
    
    ImGui::Spacing();
    
    // Addon list with checkboxes
    ImGui::BeginChild("AddonList", ImVec2(0, 150), true);
    
    std::string filter_lower = addon_search_;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
    
    for (size_t i = 0; i < batch_addons_.size(); ++i) {
        auto& addon = batch_addons_[i];
        
        // Apply search filter
        if (!filter_lower.empty()) {
            std::string name_lower = addon.name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            if (name_lower.find(filter_lower) == std::string::npos) {
                continue;
            }
        }
        
        ImGui::PushID(static_cast<int>(i));
        
        ImGui::Checkbox("##sel", &addon.selected);
        ImGui::SameLine();
        ImGui::Text("%s", addon.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", format_size(addon.total_size).c_str());
        
        ImGui::PopID();
    }
    
    ImGui::EndChild();
}

void ExportDialog::render_progress() {
    if (batch_mode_ && total_addons_ > 1) {
        ImGui::Text("Exporting mod packs... (%d / %d)", addons_processed_, total_addons_);
        ImGui::TextDisabled("Current: %s", current_addon_.c_str());
    } else {
        ImGui::Text("Exporting...");
    }
    
    ImGui::Spacing();

    ImGui::ProgressBar(progress_, ImVec2(-1, 0), "");

    ImGui::Spacing();
    ImGui::TextDisabled("File: %s", current_file_.c_str());
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
            if (dialog_open_) *dialog_open_ = false;
            ImGui::CloseCurrentPopup();
        } else {
            // Show error
            App::instance().set_status("Export failed: " + error_message_);
        }
    }
}

void ExportDialog::init_from_settings() {
    auto& settings = App::instance().settings();
    if (!settings.last_export_path.empty()) {
        output_path_ = settings.last_export_path;
    }
    convert_textures_ = settings.convert_textures_to_png;
}

std::string ExportDialog::format_size(size_t bytes) const {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit < 3) {
        size /= 1024.0;
        unit++;
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.1f %s", size, units[unit]);
    return buffer;
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
    addons_processed_ = 0;
    total_addons_ = 0;
    current_file_.clear();
    current_addon_.clear();
    error_message_.clear();
    cancel_requested_ = false;
    export_finished_ = false;
    
    // Save the export path to settings
    App::instance().settings().last_export_path = output_path_;
    App::instance().save_settings();

    // Build list of addons to export
    std::vector<std::filesystem::path> addons_to_export;
    if (batch_mode_) {
        for (const auto& addon : batch_addons_) {
            if (addon.selected) {
                addons_to_export.push_back(addon.path);
            }
        }
    } else {
        addons_to_export.push_back(source_path_);
    }
    
    total_addons_ = static_cast<int>(addons_to_export.size());

    // Start export in background thread
    std::thread([this, addons_to_export]() {
        try {
            for (size_t addon_idx = 0; addon_idx < addons_to_export.size() && !cancel_requested_; ++addon_idx) {
                const auto& addon_path = addons_to_export[addon_idx];
                current_addon_ = addon_path.filename().string();
                addons_processed_ = static_cast<int>(addon_idx + 1);
                
                AddonExtractor extractor;
                
                if (!extractor.load(addon_path)) {
                    // Skip this addon but continue with others
                    continue;
                }
                
                auto all_files = extractor.list_files();
                std::vector<RdbFile> files_to_export;
                
                // Filter by selected file types
                for (const auto& file : all_files) {
                    std::string ext = std::filesystem::path(file.path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    bool is_texture = (ext == ".edds" || ext == ".dds");
                    bool is_mesh = (ext == ".xob");
                    
                    bool include = false;
                    if (export_textures_ && is_texture) include = true;
                    else if (export_meshes_ && is_mesh) include = true;
                    else if (export_others_ && !is_texture && !is_mesh) include = true;
                    
                    if (include) {
                        files_to_export.push_back(file);
                    }
                }
                
                if (files_to_export.empty()) {
                    continue;
                }
                
                total_files_ = static_cast<int>(files_to_export.size());
                files_processed_ = 0;
                
                for (size_t i = 0; i < files_to_export.size() && !cancel_requested_; ++i) {
                    const auto& file = files_to_export[i];
                    current_file_ = file.path;
                    files_processed_ = static_cast<int>(i + 1);
                    
                    // Calculate overall progress
                    float addon_progress = static_cast<float>(addon_idx) / static_cast<float>(addons_to_export.size());
                    float file_progress = static_cast<float>(i + 1) / static_cast<float>(files_to_export.size());
                    float addon_weight = 1.0f / static_cast<float>(addons_to_export.size());
                    progress_ = addon_progress + (file_progress * addon_weight);
                    
                    // Read file data
                    auto data = extractor.read_file(file);
                    if (data.empty()) continue;
                    
                    // Determine output path - include addon name in path for batch mode
                    std::filesystem::path out_path;
                    if (preserve_structure_) {
                        if (batch_mode_) {
                            out_path = output_path_ / addon_path.filename() / file.path;
                        } else {
                            out_path = output_path_ / file.path;
                        }
                    } else {
                        out_path = output_path_ / std::filesystem::path(file.path).filename();
                    }
                    
                    std::string ext = out_path.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    // Create output directory
                    std::filesystem::create_directories(out_path.parent_path());
                    
                    // Convert meshes if requested (.xob -> OBJ/FBX)
                    if (convert_meshes_ && ext == ".xob") {
                        // Get filename without extension for the mesh name
                        std::string mesh_name = out_path.stem().string();
                        
                        // Create MeshConverter and convert
                        MeshConverter converter(std::span<const uint8_t>(data.data(), data.size()), mesh_name);
                        
                        if (converter.mesh()) {
                            ExportOptions options;
                            options.format = mesh_format_;
                            options.export_normals = export_normals_;
                            options.export_uvs = export_uvs_;
                            options.export_materials = export_materials_;
                            
                            // Convert the mesh
                            auto result = converter.convert(options);
                            if (result) {
                                // Write the converted file(s)
                                if (mesh_format_ == ExportFormat::OBJ) {
                                    // Write OBJ file
                                    std::filesystem::path obj_path = out_path;
                                    obj_path.replace_extension(".obj");
                                    std::ofstream obj_file(obj_path);
                                    if (obj_file) {
                                        obj_file << result->primary;
                                    }
                                    
                                    // Write MTL file
                                    std::filesystem::path mtl_path = out_path;
                                    mtl_path.replace_extension(".mtl");
                                    std::ofstream mtl_file(mtl_path);
                                    if (mtl_file) {
                                        mtl_file << result->material;
                                    }
                                } else if (mesh_format_ == ExportFormat::FBX) {
                                    // Write FBX file
                                    std::filesystem::path fbx_path = out_path;
                                    fbx_path.replace_extension(".fbx");
                                    std::ofstream fbx_file(fbx_path);
                                    if (fbx_file) {
                                        fbx_file << result->primary;
                                    }
                                }
                                
                                // Skip writing original XOB if not keeping originals
                                if (!keep_originals_) {
                                    continue;
                                }
                            }
                        }
                    }
                    
                    // Convert textures if requested (.edds/.dds -> PNG)
                    if (convert_textures_ && (ext == ".edds" || ext == ".dds")) {
                        // Convert EDDS to DDS first
                        std::vector<uint8_t> dds_data;
                        if (ext == ".edds") {
                            EddsConverter converter(std::span<const uint8_t>(data.data(), data.size()));
                            if (converter.is_edds()) {
                                dds_data = converter.convert();
                            }
                        } else {
                            dds_data = data;  // Already DDS
                        }
                        
                        if (!dds_data.empty()) {
                            // Load DDS and convert to PNG
                            auto texture = DdsLoader::load(std::span<const uint8_t>(dds_data.data(), dds_data.size()));
                            if (texture && !texture->pixels.empty()) {
                                // Write PNG using stb_image_write
                                std::filesystem::path png_path = out_path;
                                png_path.replace_extension(".png");
                                
                                // For now, write as DDS since PNG writing requires stb_image_write
                                // TODO: Add stb_image_write for proper PNG export
                                std::filesystem::path dds_out_path = out_path;
                                dds_out_path.replace_extension(".dds");
                                write_file(dds_out_path, dds_data);
                            }
                            
                            if (!keep_originals_) {
                                continue;  // Don't write original file
                            }
                        }
                    }
                    
                    // Write original file
                    write_file(out_path, data);
                }
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
