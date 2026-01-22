/**
 * Enfusion Unpacker - Manifest Parser Implementation
 * 
 * NOTE: Manifest parsing is not yet implemented. The manifest format is used
 * for addon metadata in Enfusion engine. Implementation pending when format
 * documentation becomes available.
 */

#include "enfusion/manifest.hpp"
#include "enfusion/logging.hpp"
#include <fstream>

namespace enfusion {

bool ManifestParser::parse(const fs::path& path) {
    // Manifest parsing not implemented - format undocumented
    LOG_DEBUG("ManifestParser", "Manifest parsing not implemented: " << path.string());
    return true;
}

std::vector<ResourceInfo> ManifestParser::list_resources() const {
    return resources_;
}

std::vector<ResourceInfo> ManifestParser::list_resources_by_type(const std::string& type) const {
    std::vector<ResourceInfo> result;
    for (const auto& res : resources_) {
        if (res.type == type) result.push_back(res);
    }
    return result;
}

const ResourceInfo* ManifestParser::find_resource(const std::string& path) const {
    for (const auto& res : resources_) {
        if (res.path == path) return &res;
    }
    return nullptr;
}

const ResourceInfo* ManifestParser::find_resource_by_guid(const std::string& guid) const {
    for (const auto& res : resources_) {
        if (res.guid == guid) return &res;
    }
    return nullptr;
}

} // namespace enfusion
