/**
 * Enfusion Unpacker - Addon Browser Implementation
 */

#include "gui/addon_browser.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstring>

namespace enfusion {

void AddonBrowser::set_addons_path(const fs::path& path) {
    addons_path_ = path;
    scan_addons();
}

void AddonBrowser::scan_addons() {
    addons_.clear();

    if (addons_path_.empty() || !fs::exists(addons_path_)) {
        return;
    }

    try {
        for (const auto& entry : fs::directory_iterator(addons_path_)) {
            if (entry.is_directory()) {
                AddonInfo addon;
                addon.name = entry.path().filename().string();
                addon.path = entry.path();

                // Check for PAK files
                for (const auto& file : fs::directory_iterator(entry.path())) {
                    if (file.is_regular_file()) {
                        auto ext = file.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".pak") {
                            addon.pak_files.push_back(file.path());
                        }
                    }
                }

                // Calculate total size
                for (const auto& pak : addon.pak_files) {
                    addon.total_size += fs::file_size(pak);
                }

                if (!addon.pak_files.empty()) {
                    addons_.push_back(addon);
                }
            }
        }

        apply_filter();
    } catch (const std::exception& e) {
        // Log error
    }
}

void AddonBrowser::scan_folder(const fs::path& folder) {
    addons_path_ = folder;
    scan_addons();
}

void AddonBrowser::refresh() {
    scan_addons();
}

void AddonBrowser::apply_filter() {
    filtered_indices_.clear();

    std::string filter_lower = search_filter_;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);

    for (size_t i = 0; i < addons_.size(); ++i) {
        if (filter_lower.empty()) {
            filtered_indices_.push_back(i);
        } else {
            std::string name_lower = addons_[i].name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

            if (name_lower.find(filter_lower) != std::string::npos) {
                filtered_indices_.push_back(i);
            }
        }
    }
}

void AddonBrowser::render() {
    // Search bar
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##AddonSearch", "Search addons...", search_filter_, sizeof(search_filter_))) {
        apply_filter();
    }

    ImGui::Spacing();

    // Addons list
    if (addons_path_.empty()) {
        ImGui::TextDisabled("No addons folder set.");
        ImGui::TextDisabled("File > Open Addons Folder...");
        return;
    }

    if (addons_.empty()) {
        ImGui::TextDisabled("No addons found in:");
        ImGui::TextWrapped("%s", addons_path_.string().c_str());
        return;
    }

    ImGui::BeginChild("AddonsList", ImVec2(0, 0), true);

    for (size_t idx : filtered_indices_) {
        const auto& addon = addons_[idx];

        bool is_selected = (selected_index_ == static_cast<int>(idx));

        ImGui::PushID(static_cast<int>(idx));

        // Calculate item height for custom rendering
        float item_height = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y;

        if (ImGui::Selectable("##AddonItem", is_selected, 0, ImVec2(0, item_height))) {
            selected_index_ = static_cast<int>(idx);
            if (on_addon_selected) {
                on_addon_selected(addon.path);
            }
        }

        // Draw custom content
        ImVec2 item_min = ImGui::GetItemRectMin();

        ImGui::SetCursorScreenPos(ImVec2(item_min.x + 8, item_min.y + 4));
        ImGui::Text("%s", addon.name.c_str());

        ImGui::SetCursorScreenPos(ImVec2(item_min.x + 8, item_min.y + ImGui::GetTextLineHeight() + 6));
        ImGui::TextDisabled("%zu PAK files, %s", addon.pak_files.size(), format_size(addon.total_size).c_str());

        ImGui::PopID();
    }

    ImGui::EndChild();
}

std::string AddonBrowser::format_size(size_t bytes) const {
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
