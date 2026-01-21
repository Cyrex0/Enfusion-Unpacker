/**
 * Enfusion Unpacker - Export Dialog
 */

#pragma once

#include "enfusion/types.hpp"
#include "enfusion/mesh_converter.hpp"
#include <filesystem>
#include <functional>
#include <vector>
#include <string>
#include <atomic>

namespace enfusion {

class AddonExtractor;

/**
 * Dialog for exporting files from addon.
 */
class ExportDialog {
public:
    ExportDialog() = default;
    ~ExportDialog() = default;

    void render(bool* open);

    void set_source(const std::filesystem::path& path, bool batch = false) {
        source_path_ = path;
        batch_mode_ = batch;
        selected_file_.clear();  // Clear selected file when source changes
    }
    
    void set_selected_file(const std::string& file) {
        selected_file_ = file;
    }

    void set_batch_mode(bool batch) { batch_mode_ = batch; }

private:
    void render_content();
    void render_progress();
    void start_export();
    void browse_output_folder();

    // Source
    std::filesystem::path source_path_;
    std::string selected_file_;  // For single file export
    bool batch_mode_ = false;

    // Export options
    std::filesystem::path output_path_;
    bool convert_textures_ = true;
    bool convert_meshes_ = true;
    bool keep_originals_ = false;
    bool preserve_structure_ = true;
    
    // Mesh export format
    ExportFormat mesh_format_ = ExportFormat::OBJ;
    bool export_normals_ = true;
    bool export_uvs_ = true;
    bool export_materials_ = true;

    // Batch mode options
    bool export_textures_ = true;
    bool export_meshes_ = true;
    bool export_others_ = true;

    // State
    bool exporting_ = false;
    float progress_ = 0.0f;
    std::string current_file_;
    int files_processed_ = 0;
    int total_files_ = 0;

    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> export_finished_{false};
    std::string error_message_;
};

} // namespace enfusion
