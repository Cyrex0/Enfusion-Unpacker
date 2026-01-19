/**
 * Enfusion Unpacker - RDB Parser Header
 */

#pragma once

#include "types.hpp"
#include <vector>
#include <span>
#include <optional>
#include <filesystem>

namespace enfusion {

class RdbParser {
public:
    RdbParser() = default;
    ~RdbParser() = default;

    bool parse(const std::filesystem::path& path);
    bool parse(std::span<const uint8_t> data);

    const std::vector<RdbEntry>& entries() const { return entries_; }
    const RdbEntry* find_entry_by_path(const std::string& path) const;
    const RdbEntry* find_entry_by_id(const std::array<uint8_t, 16>& id) const;
    
    std::vector<Fragment> read_fragments(const RdbEntry& entry) const;
    std::vector<uint8_t> decompress_resource(const RdbEntry& entry) const;

private:
    std::vector<RdbEntry> entries_;
    std::vector<uint8_t> data_;
};

} // namespace enfusion
