/**
 * Enfusion Unpacker - RDB Parser Implementation (Stub)
 */

#include "enfusion/rdb_parser.hpp"
#include <fstream>

namespace enfusion {

bool RdbParser::parse(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    data_.resize(size);
    file.read(reinterpret_cast<char*>(data_.data()), size);
    
    return parse(std::span<const uint8_t>(data_.data(), data_.size()));
}

bool RdbParser::parse(std::span<const uint8_t> data) {
    // TODO: Actual parsing
    return true;
}

const RdbEntry* RdbParser::find_entry_by_path(const std::string& path) const {
    for (const auto& entry : entries_) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

const RdbEntry* RdbParser::find_entry_by_id(const std::array<uint8_t, 16>& id) const {
    for (const auto& entry : entries_) {
        if (entry.resource_id == id) return &entry;
    }
    return nullptr;
}

std::vector<Fragment> RdbParser::read_fragments(const RdbEntry& entry) const {
    return {};
}

std::vector<uint8_t> RdbParser::decompress_resource(const RdbEntry& entry) const {
    return {};
}

} // namespace enfusion
