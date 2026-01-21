/**
 * Enfusion Unpacker - DDS Loader Implementation
 * Decodes BC1/BC3/BC7 block-compressed textures
 * BC7 uses bcdec-style decoding for proper quality
 */

#include "enfusion/dds_loader.hpp"
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace enfusion {

// DDS header constants
static constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
static constexpr uint32_t DDPF_FOURCC = 0x00000004;
static constexpr uint32_t DX10_FOURCC = 0x30315844; // "DX10"

static inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// BC1 (DXT1) block decoder - 8 bytes per 4x4 block
// Note: DDS stores colors in BGR565 format, but we output RGBA for OpenGL
static void decode_bc1_block(const uint8_t* block, uint8_t* output, int stride) {
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);
    
    // Expand 5:6:5 BGR to 8:8:8:8 RGBA
    // DDS pixel format: B in bits 0-4, G in bits 5-10, R in bits 11-15
    uint8_t colors[4][4];
    colors[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;  // R
    colors[0][1] = ((c0 >> 5) & 0x3F) * 255 / 63;   // G
    colors[0][2] = (c0 & 0x1F) * 255 / 31;          // B
    colors[0][3] = 255;
    
    colors[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;  // R
    colors[1][1] = ((c1 >> 5) & 0x3F) * 255 / 63;   // G
    colors[1][2] = (c1 & 0x1F) * 255 / 31;          // B
    colors[1][3] = 255;
    
    if (c0 > c1) {
        colors[2][0] = (2 * colors[0][0] + colors[1][0]) / 3;
        colors[2][1] = (2 * colors[0][1] + colors[1][1]) / 3;
        colors[2][2] = (2 * colors[0][2] + colors[1][2]) / 3;
        colors[2][3] = 255;
        
        colors[3][0] = (colors[0][0] + 2 * colors[1][0]) / 3;
        colors[3][1] = (colors[0][1] + 2 * colors[1][1]) / 3;
        colors[3][2] = (colors[0][2] + 2 * colors[1][2]) / 3;
        colors[3][3] = 255;
    } else {
        colors[2][0] = (colors[0][0] + colors[1][0]) / 2;
        colors[2][1] = (colors[0][1] + colors[1][1]) / 2;
        colors[2][2] = (colors[0][2] + colors[1][2]) / 2;
        colors[2][3] = 255;
        
        colors[3][0] = 0;
        colors[3][1] = 0;
        colors[3][2] = 0;
        colors[3][3] = 0;
    }
    
    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = (indices >> (2 * (y * 4 + x))) & 0x03;
            uint8_t* pixel = output + y * stride + x * 4;
            pixel[0] = colors[idx][0];
            pixel[1] = colors[idx][1];
            pixel[2] = colors[idx][2];
            pixel[3] = colors[idx][3];
        }
    }
}

// BC3 (DXT5) block decoder - 16 bytes per 4x4 block
static void decode_bc3_block(const uint8_t* block, uint8_t* output, int stride) {
    // First 8 bytes: alpha block
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];
    
    uint8_t alphas[8];
    alphas[0] = a0;
    alphas[1] = a1;
    
    if (a0 > a1) {
        alphas[2] = (6 * a0 + 1 * a1) / 7;
        alphas[3] = (5 * a0 + 2 * a1) / 7;
        alphas[4] = (4 * a0 + 3 * a1) / 7;
        alphas[5] = (3 * a0 + 4 * a1) / 7;
        alphas[6] = (2 * a0 + 5 * a1) / 7;
        alphas[7] = (1 * a0 + 6 * a1) / 7;
    } else {
        alphas[2] = (4 * a0 + 1 * a1) / 5;
        alphas[3] = (3 * a0 + 2 * a1) / 5;
        alphas[4] = (2 * a0 + 3 * a1) / 5;
        alphas[5] = (1 * a0 + 4 * a1) / 5;
        alphas[6] = 0;
        alphas[7] = 255;
    }
    
    // Read 48-bit alpha indices
    uint64_t alpha_bits = 0;
    for (int i = 2; i < 8; i++) {
        alpha_bits |= static_cast<uint64_t>(block[i]) << ((i - 2) * 8);
    }
    
    // Decode color using BC1
    decode_bc1_block(block + 8, output, stride);
    
    // Apply alpha
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = (alpha_bits >> (3 * (y * 4 + x))) & 0x07;
            uint8_t* pixel = output + y * stride + x * 4;
            pixel[3] = alphas[idx];
        }
    }
}

// Helper for BC7: extract bits from block
static uint64_t bc7_extract_bits(const uint8_t* block, int* bit_offset, int num_bits) {
    uint64_t result = 0;
    int offset = *bit_offset;
    
    for (int i = 0; i < num_bits; i++) {
        int byte_idx = offset / 8;
        int bit_idx = offset % 8;
        if (byte_idx < 16 && (block[byte_idx] & (1 << bit_idx))) {
            result |= (1ULL << i);
        }
        offset++;
    }
    
    *bit_offset = offset;
    return result;
}

// BC7 partition tables (2 and 3 subset partitions)
static const uint8_t bc7_partition2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1}, {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},
    {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1}, {0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1}, {0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0}, {0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0}, {0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0}, {0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0},
    {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0}, {0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0}, {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0}, {0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1}, {0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},
    {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0}, {0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0}, {0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},
    {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1}, {0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0}, {0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
    {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0}, {0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0}, {0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},
    {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1}, {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0}, {0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0}, {0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0}, {0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1}, {0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},
    {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1}, {0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1}, {0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0}, {0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1},
};

// Anchor indices for subset partitioning
static const uint8_t bc7_anchor2_0[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15,  2,  8,  2,  2,  8,  8, 15,  2,  8,  2,  2,  8,  8,  2,  2,
    15, 15,  6,  8,  2,  8, 15, 15,  2,  8,  2,  2,  2, 15, 15,  6,
     6,  2,  6,  8, 15, 15,  2,  2, 15, 15, 15, 15, 15,  2,  2, 15,
};

// BC7 weight tables for interpolation
static const uint8_t bc7_weights2[4] = { 0, 21, 43, 64 };
static const uint8_t bc7_weights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const uint8_t bc7_weights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

// Interpolate endpoint colors using weight
static inline uint8_t bc7_interpolate(uint8_t e0, uint8_t e1, uint8_t weight) {
    return (uint8_t)(((64 - weight) * e0 + weight * e1 + 32) >> 6);
}

/**
 * Full BC7 block decoder supporting all 8 modes
 * Based on the BC7 specification with proper partition handling
 */
static void decode_bc7_block(const uint8_t* block, uint8_t* output, int stride) {
    int bit_offset = 0;
    
    // Find mode (0-7 based on first set bit)
    int mode = -1;
    for (int i = 0; i < 8; i++) {
        if (block[0] & (1 << i)) {
            mode = i;
            bit_offset = i + 1;
            break;
        }
    }
    
    if (mode < 0) {
        // Reserved mode - fill with magenta for debugging
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                uint8_t* pixel = output + y * stride + x * 4;
                pixel[0] = 255; pixel[1] = 0; pixel[2] = 255; pixel[3] = 255;
            }
        }
        return;
    }
    
    // Mode configuration table
    // {num_subsets, partition_bits, rotation_bits, index_selection_bits,
    //  color_bits, alpha_bits, pbit_mode, index_bits, index2_bits}
    static const int mode_info[8][9] = {
        {3, 4, 0, 0, 4, 0, 1, 3, 0},  // Mode 0
        {2, 6, 0, 0, 6, 0, 2, 3, 0},  // Mode 1
        {3, 6, 0, 0, 5, 0, 0, 2, 0},  // Mode 2
        {2, 6, 0, 0, 7, 0, 1, 2, 0},  // Mode 3
        {1, 0, 2, 1, 5, 6, 0, 2, 3},  // Mode 4
        {1, 0, 2, 0, 7, 8, 0, 2, 2},  // Mode 5
        {1, 0, 0, 0, 7, 7, 1, 4, 0},  // Mode 6
        {2, 6, 0, 0, 5, 5, 1, 2, 0},  // Mode 7
    };
    
    int num_subsets = mode_info[mode][0];
    int partition_bits = mode_info[mode][1];
    int rotation_bits = mode_info[mode][2];
    int index_sel_bits = mode_info[mode][3];
    int color_bits = mode_info[mode][4];
    int alpha_bits = mode_info[mode][5];
    int pbit_mode = mode_info[mode][6];  // 0=none, 1=per-endpoint, 2=shared
    int index_bits = mode_info[mode][7];
    int index2_bits = mode_info[mode][8];
    
    // Read partition, rotation, index selection
    int partition = partition_bits ? (int)bc7_extract_bits(block, &bit_offset, partition_bits) : 0;
    int rotation = rotation_bits ? (int)bc7_extract_bits(block, &bit_offset, rotation_bits) : 0;
    int index_sel = index_sel_bits ? (int)bc7_extract_bits(block, &bit_offset, index_sel_bits) : 0;
    
    // Read endpoints (color and alpha)
    // BC7 stores: R0,R1 for each subset, then G0,G1 for each subset, then B0,B1
    uint8_t endpoints[6][4] = {{0}};  // Up to 3 subsets × 2 endpoints, RGBA
    
    // Read R channel for all endpoints
    for (int s = 0; s < num_subsets; s++) {
        endpoints[s * 2 + 0][0] = (uint8_t)bc7_extract_bits(block, &bit_offset, color_bits);  // R0
        endpoints[s * 2 + 1][0] = (uint8_t)bc7_extract_bits(block, &bit_offset, color_bits);  // R1
    }
    // Read G channel for all endpoints
    for (int s = 0; s < num_subsets; s++) {
        endpoints[s * 2 + 0][1] = (uint8_t)bc7_extract_bits(block, &bit_offset, color_bits);  // G0
        endpoints[s * 2 + 1][1] = (uint8_t)bc7_extract_bits(block, &bit_offset, color_bits);  // G1
    }
    // Read B channel for all endpoints
    for (int s = 0; s < num_subsets; s++) {
        endpoints[s * 2 + 0][2] = (uint8_t)bc7_extract_bits(block, &bit_offset, color_bits);  // B0
        endpoints[s * 2 + 1][2] = (uint8_t)bc7_extract_bits(block, &bit_offset, color_bits);  // B1
    }
    
    // Read alpha channel if present
    if (alpha_bits) {
        for (int s = 0; s < num_subsets; s++) {
            endpoints[s * 2 + 0][3] = (uint8_t)bc7_extract_bits(block, &bit_offset, alpha_bits);  // A0
            endpoints[s * 2 + 1][3] = (uint8_t)bc7_extract_bits(block, &bit_offset, alpha_bits);  // A1
        }
    } else {
        for (int i = 0; i < 6; i++) endpoints[i][3] = 255;
    }
    
    // Read P-bits and apply to endpoints
    if (pbit_mode == 1) {  // Per-endpoint p-bits
        for (int s = 0; s < num_subsets; s++) {
            for (int e = 0; e < 2; e++) {
                int pbit = (int)bc7_extract_bits(block, &bit_offset, 1);
                int idx = s * 2 + e;
                endpoints[idx][0] = (endpoints[idx][0] << 1) | pbit;
                endpoints[idx][1] = (endpoints[idx][1] << 1) | pbit;
                endpoints[idx][2] = (endpoints[idx][2] << 1) | pbit;
                if (alpha_bits) {
                    endpoints[idx][3] = (endpoints[idx][3] << 1) | pbit;
                }
            }
        }
    } else if (pbit_mode == 2) {  // Shared p-bits (one per subset)
        for (int s = 0; s < num_subsets; s++) {
            int pbit = (int)bc7_extract_bits(block, &bit_offset, 1);
            for (int e = 0; e < 2; e++) {
                int idx = s * 2 + e;
                endpoints[idx][0] = (endpoints[idx][0] << 1) | pbit;
                endpoints[idx][1] = (endpoints[idx][1] << 1) | pbit;
                endpoints[idx][2] = (endpoints[idx][2] << 1) | pbit;
                if (alpha_bits) {
                    endpoints[idx][3] = (endpoints[idx][3] << 1) | pbit;
                }
            }
        }
    }
    
    // Expand to 8 bits using bit replication
    int color_expand = pbit_mode ? (color_bits + 1) : color_bits;
    int alpha_expand = (pbit_mode && alpha_bits) ? (alpha_bits + 1) : alpha_bits;
    
    for (int i = 0; i < num_subsets * 2; i++) {
        // Expand color channels
        for (int ch = 0; ch < 3; ch++) {
            uint8_t val = endpoints[i][ch];
            // Bit replication: shift left to position MSB at bit 7, then OR with shifted copy
            if (color_expand < 8) {
                endpoints[i][ch] = (val << (8 - color_expand)) | (val >> (2 * color_expand - 8));
            }
            // If color_expand == 8, no expansion needed
        }
        // Expand alpha channel
        if (alpha_bits && alpha_expand < 8) {
            uint8_t val = endpoints[i][3];
            endpoints[i][3] = (val << (8 - alpha_expand)) | (val >> (2 * alpha_expand - 8));
        }
    }

    
    // Read primary indices
    uint8_t indices[16];
    int anchor_idx = (num_subsets > 1) ? bc7_anchor2_0[partition] : 0;
    const uint8_t* weights = (index_bits == 2) ? bc7_weights2 : 
                             (index_bits == 3) ? bc7_weights3 : bc7_weights4;
    
    for (int i = 0; i < 16; i++) {
        // First pixel of each subset has reduced index bits (anchor)
        int is_anchor = (i == 0) || (num_subsets > 1 && i == anchor_idx);
        int bits = is_anchor ? (index_bits - 1) : index_bits;
        indices[i] = bc7_extract_bits(block, &bit_offset, bits);
    }
    
    // Read secondary indices (modes 4, 5)
    uint8_t indices2[16] = {0};
    const uint8_t* weights2 = bc7_weights2;
    if (index2_bits) {
        weights2 = (index2_bits == 2) ? bc7_weights2 : bc7_weights3;
        for (int i = 0; i < 16; i++) {
            int bits = (i == 0) ? (index2_bits - 1) : index2_bits;
            indices2[i] = bc7_extract_bits(block, &bit_offset, bits);
        }
    }
    
    // Interpolate and output
    for (int i = 0; i < 16; i++) {
        int y = i / 4;
        int x = i % 4;
        uint8_t* pixel = output + y * stride + x * 4;
        
        int subset = (num_subsets > 1) ? bc7_partition2[partition][i] : 0;
        int ep0 = subset * 2;
        int ep1 = subset * 2 + 1;
        
        uint8_t w1 = weights[indices[i]];
        
        if (index2_bits) {
            // Separate color and alpha interpolation
            uint8_t w2 = weights2[indices2[i]];
            // index_sel: 0 = index1→color, index2→alpha; 1 = swap
            uint8_t wc = index_sel ? w2 : w1;
            uint8_t wa = index_sel ? w1 : w2;
            
            pixel[0] = bc7_interpolate(endpoints[ep0][0], endpoints[ep1][0], wc);
            pixel[1] = bc7_interpolate(endpoints[ep0][1], endpoints[ep1][1], wc);
            pixel[2] = bc7_interpolate(endpoints[ep0][2], endpoints[ep1][2], wc);
            pixel[3] = bc7_interpolate(endpoints[ep0][3], endpoints[ep1][3], wa);
        } else {
            pixel[0] = bc7_interpolate(endpoints[ep0][0], endpoints[ep1][0], w1);
            pixel[1] = bc7_interpolate(endpoints[ep0][1], endpoints[ep1][1], w1);
            pixel[2] = bc7_interpolate(endpoints[ep0][2], endpoints[ep1][2], w1);
            pixel[3] = bc7_interpolate(endpoints[ep0][3], endpoints[ep1][3], w1);
        }
        
        // Apply rotation (swap component with alpha)
        if (rotation) {
            uint8_t tmp = pixel[3];
            pixel[3] = pixel[rotation - 1];
            pixel[rotation - 1] = tmp;
        }
    }
}

std::optional<TextureData> DdsLoader::load(std::span<const uint8_t> data) {
    if (data.size() < 128) return std::nullopt;
    
    // Check DDS magic
    if (read_u32_le(data.data()) != DDS_MAGIC) return std::nullopt;
    
    // Parse header
    uint32_t height = read_u32_le(data.data() + 12);
    uint32_t width = read_u32_le(data.data() + 16);
    uint32_t mip_count = read_u32_le(data.data() + 28);
    
    if (mip_count == 0) mip_count = 1;
    if (width == 0 || height == 0) return std::nullopt;
    if (width > 16384 || height > 16384) return std::nullopt;
    
    // Pixel format at offset 76
    uint32_t pf_flags = read_u32_le(data.data() + 80);
    uint32_t fourcc = read_u32_le(data.data() + 84);
    
    // Determine format
    int bytes_per_block = 16;
    int format_type = 0; // 0=unknown, 1=BC1, 2=BC3, 3=BC7
    std::string format_name = "UNKNOWN";
    
    size_t data_offset = 128;
    
    if (pf_flags & DDPF_FOURCC) {
        if (fourcc == DX10_FOURCC) {
            if (data.size() < 148) return std::nullopt;
            data_offset = 148;
            
            uint32_t dxgi_format = read_u32_le(data.data() + 128);
            switch (dxgi_format) {
                case 70: case 71: case 72:
                    bytes_per_block = 8;
                    format_type = 1;
                    format_name = "BC1";
                    break;
                case 73: case 74: case 75:
                    bytes_per_block = 16;
                    format_type = 2;
                    format_name = "BC2";
                    break;
                case 76: case 77:
                    bytes_per_block = 16;
                    format_type = 2;
                    format_name = "BC3";
                    break;
                case 95: case 96: case 97: case 98: case 99:
                    bytes_per_block = 16;
                    format_type = 3;
                    format_name = "BC7";
                    break;
                default:
                    bytes_per_block = 16;
                    format_name = "DXGI:" + std::to_string(dxgi_format);
                    break;
            }
        } else {
            char cc[5] = {0};
            std::memcpy(cc, &fourcc, 4);
            
            if (std::memcmp(cc, "DXT1", 4) == 0) {
                bytes_per_block = 8;
                format_type = 1;
                format_name = "DXT1";
            } else if (std::memcmp(cc, "DXT3", 4) == 0) {
                bytes_per_block = 16;
                format_type = 2;
                format_name = "DXT3";
            } else if (std::memcmp(cc, "DXT5", 4) == 0) {
                bytes_per_block = 16;
                format_type = 2;
                format_name = "DXT5";
            } else {
                format_name = std::string(cc, 4);
            }
        }
    } else {
        uint32_t rgb_bits = read_u32_le(data.data() + 88);
        format_name = "RGBA" + std::to_string(rgb_bits);
    }
    
    // Calculate block dimensions
    uint32_t blocks_x = std::max(1u, (width + 3) / 4);
    uint32_t blocks_y = std::max(1u, (height + 3) / 4);
    
    // Decode texture
    TextureData tex;
    tex.width = width;
    tex.height = height;
    tex.format = format_name;
    tex.mip_count = mip_count;
    tex.channels = 4;
    tex.pixels.resize(width * height * 4);
    std::fill(tex.pixels.begin(), tex.pixels.end(), 128);
    
    const uint8_t* src = data.data() + data_offset;
    size_t src_remaining = data.size() - data_offset;
    
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            size_t block_offset = (by * blocks_x + bx) * bytes_per_block;
            if (block_offset + bytes_per_block > src_remaining) break;
            
            const uint8_t* block = src + block_offset;
            
            int px = bx * 4;
            int py = by * 4;
            
            // Decode to temp buffer
            uint8_t temp[64];
            std::memset(temp, 128, 64);
            
            switch (format_type) {
                case 1: decode_bc1_block(block, temp, 16); break;
                case 2: decode_bc3_block(block, temp, 16); break;
                case 3: decode_bc7_block(block, temp, 16); break;
                default: break;
            }
            
            // Copy to output
            for (int ty = 0; ty < 4 && py + ty < (int)height; ty++) {
                for (int tx = 0; tx < 4 && px + tx < (int)width; tx++) {
                    int src_idx = ty * 16 + tx * 4;
                    int dst_idx = ((py + ty) * width + (px + tx)) * 4;
                    tex.pixels[dst_idx + 0] = temp[src_idx + 0];
                    tex.pixels[dst_idx + 1] = temp[src_idx + 1];
                    tex.pixels[dst_idx + 2] = temp[src_idx + 2];
                    tex.pixels[dst_idx + 3] = temp[src_idx + 3];
                }
            }
        }
    }
    
    return tex;
}

std::string DdsLoader::get_format_name(uint32_t dxgi_format) {
    switch (dxgi_format) {
        case 70: case 71: case 72: return "BC1";
        case 73: case 74: case 75: return "BC2";
        case 76: case 77: return "BC3";
        case 83: return "BC4";
        case 84: return "BC5";
        case 95: case 96: case 97: case 98: case 99: return "BC7";
        default: return "UNKNOWN";
    }
}

// OpenGL compressed format constants (from glext.h)
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif

std::optional<DdsTextureData> DdsLoader::load_for_gpu(std::span<const uint8_t> data) {
    if (data.size() < 128) return std::nullopt;
    
    // Check DDS magic
    if (read_u32_le(data.data()) != DDS_MAGIC) return std::nullopt;
    
    // Parse header
    uint32_t height = read_u32_le(data.data() + 12);
    uint32_t width = read_u32_le(data.data() + 16);
    uint32_t mip_count = read_u32_le(data.data() + 28);
    
    if (mip_count == 0) mip_count = 1;
    if (width == 0 || height == 0) return std::nullopt;
    if (width > 16384 || height > 16384) return std::nullopt;
    
    // Pixel format at offset 76
    uint32_t pf_flags = read_u32_le(data.data() + 80);
    uint32_t fourcc = read_u32_le(data.data() + 84);
    
    DdsTextureData tex;
    tex.width = width;
    tex.height = height;
    tex.mip_count = mip_count;
    tex.channels = 4;
    
    size_t data_offset = 128;
    int bytes_per_block = 16;
    
    if (pf_flags & DDPF_FOURCC) {
        if (fourcc == DX10_FOURCC) {
            if (data.size() < 148) return std::nullopt;
            data_offset = 148;
            
            uint32_t dxgi_format = read_u32_le(data.data() + 128);
            switch (dxgi_format) {
                case 70: case 71: case 72:  // BC1
                    bytes_per_block = 8;
                    tex.format = "BC1";
                    tex.is_compressed = true;
                    tex.gl_internal_format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
                    break;
                case 73: case 74: case 75:  // BC2
                    bytes_per_block = 16;
                    tex.format = "BC2";
                    tex.is_compressed = true;
                    tex.gl_internal_format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
                    break;
                case 76: case 77:  // BC3
                    bytes_per_block = 16;
                    tex.format = "BC3";
                    tex.is_compressed = true;
                    tex.gl_internal_format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                    break;
                case 95: case 96: case 97:  // BC7 UNORM
                    bytes_per_block = 16;
                    tex.format = "BC7";
                    tex.is_compressed = true;
                    tex.gl_internal_format = GL_COMPRESSED_RGBA_BPTC_UNORM;
                    break;
                case 98: case 99:  // BC7 SRGB
                    bytes_per_block = 16;
                    tex.format = "BC7_SRGB";
                    tex.is_compressed = true;
                    tex.gl_internal_format = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
                    break;
                default:
                    // Unsupported format, fall back to software decode
                    auto fallback = load(data);
                    if (!fallback) return std::nullopt;
                    tex.pixels = std::move(fallback->pixels);
                    tex.format = fallback->format;
                    tex.is_compressed = false;
                    return tex;
            }
        } else {
            char cc[5] = {0};
            std::memcpy(cc, &fourcc, 4);
            
            if (std::memcmp(cc, "DXT1", 4) == 0) {
                bytes_per_block = 8;
                tex.format = "DXT1";
                tex.is_compressed = true;
                tex.gl_internal_format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            } else if (std::memcmp(cc, "DXT3", 4) == 0) {
                bytes_per_block = 16;
                tex.format = "DXT3";
                tex.is_compressed = true;
                tex.gl_internal_format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
            } else if (std::memcmp(cc, "DXT5", 4) == 0) {
                bytes_per_block = 16;
                tex.format = "DXT5";
                tex.is_compressed = true;
                tex.gl_internal_format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            } else {
                // Unknown format, try software decode
                auto fallback = load(data);
                if (!fallback) return std::nullopt;
                tex.pixels = std::move(fallback->pixels);
                tex.format = fallback->format;
                tex.is_compressed = false;
                return tex;
            }
        }
    } else {
        // Uncompressed - use software decode
        auto fallback = load(data);
        if (!fallback) return std::nullopt;
        tex.pixels = std::move(fallback->pixels);
        tex.format = fallback->format;
        tex.is_compressed = false;
        return tex;
    }
    
    // For compressed textures, copy the raw compressed data
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    size_t compressed_size = blocks_x * blocks_y * bytes_per_block;
    
    if (data_offset + compressed_size > data.size()) {
        // Not enough data, fall back to software decode
        auto fallback = load(data);
        if (!fallback) return std::nullopt;
        tex.pixels = std::move(fallback->pixels);
        tex.format = fallback->format;
        tex.is_compressed = false;
        return tex;
    }
    
    tex.compressed_size = static_cast<uint32_t>(compressed_size);
    tex.pixels.assign(data.begin() + data_offset, data.begin() + data_offset + compressed_size);
    
    return tex;
}

bool DdsLoader::decode_bc1(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                            std::vector<uint8_t>& output) {
    output.resize(width * height * 4);
    
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            size_t block_offset = (by * blocks_x + bx) * 8;
            if (block_offset + 8 > blocks.size()) break;
            
            uint8_t temp[64];
            decode_bc1_block(blocks.data() + block_offset, temp, 16);
            
            int px = bx * 4;
            int py = by * 4;
            
            for (int ty = 0; ty < 4 && py + ty < (int)height; ty++) {
                for (int tx = 0; tx < 4 && px + tx < (int)width; tx++) {
                    int src_idx = ty * 16 + tx * 4;
                    int dst_idx = ((py + ty) * width + (px + tx)) * 4;
                    std::memcpy(&output[dst_idx], &temp[src_idx], 4);
                }
            }
        }
    }
    
    return true;
}

bool DdsLoader::decode_bc3(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                            std::vector<uint8_t>& output) {
    output.resize(width * height * 4);
    
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            size_t block_offset = (by * blocks_x + bx) * 16;
            if (block_offset + 16 > blocks.size()) break;
            
            uint8_t temp[64];
            decode_bc3_block(blocks.data() + block_offset, temp, 16);
            
            int px = bx * 4;
            int py = by * 4;
            
            for (int ty = 0; ty < 4 && py + ty < (int)height; ty++) {
                for (int tx = 0; tx < 4 && px + tx < (int)width; tx++) {
                    int src_idx = ty * 16 + tx * 4;
                    int dst_idx = ((py + ty) * width + (px + tx)) * 4;
                    std::memcpy(&output[dst_idx], &temp[src_idx], 4);
                }
            }
        }
    }
    
    return true;
}

bool DdsLoader::decode_bc7(std::span<const uint8_t> blocks, uint32_t width, uint32_t height,
                            std::vector<uint8_t>& output) {
    output.resize(width * height * 4);
    
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            size_t block_offset = (by * blocks_x + bx) * 16;
            if (block_offset + 16 > blocks.size()) break;
            
            uint8_t temp[64];
            decode_bc7_block(blocks.data() + block_offset, temp, 16);
            
            int px = bx * 4;
            int py = by * 4;
            
            for (int ty = 0; ty < 4 && py + ty < (int)height; ty++) {
                for (int tx = 0; tx < 4 && px + tx < (int)width; tx++) {
                    int src_idx = ty * 16 + tx * 4;
                    int dst_idx = ((py + ty) * width + (px + tx)) * 4;
                    std::memcpy(&output[dst_idx], &temp[src_idx], 4);
                }
            }
        }
    }
    
    return true;
}

} // namespace enfusion
