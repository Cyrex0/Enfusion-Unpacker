/**
 * XOB9 Format Types and Constants
 * 
 * Shared type definitions for XOB parser components.
 * See XOB9_FORMAT_SPEC_v8.md for format documentation.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <glm/glm.hpp>

namespace enfusion {
namespace xob {

// ============================================================================
// LZ4 Decompression Parameters
// ============================================================================
constexpr size_t LZ4_MAX_BLOCK_SIZE = 0x20000;  // 128KB
constexpr size_t LZ4_DICT_SIZE = 65536;          // 64KB dictionary

// ============================================================================
// LZO4 Descriptor (116 bytes per LOD)
// ============================================================================
constexpr size_t LZO4_MARKER_SIZE = 4;
constexpr size_t LZO4_DESCRIPTOR_SIZE = 116;

// Field offsets from "LZO4" marker
constexpr size_t LZO4_OFF_QUALITY_TIER = 0x04;
constexpr size_t LZO4_OFF_SWITCH_DIST = 0x0C;
constexpr size_t LZO4_OFF_COMPRESSED = 0x14;
constexpr size_t LZO4_OFF_DECOMPRESSED = 0x1C;
constexpr size_t LZO4_OFF_FORMAT_FLAGS = 0x20;
constexpr size_t LZO4_OFF_BBOX_MIN = 0x24;
constexpr size_t LZO4_OFF_BBOX_MAX = 0x30;
constexpr size_t LZO4_OFF_TRIANGLE_COUNT = 0x4C;
constexpr size_t LZO4_OFF_UNIQUE_VERTS = 0x4E;
constexpr size_t LZO4_OFF_ORIG_VERTS = 0x50;
constexpr size_t LZO4_OFF_SUBMESH_IDX = 0x52;
constexpr size_t LZO4_OFF_ATTR_CONFIG = 0x58;
constexpr size_t LZO4_OFF_UV_BOUNDS = 0x60;
constexpr size_t LZO4_OFF_SURFACE_SCALE = 0x70;

// ============================================================================
// Mesh Type Constants (high byte of format_flags)
// ============================================================================
constexpr uint8_t MESH_STATIC = 0x0F;
constexpr uint8_t MESH_SKINNED = 0x1F;
constexpr uint8_t MESH_EMISSIVE = 0x8F;
constexpr uint8_t MESH_SKINNED_EMISSIVE = 0x9F;

// Position stride by mesh type
constexpr int POSITION_STRIDE_12 = 12;  // XYZ floats
constexpr int POSITION_STRIDE_16 = 16;  // XYZW floats

// Vertex attribute sizes
constexpr size_t INDEX_SIZE = 2;
constexpr size_t NORMAL_SIZE = 4;
constexpr size_t TANGENT_SIZE = 4;
constexpr size_t UV_SIZE_HALF = 4;
constexpr size_t VERTEX_COLOR_SIZE = 4;

// Attribute config indices
constexpr size_t ATTR_CFG_LOD_FLAG = 0;
constexpr size_t ATTR_CFG_UV_SETS = 2;
constexpr size_t ATTR_CFG_MAT_SLOTS = 3;
constexpr size_t ATTR_CFG_BONE_STREAMS = 4;

// ============================================================================
// Binary Read Helpers
// ============================================================================
inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline float read_f32_le(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, sizeof(float));
    return f;
}

/**
 * Convert IEEE 754 half-float (16-bit) to single-precision (32-bit).
 */
inline float half_to_float(uint16_t h) {
    union { uint32_t u; float f; } result;
    
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    
    if (exp == 0) {
        if (mant == 0) {
            result.u = sign << 31;
        } else {
            float value = static_cast<float>(mant) / 1024.0f * (1.0f / 16384.0f);
            result.f = sign ? -value : value;
        }
    } else if (exp == 31) {
        result.u = (sign << 31) | (mant == 0 ? 0x7F800000 : 0x7FC00000);
    } else {
        result.u = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    
    return result.f;
}

// ============================================================================
// Mesh Layout Detection Result
// ============================================================================
struct VertexLayout {
    bool use_separated_streams = false;
    bool use_single_index = false;
    bool has_color_before_uv = false;
    bool uv_probes_valid = false;
    bool uv_is_f32 = false;
    
    size_t vertex_data_offset = 0;
    size_t found_uv_offset = 0;
    size_t position_stride = 12;
    
    // Stream offsets (only valid if use_separated_streams)
    size_t pos_offset = 0;
    size_t norm_offset = 0;
    size_t tangent_offset = 0;
    size_t uv0_offset = 0;
    size_t uv1_offset = 0;
    size_t color_offset = 0;
};

} // namespace xob
} // namespace enfusion
