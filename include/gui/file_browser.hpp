/**
 * Enfusion Unpacker - File Browser Panel
 */

#pragma once

#include "enfusion/types.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace enfusion {

class AddonExtractor;

/**
 * File type enumeration.
 */
enum class FileType {
    Texture,
    Mesh,
    Database,
    Archive,
    Binary,
    Other
};

/**
 * File entry information.
 */
struct FileEntry {
    std::string path;
    std::string name;
    size_t size = 0;
    FileType type = FileType::Other;
    bool is_directory = false;
    int rdb_index = -1;  // Index in RDB for reading from PAK
};

/**
 * Tree node for hierarchical file view.
 */
struct TreeNode {
    std::string name;
    bool is_file = false;
    const FileEntry* entry = nullptr;
    std::vector<TreeNode> children;
};

/**
 * View mode for file browser.
 */
enum class ViewMode {
    Tree = 0,
    List = 1
};

/**
 * Panel for browsing files within an addon.
 */
class FileBrowser {
public:
    using FileSelectCallback = std::function<void(const std::string&)>;

    std::function<void(const std::string&)> on_file_selected;
    std::function<void(const std::string&)> on_export_requested;  // Called when user requests export from context menu

    FileBrowser() = default;
    ~FileBrowser() = default;

    void load(const std::filesystem::path& addon_path);
    void render();

    const FileEntry* selected() const { return selected_entry_; }
    
    /**
     * Read the currently selected file data from PAK
     */
    std::vector<uint8_t> read_selected_file();
    
    /**
     * Get the addon extractor (for reading files)
     */
    std::shared_ptr<AddonExtractor> extractor() const { return extractor_; }
    
    /**
     * Get all texture file paths (.edds files)
     */
    std::vector<std::string> get_texture_paths() const {
        std::vector<std::string> textures;
        for (const auto& entry : entries_) {
            if (entry.type == FileType::Texture && !entry.is_directory) {
                textures.push_back(entry.path);
            }
        }
        return textures;
    }

    void clear() {
        entries_.clear();
        filtered_entries_.clear();
        tree_root_.children.clear();
        selected_entry_ = nullptr;
        extractor_.reset();
    }

private:
    void load_from_directory(const std::filesystem::path& dir_path);
    void load_from_addon(const std::filesystem::path& addon_dir);
    void build_tree();
    void add_to_tree(TreeNode* parent, const std::string& path, const FileEntry* entry);
    void apply_filter();

    void render_tree_node(const TreeNode& node);
    void render_flat_list();

    const char* get_type_icon(FileType type) const;
    std::string format_size(size_t bytes) const;

    std::filesystem::path root_path_;
    std::vector<FileEntry> entries_;
    std::vector<const FileEntry*> filtered_entries_;
    TreeNode tree_root_;

    const FileEntry* selected_entry_ = nullptr;
    std::string search_filter_;

    ViewMode view_mode_ = ViewMode::Tree;
    bool filter_textures_ = false;
    bool filter_meshes_ = false;
    
    std::shared_ptr<AddonExtractor> extractor_;
};

} // namespace enfusion
