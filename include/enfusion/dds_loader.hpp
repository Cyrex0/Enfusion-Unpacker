/**
 * Enfusion Unpacker - DDS Loader
 * 
 * Loads DDS textures into RGBA pixel data for display.
 */

#pragma once

#include "types.hpp"
#include <vector>
#include <span>
#include <optional>

namespace enfusion {

class DdsLoader {
public:
    /**
     * Load a DDS texture and decode to RGBA pixels.
     * 
     * @param data Raw DDS file data
     * @return TextureData with decoded pixels, or nullopt on failure
     */
    static std::optional<TextureData> load(std::span<const uint8_t> data);
    
    /**
     * Get format description string.
     */
    static std::string get_format_name(uint32_t dxgi_format);
    
private:
    static bool decode_bc1(std::span<const uint8_t> blocks, uint32_t width, uint32_t height, 
                           std::vector<uint8_t>& output);
    static bool decode_bc3(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& output);
    static bool decode_bc7(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& output);
};

} // namespace enfusion
