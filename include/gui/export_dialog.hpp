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
 * Info about an addon for batch export selection
 */
struct BatchAddonInfo {
    std::string name;
    std::filesystem::path path;
    size_t total_size = 0;
    bool selected = false;
};

/**
 * Dialog for exporting files from addon.
 */
class ExportDialog {
public:
    ExportDialog() = default;
    ~ExportDialog() = default;

    void render(bool* open);

    // Set single addon source for "Export Selected" (exports entire mod pack)
    void set_source(const std::filesystem::path& path) {
        source_path_ = path;
        batch_mode_ = false;
        selected_file_.clear();
    }
    
    // Set batch mode with list of available addons for selection
    void set_batch_mode(const std::vector<AddonInfo>& available_addons) {
        batch_mode_ = true;
        source_path_.clear();
        selected_file_.clear();
        
        // Convert to batch addon info with selection state
        batch_addons_.clear();
        for (const auto& addon : available_addons) {
            BatchAddonInfo info;
            info.name = addon.name;
            info.path = addon.path;
            info.total_size = addon.total_size;
            info.selected = false;  // Start unselected
            batch_addons_.push_back(info);
        }
    }
    
    // Initialize with default export path from settings
    void init_from_settings();

private:
    void render_content(bool* open);
    void render_addon_selection();
    void render_progress();
    void start_export();
    void browse_output_folder();
    std::string format_size(size_t bytes) const;
    
    bool* dialog_open_ = nullptr;  // Pointer to the open flag

    // Source
    std::filesystem::path source_path_;  // For single addon export
    std::string selected_file_;  // Deprecated - kept for compatibility
    bool batch_mode_ = false;
    
    // Batch mode addon selection
    std::vector<BatchAddonInfo> batch_addons_;
    char addon_search_[256] = {};

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
    std::string current_addon_;
    int files_processed_ = 0;
    int total_files_ = 0;
    int addons_processed_ = 0;
    int total_addons_ = 0;

    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> export_finished_{false};
    std::string error_message_;
};

} // namespace enfusion
