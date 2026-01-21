/**
 * Enfusion Unpacker - File Browser Implementation
 */

#include "gui/file_browser.hpp"
#include "gui/widgets.hpp"
#include "enfusion/addon_extractor.hpp"
#include "enfusion/pak_manager.hpp"

#include <imgui.h>
#include <algorithm>

namespace enfusion {

void FileBrowser::load(const std::filesystem::path& addon_path) {
    root_path_ = addon_path;
    entries_.clear();
    tree_root_.children.clear();
    tree_root_.name = addon_path.filename().string();
    
    // Register the PAK with PakManager for cross-reference lookups
    auto& pak_mgr = PakManager::instance();

    // Try to load from addon directory (with data.pak, rdb, manifest)
    if (std::filesystem::is_directory(addon_path)) {
        auto pak_path = addon_path / "data.pak";
        auto rdb_path = addon_path / "resourceDatabase.rdb";
        
        if (std::filesystem::exists(pak_path) && std::filesystem::exists(rdb_path)) {
            // Register with PakManager
            pak_mgr.load_pak(addon_path);
            // Load from addon using AddonExtractor
            load_from_addon(addon_path);
        } else {
            // Just list filesystem
            load_from_directory(addon_path);
        }
    } else if (addon_path.extension() == ".pak") {
        // Single PAK file - load parent dir as addon
        auto parent = addon_path.parent_path();
        if (std::filesystem::exists(parent / "resourceDatabase.rdb")) {
            load_from_addon(parent);
        }
    }

    build_tree();
    apply_filter();
}

void FileBrowser::load_from_addon(const std::filesystem::path& addon_dir) {
    try {
        extractor_ = std::make_shared<AddonExtractor>();
        if (!extractor_->load(addon_dir)) {
            extractor_.reset();
            load_from_directory(addon_dir);  // Fallback
            return;
        }
        
        auto files = extractor_->list_files();
        for (const auto& file : files) {
            FileEntry fe;
            fe.path = file.path;
            fe.name = std::filesystem::path(file.path).filename().string();
            fe.size = file.size;
            fe.is_directory = false;
            fe.rdb_index = file.index;

            auto ext = std::filesystem::path(file.path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".edds" || ext == ".dds") {
                fe.type = FileType::Texture;
            } else if (ext == ".xob") {
                fe.type = FileType::Mesh;
            } else if (ext == ".rdb") {
                fe.type = FileType::Database;
            } else if (ext == ".pak") {
                fe.type = FileType::Archive;
            } else if (ext == ".bin" || ext == ".dat") {
                fe.type = FileType::Binary;
            } else {
                fe.type = FileType::Other;
            }

            entries_.push_back(fe);
        }
    } catch (...) {
        extractor_.reset();
    }
}

void FileBrowser::load_from_directory(const std::filesystem::path& dir_path) {
    extractor_.reset();  // Not using extractor for plain directory
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
            if (entry.is_regular_file()) {
                FileEntry fe;
                fe.path = std::filesystem::relative(entry.path(), dir_path).string();
                fe.name = entry.path().filename().string();
                fe.size = entry.file_size();
                fe.is_directory = false;
                fe.rdb_index = -1;

                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".edds" || ext == ".dds") {
                    fe.type = FileType::Texture;
                } else if (ext == ".xob") {
                    fe.type = FileType::Mesh;
                } else if (ext == ".rdb") {
                    fe.type = FileType::Database;
                } else if (ext == ".pak") {
                    fe.type = FileType::Archive;
                } else if (ext == ".bin" || ext == ".dat") {
                    fe.type = FileType::Binary;
                } else {
                    fe.type = FileType::Other;
                }

                entries_.push_back(fe);
            }
        }
    } catch (...) {
        // Handle error
    }
}

void FileBrowser::build_tree() {
    for (const auto& entry : entries_) {
        add_to_tree(&tree_root_, entry.path, &entry);
    }
}

void FileBrowser::add_to_tree(TreeNode* parent, const std::string& path, const FileEntry* entry) {
    size_t pos = path.find('/');
    if (pos == std::string::npos) {
        pos = path.find('\\');
    }

    if (pos == std::string::npos) {
        // Leaf node (file)
        TreeNode node;
        node.name = path;
        node.is_file = true;
        node.entry = entry;
        parent->children.push_back(node);
    } else {
        // Directory
        std::string dir_name = path.substr(0, pos);
        std::string remaining = path.substr(pos + 1);

        // Find or create directory node
        TreeNode* dir_node = nullptr;
        for (auto& child : parent->children) {
            if (!child.is_file && child.name == dir_name) {
                dir_node = &child;
                break;
            }
        }

        if (!dir_node) {
            TreeNode node;
            node.name = dir_name;
            node.is_file = false;
            parent->children.push_back(node);
            dir_node = &parent->children.back();
        }

        add_to_tree(dir_node, remaining, entry);
    }
}

void FileBrowser::apply_filter() {
    filtered_entries_.clear();

    std::string filter_lower = search_filter_;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);

    for (const auto& entry : entries_) {
        bool matches = filter_lower.empty();

        if (!matches) {
            std::string name_lower = entry.name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            matches = name_lower.find(filter_lower) != std::string::npos;
        }

        if (matches) {
            // Apply type filter
            if (filter_textures_ && entry.type != FileType::Texture) continue;
            if (filter_meshes_ && entry.type != FileType::Mesh) continue;

            filtered_entries_.push_back(&entry);
        }
    }
}

std::vector<uint8_t> FileBrowser::read_selected_file() {
    if (!selected_entry_ || !extractor_) return {};
    
    return extractor_->read_file(selected_entry_->path);
}

void FileBrowser::render() {
    // Search and filter bar
    ImGui::SetNextItemWidth(-1);
    if (widgets::SearchInput("##FileSearch", search_filter_, 256)) {
        apply_filter();
    }

    ImGui::Spacing();

    // Filter toggles
    if (ImGui::SmallButton(filter_textures_ ? "[Textures]" : "Textures")) {
        filter_textures_ = !filter_textures_;
        filter_meshes_ = false;
        apply_filter();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(filter_meshes_ ? "[Meshes]" : "Meshes")) {
        filter_meshes_ = !filter_meshes_;
        filter_textures_ = false;
        apply_filter();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) {
        filter_textures_ = false;
        filter_meshes_ = false;
        apply_filter();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // View mode toggle
    ImGui::RadioButton("Tree", reinterpret_cast<int*>(&view_mode_), 0);
    ImGui::SameLine();
    ImGui::RadioButton("List", reinterpret_cast<int*>(&view_mode_), 1);

    ImGui::Spacing();

    // File list/tree
    ImGui::BeginChild("FileList", ImVec2(0, 0), true);

    if (entries_.empty()) {
        ImGui::TextDisabled("No files loaded.");
        ImGui::TextDisabled("Select an addon to browse.");
    } else if (view_mode_ == ViewMode::Tree) {
        render_tree_node(tree_root_);
    } else {
        render_flat_list();
    }

    ImGui::EndChild();
}

void FileBrowser::render_tree_node(const TreeNode& node) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;

    if (node.is_file) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

        // Get icon based on type
        const char* icon = get_type_icon(node.entry ? node.entry->type : FileType::Other);

        bool selected = (selected_entry_ == node.entry);
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::TreeNodeEx(node.name.c_str(), flags, "%s %s", icon, node.name.c_str());

        if (ImGui::IsItemClicked()) {
            selected_entry_ = node.entry;
            if (node.entry && on_file_selected) {
                // Pass the file path - the callback will read data from extractor
                on_file_selected(node.entry->path);
            }
        }

        // Context menu for tree view files
        if (node.entry) {
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Export...")) {
                    if (on_export_requested) {
                        on_export_requested(node.entry->path);
                    }
                }
                if (ImGui::MenuItem("Copy Path")) {
                    ImGui::SetClipboardText(node.entry->path.c_str());
                }
                ImGui::EndPopup();
            }
        }

        // Tooltip with size
        if (ImGui::IsItemHovered() && node.entry) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", node.entry->path.c_str());
            ImGui::TextDisabled("Size: %s", format_size(node.entry->size).c_str());
            ImGui::EndTooltip();
        }
    } else {
        // Directory
        if (ImGui::TreeNodeEx(node.name.c_str(), flags, "[D] %s", node.name.c_str())) {
            for (const auto& child : node.children) {
                render_tree_node(child);
            }
            ImGui::TreePop();
        }
    }
}

void FileBrowser::render_flat_list() {
    ImGui::Text("%zu files", filtered_entries_.size());
    ImGui::Separator();

    for (const auto* entry : filtered_entries_) {
        const char* icon = get_type_icon(entry->type);

        bool selected = (selected_entry_ == entry);

        if (ImGui::Selectable((std::string(icon) + " " + entry->name).c_str(), selected)) {
            selected_entry_ = entry;
            if (on_file_selected) {
                on_file_selected(entry->path);
            }
        }

        // Context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Export...")) {
                if (on_export_requested) {
                    on_export_requested(entry->path);
                }
            }
            if (ImGui::MenuItem("Copy Path")) {
                ImGui::SetClipboardText(entry->path.c_str());
            }
            ImGui::EndPopup();
        }

        // Show size on hover
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", entry->path.c_str());
            ImGui::TextDisabled("Size: %s", format_size(entry->size).c_str());
            ImGui::EndTooltip();
        }
    }
}

const char* FileBrowser::get_type_icon(FileType type) const {
    switch (type) {
        case FileType::Texture:  return "[T]";
        case FileType::Mesh:     return "[M]";
        case FileType::Database: return "[D]";
        case FileType::Archive:  return "[A]";
        case FileType::Binary:   return "[B]";
        default:                 return "[?]";
    }
}

std::string FileBrowser::format_size(size_t bytes) const {
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

} // namespace enfusion
