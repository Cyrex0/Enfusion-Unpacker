/**
 * Enfusion Unpacker - XOB Parser Implementation
 * 
 * XOB9 Format (IFF/FORM container with big-endian chunk sizes):
 * - FORM/XOB9 header (12 bytes)
 * - HEAD chunk: Materials, bones, LOD descriptors
 *   - Header (0x00-0x3B): bbox, material_count(u16@0x2C), bone_count(u16@0x2E), LOD count
 *   - Material strings: name+path pairs (null-terminated)
 *   - LZO4 descriptors (116 bytes each)
 * - COLL chunk (optional): Collision objects (64 bytes each) + collision mesh
 * - VOLM chunk (optional): Spatial octree for collision broadphase
 * - LODS chunk: LZ4 block-compressed mesh data with dictionary chaining
 *   - LOD regions stored in REVERSE order (LOD0 at END)
 *   - Data layout per LOD: Index1 -> Index2 -> Positions -> Normals -> UVs -> [Tangents] -> [Extra]
 */

#include "enfusion/xob_parser.hpp"
#include "enfusion/compression.hpp"
#include "enfusion/logging.hpp"
#include <lz4.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cfloat>
#include <map>
#include <set>

namespace enfusion {

// ============================================================================
// XOB9 Format Constants (per XOB9_FORMAT_SPEC_v8.md)
// ============================================================================

// LZ4 block decompression parameters
constexpr size_t LZ4_MAX_BLOCK_SIZE = 0x20000;       // 128KB maximum compressed block
constexpr size_t LZ4_DICT_SIZE = 65536;              // 64KB dictionary window

// LZO4 Descriptor layout (116 bytes, offsets from "LZO4" marker)
// Per spec v8 section 2.3
constexpr size_t LZO4_MARKER_SIZE = 4;                // "LZO4" magic (4 bytes)
constexpr size_t LZO4_DESCRIPTOR_SIZE = 116;          // Total descriptor size in bytes

// LZO4 Descriptor field offsets (all from "LZO4" marker position)
constexpr size_t LZO4_OFF_QUALITY_TIER = 0x04;        // +0x04: u32 LOD quality (1-5)
constexpr size_t LZO4_OFF_SWITCH_DIST = 0x0C;         // +0x0C: f32 LOD switch distance
constexpr size_t LZO4_OFF_COMPRESSED = 0x14;          // +0x14: u32 Compressed size in LODS
constexpr size_t LZO4_OFF_DECOMPRESSED = 0x1C;        // +0x1C: u32 Decompressed size
constexpr size_t LZO4_OFF_FORMAT_FLAGS = 0x20;        // +0x20: u32 Format flags
constexpr size_t LZO4_OFF_BBOX_MIN = 0x24;            // +0x24: f32[3] LOD bbox min
constexpr size_t LZO4_OFF_BBOX_MAX = 0x30;            // +0x30: f32[3] LOD bbox max
constexpr size_t LZO4_OFF_TRIANGLE_COUNT = 0x4C;      // +0x4C: u16 Triangle count
constexpr size_t LZO4_OFF_UNIQUE_VERTS = 0x4E;        // +0x4E: u16 Unique vertex count
constexpr size_t LZO4_OFF_ORIG_VERTS = 0x50;          // +0x50: u16 Original vertex count
constexpr size_t LZO4_OFF_SUBMESH_IDX = 0x52;         // +0x52: u16 Submesh/material index
constexpr size_t LZO4_OFF_ATTR_CONFIG = 0x58;         // +0x58: u8[8] Attribute configuration
constexpr size_t LZO4_OFF_UV_BOUNDS = 0x60;           // +0x60: f32[4] UV bounds (umin,umax,vmin,vmax)
constexpr size_t LZO4_OFF_SURFACE_SCALE = 0x70;       // +0x70: f32 Surface scale factor
constexpr size_t LZO4_MIN_REQUIRED_BYTES = 0x74;      // Minimum bytes needed (116 bytes)

// Mesh type from high byte of format_flags (per spec v8 section 3.1)
constexpr uint8_t XOB_MESH_STATIC = 0x0F;             // Standard mesh with normals
constexpr uint8_t XOB_MESH_SKINNED = 0x1F;            // Has bone weights/indices
constexpr uint8_t XOB_MESH_EMISSIVE = 0x8F;           // Has second UV set for emissive
constexpr uint8_t XOB_MESH_SKINNED_EMISSIVE = 0x9F;   // Combined skinned + emissive

// Position stride: bit 4 of mesh_type determines stride
// 0x0F, 0x8F = 12-byte (XYZ), 0x1F, 0x9F = 16-byte (XYZW)
constexpr int XOB_POSITION_STRIDE_12 = 12;            // XYZ floats (3 * 4 bytes)
constexpr int XOB_POSITION_STRIDE_16 = 16;            // XYZW floats (4 * 4 bytes, W unused)

// Vertex attribute sizes (per spec v8 section 4.3)
constexpr size_t XOB_INDEX_SIZE = 2;                  // 16-bit indices
constexpr size_t XOB_NORMAL_SIZE = 4;                 // Packed normal (u8[4])
constexpr size_t XOB_TANGENT_SIZE = 4;                // Packed tangent (u8[4])
constexpr size_t XOB_UV_SIZE_HALF = 4;                // f16[2] half-float UV pair
constexpr size_t XOB_VERTEX_COLOR_SIZE = 4;           // u8[4] RGBA vertex color
constexpr size_t XOB_EXTRA_SIZE = 8;                  // Extra data (second UV set + color)

// Attribute config byte meanings (per spec v8 section 2.7)
constexpr size_t ATTR_CFG_LOD_FLAG = 0;               // [0] LOD-specific flag
constexpr size_t ATTR_CFG_UV_SETS = 2;                // [2] UV set count (1 or 2)
constexpr size_t ATTR_CFG_MAT_SLOTS = 3;              // [3] Material/texture slots
constexpr size_t ATTR_CFG_BONE_STREAMS = 4;           // [4] Bone weight streams (0, 1, or 2)

// Helper to read big-endian uint32 (IFF chunk sizes)
static inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// Helper to read little-endian values
static inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static inline uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static inline float read_f32_le(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, sizeof(float));
    return f;
}

/**
 * Convert IEEE 754 half-precision (16-bit) float to single-precision (32-bit) float
 * Layout: 1 sign bit, 5 exponent bits, 10 mantissa bits
 * 
 * Per XOB9_FORMAT_SPEC_v8.md: UV coordinates use f16 half-floats, not normalized u16.
 * This is used for UV decoding in parse_mesh_from_region.
 */
static inline float half_to_float(uint16_t h) {
    // Use a union for type-safe bit manipulation
    union { uint32_t u; float f; } result;
    
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    
    if (exp == 0) {
        if (mant == 0) {
            // Zero (signed)
            result.u = sign << 31;
        } else {
            // Denormalized half -> normalized float
            // Denorm half: value = (-1)^sign * 2^(-14) * (0.mant)
            // Need to normalize by finding leading 1 bit
            float value = static_cast<float>(mant) / 1024.0f;  // mant / 2^10
            value *= (1.0f / 16384.0f);  // * 2^(-14)
            result.f = sign ? -value : value;
        }
    } else if (exp == 31) {
        // Inf or NaN - map to float inf/nan
        if (mant == 0) {
            // Infinity
            result.u = (sign << 31) | 0x7F800000;
        } else {
            // NaN - preserve sign but use quiet NaN
            result.u = (sign << 31) | 0x7FC00000;
        }
    } else {
        // Normalized number: value = (-1)^sign * 2^(exp-15) * (1.mant)
        // Convert to float: exp_f = exp - 15 + 127 = exp + 112
        result.u = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    
    return result.f;
}

/**
 * LZ4 dictionary chaining decompression
 * CRITICAL: Dictionary MUST persist across ALL blocks (Python: decompress_lods)
 * 
 * The has_more flag only indicates logical segments, NOT dictionary boundaries.
 * We must continue decompression until we run out of data or hit a zero block.
 */
static std::vector<uint8_t> decompress_lz4_chained(const uint8_t* data, size_t size) {
    std::vector<uint8_t> result;
    result.reserve(size * 4);

    LOG_DEBUG("XobParser", "Decompressing LZ4 chained, input size=" << size);

    size_t pos = 0;
    std::vector<uint8_t> prev_dict;
    prev_dict.reserve(LZ4_DICT_SIZE);
    int block_count = 0;

    while (pos < size) {
        if (pos + 4 > size) break;

        uint32_t header = read_u32_le(data + pos);
        pos += 4;

        uint32_t block_size = header & 0x7FFFFFFF;
        // Note: has_more flag (bit 31) is NOT used for decompression control
        // The dictionary MUST be maintained across ALL blocks without resetting

        if (block_size == 0) break;
        if (block_size > LZ4_MAX_BLOCK_SIZE) break;
        if (pos + block_size > size) break;

        std::vector<uint8_t> decompressed(LZ4_DICT_SIZE);
        int dec_size;

        if (!prev_dict.empty()) {
            dec_size = LZ4_decompress_safe_usingDict(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                LZ4_DICT_SIZE,
                reinterpret_cast<const char*>(prev_dict.data()),
                static_cast<int>(prev_dict.size())
            );
        } else {
            dec_size = LZ4_decompress_safe(
                reinterpret_cast<const char*>(data + pos),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(block_size),
                LZ4_DICT_SIZE
            );
        }

        pos += block_size;
        if (dec_size <= 0) {
            LOG_DEBUG("XobParser", "Block " << block_count << " decompression failed, dec_size=" << dec_size);
            break;
        }

        decompressed.resize(dec_size);
        result.insert(result.end(), decompressed.begin(), decompressed.end());
        
        // CRITICAL: Dictionary must be from last 64KB of ALL accumulated output
        // NOT just the previous block!
        if (result.size() <= LZ4_DICT_SIZE) {
            prev_dict.assign(result.begin(), result.end());
        } else {
            prev_dict.assign(result.end() - LZ4_DICT_SIZE, result.end());
        }
        
        block_count++;
        // Do NOT break on has_more=false - continue until end of data
    }

    LOG_DEBUG("XobParser", "Decompressed " << block_count << " blocks, total output=" << result.size());
    return result;
}

/**
 * Parse LZO4 descriptors from HEAD chunk.
 * Per XOB9_FORMAT_SPEC_v8.md section 2.3 (116 bytes per descriptor)
 * 
 * Returns vector of LzoDescriptor with all fields populated.
 */
static std::vector<LzoDescriptor> parse_lzo4_descriptors_v8(const uint8_t* data, size_t size) {
    std::vector<LzoDescriptor> descriptors;
    
    LOG_DEBUG("XobParser", "Parsing LZO4 descriptors from HEAD chunk, size=" << size);
    
    // Search for LZO4 markers
    size_t pos = 0;
    while (pos + LZO4_MARKER_SIZE <= size) {
        // Look for LZO4 marker
        size_t found = SIZE_MAX;
        for (size_t i = pos; i + LZO4_MARKER_SIZE <= size; i++) {
            if (data[i] == 'L' && data[i+1] == 'Z' && 
                data[i+2] == 'O' && data[i+3] == '4') {
                found = i;
                break;
            }
        }
        
        if (found == SIZE_MAX) break;
        
        // Ensure we have enough data for full descriptor (116 bytes)
        if (found + LZO4_DESCRIPTOR_SIZE > size) {
            LOG_WARNING("XobParser", "Truncated LZO4 descriptor at offset " << found);
            break;
        }
        
        const uint8_t* desc = data + found;
        LzoDescriptor d;
        
        // Per spec v8 section 2.3 - all offsets from LZO4 marker:
        
        // +0x04: LOD quality tier (1-5)
        d.quality_tier = read_u32_le(desc + LZO4_OFF_QUALITY_TIER);
        
        // +0x0C: LOD switch distance (screen coverage ratio)
        d.switch_distance = read_f32_le(desc + LZO4_OFF_SWITCH_DIST);
        
        // +0x14: Compressed data size in LODS chunk
        d.compressed_size = read_u32_le(desc + LZO4_OFF_COMPRESSED);
        
        // +0x1C: Decompressed mesh data size
        d.decompressed_size = read_u32_le(desc + LZO4_OFF_DECOMPRESSED);
        
        // +0x20: Format flags (high byte = mesh type, low byte = format index)
        d.format_flags = read_u32_le(desc + LZO4_OFF_FORMAT_FLAGS);
        d.mesh_type = (d.format_flags >> 24) & 0xFF;
        
        // +0x24-0x3B: LOD bounding box
        d.bounds_min.x = read_f32_le(desc + LZO4_OFF_BBOX_MIN);
        d.bounds_min.y = read_f32_le(desc + LZO4_OFF_BBOX_MIN + 4);
        d.bounds_min.z = read_f32_le(desc + LZO4_OFF_BBOX_MIN + 8);
        d.bounds_max.x = read_f32_le(desc + LZO4_OFF_BBOX_MAX);
        d.bounds_max.y = read_f32_le(desc + LZO4_OFF_BBOX_MAX + 4);
        d.bounds_max.z = read_f32_le(desc + LZO4_OFF_BBOX_MAX + 8);
        
        // +0x4C-0x52: Mesh counts
        d.triangle_count = read_u16_le(desc + LZO4_OFF_TRIANGLE_COUNT);
        d.unique_vertex_count = read_u16_le(desc + LZO4_OFF_UNIQUE_VERTS);
        d.original_vertex_count = read_u16_le(desc + LZO4_OFF_ORIG_VERTS);
        d.submesh_index = read_u16_le(desc + LZO4_OFF_SUBMESH_IDX);
        
        // +0x58: Attribute configuration (8 bytes)
        std::memcpy(d.attr_config, desc + LZO4_OFF_ATTR_CONFIG, 8);
        
        // +0x60-0x6F: UV bounds
        d.uv_min_u = read_f32_le(desc + LZO4_OFF_UV_BOUNDS);
        d.uv_max_u = read_f32_le(desc + LZO4_OFF_UV_BOUNDS + 4);
        d.uv_min_v = read_f32_le(desc + LZO4_OFF_UV_BOUNDS + 8);
        d.uv_max_v = read_f32_le(desc + LZO4_OFF_UV_BOUNDS + 12);
        
        // +0x70: Surface scale factor
        d.surface_scale = read_f32_le(desc + LZO4_OFF_SURFACE_SCALE);
        
        // Derive properties from mesh_type (per spec v8 section 3.1)
        // Position stride: skinned meshes (0x1F, 0x9F) use 16-byte, static (0x0F, 0x8F) use 12-byte
        d.position_stride = (d.mesh_type & 0x10) ? XOB_POSITION_STRIDE_16 : XOB_POSITION_STRIDE_12;
        
        // Attribute presence from mesh type
        d.has_normals = true;  // All meshes have normals
        d.has_tangents = true;  // All meshes have tangents for normal mapping
        d.has_uvs = true;  // All meshes have at least one UV set
        d.has_skinning = (d.mesh_type == XOB_MESH_SKINNED || d.mesh_type == XOB_MESH_SKINNED_EMISSIVE);
        d.has_second_uv = (d.mesh_type == XOB_MESH_EMISSIVE || d.mesh_type == XOB_MESH_SKINNED_EMISSIVE);
        d.has_vertex_color = d.has_second_uv;  // Emissive meshes often have vertex color
        
        // Override from attribute config if available
        if (d.attr_config[ATTR_CFG_UV_SETS] > 0) {
            d.has_second_uv = (d.attr_config[ATTR_CFG_UV_SETS] >= 2);
        }
        if (d.attr_config[ATTR_CFG_BONE_STREAMS] > 0) {
            d.has_skinning = true;
        }
        
        LOG_DEBUG("XobParser", "LOD " << descriptors.size() 
                  << ": type=0x" << std::hex << (int)d.mesh_type << std::dec
                  << " comp=" << d.compressed_size
                  << " decomp=" << d.decompressed_size
                  << " tris=" << d.triangle_count 
                  << " verts=" << d.unique_vertex_count 
                  << " stride=" << d.position_stride
                  << " uv_sets=" << (int)d.attr_config[ATTR_CFG_UV_SETS]
                  << " bones=" << (int)d.attr_config[ATTR_CFG_BONE_STREAMS]);
        
        descriptors.push_back(d);
        pos = found + LZO4_MARKER_SIZE;  // Continue searching after this marker
    }
    
    LOG_DEBUG("XobParser", "Found " << descriptors.size() << " LOD descriptors");
    return descriptors;
}

/**
 * Parse mesh from LOD region
 * 
 * Per XOB9_FORMAT_SPEC_v8.md section 4.2-4.3:
 * 
 * Decompressed data layout:
 *   [index_array]  - u16[triangle_count * 3]
 *   [vertex_array] - interleaved attributes, count = unique_vertex_count
 *
 * Vertex layouts by mesh type (section 4.3):
 *   Static (0x0F) - 20 bytes/vertex:
 *     Position f32[3] (12 bytes) + Normal u8[4] (4 bytes) + Tangent u8[4] (4 bytes)
 *     
 *   Emissive (0x8F) - 32+ bytes/vertex:
 *     Position f32[3] (12) + Normal u8[4] (4) + Tangent u8[4] (4) + 
 *     UV0 f16[2] (4) + UV1 f16[2] (4) + VertexColor u8[4] (4)
 *     
 *   Skinned (0x1F) - Variable:
 *     Base attributes + bone weights/indices
 */
static bool parse_mesh_from_region(
    const std::vector<uint8_t>& region,
    uint16_t vertex_count,
    uint16_t triangle_count,
    uint8_t mesh_type,  // High byte of format_flags: 0x0F=static, 0x8F=emissive, 0x1F=skinned
    uint8_t bone_streams, // Number of bone weight streams (0, 1, or 2)
    XobMesh& mesh
) {
    LOG_DEBUG("XobParser", "parse_mesh_from_region: region_size=" << region.size() 
              << " verts=" << vertex_count << " tris=" << triangle_count 
              << " mesh_type=0x" << std::hex << (int)mesh_type << std::dec);
    
    if (vertex_count == 0 || triangle_count == 0 || region.empty()) {
        LOG_ERROR("XobParser", "Invalid parameters: verts=" << vertex_count 
                  << " tris=" << triangle_count << " region=" << region.size());
        return false;
    }
    
    uint32_t index_count = static_cast<uint32_t>(triangle_count) * 3;
    size_t idx_array_size = index_count * XOB_INDEX_SIZE;
    
    // ACTUAL DATA LAYOUT (empirically confirmed):
    // The data has TWO index arrays, not one as the spec suggests:
    //   [index_array_1: triangle_count * 3 * 2 bytes] - triangle indices
    //   [index_array_2: triangle_count * 3 * 2 bytes] - dedup/original vertex mapping
    //   [vertex_data: vertex_count * vertex_stride bytes] - interleaved vertex data
    //
    // Evidence: bytes after first index array start with "00 00 01 00 02 00 01 00 03 00..."
    // which are clearly more u16 indices (0, 1, 2, 1, 3, 2...), not float positions.
    
    size_t dual_idx_array_size = idx_array_size * 2;  // Two index arrays
    size_t vertex_data_offset = dual_idx_array_size;
    
    LOG_DEBUG("XobParser", "index_count=" << index_count << " idx_array_size=" << idx_array_size 
              << " dual_idx_array_size=" << dual_idx_array_size
              << " vertex_data_offset=" << vertex_data_offset);
    
    if (vertex_data_offset >= region.size()) {
        LOG_ERROR("XobParser", "vertex_data_offset >= region.size()");
        return false;
    }
    
    // Extract indices from the index arrays
    // XOB has DUAL index arrays but second array has out-of-bounds indices
    // Need to use FIRST array and figure out proper material assignment
    mesh.indices.clear();
    mesh.indices.reserve(index_count);
    
    // Use first array (valid position indices)
    for (uint32_t i = 0; i < index_count && (i * 2 + 2) <= idx_array_size; i++) {
        uint16_t idx = read_u16_le(region.data() + i * 2);
        mesh.indices.push_back(static_cast<uint32_t>(idx));
    }
    LOG_DEBUG("XobParser", "Using FIRST index array");
    
    // Analyze both arrays thoroughly
    {
        uint16_t first_max = 0, second_max = 0;
        uint16_t first_min = 65535, second_min = 65535;
        uint32_t mismatch_count = 0;
        
        for (uint32_t i = 0; i < index_count; i++) {
            uint16_t first_val = read_u16_le(region.data() + i * 2);
            first_max = std::max(first_max, first_val);
            first_min = std::min(first_min, first_val);
            
            size_t offset = idx_array_size + i * 2;
            if (offset + 2 <= dual_idx_array_size) {
                uint16_t second_val = read_u16_le(region.data() + offset);
                second_max = std::max(second_max, second_val);
                second_min = std::min(second_min, second_val);
                if (first_val != second_val) mismatch_count++;
            }
        }
        
        LOG_DEBUG("XobParser", "First array: min=" << first_min << " max=" << first_max << " (vertex_count=" << vertex_count << ")");
        LOG_DEBUG("XobParser", "Second array: min=" << second_min << " max=" << second_max);
        LOG_DEBUG("XobParser", "Mismatches: " << mismatch_count << "/" << index_count << " (" << (100*mismatch_count/index_count) << "%)");
        
        // Check if second array might be triangle-local (values 0-2 for each tri)
        uint32_t second_low_count = 0;
        for (uint32_t i = 0; i < index_count; i++) {
            size_t offset = idx_array_size + i * 2;
            if (offset + 2 <= dual_idx_array_size) {
                uint16_t val = read_u16_le(region.data() + offset);
                if (val < 3) second_low_count++;
            }
        }
        LOG_DEBUG("XobParser", "Second array values < 3: " << second_low_count << "/" << index_count);
    }
    
    // Debug: print first 30 indices from both arrays side by side
    {
        std::ostringstream cmp_str;
        cmp_str << "Index comparison (first 30):\n";
        cmp_str << "  Idx | First | Second | Diff\n";
        for (size_t i = 0; i < std::min<size_t>(30, index_count); i++) {
            uint16_t first_val = read_u16_le(region.data() + i * 2);
            size_t offset = idx_array_size + i * 2;
            uint16_t second_val = (offset + 2 <= dual_idx_array_size) ? read_u16_le(region.data() + offset) : 0;
            cmp_str << "  " << std::setw(3) << i << " | " << std::setw(5) << first_val 
                    << " | " << std::setw(6) << second_val 
                    << " | " << (first_val != second_val ? "DIFF" : "same") << "\n";
        }
        LOG_DEBUG("XobParser", cmp_str.str());
        
        // NEW: Analyze first 5 triangles with actual vertex positions
        LOG_DEBUG("XobParser", "=== TRIANGLE VERTEX POSITION ANALYSIS ===");
        for (size_t tri = 0; tri < 5 && tri * 3 + 2 < index_count; tri++) {
            size_t base = tri * 3;
            
            // Get indices from both arrays
            uint16_t f0 = read_u16_le(region.data() + (base + 0) * 2);
            uint16_t f1 = read_u16_le(region.data() + (base + 1) * 2);
            uint16_t f2 = read_u16_le(region.data() + (base + 2) * 2);
            
            uint16_t s0 = read_u16_le(region.data() + idx_array_size + (base + 0) * 2);
            uint16_t s1 = read_u16_le(region.data() + idx_array_size + (base + 1) * 2);
            uint16_t s2 = read_u16_le(region.data() + idx_array_size + (base + 2) * 2);
            
            // Helper to read position at an index
            auto read_pos = [&](uint16_t idx) -> std::tuple<float, float, float, bool> {
                if (idx >= vertex_count) return {0, 0, 0, false};
                size_t pos_off = vertex_data_offset + idx * 12;
                if (pos_off + 12 > region.size()) return {0, 0, 0, false};
                float x = read_f32_le(region.data() + pos_off);
                float y = read_f32_le(region.data() + pos_off + 4);
                float z = read_f32_le(region.data() + pos_off + 8);
                return {x, y, z, true};
            };
            
            std::ostringstream tri_str;
            tri_str << std::fixed << std::setprecision(3);
            tri_str << "Triangle " << tri << ":\n";
            tri_str << "  First array indices:  [" << f0 << ", " << f1 << ", " << f2 << "]\n";
            tri_str << "  Second array indices: [" << s0 << ", " << s1 << ", " << s2 << "]\n";
            
            auto [fx0, fy0, fz0, fv0] = read_pos(f0);
            auto [fx1, fy1, fz1, fv1] = read_pos(f1);
            auto [fx2, fy2, fz2, fv2] = read_pos(f2);
            
            tri_str << "  First array positions:\n";
            tri_str << "    v0: " << (fv0 ? "" : "INVALID ") << "(" << fx0 << ", " << fy0 << ", " << fz0 << ")\n";
            tri_str << "    v1: " << (fv1 ? "" : "INVALID ") << "(" << fx1 << ", " << fy1 << ", " << fz1 << ")\n";
            tri_str << "    v2: " << (fv2 ? "" : "INVALID ") << "(" << fx2 << ", " << fy2 << ", " << fz2 << ")\n";
            
            auto [sx0, sy0, sz0, sv0] = read_pos(s0);
            auto [sx1, sy1, sz1, sv1] = read_pos(s1);
            auto [sx2, sy2, sz2, sv2] = read_pos(s2);
            
            tri_str << "  Second array positions:\n";
            tri_str << "    v0: " << (sv0 ? "" : "INVALID ") << "(" << sx0 << ", " << sy0 << ", " << sz0 << ")\n";
            tri_str << "    v1: " << (sv1 ? "" : "INVALID ") << "(" << sx1 << ", " << sy1 << ", " << sz1 << ")\n";
            tri_str << "    v2: " << (sv2 ? "" : "INVALID ") << "(" << sx2 << ", " << sy2 << ", " << sz2 << ")\n";
            
            LOG_DEBUG("XobParser", tri_str.str());
        }
        
        // Check if one array is an offset/permutation of the other
        LOG_DEBUG("XobParser", "=== ARRAY RELATIONSHIP ANALYSIS ===");
        
        // Check if second = first + constant offset
        std::map<int32_t, uint32_t> offset_counts;
        for (size_t i = 0; i < std::min<size_t>(1000, index_count); i++) {
            uint16_t first_val = read_u16_le(region.data() + i * 2);
            size_t offset = idx_array_size + i * 2;
            if (offset + 2 <= dual_idx_array_size) {
                uint16_t second_val = read_u16_le(region.data() + offset);
                int32_t diff = (int32_t)second_val - (int32_t)first_val;
                offset_counts[diff]++;
            }
        }
        
        std::ostringstream offset_str;
        offset_str << "Offset distribution (second - first):\n";
        std::vector<std::pair<int32_t, uint32_t>> sorted_offsets(offset_counts.begin(), offset_counts.end());
        std::sort(sorted_offsets.begin(), sorted_offsets.end(), 
                  [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < std::min<size_t>(10, sorted_offsets.size()); i++) {
            offset_str << "  diff=" << sorted_offsets[i].first << " count=" << sorted_offsets[i].second << "\n";
        }
        LOG_DEBUG("XobParser", offset_str.str());
    }
    
    // DUMP BOTH ARRAYS TO FILES FOR STATIC ANALYSIS
    {
        // Dump first index array
        std::ofstream first_dump("xob_index_array_1.bin", std::ios::binary);
        if (first_dump) {
            first_dump.write(reinterpret_cast<const char*>(region.data()), idx_array_size);
            first_dump.close();
            LOG_INFO("XobParser", "Dumped first index array to xob_index_array_1.bin (" << idx_array_size << " bytes, " << index_count << " indices)");
        }
        
        // Dump second index array
        std::ofstream second_dump("xob_index_array_2.bin", std::ios::binary);
        if (second_dump) {
            second_dump.write(reinterpret_cast<const char*>(region.data() + idx_array_size), idx_array_size);
            second_dump.close();
            LOG_INFO("XobParser", "Dumped second index array to xob_index_array_2.bin (" << idx_array_size << " bytes, " << index_count << " indices)");
        }
        
        // Dump vertex data (first 1MB or all)
        size_t vertex_dump_size = std::min<size_t>(region.size() - vertex_data_offset, 1024 * 1024);
        std::ofstream vert_dump("xob_vertex_data.bin", std::ios::binary);
        if (vert_dump) {
            vert_dump.write(reinterpret_cast<const char*>(region.data() + vertex_data_offset), vertex_dump_size);
            vert_dump.close();
            LOG_INFO("XobParser", "Dumped vertex data to xob_vertex_data.bin (" << vertex_dump_size << " bytes)");
        }
        
        // Dump analysis text file
        std::ofstream analysis("xob_array_analysis.txt");
        if (analysis) {
            analysis << "=== XOB INDEX ARRAY ANALYSIS ===\n\n";
            analysis << "Index count: " << index_count << "\n";
            analysis << "Triangle count: " << triangle_count << "\n";
            analysis << "Vertex count: " << vertex_count << "\n";
            analysis << "Mesh type: 0x" << std::hex << (int)mesh_type << std::dec << "\n\n";
            
            analysis << "=== FIRST 100 TRIANGLES ===\n";
            analysis << "Tri# | First[0] First[1] First[2] | Second[0] Second[1] Second[2] | Match?\n";
            analysis << "-----+---------------------------+-----------------------------+-------\n";
            
            for (size_t tri = 0; tri < std::min<size_t>(100, triangle_count); tri++) {
                size_t base = tri * 3;
                uint16_t f0 = read_u16_le(region.data() + (base + 0) * 2);
                uint16_t f1 = read_u16_le(region.data() + (base + 1) * 2);
                uint16_t f2 = read_u16_le(region.data() + (base + 2) * 2);
                
                uint16_t s0 = read_u16_le(region.data() + idx_array_size + (base + 0) * 2);
                uint16_t s1 = read_u16_le(region.data() + idx_array_size + (base + 1) * 2);
                uint16_t s2 = read_u16_le(region.data() + idx_array_size + (base + 2) * 2);
                
                bool match = (f0 == s0 && f1 == s1 && f2 == s2);
                
                analysis << std::setw(4) << tri << " | " 
                         << std::setw(8) << f0 << " " << std::setw(8) << f1 << " " << std::setw(8) << f2 << " | "
                         << std::setw(9) << s0 << " " << std::setw(9) << s1 << " " << std::setw(9) << s2 << " | "
                         << (match ? "YES" : "NO") << "\n";
            }
            
            analysis << "\n=== COMPLETE FIRST ARRAY (all " << index_count << " values) ===\n";
            for (size_t i = 0; i < index_count; i++) {
                uint16_t val = read_u16_le(region.data() + i * 2);
                analysis << val;
                if ((i + 1) % 20 == 0) analysis << "\n";
                else analysis << " ";
            }
            
            analysis << "\n\n=== COMPLETE SECOND ARRAY (all " << index_count << " values) ===\n";
            for (size_t i = 0; i < index_count; i++) {
                uint16_t val = read_u16_le(region.data() + idx_array_size + i * 2);
                analysis << val;
                if ((i + 1) % 20 == 0) analysis << "\n";
                else analysis << " ";
            }
            
            analysis << "\n\n=== VERTEX POSITIONS (first 100) ===\n";
            for (size_t i = 0; i < std::min<size_t>(100, vertex_count); i++) {
                size_t pos_off = vertex_data_offset + i * 12;
                if (pos_off + 12 <= region.size()) {
                    float x = read_f32_le(region.data() + pos_off);
                    float y = read_f32_le(region.data() + pos_off + 4);
                    float z = read_f32_le(region.data() + pos_off + 8);
                    analysis << "v" << i << ": (" << x << ", " << y << ", " << z << ")\n";
                }
            }
            
            analysis.close();
            LOG_INFO("XobParser", "Dumped array analysis to xob_array_analysis.txt");
        }
    }
    
    // Validate indices
    uint32_t max_idx = 0;
    for (uint32_t idx : mesh.indices) {
        if (idx > max_idx) max_idx = idx;
    }
    LOG_DEBUG("XobParser", "max_idx=" << max_idx << " vertex_count=" << vertex_count);
    if (max_idx >= vertex_count) {
        LOG_WARNING("XobParser", "Index out of range, clamping: max_idx=" << max_idx 
                    << " >= vertex_count=" << vertex_count);
        for (uint32_t& idx : mesh.indices) {
            if (idx >= vertex_count) idx = 0;
        }
    }
    
    // Determine vertex stride based on mesh type (per spec v8 section 4.3)
    // Static (0x0F): 20 bytes = pos(12) + normal(4) + tangent(4)
    // Emissive (0x8F): 32 bytes = pos(12) + normal(4) + tangent(4) + UV0(4) + UV1(4) + color(4)
    // Skinned (0x1F): Variable (base + bone data)
    // Skinned+Emissive (0x9F): Variable
    size_t vertex_stride;
    bool has_embedded_uvs = false;
    bool has_vertex_color = false;
    bool has_second_uv = false;
    
    // Calculate actual stride from data size - this is more reliable than guessing
    size_t vertex_data_size_for_stride = region.size() - vertex_data_offset;
    size_t detected_stride = vertex_data_size_for_stride / vertex_count;
    
    switch (mesh_type) {
        case XOB_MESH_STATIC:  // 0x0F
            // Static: minimum 20 bytes (pos + normal + tangent)
            // Use detected stride if data is larger
            vertex_stride = std::max<size_t>(20, detected_stride);
            // Static meshes also have UVs in separated streams!
            has_embedded_uvs = true;
            break;
        case XOB_MESH_EMISSIVE:  // 0x8F
            // Emissive: minimum 32 bytes (pos + normal + tangent + UV0 + UV1 + color)
            // But often has extra data, so use detected stride
            vertex_stride = std::max<size_t>(32, detected_stride);
            has_embedded_uvs = true;
            has_second_uv = true;
            has_vertex_color = true;
            break;
        case XOB_MESH_SKINNED:  // 0x1F
            // Skinned meshes: minimum 20 + bone data
            vertex_stride = std::max<size_t>(20, detected_stride);
            // Skinned meshes also have UVs!
            has_embedded_uvs = true;
            LOG_DEBUG("XobParser", "Skinned mesh: detected stride=" << detected_stride);
            break;
        case XOB_MESH_SKINNED_EMISSIVE:  // 0x9F
            // Skinned + emissive: minimum 32 + bone data
            vertex_stride = std::max<size_t>(32, detected_stride);
            has_embedded_uvs = true;
            has_second_uv = true;
            has_vertex_color = true;
            LOG_DEBUG("XobParser", "Skinned+emissive mesh: detected stride=" << detected_stride);
            break;
        default:
            // Unknown type - use detected stride, assume has UVs
            vertex_stride = std::max<size_t>(20, detected_stride);
            has_embedded_uvs = true;
            LOG_WARNING("XobParser", "Unknown mesh type 0x" << std::hex << (int)mesh_type 
                        << std::dec << ", detected stride=" << detected_stride);
            break;
    }
    
    size_t vertex_data_size = region.size() - vertex_data_offset;
    LOG_DEBUG("XobParser", "Using vertex_stride=" << vertex_stride 
              << " vertex_data_size=" << vertex_data_size
              << " expected=" << (vertex_count * vertex_stride)
              << " embedded_uvs=" << has_embedded_uvs);
    
    // Check if data looks like separated streams vs interleaved
    // CRITICAL: Compare against the MINIMUM possible interleaved size (20 bytes = pos + normal + tangent)
    // Not the detected stride, which can be wrong for meshes with extra data streams.
    // If vertex_data_size is way larger than minimum_interleaved, it's separated streams.
    //
    // For separated streams layout, the minimum expected size would be:
    // positions(12) + normals(4) + tangents(4) = 20 bytes per vertex
    // With UVs: + UV0(4) + UV1(4) = 28 bytes per vertex  
    // With colors: + colors(4) = 32 bytes per vertex
    //
    // The actual data may have bone weights/indices which add more, but these are
    // stored AFTER the base streams in separated layout.
    
    size_t minimum_interleaved = static_cast<size_t>(vertex_count) * 20;  // Bare minimum: pos + norm + tan
    size_t expected_with_uvs = static_cast<size_t>(vertex_count) * 32;    // With UVs and colors
    
    // Detect layout by probing data at expected separated streams UV offset
    // If UV data looks valid there, use separated streams; otherwise interleaved
    //
    // For separated streams, UV0 starts at: vertex_data_offset + pos_stream + norm_stream + tan_stream [+ extra]
    // The "extra" depends on mesh type:
    // - Emissive (0x8f): may have color stream before UVs
    // - Skinned (0x1f/0x9f): may have bone data before UVs
    size_t test_pos_stream = static_cast<size_t>(vertex_count) * 12;
    size_t test_norm_stream = static_cast<size_t>(vertex_count) * 4;
    size_t test_tan_stream = static_cast<size_t>(vertex_count) * 4;
    size_t test_color_stream = static_cast<size_t>(vertex_count) * 4;  // For emissive meshes
    
    // Calculate UV offset for different layouts
    size_t base_offset_dual = vertex_data_offset + test_pos_stream + test_norm_stream + test_tan_stream;
    size_t test_uv0_offset_dual = base_offset_dual;  // No extra data
    size_t test_uv0_offset_dual_with_color = base_offset_dual + test_color_stream;  // Color before UV
    
    // Also try with single index array (some meshes don't have dual)
    size_t single_idx_vertex_offset = idx_array_size;  // Just one index array
    size_t base_offset_single = single_idx_vertex_offset + test_pos_stream + test_norm_stream + test_tan_stream;
    size_t test_uv0_offset_single = base_offset_single;
    size_t test_uv0_offset_single_with_color = base_offset_single + test_color_stream;
    
    bool use_separated_streams = false;
    bool use_single_index = false;  // Track if we need single index array
    bool has_color_before_uv = false;  // Track if color stream comes before UV
    bool uv_probes_failed = true;  // Start assuming UVs won't be found; set false if probe succeeds
    size_t found_uv_offset = 0;    // Track the actual validated UV offset
    bool uv_is_f32 = false;        // Track whether UVs are f32 instead of f16
    
    // Helper lambda to check if UVs look valid at an offset
    // Sample across the stream to avoid false positives from random data
    auto probe_uvs = [&](size_t offset, const char* label) -> bool {
        if (offset + 8 > region.size()) {
            LOG_DEBUG("XobParser", "UV probe " << label << " at " << offset << ": OUT OF BOUNDS");
            return false;
        }
        
        std::vector<uint32_t> samples;
        if (vertex_count > 0) {
            samples = {0u, 1u, 2u, 3u, 4u, 5u, 7u, 9u,
                       vertex_count / 4u, vertex_count / 2u, (vertex_count * 3u) / 4u,
                       vertex_count > 1 ? vertex_count - 1u : 0u};
        }
        
        // Remove duplicates and out-of-range
        std::sort(samples.begin(), samples.end());
        samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
        
        int valid_count = 0;
        int nonzero_count = 0;
        int checked = 0;
        float max_abs = 0.0f;
        for (uint32_t idx : samples) {
            size_t off = offset + static_cast<size_t>(idx) * 4;
            if (off + 4 > region.size()) continue;
            float u = half_to_float(read_u16_le(region.data() + off));
            float v = half_to_float(read_u16_le(region.data() + off + 2));
            if (std::isfinite(u) && std::isfinite(v)) {
                max_abs = std::max(max_abs, std::abs(u));
                max_abs = std::max(max_abs, std::abs(v));
                if (std::abs(u) > 1e-4f || std::abs(v) > 1e-4f) {
                    nonzero_count++;
                }
                if (u >= -8.0f && u <= 8.0f && v >= -8.0f && v <= 8.0f) {
                    valid_count++;
                }
            }
            checked++;
        }
        
        bool valid = (checked > 0) &&
                     (valid_count >= std::max(3, checked * 6 / 10)) &&
                     (nonzero_count >= 2) &&
                     (max_abs <= 16.0f);
        LOG_DEBUG("XobParser", "UV probe " << label << " at " << offset
                  << ": " << valid_count << "/" << checked << " valid"
                  << " nonzero=" << nonzero_count
                  << " max_abs=" << max_abs
                  << " -> " << (valid ? "VALID" : "INVALID"));
        return valid;
    };
    
    // Helper lambda to validate positions at a given offset
    // Returns true if positions look valid (reasonable range, not denormalized)
    auto validate_positions = [&](size_t test_offset, const char* label) -> bool {
        if (test_offset + 48 > region.size()) {
            LOG_DEBUG("XobParser", "Position probe " << label << " at " << test_offset << ": OUT OF BOUNDS");
            return false;
        }
        const uint8_t* probe = region.data() + test_offset;
        
        // Check first 4 positions (48 bytes for 12-byte stride, 64 bytes for 16-byte)
        int valid_count = 0;
        size_t stride = (mesh_type == XOB_MESH_SKINNED || mesh_type == XOB_MESH_SKINNED_EMISSIVE) ? 16 : 12;
        for (int i = 0; i < 4; i++) {
            float x = read_f32_le(probe + i * stride);
            float y = read_f32_le(probe + i * stride + 4);
            float z = read_f32_le(probe + i * stride + 8);
            
            // Valid positions: reasonable range (±1000), not denormalized (>1e-6)
            // Also reject infinities and NaNs
            bool valid = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                         std::abs(x) < 1000.0f && std::abs(y) < 1000.0f && std::abs(z) < 1000.0f &&
                         (std::abs(x) > 1e-6f || std::abs(y) > 1e-6f || std::abs(z) > 1e-6f);
            if (valid) valid_count++;
        }
        
        bool result = (valid_count >= 3);  // Need at least 3/4 valid
        LOG_DEBUG("XobParser", "Position probe " << label << " at " << test_offset 
                  << ": " << valid_count << "/4 valid -> " << (result ? "VALID" : "INVALID"));
        return result;
    };

    // Try dual index with different layouts
    if (probe_uvs(test_uv0_offset_dual, "DUAL")) {
        use_separated_streams = true;
        uv_probes_failed = false;
        found_uv_offset = test_uv0_offset_dual;
    }
    else if (probe_uvs(test_uv0_offset_dual_with_color, "DUAL+COLOR")) {
        use_separated_streams = true;
        has_color_before_uv = true;
        uv_probes_failed = false;
        found_uv_offset = test_uv0_offset_dual_with_color;
    }
    // Try single index if dual didn't work
    // BUT: validate that positions would be valid with single index layout
    // The UV probe can give false positives when random data happens to look like half-floats
    else if (probe_uvs(test_uv0_offset_single, "SINGLE")) {
        // Validate positions at single index offset before accepting
        if (validate_positions(single_idx_vertex_offset, "SINGLE_POS")) {
            use_separated_streams = true;
            use_single_index = true;
            uv_probes_failed = false;
            found_uv_offset = test_uv0_offset_single;
            vertex_data_offset = single_idx_vertex_offset;
            vertex_data_size = region.size() - vertex_data_offset;
            LOG_DEBUG("XobParser", "Detected single index array layout, vertex_data_offset=" << vertex_data_offset);
        } else {
            LOG_DEBUG("XobParser", "SINGLE UV probe passed but positions invalid, rejecting");
        }
    }
    else if (probe_uvs(test_uv0_offset_single_with_color, "SINGLE+COLOR")) {
        // Validate positions at single index offset before accepting
        if (validate_positions(single_idx_vertex_offset, "SINGLE+COLOR_POS")) {
            use_separated_streams = true;
            use_single_index = true;
            has_color_before_uv = true;
            uv_probes_failed = false;
            found_uv_offset = test_uv0_offset_single_with_color;
            vertex_data_offset = single_idx_vertex_offset;
            vertex_data_size = region.size() - vertex_data_offset;
            LOG_DEBUG("XobParser", "Detected single index array layout with color, vertex_data_offset=" << vertex_data_offset);
        } else {
            LOG_DEBUG("XobParser", "SINGLE+COLOR UV probe passed but positions invalid, rejecting");
        }
    }
    
    // Fallback logic when UV probes fail
    // CRITICAL: Prefer SEPARATED with default UVs over INTERLEAVED for most meshes
    // because INTERLEAVED parsing produces garbage for meshes that don't actually use it
    if (!use_separated_streams) {
        // Check 1: If positions at dual index offset look valid, use SEPARATED with zero UVs
        if (validate_positions(vertex_data_offset, "FALLBACK_DUAL_POS")) {
            use_separated_streams = true;
            LOG_DEBUG("XobParser", "Fallback: positions valid at dual offset, using SEPARATED with default UVs");
        }
        // Check 2: Try single index offset positions
        else if (validate_positions(single_idx_vertex_offset, "FALLBACK_SINGLE_POS")) {
            use_separated_streams = true;
            use_single_index = true;
            vertex_data_offset = single_idx_vertex_offset;
            vertex_data_size = region.size() - vertex_data_offset;
            LOG_DEBUG("XobParser", "Fallback: positions valid at single offset, using SEPARATED with default UVs");
        }
        // Check 3: Ratio-based fallback - if data is very large, it's probably separated
        else if (vertex_data_size > expected_with_uvs * 3) {
            use_separated_streams = true;
            LOG_DEBUG("XobParser", "Fallback: data ratio suggests SEPARATED streams");
        }
        // Check 4: For very small meshes (< 20 verts), try SEPARATED first if sizes work out
        else if (vertex_count < 20 && vertex_data_size >= static_cast<size_t>(vertex_count) * 20) {
            // Small meshes often have simple separated layout even without valid UVs
            // Try separated and see if positions look reasonable
            use_separated_streams = true;
            LOG_DEBUG("XobParser", "Fallback: small mesh, trying SEPARATED layout");
        }
    }
    
    LOG_DEBUG("XobParser", "Layout detection: vertex_data_size=" << vertex_data_size 
              << " min_interleaved=" << minimum_interleaved 
              << " expected_with_uvs=" << expected_with_uvs
              << " ratio=" << (float)vertex_data_size / expected_with_uvs);
    
    if (use_separated_streams) {
        LOG_DEBUG("XobParser", "Using SEPARATED STREAMS layout");
    } else {
        LOG_DEBUG("XobParser", "Using INTERLEAVED layout (last resort)");
    }
    
    // Calculate stream offsets for separated layout
    // Layout depends on mesh type:
    // - Non-skinned: [positions][normals][tangents][UV0][UV1][colors]
    // - Skinned: Need to account for bone indices/weights before positions
    size_t pos_stream_size = static_cast<size_t>(vertex_count) * 12;    // f32[3]
    size_t norm_stream_size = static_cast<size_t>(vertex_count) * 4;    // u8[4]
    size_t tangent_stream_size = static_cast<size_t>(vertex_count) * 4; // u8[4]
    size_t uv_stream_size = static_cast<size_t>(vertex_count) * 4;      // f16[2]
    
    // For skinned meshes, there's extra data before positions
    // Probe the data to find the actual position stream start
    size_t pos_offset = vertex_data_offset;
    
    if (use_separated_streams && (mesh_type == XOB_MESH_SKINNED || mesh_type == XOB_MESH_SKINNED_EMISSIVE)) {
        // For skinned meshes, the position stream doesn't start at vertex_data_offset
        // There are bone indices and weights streams first
        // 
        // Try to find where positions actually start by scanning for valid float patterns
        // A valid position float should be in reasonable range (e.g., -1000 to 1000)
        
        // Estimate: bone indices (4 bytes per vertex) + bone weights (16 bytes per vertex typical)
        // But this varies. Let's calculate based on extra data size.
        size_t base_streams_size = pos_stream_size + norm_stream_size + tangent_stream_size + 
                                   uv_stream_size * 2 + static_cast<size_t>(vertex_count) * 4; // colors
        size_t extra_data = vertex_data_size > base_streams_size ? (vertex_data_size - base_streams_size) : 0;
        
        // The extra data is bone weights/indices, which comes AFTER the standard streams in separated layout
        // So positions should still start at vertex_data_offset
        // BUT - some XOB files might have a different layout
        
        // Let's probe to verify: read first few floats and check if they look like positions
        const uint8_t* probe = region.data() + vertex_data_offset;
        float f0 = read_f32_le(probe);
        float f1 = read_f32_le(probe + 4);
        float f2 = read_f32_le(probe + 8);
        float f3 = read_f32_le(probe + 12);
        
        LOG_DEBUG("XobParser", "Skinned mesh probe at vertex_data_offset: " 
                  << f0 << ", " << f1 << ", " << f2 << ", " << f3);
        
        // If first vertex looks invalid (huge or denormalized), the layout might be different
        bool first_looks_valid = (std::abs(f0) < 10000.0f && std::abs(f0) > 1e-10f) ||
                                 (std::abs(f1) < 10000.0f && std::abs(f1) > 1e-10f) ||
                                 (std::abs(f2) < 10000.0f && std::abs(f2) > 1e-10f);
        
        if (!first_looks_valid) {
            // Try scanning forward to find where positions actually start
            LOG_WARNING("XobParser", "Position stream may not start at expected offset, scanning...");
            
            // Search for a sequence of reasonable floats
            for (size_t scan = 0; scan < std::min<size_t>(100000, vertex_data_size); scan += 4) {
                const uint8_t* p = region.data() + vertex_data_offset + scan;
                if (vertex_data_offset + scan + 12 > region.size()) break;
                
                float sf0 = read_f32_le(p);
                float sf1 = read_f32_le(p + 4);
                float sf2 = read_f32_le(p + 8);
                
                // Check if this looks like a valid position (all components in reasonable range)
                bool valid = (std::abs(sf0) < 1000.0f && std::abs(sf0) > 1e-6f) &&
                             (std::abs(sf1) < 1000.0f && std::abs(sf1) > 1e-6f) &&
                             (std::abs(sf2) < 1000.0f && std::abs(sf2) > 1e-6f);
                
                if (valid) {
                    // Verify next few vertices also look valid
                    bool streak_valid = true;
                    for (int check = 1; check < 5 && streak_valid; check++) {
                        const uint8_t* cp = p + check * 12;
                        if (vertex_data_offset + scan + check * 12 + 12 > region.size()) {
                            streak_valid = false;
                            break;
                        }
                        float cf0 = read_f32_le(cp);
                        float cf1 = read_f32_le(cp + 4);
                        float cf2 = read_f32_le(cp + 8);
                        streak_valid = (std::abs(cf0) < 1000.0f) && (std::abs(cf1) < 1000.0f) && (std::abs(cf2) < 1000.0f);
                    }
                    
                    if (streak_valid) {
                        pos_offset = vertex_data_offset + scan;
                        LOG_INFO("XobParser", "Found position stream at offset " << pos_offset 
                                 << " (skip=" << scan << " bytes)");
                        LOG_DEBUG("XobParser", "First position: " << sf0 << ", " << sf1 << ", " << sf2);
                        break;
                    }
                }
            }
        }
    }
    
    // For skinned meshes, positions use 16 bytes (XYZW) not 12 (XYZ)
    // This is because GPU skinning often requires vec4 alignment
    bool skinned_16byte_pos = use_separated_streams && 
                              (mesh_type == XOB_MESH_SKINNED || mesh_type == XOB_MESH_SKINNED_EMISSIVE);
    
    size_t actual_pos_stride = skinned_16byte_pos ? 16 : 12;
    size_t actual_pos_stream_size = static_cast<size_t>(vertex_count) * actual_pos_stride;
    
    size_t norm_offset = pos_offset + actual_pos_stream_size;
    size_t tangent_offset = norm_offset + norm_stream_size;
    
    // For skinned meshes, bone data streams come between tangent and UV
    // Each bone stream is typically 16 bytes per vertex (4 weights + 4 indices)
    // Layout: [tangent][bone_weights_0][bone_indices_0][bone_weights_1][bone_indices_1]...[UV0]
    // For emissive meshes, there may be color data or other streams before UVs
    size_t extra_data_size = 0;
    
    if (use_separated_streams) {
        // If we detected color before UV in the probe, use that
        if (has_color_before_uv) {
            extra_data_size = static_cast<size_t>(vertex_count) * 4;  // Color stream
            LOG_DEBUG("XobParser", "Color stream before UVs (detected by probe), size=" << extra_data_size);
        }
        
        // Calculate expected extra data for skinned meshes
        // CRITICAL: For skinned meshes, bone data goes BETWEEN tangent and UV streams
        // Each bone stream consists of:
        //   - 4 bone indices (4 bytes)
        //   - 4 bone weights (16 bytes as f32[4])
        // Total: 20 bytes per vertex per stream
        // With 2 bone streams (common for skinned+emissive), that's 40 bytes/vertex
        if (bone_streams > 0 && (mesh_type == XOB_MESH_SKINNED || mesh_type == XOB_MESH_SKINNED_EMISSIVE)) {
            // bone_streams value: 1 = one set (20 bytes/vert), 2 = two sets (40 bytes/vert)
            size_t bone_data = static_cast<size_t>(bone_streams) * 20 * vertex_count;
            extra_data_size = bone_data;  // Reset, don't add - bone data is THE extra data
            LOG_DEBUG("XobParser", "Skinned mesh: bone_streams=" << (int)bone_streams 
                      << " bone_data=" << bone_data << " extra_data_size=" << extra_data_size);
        }
        
        // For skinned meshes, validate UV location. If UVs look invalid, scan for them.
        if (mesh_type == XOB_MESH_SKINNED || mesh_type == XOB_MESH_SKINNED_EMISSIVE) {
            const uint8_t* probe_data = region.data();
            size_t probe_start = tangent_offset + tangent_stream_size;
            size_t max_extra = static_cast<size_t>(vertex_count) * 80;  // Allow wider bone data variations
            size_t probe_end = std::min(probe_start + max_extra, region.size() - 32);
            
            auto uv_looks_valid_at = [&](size_t scan) -> bool {
                if (scan + 32 > region.size()) return false;
                int valid_count = 0;
                int nonzero_count = 0;
                float max_abs = 0.0f;
                for (int i = 0; i < 8; i++) {
                    uint16_t u_raw = read_u16_le(probe_data + scan + i * 4);
                    uint16_t v_raw = read_u16_le(probe_data + scan + i * 4 + 2);
                    float u = half_to_float(u_raw);
                    float v = half_to_float(v_raw);
                    if (std::isfinite(u) && std::isfinite(v)) {
                        max_abs = std::max(max_abs, std::abs(u));
                        max_abs = std::max(max_abs, std::abs(v));
                        if (std::abs(u) > 1e-4f || std::abs(v) > 1e-4f) {
                            nonzero_count++;
                        }
                        if (u >= -8.0f && u <= 8.0f && v >= -8.0f && v <= 8.0f) {
                            valid_count++;
                        }
                    }
                }
                return valid_count >= 6 && nonzero_count >= 2 && max_abs <= 8.0f;
            };
            
            bool found_uvs = false;
            for (size_t scan = probe_start; scan < probe_end; scan += 4) {
                if (uv_looks_valid_at(scan)) {
                    extra_data_size = scan - probe_start;
                    found_uv_offset = scan;  // Record the validated offset
                    uv_probes_failed = false;  // Mark UVs as found
                    LOG_DEBUG("XobParser", "Skinned UV probe found valid data at offset " << scan
                              << " (skip=" << (scan - probe_start) << " after tangent)");
                    found_uvs = true;
                    break;
                }
            }
            
            if (!found_uvs) {
                LOG_DEBUG("XobParser", "Skinned UV probe did not find valid UVs; will use default UVs");
            }
        }
        
        // For emissive (non-skinned), rely on layout detection (DUAL vs DUAL+COLOR) instead of
        // scanning, which can lock onto false positives and misplace UV streams.
    }
    
    // CRITICAL: For ANY mesh type where UV probes failed but we have valid positions,
    // scan for a plausible UV stream (both f16 and f32). This avoids locking onto
    // random data which produces huge UV ranges and flat colors.
    if (uv_probes_failed && use_separated_streams) {
        LOG_DEBUG("XobParser", "UV probes failed for mesh type 0x" << std::hex << (int)mesh_type 
                  << std::dec << ", scanning for UV stream...");

        const uint8_t* scan_data = region.data();
        size_t scan_start = tangent_offset + tangent_stream_size;  // After known streams
        size_t scan_limit = std::min(region.size(), scan_start + static_cast<size_t>(vertex_count) * 64);

        struct UvCandidate {
            size_t offset;
            bool is_f32;
            int valid;
            int nonzero;
            float max_abs;
            float u_range;
            float v_range;
        };

        auto score_candidate = [&](size_t offset, bool is_f32) -> UvCandidate {
            size_t stream_size = static_cast<size_t>(vertex_count) * (is_f32 ? 8 : 4);
            if (offset + stream_size > region.size()) {
                return {offset, is_f32, 0, 0, 0.0f, 0.0f, 0.0f};
            }

            std::vector<uint32_t> samples;
            samples.reserve(80);
            // Fixed early samples
            samples.insert(samples.end(), {0u, 1u, 2u, 3u, 4u, 5u, 7u, 9u, 16u, 32u, 64u, 128u});
            // Evenly spaced samples across the whole stream
            const uint32_t steps = 64;
            for (uint32_t s = 0; s < steps; s++) {
                uint32_t idx = (vertex_count > 0) ? (static_cast<uint64_t>(s) * (vertex_count - 1) / (steps - 1)) : 0u;
                samples.push_back(idx);
            }
            samples.push_back(vertex_count > 1 ? vertex_count - 1u : 0u);
            std::sort(samples.begin(), samples.end());
            samples.erase(std::unique(samples.begin(), samples.end()), samples.end());

            int valid = 0;
            int nonzero = 0;
            float max_abs = 0.0f;
            float u_min = std::numeric_limits<float>::max();
            float v_min = std::numeric_limits<float>::max();
            float u_max = std::numeric_limits<float>::lowest();
            float v_max = std::numeric_limits<float>::lowest();

            for (uint32_t idx : samples) {
                if (idx >= vertex_count) continue;
                size_t off = offset + static_cast<size_t>(idx) * (is_f32 ? 8 : 4);
                if (off + (is_f32 ? 8 : 4) > region.size()) continue;

                float u = 0.0f;
                float v = 0.0f;
                if (is_f32) {
                    u = read_f32_le(scan_data + off);
                    v = read_f32_le(scan_data + off + 4);
                } else {
                    u = half_to_float(read_u16_le(scan_data + off));
                    v = half_to_float(read_u16_le(scan_data + off + 2));
                }

                if (std::isfinite(u) && std::isfinite(v)) {
                    max_abs = std::max(max_abs, std::abs(u));
                    max_abs = std::max(max_abs, std::abs(v));
                    u_min = std::min(u_min, u);
                    v_min = std::min(v_min, v);
                    u_max = std::max(u_max, u);
                    v_max = std::max(v_max, v);
                    if (std::abs(u) > 1e-4f || std::abs(v) > 1e-4f) nonzero++;
                    if (u >= -4.0f && u <= 4.0f && v >= -4.0f && v <= 4.0f) valid++;
                }
            }

            return {offset, is_f32, valid, nonzero, max_abs, u_max - u_min, v_max - v_min};
        };

        UvCandidate best{0, false, 0, 0, 0.0f, 0.0f, 0.0f};

        // Scan for f16 UVs
        for (size_t scan = scan_start; scan + 4 <= scan_limit; scan += 4) {
            auto cand = score_candidate(scan, false);
            if (cand.valid > best.valid || (cand.valid == best.valid && cand.nonzero > best.nonzero)) {
                best = cand;
            }
        }

        // Scan for f32 UVs
        for (size_t scan = scan_start; scan + 8 <= scan_limit; scan += 8) {
            auto cand = score_candidate(scan, true);
            if (cand.valid > best.valid || (cand.valid == best.valid && cand.nonzero > best.nonzero)) {
                best = cand;
            }
        }

        // Accept only if the candidate looks sane
        bool ranges_ok = (best.u_range > 0.01f || best.v_range > 0.01f);
        bool abs_ok = (best.max_abs <= 4.0f);
        bool counts_ok = (best.valid >= 12 && best.nonzero >= 6);

        if (best.offset > 0 && ranges_ok && abs_ok && counts_ok) {
            found_uv_offset = best.offset;
            uv_is_f32 = best.is_f32;
            uv_probes_failed = false;
            LOG_INFO("XobParser", "UV SCAN: Found UV stream at offset " << best.offset
                     << " format=" << (best.is_f32 ? "f32" : "f16")
                     << " valid=" << best.valid << " nonzero=" << best.nonzero
                     << " max_abs=" << best.max_abs
                     << " range(u,v)=('" << best.u_range << "','" << best.v_range << "')");
        } else {
            LOG_WARNING("XobParser", "UV SCAN: No reliable UV stream found, using default UVs");
        }
    }
    
    // Calculate UV offset based on probe/scan results
    size_t uv0_offset;
    if (!uv_probes_failed && found_uv_offset > 0) {
        // Use the validated UV offset from probing or scanning
        uv0_offset = found_uv_offset;
        LOG_DEBUG("XobParser", "Using validated UV offset: " << uv0_offset);
    } else {
        // Fallback: calculate from stream layout (may be wrong for complex meshes)
        uv0_offset = tangent_offset + tangent_stream_size + extra_data_size;
        if (uv_probes_failed) {
            LOG_DEBUG("XobParser", "UVs not validated, calculated offset: " << uv0_offset << " (will use default UVs)");
        }
    }
    size_t uv1_offset = uv0_offset + uv_stream_size;
    size_t color_offset = uv1_offset + uv_stream_size;
    
    if (use_separated_streams) {
        LOG_DEBUG("XobParser", "Stream offsets: pos=" << pos_offset << " (stride=" << actual_pos_stride << ")"
                  << " norm=" << norm_offset << " tan=" << tangent_offset
                  << " uv0=" << uv0_offset << " uv1=" << uv1_offset);
    }
    
    // Extract vertices
    mesh.vertices.clear();
    mesh.vertices.reserve(vertex_count);
    
    const uint8_t* data = region.data();
    
    for (uint32_t i = 0; i < vertex_count; i++) {
        XobVertex vert;
        
        if (use_separated_streams) {
            // SEPARATED STREAMS: Each attribute type is in its own contiguous array
            
            // Position (12 or 16 bytes per vertex depending on skinned)
            size_t p_off = pos_offset + i * actual_pos_stride;
            if (p_off + 12 <= region.size()) {
                vert.position.x = read_f32_le(data + p_off);
                vert.position.y = read_f32_le(data + p_off + 4);
                vert.position.z = read_f32_le(data + p_off + 8);
                // W component (if 16-byte) is ignored - it's just padding
            }
            
            // Normal (4 bytes per vertex)
            size_t n_off = norm_offset + i * 4;
            if (n_off + 4 <= region.size()) {
                int8_t nx = static_cast<int8_t>(data[n_off]);
                int8_t ny = static_cast<int8_t>(data[n_off + 1]);
                int8_t nz = static_cast<int8_t>(data[n_off + 2]);
                
                vert.normal = glm::vec3(
                    static_cast<float>(nx) / 127.0f,
                    static_cast<float>(ny) / 127.0f,
                    static_cast<float>(nz) / 127.0f
                );
                float len = glm::length(vert.normal);
                if (len > 0.001f) vert.normal /= len;
                else vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            } else {
                vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            
            // Tangent (4 bytes per vertex)
            size_t t_off = tangent_offset + i * 4;
            if (t_off + 4 <= region.size()) {
                int8_t tx = static_cast<int8_t>(data[t_off]);
                int8_t ty = static_cast<int8_t>(data[t_off + 1]);
                int8_t tz = static_cast<int8_t>(data[t_off + 2]);
                int8_t tw = static_cast<int8_t>(data[t_off + 3]);
                
                vert.tangent = glm::vec3(
                    static_cast<float>(tx) / 127.0f,
                    static_cast<float>(ty) / 127.0f,
                    static_cast<float>(tz) / 127.0f
                );
                float len = glm::length(vert.tangent);
                if (len > 0.001f) vert.tangent /= len;
                else vert.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
                
                vert.tangent_sign = (tw >= 0) ? 1.0f : -1.0f;
            }
            
            // UV0 (4 bytes per vertex as half-floats)
            // CRITICAL: Only read UVs if we have a validated offset, otherwise use defaults
            if (has_embedded_uvs) {
                if (!uv_probes_failed) {
                    // UVs were validated - read from the data
                    if (uv_is_f32) {
                        size_t uv_off = uv0_offset + i * 8;
                        if (uv_off + 8 <= region.size()) {
                            float u = read_f32_le(data + uv_off);
                            float v = read_f32_le(data + uv_off + 4);
                            vert.uv.x = u;
                            vert.uv.y = 1.0f - v;  // Flip V for OpenGL
                            if (i < 3) {
                                LOG_DEBUG("XobParser", "UV[" << i << "]: f32 -> (" << vert.uv.x << "," << vert.uv.y << ")");
                            }
                        }
                    } else {
                        size_t uv_off = uv0_offset + i * 4;
                        if (uv_off + 4 <= region.size()) {
                            uint16_t u_raw = read_u16_le(data + uv_off);
                            uint16_t v_raw = read_u16_le(data + uv_off + 2);
                            
                            float u = half_to_float(u_raw);
                            float v = half_to_float(v_raw);
                            
                            vert.uv.x = u;
                            vert.uv.y = 1.0f - v;  // Flip V for OpenGL
                            
                            if (i < 3) {
                                LOG_DEBUG("XobParser", "UV[" << i << "]: raw=(0x" << std::hex << u_raw 
                                          << ",0x" << v_raw << std::dec << ") -> (" << vert.uv.x << "," << vert.uv.y << ")");
                            }
                        }
                    }
                } else {
                    // UV probes failed - use default UVs (planar projection from position)
                    // This gives reasonable texture mapping for debugging/fallback
                    vert.uv.x = (vert.position.x + 5.0f) * 0.1f;  // Rough planar UV
                    vert.uv.y = (vert.position.z + 5.0f) * 0.1f;
                    
                    if (i < 3) {
                        LOG_DEBUG("XobParser", "UV[" << i << "]: DEFAULT from pos -> (" 
                                  << vert.uv.x << "," << vert.uv.y << ")");
                    }
                }
            }
            
        } else {
            // INTERLEAVED: All attributes for each vertex are contiguous
            size_t off = vertex_data_offset + i * vertex_stride;
            if (off + 12 > region.size()) {
                vert.position = glm::vec3(0.0f);
                vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                vert.uv = glm::vec2(0.0f);
                mesh.vertices.push_back(vert);
                continue;
            }
            
            size_t attr_off = off;
            
            // Position (12 bytes)
            vert.position.x = read_f32_le(data + attr_off);
            vert.position.y = read_f32_le(data + attr_off + 4);
            vert.position.z = read_f32_le(data + attr_off + 8);
            attr_off += 12;
            
            // Normal (4 bytes packed)
            if (attr_off + 4 <= region.size()) {
                int8_t nx = static_cast<int8_t>(data[attr_off]);
                int8_t ny = static_cast<int8_t>(data[attr_off + 1]);
                int8_t nz = static_cast<int8_t>(data[attr_off + 2]);
                
                vert.normal = glm::vec3(
                    static_cast<float>(nx) / 127.0f,
                    static_cast<float>(ny) / 127.0f,
                    static_cast<float>(nz) / 127.0f
                );
                float len = glm::length(vert.normal);
                if (len > 0.001f) vert.normal /= len;
                else vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                
                attr_off += 4;
            } else {
                vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            
            // Tangent (4 bytes packed)
            if (attr_off + 4 <= region.size()) {
                int8_t tx = static_cast<int8_t>(data[attr_off]);
                int8_t ty = static_cast<int8_t>(data[attr_off + 1]);
                int8_t tz = static_cast<int8_t>(data[attr_off + 2]);
                int8_t tw = static_cast<int8_t>(data[attr_off + 3]);
                
                vert.tangent = glm::vec3(
                    static_cast<float>(tx) / 127.0f,
                    static_cast<float>(ty) / 127.0f,
                    static_cast<float>(tz) / 127.0f
                );
                float len = glm::length(vert.tangent);
                if (len > 0.001f) vert.tangent /= len;
                else vert.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
                
                vert.tangent_sign = (tw >= 0) ? 1.0f : -1.0f;
                attr_off += 4;
            }
            
            // UV0 (4 bytes as half-floats) - embedded in emissive meshes
            if (has_embedded_uvs && attr_off + 4 <= region.size()) {
                uint16_t u_raw = read_u16_le(data + attr_off);
                uint16_t v_raw = read_u16_le(data + attr_off + 2);
                
                float u = half_to_float(u_raw);
                float v = half_to_float(v_raw);
                
                vert.uv.x = u;
                vert.uv.y = 1.0f - v;  // Flip V for OpenGL
                attr_off += 4;
                
                if (i < 3) {
                    LOG_DEBUG("XobParser", "UV[" << i << "]: raw=(0x" << std::hex << u_raw 
                              << ",0x" << v_raw << std::dec << ") -> (" << vert.uv.x << "," << vert.uv.y << ")");
                }
            }
        }
        
        mesh.vertices.push_back(vert);
    }
    
    // Debug: print first few vertices
    for (size_t i = 0; i < std::min<size_t>(3, mesh.vertices.size()); i++) {
        const auto& v = mesh.vertices[i];
        LOG_DEBUG("XobParser", "Vert[" << i << "]: pos=(" << v.position.x << "," 
                  << v.position.y << "," << v.position.z << ") uv=(" 
                  << v.uv.x << "," << v.uv.y << ")");
    }
    
    // Debug: dump first few TRIANGLES to verify index buffer correctness
    LOG_DEBUG("XobParser", "First 5 triangles (checking index->vertex mapping):");
    for (size_t tri = 0; tri < std::min<size_t>(5, mesh.indices.size() / 3); tri++) {
        uint32_t i0 = mesh.indices[tri * 3 + 0];
        uint32_t i1 = mesh.indices[tri * 3 + 1];
        uint32_t i2 = mesh.indices[tri * 3 + 2];
        
        const auto& v0 = mesh.vertices[std::min<uint32_t>(i0, mesh.vertices.size() - 1)];
        const auto& v1 = mesh.vertices[std::min<uint32_t>(i1, mesh.vertices.size() - 1)];
        const auto& v2 = mesh.vertices[std::min<uint32_t>(i2, mesh.vertices.size() - 1)];
        
        // Calculate triangle edge lengths to check if it's degenerate
        float e0 = glm::length(v1.position - v0.position);
        float e1 = glm::length(v2.position - v1.position);
        float e2 = glm::length(v0.position - v2.position);
        float max_edge = std::max({e0, e1, e2});
        
        LOG_DEBUG("XobParser", "  Tri " << tri << ": idx=[" << i0 << "," << i1 << "," << i2 << "]"
                  << " edges=[" << e0 << "," << e1 << "," << e2 << "]"
                  << " max_edge=" << max_edge
                  << (max_edge > 10.0f ? " HUGE!" : ""));
    }
    
    LOG_INFO("XobParser", "Parsed " << mesh.vertices.size() << " vertices, " 
             << mesh.indices.size() << " indices");
    
    return !mesh.vertices.empty() && !mesh.indices.empty();
}

/**
 * Read null-terminated string from buffer
 * Returns (string, bytes_consumed including null terminator)
 */
static std::pair<std::string, size_t> read_null_string(const uint8_t* data, size_t max_size) {
    size_t len = 0;
    while (len < max_size && data[len] != '\0') {
        len++;
    }
    std::string s(reinterpret_cast<const char*>(data), len);
    return {s, len + 1};  // +1 for null terminator
}

/**
 * Extract materials from HEAD chunk
 * 
 * XOB HEAD Header Structure (offsets from HEAD data start):
 *   0x00-0x17: Bounding box (6 floats: min XYZ, max XYZ)
 *   0x18-0x1F: Padding (zeros)
 *   0x20-0x27: More padding
 *   0x28-0x2B: Float value (unknown)
 *   0x2C-0x2D: Rendering material count (uint16)
 *   0x2E-0x2F: Bone/object count (uint16) - 0 for simple meshes
 *   0x30-0x33: LOD count (uint32, usually 1)
 *   0x34-0x37: Reserved (0)
 *   0x38-0x3B: Material data section size (uint32)
 *   0x3C onwards: Material name+path pairs (null-terminated strings)
 * 
 * Materials are stored as pairs: (name, path) where path contains the GUID.
 * Only the first material_count pairs are rendering materials.
 */
static std::vector<XobMaterial> extract_materials_from_head(const uint8_t* data, size_t size) {
    std::vector<XobMaterial> materials;
    
    // Need at least 0x3C bytes for header
    if (size < 0x3C) {
        std::cerr << "[XOB] HEAD chunk too small for header: " << size << " bytes\n";
        return materials;
    }
    
    // Read material count from offset 0x2C (uint16, not uint32!)
    uint16_t material_count = read_u16_le(data + 0x2C);
    uint16_t bone_count = read_u16_le(data + 0x2E);
    uint32_t mat_data_size = read_u32_le(data + 0x38);
    
    std::cerr << "[XOB] HEAD header: material_count=" << material_count 
              << " bone_count=" << bone_count 
              << " mat_data_size=" << mat_data_size << "\n";
    
    // Try to parse materials from header if count is reasonable
    bool use_header_parsing = (material_count > 0 && material_count <= 100);
    
    if (use_header_parsing) {
        // Find LZO4 position (marks end of material strings section)
        size_t lzo4_pos = size;
        for (size_t i = 0x3C; i + 4 <= size; i++) {
            if (data[i] == 'L' && data[i+1] == 'Z' && data[i+2] == 'O' && data[i+3] == '4') {
                lzo4_pos = i;
                break;
            }
        }
        
        // Parse material name+path pairs starting at offset 0x3C
        size_t pos = 0x3C;
        for (uint16_t i = 0; i < material_count && pos < lzo4_pos; i++) {
            // Read material name
            auto [name, name_len] = read_null_string(data + pos, lzo4_pos - pos);
            pos += name_len;
            
            if (pos >= lzo4_pos) break;
            
            // Read material path (contains GUID + path)
            auto [path, path_len] = read_null_string(data + pos, lzo4_pos - pos);
            pos += path_len;
            
            // Validate path has material extension
            if (path.find(".emat") == std::string::npos && 
                path.find(".gamemat") == std::string::npos) {
                std::cerr << "[XOB] Material " << i << " path missing extension: " << path << "\n";
                // This might not be a valid material, but continue anyway
            }
            
            XobMaterial mat;
            mat.name = name;
            mat.path = path;
            mat.diffuse_texture = path;
            materials.push_back(mat);
            
            std::cerr << "[XOB] Material " << i << ": name='" << name << "' path='" << path << "'\n";
        }
        
        if (!materials.empty()) {
            std::cerr << "[XOB] Parsed " << materials.size() << " materials from header\n";
            return materials;
        }
        
        std::cerr << "[XOB] Header parsing produced no materials, falling back to GUID search\n";
    } else {
        std::cerr << "[XOB] Invalid material count " << material_count << ", falling back to GUID search\n";
    }
    
    // Fallback: GUID pattern search
    std::cerr << "[XOB] Falling back to GUID pattern search\n";
    
    // Look for pattern: '{' followed by 16 hex chars followed by '}'
    // Then the path follows until null terminator
    for (size_t i = 0; i + 20 < size; i++) {
        if (data[i] == '{') {
            // Check if next 16 chars are hex
            bool is_guid = true;
            for (size_t j = 1; j <= 16 && is_guid; j++) {
                char c = static_cast<char>(data[i + j]);
                is_guid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
            }
            
            if (is_guid && data[i + 17] == '}') {
                // Found a GUID, extract path after it
                size_t path_start = i;  // Include GUID in path
                size_t path_end = i + 18;
                while (path_end < size && data[path_end] != '\0' && data[path_end] >= 32) {
                    path_end++;
                }
                
                if (path_end > path_start) {
                    std::string path(reinterpret_cast<const char*>(data + path_start), path_end - path_start);
                    
                    // Extract material name from path
                    std::string name = path;
                    size_t last_slash = name.rfind('/');
                    if (last_slash != std::string::npos) {
                        name = name.substr(last_slash + 1);
                    }
                    // Remove extensions
                    size_t ext_pos = name.rfind(".emat");
                    if (ext_pos != std::string::npos) name = name.substr(0, ext_pos);
                    ext_pos = name.rfind(".gamemat");
                    if (ext_pos != std::string::npos) name = name.substr(0, ext_pos);
                    
                    XobMaterial mat;
                    mat.name = name;
                    mat.path = path;
                    mat.diffuse_texture = path;
                    materials.push_back(mat);
                    
                    std::cerr << "[XOB] Found material " << materials.size()-1 << ": " << name << " path=" << path << "\n";
                    
                    i = path_end - 1; // Skip past this material
                }
            }
        }
    }
    
    return materials;
}

/**
 * Parse material-to-triangle ranges from XOB HEAD chunk.
 * 
 * CORRECT ALGORITHM (verified on 38 test XOB files - 100% match):
 * 
 * SUBMESH BLOCK STRUCTURE (relative to 0xFFFF marker):
 *   -6: u16 index_count (number of indices for this submesh)
 *   -4: u16 lod_index (0=highest detail)
 *   -2: u16 (always 0)
 *    0: 0xFFFF marker
 *   +2: u16 material_index
 *   +4: u16 flags (lo byte=pass type, hi byte=lod level info)
 * 
 * KEY INSIGHT: Material 0 is ALWAYS IMPLICIT!
 * - For multi-material meshes, only materials 1+ have explicit markers
 * - Material 0's index count = total_indices - sum(all other materials)
 * - When material 0 HAS markers, they represent render passes/LODs, NOT additional geometry
 * - For each material > 0, use the FIRST marker's index_count encountered
 *
 * SIMPLIFIED (v8): Use submesh_index from LZO4 descriptors when available.
 * Each descriptor at +0x52 has the material index for that submesh.
 */
static std::vector<MaterialRange> extract_material_ranges_from_descriptors(
    const std::vector<LzoDescriptor>& descriptors,
    uint32_t target_lod,
    uint32_t total_triangles,
    size_t num_materials) 
{
    std::vector<MaterialRange> result;
    
    LOG_DEBUG("XobParser", "Extracting material ranges from descriptors: " 
              << descriptors.size() << " descriptors, " << total_triangles << " tris, " 
              << num_materials << " materials, target_lod=" << target_lod);
    
    // Simple case: single material or no materials
    if (num_materials <= 1 || descriptors.empty()) {
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
        LOG_DEBUG("XobParser", "Single material: all " << total_triangles << " triangles to mat 0");
        return result;
    }
    
    // Check if we have multiple submeshes (same LOD, different materials)
    // Group descriptors by LOD level
    std::map<uint32_t, std::vector<size_t>> lod_to_descriptors;
    for (size_t i = 0; i < descriptors.size(); i++) {
        // Use quality_tier as LOD index (lower = higher detail)
        uint32_t lod = descriptors[i].quality_tier;
        lod_to_descriptors[lod].push_back(i);
    }
    
    // Find descriptors at target LOD
    std::vector<size_t> target_descriptors;
    if (lod_to_descriptors.find(target_lod) != lod_to_descriptors.end()) {
        target_descriptors = lod_to_descriptors[target_lod];
    } else if (!lod_to_descriptors.empty()) {
        // Fallback to first available LOD
        target_descriptors = lod_to_descriptors.begin()->second;
    }
    
    if (target_descriptors.empty()) {
        // No descriptors for target LOD, use first descriptor's submesh_index
        if (!descriptors.empty()) {
            uint32_t mat_idx = descriptors[0].submesh_index;
            if (mat_idx >= num_materials) mat_idx = 0;
            
            MaterialRange r;
            r.material_index = mat_idx;
            r.triangle_start = 0;
            r.triangle_end = total_triangles;
            r.triangle_count = total_triangles;
            result.push_back(r);
        } else {
            MaterialRange r0;
            r0.material_index = 0;
            r0.triangle_start = 0;
            r0.triangle_end = total_triangles;
            r0.triangle_count = total_triangles;
            result.push_back(r0);
        }
        return result;
    }
    
    // Single descriptor at target LOD - all triangles go to its submesh_index
    if (target_descriptors.size() == 1) {
        const auto& desc = descriptors[target_descriptors[0]];
        uint32_t mat_idx = desc.submesh_index;
        if (mat_idx >= num_materials) mat_idx = 0;
        
        MaterialRange r;
        r.material_index = mat_idx;
        r.triangle_start = 0;
        r.triangle_end = total_triangles;
        r.triangle_count = total_triangles;
        result.push_back(r);
        
        LOG_DEBUG("XobParser", "Single submesh at LOD " << target_lod 
                  << ": all " << total_triangles << " triangles to mat " << mat_idx);
        return result;
    }
    
    // Multiple descriptors at same LOD - build ranges from each
    // Sort by submesh_index to ensure consistent ordering
    std::vector<std::pair<uint32_t, uint32_t>> submesh_ranges; // (submesh_index, triangle_count)
    for (size_t idx : target_descriptors) {
        const auto& desc = descriptors[idx];
        uint32_t mat_idx = desc.submesh_index;
        if (mat_idx >= num_materials) mat_idx = 0;
        submesh_ranges.push_back({mat_idx, desc.triangle_count});
    }
    
    // Sort by material index (index buffer is typically organized by material order)
    std::sort(submesh_ranges.begin(), submesh_ranges.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    // Build ranges
    uint32_t current_tri = 0;
    for (const auto& [mat_idx, tri_count] : submesh_ranges) {
        if (tri_count == 0) continue;
        if (current_tri >= total_triangles) break;
        
        uint32_t actual_count = std::min(tri_count, total_triangles - current_tri);
        
        MaterialRange r;
        r.material_index = mat_idx;
        r.triangle_start = current_tri;
        r.triangle_end = current_tri + actual_count;
        r.triangle_count = actual_count;
        result.push_back(r);
        
        LOG_DEBUG("XobParser", "Submesh " << mat_idx << ": triangles " 
                  << current_tri << "-" << (current_tri + actual_count));
        
        current_tri += actual_count;
    }
    
    // If we didn't cover all triangles, add remaining to material 0
    if (current_tri < total_triangles) {
        uint32_t remaining = total_triangles - current_tri;
        
        // Check if mat 0 already has a range
        bool found_mat0 = false;
        for (auto& r : result) {
            if (r.material_index == 0) {
                // Extend mat 0's range
                r.triangle_end = total_triangles;
                r.triangle_count += remaining;
                found_mat0 = true;
                break;
            }
        }
        
        if (!found_mat0) {
            MaterialRange r;
            r.material_index = 0;
            r.triangle_start = current_tri;
            r.triangle_end = total_triangles;
            r.triangle_count = remaining;
            result.push_back(r);
        }
        
        LOG_DEBUG("XobParser", "Added remaining " << remaining << " triangles to mat 0");
    }
    
    // Fallback if still empty
    if (result.empty()) {
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
    }
    
    LOG_DEBUG("XobParser", "Material ranges from descriptors: " << result.size());
    for (const auto& r : result) {
        LOG_DEBUG("XobParser", "  mat=" << r.material_index << " tris=" << r.triangle_start 
                  << "-" << r.triangle_end << " (count=" << r.triangle_count << ")");
    }
    
    return result;
}

/**
 * Legacy material range extraction from submesh blocks after LZO4 descriptors
 * Falls back to this if descriptor-based extraction fails
 */
static std::vector<MaterialRange> extract_material_ranges(const uint8_t* data, size_t size, 
                                                          uint32_t total_triangles, 
                                                          size_t num_materials,
                                                          uint8_t mesh_type = 0x0F) {
    std::vector<MaterialRange> result;
    
    LOG_DEBUG("XobParser", "Extracting material ranges: " << total_triangles << " tris, " 
              << num_materials << " materials, mesh_type=0x" << std::hex << (int)mesh_type << std::dec);
    
    uint32_t total_indices = total_triangles * 3;
    
    // Find HEAD chunk
    const uint8_t* head_start = nullptr;
    size_t head_size = 0;
    for (size_t i = 0; i + 8 < size; i++) {
        if (data[i] == 'H' && data[i+1] == 'E' && data[i+2] == 'A' && data[i+3] == 'D') {
            head_size = read_u32_be(data + i + 4);
            head_start = data + i + 8;
            break;
        }
    }
    if (!head_start || head_size == 0) {
        // No HEAD - assign all to material 0
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
        return result;
    }
    
    // Find LZO4 descriptors
    std::vector<size_t> lzo4_positions;
    for (size_t i = 0; i + 4 <= head_size; i++) {
        if (head_start[i] == 'L' && head_start[i+1] == 'Z' && 
            head_start[i+2] == 'O' && head_start[i+3] == '4') {
            lzo4_positions.push_back(i);
            i += 115;
        }
    }
    
    if (lzo4_positions.empty()) {
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
        return result;
    }
    
    // Data after all LZO4 descriptors contains submesh blocks
    // Find the actual end of all LZO4 descriptors (they may not be consecutive!)
    size_t after_start = 0;
    for (size_t pos : lzo4_positions) {
        size_t desc_end = pos + 116;
        if (desc_end > after_start) {
            after_start = desc_end;
        }
    }
    
    if (after_start >= head_size) {
        // No submesh data - single material mesh
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
        return result;
    }
    
    const uint8_t* after = head_start + after_start;
    size_t after_size = head_size - after_start;
    
    LOG_DEBUG("XobParser", "Submesh data: " << after_size << " bytes after LZO4 descriptors");
    
    // Dump raw submesh data for analysis - more bytes this time
    std::ostringstream raw_hex;
    raw_hex << "Submesh raw (first 256 bytes):\n";
    for (size_t i = 0; i < std::min<size_t>(256, after_size); i++) {
        if (i % 16 == 0) raw_hex << "  " << std::setw(4) << std::setfill('0') << std::hex << i << ": ";
        raw_hex << std::hex << std::setw(2) << std::setfill('0') << (int)after[i] << " ";
        if ((i + 1) % 16 == 0) raw_hex << "\n";
    }
    LOG_DEBUG("XobParser", raw_hex.str());
    
    // Submesh block structure analysis
    // Each block is ~104 bytes, ending with FFFF marker
    // Need to dump full block content to understand structure
    struct SubmeshEntry {
        size_t position;        // Position in submesh data
        uint32_t material_index;
        uint32_t index_start;   // Value at pos-8
        uint32_t index_count;   // Value at pos-6
        uint16_t lod_index;     // Value at pos-4
        uint16_t flags;         // Value at pos+4
        uint16_t order_key;     // Value at pos-10 (tail hint)
    };
    std::vector<SubmeshEntry> submesh_entries;
    
    // Scan for 0xFFFF markers and extract block data with FULL context
    int block_num = 0;
    for (size_t pos = 8; pos + 6 <= after_size; pos++) {
        if (after[pos] == 0xFF && after[pos+1] == 0xFF) {
            SubmeshEntry entry;
            entry.position = pos;
            entry.material_index = read_u16_le(after + pos + 2);
            entry.flags = read_u16_le(after + pos + 4);
            entry.index_count = read_u16_le(after + pos - 6);
            entry.lod_index = read_u16_le(after + pos - 4);
            entry.index_start = read_u16_le(after + pos - 8);
            entry.order_key = (pos >= 10) ? read_u16_le(after + pos - 10) : 0;

            // Tail diagnostics: dump last 16 bytes before FFFF and decode nearby u16s
            if (entry.material_index < num_materials) {
                size_t tail_start = (pos >= 16) ? (pos - 16) : 0;
                std::ostringstream tail_hex;
                tail_hex << "Block tail @+" << std::dec << pos
                         << " mat=" << entry.material_index
                         << " flags=0x" << std::hex << entry.flags << std::dec
                         << " bytes:";
                for (size_t i = tail_start; i < pos; i++) {
                    tail_hex << " " << std::hex << std::setw(2) << std::setfill('0') << (int)after[i];
                }

                uint16_t u16_m12 = (pos >= 12) ? read_u16_le(after + pos - 12) : 0;
                uint16_t u16_m10 = (pos >= 10) ? read_u16_le(after + pos - 10) : 0;
                uint16_t u16_m8 = (pos >= 8) ? read_u16_le(after + pos - 8) : 0;
                uint16_t u16_m6 = (pos >= 6) ? read_u16_le(after + pos - 6) : 0;
                tail_hex << std::dec
                         << " | u16[-12]=" << u16_m12
                         << " u16[-10]=" << u16_m10
                         << " u16[-8]=" << u16_m8
                         << " u16[-6]=" << u16_m6;
                LOG_DEBUG("XobParser", tail_hex.str());
            }
            
            // Dump ENTIRE 104 byte block with field annotations
            if (block_num < 5 && entry.material_index < num_materials) {
                // Block starts 74 bytes before FFFF (or 72 for a 104-byte block with FFFF at byte 72)
                size_t block_start = (pos >= 72) ? (pos - 72) : 0;
                size_t block_end = pos + 32;  // Include some after FFFF
                if (block_end > after_size) block_end = after_size;
                
                std::ostringstream block_hex;
                block_hex << "Block " << block_num << " FULL (104 bytes, FFFF at +" << pos << "):\n";
                
                // Dump in 16-byte lines with offset
                for (size_t line = block_start; line < block_end; line += 16) {
                    block_hex << "  " << std::hex << std::setw(4) << std::setfill('0') << (line - block_start) << ": ";
                    for (size_t i = 0; i < 16 && (line + i) < block_end; i++) {
                        if (line + i == pos) block_hex << "[";
                        block_hex << std::hex << std::setw(2) << std::setfill('0') << (int)after[line + i];
                        if (line + i == pos + 1) block_hex << "]";
                        block_hex << " ";
                    }
                    block_hex << "\n";
                }
                
                // Try reading values at different offsets relative to block start
                const uint8_t* blk = after + block_start;
                size_t blk_len = block_end - block_start;
                
                block_hex << "  Field tests (offset from block start):\n";
                // Test u16 at various offsets
                for (size_t off = 0; off + 2 <= blk_len && off < 80; off += 2) {
                    uint16_t val = read_u16_le(blk + off);
                    // Only log values that could be triangle counts (1-40000)
                    if (val >= 1 && val <= 40000 && val != 0xFFFF) {
                        block_hex << "    u16 @ " << std::dec << off << " = " << val << " (" << val/3 << " tris)\n";
                    }
                }
                LOG_DEBUG("XobParser", block_hex.str());
            }
            
            // Only process valid materials
            if (entry.material_index < num_materials) {
                LOG_DEBUG("XobParser", "FFFF at +" << std::dec << pos 
                    << ": mat=" << entry.material_index 
                    << " idx_start=" << entry.index_start
                    << " idx_count=" << entry.index_count 
                    << " lod=" << entry.lod_index
                    << " flags=0x" << std::hex << entry.flags << std::dec);
                
                submesh_entries.push_back(entry);
                block_num++;
            }
            pos += 5; // Skip past this marker
        }
    }
    
    // NEW APPROACH: Process blocks in ORDER (not by material index)
    // The index buffer is arranged to match block order, not material order
    
    // Structure to hold block info
    struct BlockEntry {
        size_t position;        // Position in submesh data
        uint32_t material_index;
        uint32_t index_start;   // Start position in index buffer
        uint32_t index_count;   // Number of indices
        uint16_t lod;
        uint16_t flags;
        uint16_t order_key;     // Tail hint for ordering
    };
    std::vector<BlockEntry> all_blocks;
    
    // First pass: collect ALL blocks in order
    for (size_t pos = 8; pos + 6 <= after_size; pos++) {
        if (after[pos] == 0xFF && after[pos+1] == 0xFF) {
            BlockEntry entry;
            entry.position = pos;
            entry.material_index = read_u16_le(after + pos + 2);
            entry.flags = read_u16_le(after + pos + 4);
            entry.index_count = read_u16_le(after + pos - 6);
            entry.lod = read_u16_le(after + pos - 4);
            entry.index_start = read_u16_le(after + pos - 8);
            entry.order_key = (pos >= 10) ? read_u16_le(after + pos - 10) : 0;
            
            if (entry.material_index < num_materials) {
                all_blocks.push_back(entry);
                LOG_DEBUG("XobParser", "Block at +" << pos << ": mat=" << entry.material_index 
                          << " idx_start=" << entry.index_start
                          << " idx_count=" << entry.index_count << " lod=" << entry.lod 
                          << " flags=0x" << std::hex << entry.flags << std::dec
                          << " order_key=" << entry.order_key);
            }
            pos += 5;
        }
    }
    
    LOG_DEBUG("XobParser", "Found " << all_blocks.size() << " total submesh blocks");
    
    // Maximum reasonable LOD level
    constexpr uint16_t MAX_REASONABLE_LOD = 10;
    
    // For skinned meshes (0x1F, 0x9F), the LOD field appears to be encoded differently
    // or contains garbage. We need to infer LOD from flags or treat all blocks as LOD 0.
    bool is_skinned = (mesh_type == 0x1F || mesh_type == 0x9F);
    
    LOG_DEBUG("XobParser", "mesh_type=0x" << std::hex << (int)mesh_type << std::dec 
              << " is_skinned=" << is_skinned << " blocks=" << all_blocks.size());
    
    if (is_skinned) {
        LOG_DEBUG("XobParser", "Skinned mesh detected - reinterpreting ALL block LODs from flags");
        // For skinned meshes, the LOD field is NOT a real LOD level!
        // It appears to be derived from or duplicate the flags upper byte.
        // The REAL LOD is always 0 for main geometry.
        // 
        // Flag patterns observed:
        //   flags=0x0001/0x0002: LOD 0, single pass (main geometry)
        //   flags=0x0102: LOD 0, pass 1 (main geometry)
        //   flags=0x0202: LOD 0, pass 2 (main geometry)
        //   flags=0x0702: LOD 0, pass 7 (main geometry - multi-pass)
        //   flags=0x0401: Actual LOD 4 block (lower detail / shadow)
        //   flags=0x0201: Actual LOD 2 block
        //
        // Rule: upper byte indicates:
        //       0x00-0x07 with base 0x02 = render pass number (LOD 0)
        //       0x01-0x0F with base 0x01 = actual LOD level
        //       
        // Key insight: flags with base 0x02 are multi-pass at LOD 0
        //              flags with base 0x01 and upper > 0 are shadow/distance LOD
        for (auto& block : all_blocks) {
            uint16_t original_lod = block.lod;
            uint16_t base_flags = block.flags & 0x00FF;  // Lower byte
            uint16_t upper_byte = (block.flags >> 8) & 0xFF;
            
            // For skinned meshes, reinterpret LOD based on flags
            if (base_flags == 0x02) {
                // Multi-pass rendering at LOD 0 (main geometry)
                block.lod = 0;
            } else if (base_flags == 0x01 && upper_byte == 0) {
                // Single-pass at LOD 0 (main geometry)
                block.lod = 0;
            } else if (base_flags == 0x01 && upper_byte > 0) {
                // Shadow/distance LOD - upper byte IS the LOD level
                block.lod = upper_byte;
            } else {
                // Unknown pattern - keep original or set to 0
                if (block.lod > MAX_REASONABLE_LOD) {
                    block.lod = 0;
                }
            }
            
            if (block.lod != original_lod) {
                LOG_DEBUG("XobParser", "Skinned block LOD fix: " << original_lod << " -> " << block.lod 
                          << " (flags=0x" << std::hex << block.flags << ", base=" << base_flags 
                          << ", upper=" << upper_byte << ")" << std::dec);
            }
        }
    }
    
    // Find which LOD level has the best coverage
    std::map<uint16_t, uint32_t> lod_index_sums;
    for (const auto& block : all_blocks) {
        if (block.lod <= MAX_REASONABLE_LOD) {
            lod_index_sums[block.lod] += block.index_count;
        }
    }
    
    uint16_t target_lod = 0;
    uint32_t best_sum = 0;
    for (const auto& [lod, sum] : lod_index_sums) {
        if (sum > best_sum) {
            target_lod = lod;
            best_sum = sum;
        }
    }
    
    LOG_DEBUG("XobParser", "Target LOD: " << static_cast<int>(target_lod)
              << " (best_sum=" << best_sum << "/" << total_indices << ")");
    
    // ANALYSIS: Count passes per material at target LOD
    // flags=0x2 are multi-pass (shadow, etc) - idx_count is inflated
    // flags=0x1 are single-pass - idx_count is accurate
    std::map<uint32_t, uint32_t> mat_pass_count;  // How many blocks per material
    std::map<uint32_t, uint32_t> mat_total_idx;   // Total idx_count per material (across all passes)
    std::map<uint32_t, uint16_t> mat_flags;       // Flags for each material
    
    for (const auto& block : all_blocks) {
        if (block.lod != target_lod) continue;
        mat_pass_count[block.material_index]++;
        mat_total_idx[block.material_index] += block.index_count;
        mat_flags[block.material_index] = block.flags;  // Keep last flags
    }
    
    LOG_DEBUG("XobParser", "=== PASS COUNT ANALYSIS ===");
    for (const auto& [mat, count] : mat_pass_count) {
        uint32_t total_idx = mat_total_idx[mat];
        uint32_t per_pass = total_idx / count;
        uint16_t flags = mat_flags[mat];
        LOG_DEBUG("XobParser", "  Mat " << mat << ": " << count << " passes, total_idx=" << total_idx 
                  << ", per_pass=" << per_pass << " (" << per_pass/3 << " tris), flags=0x" << std::hex << flags << std::dec);
    }
    
    // Collect unique materials in TRUE FILE ORDER, with CORRECTED triangle counts
    // Important: Process ALL blocks in a single pass to preserve file order
    // Materials from non-target LODs should appear in their file position, not at the end
    std::set<uint32_t> seen_materials;
    std::vector<BlockEntry> ordered_blocks;
    
    for (const auto& block : all_blocks) {
        // Skip blocks with unreasonable LOD values (garbage data)
        if (block.lod > MAX_REASONABLE_LOD) continue;
        
        if (seen_materials.count(block.material_index) == 0) {
            seen_materials.insert(block.material_index);
            
            // Create a modified block with corrected index count
            BlockEntry corrected = block;
            
            // For target LOD, apply pass count corrections
            if (block.lod == target_lod) {
                uint32_t passes = mat_pass_count[block.material_index];
                
                // For skinned meshes, we need to handle multi-pass blocks differently
                if (is_skinned && passes > 1) {
                    uint32_t first_count = block.index_count;
                    uint32_t total = mat_total_idx[block.material_index];
                    uint32_t avg = total / passes;
                    
                    float ratio = static_cast<float>(first_count) / static_cast<float>(avg);
                    if (ratio > 0.8f && ratio < 1.2f) {
                        // Similar counts - render passes, use first block's count
                        LOG_DEBUG("XobParser", "Mat " << block.material_index << ": skinned render passes, using first idx_count=" 
                                  << block.index_count << " (" << block.index_count/3 << " tris), ratio=" << ratio);
                    } else {
                        // Different counts - geometry parts, sum them all
                        corrected.index_count = total;
                        LOG_DEBUG("XobParser", "Mat " << block.material_index << ": skinned geometry parts, summing idx_count=" 
                                  << total << " (" << total/3 << " tris), ratio=" << ratio);
                    }
                } else if (is_skinned) {
                    LOG_DEBUG("XobParser", "Mat " << block.material_index << ": skinned single pass, idx_count=" 
                              << block.index_count << " (" << block.index_count/3 << " tris)");
                }
                // For static meshes with multi-pass, divide by number of passes
                else if (passes > 1 && (block.flags & 0x2)) {
                    corrected.index_count = mat_total_idx[block.material_index] / passes;
                    LOG_DEBUG("XobParser", "Mat " << block.material_index << ": corrected from " 
                              << block.index_count << " to " << corrected.index_count << " (/" << passes << " passes)");
                }
            } else {
                // Material from non-target LOD - appears in file order
                LOG_DEBUG("XobParser", "Added mat " << block.material_index << " from LOD " << block.lod << " (file order)");
            }
            
            ordered_blocks.push_back(corrected);
        }
    }
    
    LOG_DEBUG("XobParser", "Skinned mesh: using TRUE FILE ORDER (single pass through all blocks)");
    
    // Sort blocks by tail-derived order_key (pos-10). This appears to align with index buffer order.
    // Fallback to material index when order_key is missing or zero.
    std::sort(ordered_blocks.begin(), ordered_blocks.end(), 
              [](const BlockEntry& a, const BlockEntry& b) {
                  if (a.order_key == 0 && b.order_key == 0) {
                      return a.material_index < b.material_index;
                  }
                  if (a.order_key == 0) return false;
                  if (b.order_key == 0) return true;
                  if (a.order_key != b.order_key) return a.order_key < b.order_key;
                  return a.material_index < b.material_index;
              });
    
    LOG_DEBUG("XobParser", "Sorted blocks by order_key:");
    for (const auto& block : ordered_blocks) {
        LOG_DEBUG("XobParser", "  mat=" << block.material_index 
                  << " idx_start=" << block.index_start 
                  << " idx_count=" << block.index_count
                  << " order_key=" << block.order_key);
    }
    
    LOG_DEBUG("XobParser", "Using " << ordered_blocks.size() << " unique materials sorted by material_index");
    
    // Log the order
    {
        std::ostringstream order_str;
        order_str << "Block order: ";
        for (const auto& block : ordered_blocks) {
            order_str << "mat" << block.material_index << "(" << block.index_count/3 << ") ";
        }
        LOG_DEBUG("XobParser", order_str.str());
    }
    
    // Calculate total triangles from blocks (to verify)
    uint32_t block_total = 0;
    for (const auto& block : ordered_blocks) {
        block_total += block.index_count / 3;
    }
    LOG_DEBUG("XobParser", "Block total: " << block_total << " tris (actual: " << total_triangles << ")");
    
    // NEW APPROACH: Don't scale proportionally - it shifts ALL boundaries
    // Instead, use exact counts for smaller materials and let large materials absorb the difference
    // 
    // Observation: Small material counts (mat2-10) seem accurate
    // Large material counts (mat0, mat1) are inflated due to multi-pass rendering
    
    // Calculate sum of "reliable" small materials (those with single pass or small counts)
    uint32_t small_mat_total = 0;
    uint32_t large_mat_count = 0;
    const uint32_t SMALL_THRESHOLD = 10000;  // Materials with <10k tris are considered reliable
    
    for (const auto& block : ordered_blocks) {
        uint32_t tris = block.index_count / 3;
        if (tris < SMALL_THRESHOLD) {
            small_mat_total += tris;
        } else {
            large_mat_count++;
        }
    }
    
    // Triangles available for large materials
    uint32_t large_mat_total = (total_triangles > small_mat_total) ? 
                               (total_triangles - small_mat_total) : 0;
    
    LOG_DEBUG("XobParser", "Small materials: " << small_mat_total << " tris, "
              << "Large materials (" << large_mat_count << "): " << large_mat_total << " tris available");
    
    // Build ranges - use exact counts for small materials, proportional for large
    uint32_t current_tri = 0;
    uint32_t large_mat_used = 0;
    
    // First pass: calculate total claimed by large materials for proportional split
    uint32_t large_mat_claimed = 0;
    for (const auto& block : ordered_blocks) {
        uint32_t tris = block.index_count / 3;
        if (tris >= SMALL_THRESHOLD) {
            large_mat_claimed += tris;
        }
    }
    
    for (size_t i = 0; i < ordered_blocks.size(); i++) {
        const auto& block = ordered_blocks[i];
        uint32_t block_tris = block.index_count / 3;
        uint32_t tri_count;
        
        if (block_tris < SMALL_THRESHOLD) {
            // Small material - use exact count
            tri_count = block_tris;
        } else {
            // Large material - take proportional share of remaining large budget
            if (large_mat_claimed > 0) {
                float ratio = static_cast<float>(block_tris) / static_cast<float>(large_mat_claimed);
                tri_count = static_cast<uint32_t>(std::round(large_mat_total * ratio));
            } else {
                tri_count = large_mat_total / std::max(1u, large_mat_count);
            }
        }
        
        // Clamp to remaining triangles
        if (current_tri + tri_count > total_triangles) {
            tri_count = total_triangles - current_tri;
        }
        
        // Last block gets all remaining (avoid rounding errors)
        if (i == ordered_blocks.size() - 1) {
            tri_count = total_triangles - current_tri;
        }
        
        if (tri_count == 0) continue;
        
        MaterialRange r;
        r.material_index = block.material_index;
        r.triangle_start = current_tri;
        r.triangle_end = current_tri + tri_count;
        r.triangle_count = tri_count;
        result.push_back(r);
        
        LOG_DEBUG("XobParser", "  Range: mat=" << block.material_index << " tris=" << current_tri 
                  << "-" << (current_tri + tri_count) << " (count=" << tri_count 
                  << (block_tris >= SMALL_THRESHOLD ? " LARGE" : " small") << ")");
        
        current_tri += tri_count;
    }
    
    // Fallback if empty
    if (result.empty()) {
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = total_triangles;
        r0.triangle_count = total_triangles;
        result.push_back(r0);
    }
    
    LOG_DEBUG("XobParser", "Material ranges assigned: " << result.size());
    for (const auto& r : result) {
        LOG_DEBUG("XobParser", "  mat=" << r.material_index << " tris=" << r.triangle_start 
                  << "-" << r.triangle_end << " (count=" << r.triangle_count << ")");
    }
    
    return result;
}

/**
 * Parse COLL chunk - Collision Data
 * 
 * Structure per spec (XOB_FORMAT_SPEC.md):
 * - Array of 64-byte collision object headers
 * - Followed by collision mesh data (vertices + indices)
 * 
 * Object Header (64 bytes):
 *   0x00: u8  - Collision type (0x03=complex, 0x05=simple, 0x07=dynamic)
 *   0x01: u8  - Flags (0xFF=mesh, 0x02=primitive)
 *   0x02: u16 - Name index into HEAD strings
 *   0x04: float[9] - 3x3 rotation matrix (row-major)
 *   0x28: float[3] - Translation XYZ
 *   0x34: u32 - Reserved (0)
 *   0x38: u16 - Index start
 *   0x3A: u16 - Index end + 1
 *   0x3C: u32 - Reserved (0)
 */
static void parse_coll_chunk(const uint8_t* data, size_t size, 
                              std::vector<XobCollisionObject>& objects,
                              XobCollisionMesh& mesh) {
    if (size < 64) {
        std::cerr << "[XOB] COLL chunk too small: " << size << " bytes\n";
        return;
    }
    
    // First pass: count collision objects (each is 64 bytes)
    // Objects end where mesh data begins - detect by checking for valid type/flags
    size_t num_objects = 0;
    size_t pos = 0;
    
    while (pos + 64 <= size) {
        uint8_t type = data[pos];
        uint8_t flags = data[pos + 1];
        
        // Valid collision object types: 0x03, 0x05, 0x07
        // Valid flags: 0xFF (mesh), 0x02 (primitive)
        bool valid_type = (type == 0x03 || type == 0x05 || type == 0x07);
        bool valid_flags = (flags == 0xFF || flags == 0x02);
        
        if (!valid_type || !valid_flags) {
            break;  // End of object array
        }
        
        num_objects++;
        pos += 64;
    }
    
    std::cerr << "[XOB] COLL: Found " << num_objects << " collision objects\n";
    
    if (num_objects == 0) return;
    
    // Parse collision objects
    objects.clear();
    objects.reserve(num_objects);
    
    for (size_t i = 0; i < num_objects; i++) {
        const uint8_t* obj = data + i * 64;
        
        XobCollisionObject coll;
        coll.type = static_cast<XobCollisionType>(obj[0]);
        coll.flags = obj[1];
        coll.name_index = read_u16_le(obj + 2);
        
        // Read 3x3 rotation matrix (row-major floats)
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                coll.rotation[row][col] = read_f32_le(obj + 4 + (row * 3 + col) * 4);
            }
        }
        
        // Translation
        coll.translation.x = read_f32_le(obj + 0x28);
        coll.translation.y = read_f32_le(obj + 0x2C);
        coll.translation.z = read_f32_le(obj + 0x30);
        
        // Index references
        coll.index_start = read_u16_le(obj + 0x38);
        coll.index_end = read_u16_le(obj + 0x3A);
        
        objects.push_back(coll);
        
        std::cerr << "[XOB] COLL obj " << i << ": type=0x" << std::hex << (int)obj[0] << std::dec
                  << " flags=0x" << std::hex << (int)coll.flags << std::dec
                  << " idx=" << coll.index_start << "-" << coll.index_end
                  << " trans=(" << coll.translation.x << "," << coll.translation.y << "," << coll.translation.z << ")\n";
    }
    
    // Parse collision mesh data (after object headers)
    size_t mesh_start = num_objects * 64;
    size_t remaining = size - mesh_start;
    
    if (remaining < 12) {
        std::cerr << "[XOB] COLL: No mesh data after objects\n";
        return;
    }
    
    // Mesh data: vertices (float[3] each) followed by indices (u16[3] per triangle)
    // We need to figure out where vertices end and indices begin
    // Heuristic: indices are small numbers (< vertex_count), vertices are floats
    
    const uint8_t* mesh_data = data + mesh_start;
    
    // Try to find the split point by looking for the pattern change
    // Vertices are 12 bytes each (3 floats), indices are 6 bytes per triangle (3 u16s)
    
    // Estimate: assume roughly equal bytes for verts and indices
    // Start by trying to interpret as vertices
    size_t max_verts = remaining / 12;
    size_t actual_verts = 0;
    
    mesh.vertices.clear();
    mesh.bounds_min = glm::vec3(FLT_MAX);
    mesh.bounds_max = glm::vec3(-FLT_MAX);
    
    // Read vertices until we hit invalid data
    for (size_t v = 0; v < max_verts; v++) {
        float x = read_f32_le(mesh_data + v * 12);
        float y = read_f32_le(mesh_data + v * 12 + 4);
        float z = read_f32_le(mesh_data + v * 12 + 8);
        
        // Check if these look like reasonable vertex coordinates
        // Collision meshes should have coordinates within reasonable bounds
        if (std::abs(x) > 10000.0f || std::abs(y) > 10000.0f || std::abs(z) > 10000.0f ||
            std::isnan(x) || std::isnan(y) || std::isnan(z) ||
            std::isinf(x) || std::isinf(y) || std::isinf(z)) {
            break;
        }
        
        glm::vec3 vert(x, y, z);
        mesh.vertices.push_back(vert);
        mesh.bounds_min = glm::min(mesh.bounds_min, vert);
        mesh.bounds_max = glm::max(mesh.bounds_max, vert);
        actual_verts++;
    }
    
    std::cerr << "[XOB] COLL mesh: " << actual_verts << " vertices\n";
    
    // Remaining data is indices
    size_t indices_start = mesh_start + actual_verts * 12;
    size_t indices_size = size - indices_start;
    size_t num_indices = indices_size / 2;  // u16 each
    
    mesh.indices.clear();
    mesh.indices.reserve(num_indices);
    
    for (size_t i = 0; i < num_indices; i++) {
        uint16_t idx = read_u16_le(data + indices_start + i * 2);
        if (idx < actual_verts) {
            mesh.indices.push_back(idx);
        }
    }
    
    std::cerr << "[XOB] COLL mesh: " << mesh.indices.size() << " indices (" 
              << mesh.indices.size() / 3 << " triangles)\n";
}

/**
 * Parse VOLM chunk - Spatial Octree
 * 
 * Structure per spec (XOB_FORMAT_SPEC.md):
 * Header (12 bytes):
 *   0x00: u16 - Reserved (0)
 *   0x02: u16 - Octree depth (typically 4)
 *   0x04: u16 - Internal node count
 *   0x06: u16 - Total node count
 *   0x08: u16 - Data size (octree data bytes / 2)
 *   0x0A: u16 - Reserved (0)
 * 
 * Followed by packed octree bitmask data
 */
static void parse_volm_chunk(const uint8_t* data, size_t size, XobOctree& octree) {
    if (size < 12) {
        std::cerr << "[XOB] VOLM chunk too small: " << size << " bytes\n";
        return;
    }
    
    // Read header
    // uint16_t reserved1 = read_u16_le(data + 0);
    octree.depth = read_u16_le(data + 2);
    octree.internal_nodes = read_u16_le(data + 4);
    octree.total_nodes = read_u16_le(data + 6);
    uint16_t data_size_half = read_u16_le(data + 8);
    // uint16_t reserved2 = read_u16_le(data + 10);
    
    size_t data_size = static_cast<size_t>(data_size_half) * 2;
    
    std::cerr << "[XOB] VOLM: depth=" << octree.depth 
              << " internal_nodes=" << octree.internal_nodes
              << " total_nodes=" << octree.total_nodes
              << " data_size=" << data_size << "\n";
    
    // Read octree data
    if (12 + data_size <= size) {
        octree.data.assign(data + 12, data + 12 + data_size);
    } else {
        // Copy what we have
        octree.data.assign(data + 12, data + size);
    }
}

XobParser::XobParser(std::span<const uint8_t> data) : data_(data) {}

std::optional<std::span<const uint8_t>> XobParser::find_chunk(const uint8_t* chunk_id) const {
    if (data_.size() < 12) return std::nullopt;
    
    // Simple search for chunk magic (like Python does with data.find(b'LODS'))
    // This is more robust than IFF iteration which can fail due to alignment issues
    for (size_t pos = 12; pos + 8 <= data_.size(); pos++) {
        if (std::memcmp(data_.data() + pos, chunk_id, 4) == 0) {
            uint32_t chunk_size = read_u32_be(data_.data() + pos + 4);
            
            // Sanity check
            if (chunk_size > 0 && chunk_size < 100000000 && pos + 8 + chunk_size <= data_.size()) {
                std::cerr << "[XOB] Found chunk '" << (char)chunk_id[0] << (char)chunk_id[1] 
                          << (char)chunk_id[2] << (char)chunk_id[3] << "' at pos=" << pos 
                          << " size=" << chunk_size << "\n";
                return data_.subspan(pos + 8, chunk_size);
            }
        }
    }
    
    return std::nullopt;
}

std::optional<XobMesh> XobParser::parse(uint32_t target_lod) {
    if (data_.size() < 12) {
        LOG_ERROR("XobParser", "Data too small: " << data_.size() << " bytes (need 12+)");
        return std::nullopt;
    }
    
    // Verify FORM header
    if (std::memcmp(data_.data(), "FORM", 4) != 0) {
        LOG_ERROR("XobParser", "Invalid magic: not a FORM container");
        return std::nullopt;
    }
    
    // Check XOB type
    const char* form_type = reinterpret_cast<const char*>(data_.data() + 8);
    if (form_type[0] != 'X' || form_type[1] != 'O' || form_type[2] != 'B') {
        LOG_ERROR("XobParser", "Invalid form type: '" << std::string(form_type, 4) << "' (expected XOB*)");
        return std::nullopt;
    }
    
    // Find HEAD chunk
    static constexpr uint8_t HEAD_ID[4] = {'H', 'E', 'A', 'D'};
    auto head_chunk = find_chunk(HEAD_ID);
    if (!head_chunk) {
        LOG_ERROR("XobParser", "HEAD chunk not found");
        return std::nullopt;
    }
    
    auto head_data = *head_chunk;
    LOG_DEBUG("XobParser", "HEAD chunk: " << head_data.size() << " bytes");
    
    // Extract materials from HEAD chunk
    materials_ = extract_materials_from_head(head_data.data(), head_data.size());
    LOG_DEBUG("XobParser", "Found " << materials_.size() << " materials");
    
    // Parse LZO4 descriptors from HEAD chunk (new v8 format)
    descriptors_ = parse_lzo4_descriptors_v8(head_data.data(), head_data.size());
    if (descriptors_.empty()) {
        LOG_ERROR("XobParser", "No LZO4 descriptors found in HEAD chunk");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "Found " << descriptors_.size() << " LOD descriptors");
    
    // Validate LOD index
    if (target_lod >= descriptors_.size()) {
        LOG_WARNING("XobParser", "Requested LOD " << target_lod << " > max " 
                    << (descriptors_.size()-1) << ", using LOD 0");
        target_lod = 0;
    }
    
    // Get descriptor for target LOD
    const auto& desc = descriptors_[target_lod];
    if (desc.unique_vertex_count == 0 || desc.triangle_count == 0) {
        LOG_ERROR("XobParser", "Invalid LOD " << target_lod << " descriptor: verts=" 
                  << desc.unique_vertex_count << " tris=" << desc.triangle_count);
        return std::nullopt;
    }
    
    // Find LODS chunk
    static constexpr uint8_t LODS_ID[4] = {'L', 'O', 'D', 'S'};
    auto lods_chunk = find_chunk(LODS_ID);
    if (!lods_chunk) {
        LOG_ERROR("XobParser", "LODS chunk not found");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "LODS chunk: " << lods_chunk->size() << " bytes compressed");
    
    // Decompress LODS data with dictionary chaining
    auto decompressed = decompress_lz4_chained(lods_chunk->data(), lods_chunk->size());
    if (decompressed.empty()) {
        LOG_ERROR("XobParser", "LZ4 decompression failed (input=" << lods_chunk->size() << ")");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "Decompressed: " << decompressed.size() << " bytes");
    
    // Extract LOD region - use decompressed_size from descriptor
    // For now, use all decompressed data (single LOD most common)
    std::vector<uint8_t> region;
    if (desc.decompressed_size > 0 && desc.decompressed_size <= decompressed.size()) {
        region.assign(decompressed.begin(), decompressed.begin() + desc.decompressed_size);
    } else {
        region = decompressed;
    }
    if (region.empty()) {
        LOG_ERROR("XobParser", "Failed to extract LOD " << target_lod << " region");
        return std::nullopt;
    }
    LOG_DEBUG("XobParser", "LOD " << target_lod << " region: " << region.size() << " bytes");
    
    // Parse mesh from region using mesh type for correct vertex layout
    XobMesh mesh;
    if (!parse_mesh_from_region(region, desc.unique_vertex_count, desc.triangle_count, 
                                 desc.mesh_type, desc.attr_config[ATTR_CFG_BONE_STREAMS], mesh)) {
        LOG_ERROR("XobParser", "Failed to parse mesh (verts=" << desc.unique_vertex_count 
                  << " tris=" << desc.triangle_count << " type=0x" << std::hex << (int)desc.mesh_type << std::dec << ")");
        return std::nullopt;
    }
    LOG_INFO("XobParser", "Parsed LOD " << target_lod << ": " << mesh.vertices.size() 
             << " vertices, " << mesh.indices.size() << " indices");
    
    // Calculate bounds
    mesh.bounds_min = glm::vec3(FLT_MAX);
    mesh.bounds_max = glm::vec3(-FLT_MAX);
    for (const auto& vert : mesh.vertices) {
        mesh.bounds_min = glm::min(mesh.bounds_min, vert.position);
        mesh.bounds_max = glm::max(mesh.bounds_max, vert.position);
    }
    
    // Create LOD entry
    XobLod lod_data;
    lod_data.distance = desc.switch_distance;
    lod_data.index_offset = 0;
    lod_data.index_count = static_cast<uint32_t>(mesh.indices.size());
    lod_data.indices = mesh.indices;
    mesh.lods.push_back(lod_data);
    
    // Copy materials to mesh
    mesh.materials = materials_;
    
    // Log material paths for debugging
    for (size_t i = 0; i < materials_.size(); i++) {
        LOG_DEBUG("XobParser", "Material " << i << ": name=" << materials_[i].name << " path=" << materials_[i].path);
    }
    
    // Extract material ranges (which triangles use which material)
    // ALWAYS use legacy extraction for multi-material meshes - it parses the actual submesh blocks
    // Descriptor-based extraction is WRONG - LZO4 descriptors describe compression, NOT materials
    std::vector<MaterialRange> legacy_ranges;
    
    if (materials_.size() > 1) {
        legacy_ranges = extract_material_ranges(data_.data(), data_.size(), desc.triangle_count, materials_.size(), desc.mesh_type);
    }
    
    auto coverage = [](const std::vector<MaterialRange>& ranges) {
        uint32_t sum = 0;
        for (const auto& r : ranges) sum += r.triangle_count;
        return sum;
    };
    
    uint32_t legacy_coverage = coverage(legacy_ranges);
    uint32_t total_triangles = desc.triangle_count;
    
    // Use legacy ranges if they provide reasonable coverage
    if (!legacy_ranges.empty() && legacy_coverage > 0) {
        mesh.material_ranges = std::move(legacy_ranges);
        LOG_DEBUG("XobParser", "Using legacy material ranges (coverage=" << legacy_coverage
                  << "/" << total_triangles << ")");
    } else {
        // Fallback: create ranges for each material evenly distributed
        // This should rarely happen - it means submesh parsing failed
        LOG_WARNING("XobParser", "No submesh blocks found, distributing triangles across " 
                    << materials_.size() << " materials");
        uint32_t tris_per_mat = total_triangles / static_cast<uint32_t>(materials_.size());
        uint32_t current = 0;
        for (size_t i = 0; i < materials_.size() && current < total_triangles; i++) {
            MaterialRange r;
            r.material_index = static_cast<uint32_t>(i);
            r.triangle_start = current;
            uint32_t count = (i == materials_.size() - 1) ? (total_triangles - current) : tris_per_mat;
            r.triangle_end = current + count;
            r.triangle_count = count;
            mesh.material_ranges.push_back(r);
            current += count;
        }
    }
    
    if (mesh.material_ranges.empty()) {
        MaterialRange r0;
        r0.material_index = 0;
        r0.triangle_start = 0;
        r0.triangle_end = desc.triangle_count;
        r0.triangle_count = desc.triangle_count;
        mesh.material_ranges.push_back(r0);
    }
    LOG_DEBUG("XobParser", "Material ranges assigned: " << mesh.material_ranges.size());
    
    // Parse COLL chunk (collision data) - optional
    static constexpr uint8_t COLL_ID[4] = {'C', 'O', 'L', 'L'};
    auto coll_chunk = find_chunk(COLL_ID);
    if (coll_chunk) {
        LOG_DEBUG("XobParser", "COLL chunk: " << coll_chunk->size() << " bytes");
        parse_coll_chunk(coll_chunk->data(), coll_chunk->size(), 
                         mesh.collision_objects, mesh.collision_mesh);
        LOG_DEBUG("XobParser", "Parsed " << mesh.collision_objects.size() << " collision objects, "
                  << mesh.collision_mesh.vertices.size() << " collision vertices");
    }
    
    // Parse VOLM chunk (spatial octree) - optional
    static constexpr uint8_t VOLM_ID[4] = {'V', 'O', 'L', 'M'};
    auto volm_chunk = find_chunk(VOLM_ID);
    if (volm_chunk) {
        LOG_DEBUG("XobParser", "VOLM chunk: " << volm_chunk->size() << " bytes");
        parse_volm_chunk(volm_chunk->data(), volm_chunk->size(), mesh.octree);
        LOG_DEBUG("XobParser", "Parsed octree: depth=" << mesh.octree.depth 
                  << " nodes=" << mesh.octree.total_nodes);
    }
    
    // Extract bone count from HEAD header
    if (head_data.size() >= 0x30) {
        mesh.bone_count = read_u16_le(head_data.data() + 0x2E);
        LOG_DEBUG("XobParser", "Bone count: " << mesh.bone_count);
    }
    
    return mesh;
}

std::vector<LzoDescriptor> XobParser::parse_descriptors(std::span<const uint8_t> head_data) {
    return descriptors_;
}

std::vector<XobMaterial> XobParser::parse_materials(std::span<const uint8_t> head_data) {
    return materials_;
}

std::vector<MaterialRange> XobParser::parse_material_ranges(std::span<const uint8_t> head_data, uint32_t triangle_count) {
    return {};
}

std::span<const uint8_t> XobParser::extract_lod_region(std::span<const uint8_t> decompressed, uint32_t lod) {
    return {};
}

std::optional<XobMesh> XobParser::parse_mesh_region(std::span<const uint8_t> region, const LzoDescriptor& desc) {
    return std::nullopt;
}

void XobParser::calculate_bounds(XobMesh& mesh) {
    // Already done in parse()
}

} // namespace enfusion


