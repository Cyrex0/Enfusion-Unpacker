/**
 * Enfusion Unpacker - Manifest Parser Header
 */

#pragma once

#include "types.hpp"
#include <vector>
#include <filesystem>

namespace enfusion {

class ManifestParser {
public:
    ManifestParser() = default;
    ~ManifestParser() = default;

    bool parse(const std::filesystem::path& path);

    const std::string& name() const { return name_; }
    const std::string& version() const { return version_; }
    const std::string& guid() const { return guid_; }
    const std::vector<ResourceInfo>& resources() const { return resources_; }
    const std::vector<std::string>& dependencies() const { return dependencies_; }

    std::vector<ResourceInfo> list_resources() const;
    std::vector<ResourceInfo> list_resources_by_type(const std::string& type) const;
    const ResourceInfo* find_resource(const std::string& path) const;
    const ResourceInfo* find_resource_by_guid(const std::string& guid) const;

private:
    std::string name_;
    std::string version_;
    std::string guid_;
    std::vector<ResourceInfo> resources_;
    std::vector<std::string> dependencies_;
};

} // namespace enfusion
