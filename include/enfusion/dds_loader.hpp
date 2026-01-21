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

/**
 * Extended texture data that can hold either decompressed RGBA or compressed data.
 */
struct DdsTextureData {
    std::vector<uint8_t> pixels;  // Pixel data (RGBA or compressed)
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    uint32_t mip_count = 1;
    std::string format;
    
    // Compression info for GPU upload
    bool is_compressed = false;
    uint32_t gl_internal_format = 0;  // e.g., GL_COMPRESSED_RGBA_BPTC_UNORM
    uint32_t compressed_size = 0;
};

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
     * Load DDS with option to keep compressed for GPU upload.
     * This is more efficient for BC7 textures.
     */
    static std::optional<DdsTextureData> load_for_gpu(std::span<const uint8_t> data);
    
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

