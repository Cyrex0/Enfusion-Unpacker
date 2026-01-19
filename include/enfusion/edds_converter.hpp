/**
 * Enfusion Unpacker - EDDS Converter Header
 */

#pragma once

#include "types.hpp"
#include <vector>
#include <span>
#include <cstdint>
#include <filesystem>

namespace enfusion {

class EddsConverter {
public:
    static constexpr uint8_t DDS_MAGIC[4] = {'D', 'D', 'S', ' '};
    static constexpr uint8_t DX10_FOURCC[4] = {'D', 'X', '1', '0'};

    EddsConverter() = default;
    explicit EddsConverter(std::span<const uint8_t> data);
    ~EddsConverter() = default;

    bool is_edds() const;
    std::vector<uint8_t> convert();
    bool convert_to_dds(const fs::path& input, std::vector<uint8_t>& output);
    bool convert_file(const fs::path& input, const fs::path& output);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t mip_count() const { return mip_count_; }
    std::string format_name() const;

private:
    void parse_header();
    std::vector<std::pair<uint32_t, uint32_t>> parse_mip_table(size_t data_offset);
    size_t calc_mip_size(uint32_t mip_level) const;
    static std::string get_format_name(uint32_t format);

    std::span<const uint8_t> data_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t mip_count_ = 0;
    bool has_dx10_ = false;
    uint32_t bytes_per_block_ = 16;
    uint32_t dxgi_format_ = 0;
    std::string format_;
};

} // namespace enfusion
