// The console GPU's resource descriptors (GFX10): T# (image, 8 dwords), V# (buffer, 4 dwords), S# (sampler,
// 4 dwords), as the console's device methods write them into descriptor heaps. Translated shaders read them from heap
// memory: V#s directly (base, stride, size), T#s and S#s through the descriptor hash table (desc_table.h), so what
// matters most is that every view gets distinct, faithful words.
//
// Field layout from Mesa's src/amd/common/sid.h (SQ_IMG_RSRC_WORD*, SQ_BUF_RSRC_WORD*, SQ_IMG_SAMP_WORD*, MIT).
#pragma once
#include <cstdint>

namespace rt::gpu {

// SQ_RSRC_IMG_* types
enum : uint32_t { kImg1D = 8, kImg2D = 9, kImg3D = 10, kImgCube = 11, kImg1DArray = 12, kImg2DArray = 13,
                  kImg2DMsaa = 14, kImg2DMsaaArray = 15 };
// SQ_SEL_*
enum : uint8_t { kSel0 = 0, kSel1 = 1, kSelX = 4, kSelY = 5, kSelZ = 6, kSelW = 7 };

// A DXGI format as a GFX10 image / buffer format, with the component selects that put its channels in RGBA order
// (B8G8R8A8 is 8_8_8_8 read as ZYXW). False for formats without one.
bool gfx10_format(uint32_t dxgi, uint32_t* format, uint8_t sel[4]);

struct ImageDesc {
    uint64_t address;     // of mip 0 of slice 0 (256-byte aligned)
    uint32_t format;      // GFX10 image format
    uint8_t sel[4];       // DST_SEL x, y, z, w
    uint32_t width, height, depth;  // pixels; depth: 3D depth or array size
    uint32_t type;        // kImg*
    uint32_t base_level, last_level;
    uint32_t base_array, last_array;
    uint32_t swizzle_mode;
};
void encode_tsharp(const ImageDesc& d, uint32_t out[8]);

struct BufferDesc {
    uint64_t address;
    uint32_t stride;       // 0: raw bytes
    uint32_t num_records;  // bytes when stride is 0, else elements
    uint32_t format;       // GFX10 format (32_FLOAT for raw / structured)
    uint8_t sel[4];
};
void encode_vsharp(const BufferDesc& d, uint32_t out[4]);

// D3D12_SAMPLER_DESC fields, as plain values (D3D12_FILTER, D3D12_TEXTURE_ADDRESS_MODE, D3D12_COMPARISON_FUNC).
struct SamplerDesc {
    uint32_t filter;
    uint32_t address_u, address_v, address_w;
    float mip_lod_bias;
    uint32_t max_anisotropy;
    uint32_t comparison;
    float border[4];
    float min_lod, max_lod;
};
void encode_ssharp(const SamplerDesc& d, uint32_t out[4]);
// The other way, for S#s the title writes itself (literal samplers, CreateSamplerX): what the PC's sampler needs.
SamplerDesc decode_ssharp(const uint32_t s[4]);

}  // namespace rt::gpu
