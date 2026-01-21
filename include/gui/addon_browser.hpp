/**
 * Enfusion Unpacker - Addon Browser Panel
 */

#pragma once

#include "enfusion/types.hpp"
#include <vector>
#include <string>
#include <functional>

namespace enfusion {

/**
 * Panel for browsing and selecting addons.
 */
class AddonBrowser {
public:
    using SelectCallback = std::function<void(const fs::path& addon_path)>;

    AddonBrowser() = default;
    ~AddonBrowser() = default;

    void render();
    void set_on_select(SelectCallback callback) { on_select_ = callback; }

    void set_addons_path(const fs::path& path);
    void scan_folder(const fs::path& folder);
    void scan_addons();
    void refresh();

    std::function<void(const fs::path&)> on_addon_selected;
    
    // Get list of all scanned addons
    const std::vector<AddonInfo>& get_addons() const { return addons_; }

private:
    void render_addon_card(const AddonInfo& addon, int index);
    void render_search_bar();
    void render_path_selector();
    void apply_filter();
    std::string format_size(size_t bytes) const;

    std::vector<AddonInfo> addons_;
    std::vector<size_t> filtered_indices_;
    fs::path addons_path_;
    fs::path current_folder_;
    char search_filter_[256] = {};
    int selected_index_ = -1;
    int source_index_ = 0;  // 0 = mods, 1 = game addons

    SelectCallback on_select_;

    // Scanning state
    bool is_scanning_ = false;
    float scan_progress_ = 0.0f;
};

} // namespace enfusion
