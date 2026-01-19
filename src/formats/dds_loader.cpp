/**
 * Enfusion Unpacker - DDS Loader Implementation (Stub)
 */

#include "enfusion/dds_loader.hpp"
#include <cstring>

namespace enfusion {

std::optional<TextureData> DdsLoader::load(std::span<const uint8_t> data) {
    if (data.size() < 128) return std::nullopt;
    
    // Check DDS magic
    if (std::memcmp(data.data(), "DDS ", 4) != 0) return std::nullopt;
    
    // TODO: Actual DDS parsing
    TextureData tex;
    tex.width = 256;
    tex.height = 256;
    tex.format = "RGBA8";
    tex.mip_count = 1;
    tex.pixels.resize(256 * 256 * 4, 128);
    return tex;
}

std::string DdsLoader::get_format_name(uint32_t dxgi_format) {
    switch (dxgi_format) {
        case 71: return "BC1_UNORM";
        case 77: return "BC3_UNORM";
        case 98: return "BC7_UNORM";
        default: return "UNKNOWN";
    }
}

bool DdsLoader::decode_bc1(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& output) {
    output.resize(width * height * 4, 128);
    return true;
}

bool DdsLoader::decode_bc3(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& output) {
    output.resize(width * height * 4, 128);
    return true;
}

bool DdsLoader::decode_bc7(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& output) {
    output.resize(width * height * 4, 128);
    return true;
}

} // namespace enfusion
