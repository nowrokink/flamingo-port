#include "sharp.h"

#include <algorithm>
#include <cmath>

namespace rt::gpu {
namespace {

uint32_t field(uint32_t v, unsigned lo, unsigned n) { return (v & ((1u << n) - 1)) << lo; }
uint32_t bits(uint32_t w, unsigned lo, unsigned n) { return (w >> lo) & ((1u << n) - 1); }

struct Fmt {
    uint16_t dxgi, gfx10;
    uint8_t sel[4];
};
constexpr uint8_t X = kSelX, Y = kSelY, Z = kSelZ, W = kSelW, O = kSel1, N = kSel0;
// clang-format off
constexpr Fmt kFormats[] = {
    {1, 77, {X, Y, Z, W}},  {2, 77, {X, Y, Z, W}},  {3, 75, {X, Y, Z, W}},  {4, 76, {X, Y, Z, W}},
    {9, 71, {X, Y, Z, W}},  {10, 71, {X, Y, Z, W}}, {11, 65, {X, Y, Z, W}}, {12, 69, {X, Y, Z, W}},
    {13, 66, {X, Y, Z, W}}, {14, 70, {X, Y, Z, W}},
    {15, 64, {X, Y, N, O}}, {16, 64, {X, Y, N, O}}, {17, 62, {X, Y, N, O}}, {18, 63, {X, Y, N, O}},
    {19, 22, {X, N, N, O}}, {20, 22, {X, N, N, O}}, {21, 22, {X, N, N, O}}, {22, 5, {N, X, N, O}},
    {23, 50, {X, Y, Z, W}}, {24, 50, {X, Y, Z, W}}, {25, 54, {X, Y, Z, W}},
    {26, 36, {X, Y, Z, O}},
    {27, 56, {X, Y, Z, W}}, {28, 56, {X, Y, Z, W}}, {29, 130, {X, Y, Z, W}}, {30, 60, {X, Y, Z, W}},
    {31, 57, {X, Y, Z, W}}, {32, 61, {X, Y, Z, W}},
    {33, 29, {X, Y, N, O}}, {34, 29, {X, Y, N, O}}, {35, 23, {X, Y, N, O}}, {36, 27, {X, Y, N, O}},
    {37, 24, {X, Y, N, O}}, {38, 28, {X, Y, N, O}},
    {39, 22, {X, N, N, O}}, {40, 22, {X, N, N, O}}, {41, 22, {X, N, N, O}}, {42, 20, {X, N, N, O}},
    {43, 21, {X, N, N, O}},
    {44, 141, {X, N, N, O}}, {45, 141, {X, N, N, O}}, {46, 141, {X, N, N, O}}, {47, 5, {N, X, N, O}},
    {48, 14, {X, Y, N, O}}, {49, 14, {X, Y, N, O}}, {50, 18, {X, Y, N, O}}, {51, 15, {X, Y, N, O}},
    {52, 19, {X, Y, N, O}},
    {53, 13, {X, N, N, O}}, {54, 13, {X, N, N, O}}, {55, 7, {X, N, N, O}}, {56, 7, {X, N, N, O}},
    {57, 11, {X, N, N, O}}, {58, 8, {X, N, N, O}}, {59, 12, {X, N, N, O}},
    {60, 1, {X, N, N, O}}, {61, 1, {X, N, N, O}}, {62, 5, {X, N, N, O}}, {63, 2, {X, N, N, O}},
    {64, 6, {X, N, N, O}}, {65, 1, {N, N, N, X}},
    {67, 132, {X, Y, Z, O}},
    {70, 169, {X, Y, Z, W}}, {71, 169, {X, Y, Z, W}}, {72, 170, {X, Y, Z, W}},
    {73, 171, {X, Y, Z, W}}, {74, 171, {X, Y, Z, W}}, {75, 172, {X, Y, Z, W}},
    {76, 173, {X, Y, Z, W}}, {77, 173, {X, Y, Z, W}}, {78, 174, {X, Y, Z, W}},
    {79, 175, {X, N, N, O}}, {80, 175, {X, N, N, O}}, {81, 176, {X, N, N, O}},
    {82, 177, {X, Y, N, O}}, {83, 177, {X, Y, N, O}}, {84, 178, {X, Y, N, O}},
    {85, 133, {Z, Y, X, O}}, {86, 134, {Z, Y, X, W}},
    {87, 56, {Z, Y, X, W}}, {88, 56, {Z, Y, X, O}}, {90, 56, {Z, Y, X, W}}, {91, 130, {Z, Y, X, W}},
    {92, 56, {Z, Y, X, O}}, {93, 130, {Z, Y, X, O}},
    {94, 179, {X, Y, Z, O}}, {95, 179, {X, Y, Z, O}}, {96, 180, {X, Y, Z, O}},
    {97, 181, {X, Y, Z, W}}, {98, 181, {X, Y, Z, W}}, {99, 182, {X, Y, Z, W}},
    {115, 136, {Z, Y, X, W}},
};
// clang-format on

uint32_t lod_u4_8(float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 15.99f) * 256.0f); }

// D3D12_TEXTURE_ADDRESS_MODE <-> SQ_TEX_CLAMP
uint32_t clamp_of(uint32_t mode) {
    switch (mode) {
        case 2: return 1;  // MIRROR
        case 3: return 2;  // CLAMP: last texel
        case 4: return 6;  // BORDER
        case 5: return 3;  // MIRROR_ONCE: last texel
        default: return 0; // WRAP
    }
}
uint32_t mode_of(uint32_t clamp) {
    switch (clamp) {
        case 1: return 2;
        case 2: return 3;
        case 3: case 5: case 7: return 5;
        case 4: case 6: return 4;
        default: return 1;
    }
}

}  // namespace

bool gfx10_format(uint32_t dxgi, uint32_t* format, uint8_t sel[4]) {
    for (const Fmt& f : kFormats)
        if (f.dxgi == dxgi) {
            *format = f.gfx10;
            for (int i = 0; i < 4; ++i) sel[i] = f.sel[i];
            return true;
        }
    return false;
}

void encode_tsharp(const ImageDesc& d, uint32_t t[8]) {
    uint64_t a = d.address >> 8;
    uint32_t w = d.width ? d.width - 1 : 0, h = d.height ? d.height - 1 : 0, depth = d.depth ? d.depth - 1 : 0;
    t[0] = static_cast<uint32_t>(a);
    t[1] = field(static_cast<uint32_t>(a >> 32), 0, 8) | field(d.format, 20, 9) | field(w & 3, 30, 2);
    t[2] = field(w >> 2, 0, 12) | field(h, 14, 14) | (1u << 31);  // RESOURCE_LEVEL
    t[3] = field(d.sel[0], 0, 3) | field(d.sel[1], 3, 3) | field(d.sel[2], 6, 3) | field(d.sel[3], 9, 3) |
           field(d.base_level, 12, 4) | field(d.last_level, 16, 4) | field(d.swizzle_mode, 20, 5) |
           field(d.type, 28, 4);
    uint32_t last = d.type == kImg3D ? depth : d.last_array;
    t[4] = field(last, 0, 13) | field(d.base_array, 16, 13);
    t[5] = t[6] = t[7] = 0;
}

void encode_vsharp(const BufferDesc& d, uint32_t v[4]) {
    v[0] = static_cast<uint32_t>(d.address);
    v[1] = field(static_cast<uint32_t>(d.address >> 32), 0, 16) | field(d.stride, 16, 14);
    v[2] = d.num_records;
    // DST_SEL, FORMAT, RESOURCE_LEVEL, OOB_SELECT 3 (raw: checked against num_records only)
    v[3] = field(d.sel[0], 0, 3) | field(d.sel[1], 3, 3) | field(d.sel[2], 6, 3) | field(d.sel[3], 9, 3) |
           field(d.format, 12, 7) | (1u << 24) | (3u << 28);
}

void encode_ssharp(const SamplerDesc& d, uint32_t s[4]) {
    bool aniso = d.filter & 0x40;
    uint32_t reduction = (d.filter >> 7) & 3;  // 1 comparison, 2 minimum, 3 maximum
    uint32_t mip = (d.filter & 1) ? 2 : 1, mag = (d.filter >> 2) & 1, min = (d.filter >> 4) & 1;
    if (aniso) mag = min = 3, mip = 2;
    uint32_t ratio = 0;
    while (aniso && ratio < 4 && (2u << ratio) <= d.max_anisotropy) ++ratio;
    uint32_t compare = reduction == 1 && d.comparison ? d.comparison - 1 : 0;
    int32_t bias = static_cast<int32_t>(std::clamp(d.mip_lod_bias, -32.0f, 31.99f) * 256.0f);
    s[0] = field(clamp_of(d.address_u), 0, 3) | field(clamp_of(d.address_v), 3, 3) | field(clamp_of(d.address_w), 6, 3) |
           field(ratio, 9, 3) | field(compare, 12, 3) | field(reduction >= 2 ? reduction - 1 : 0, 29, 2);
    s[1] = field(lod_u4_8(d.min_lod), 0, 12) | field(lod_u4_8(d.max_lod), 12, 12);
    // MIP_POINT_PRECLAMP (bit 28) set, as in the S#s console shaders carry
    s[2] = field(static_cast<uint32_t>(bias), 0, 14) | field(mag, 20, 2) | field(min, 22, 2) | field(mip, 26, 2) |
           (1u << 28);
    uint32_t border = 3;  // a register colour
    const float* b = d.border;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0) border = b[3] == 0 ? 0 : b[3] == 1 ? 1 : 3;
    else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) border = 2;
    s[3] = field(border, 30, 2);
}

SamplerDesc decode_ssharp(const uint32_t s[4]) {
    SamplerDesc d{};
    d.address_u = mode_of(bits(s[0], 0, 3));
    d.address_v = mode_of(bits(s[0], 3, 3));
    d.address_w = mode_of(bits(s[0], 6, 3));
    uint32_t ratio = bits(s[0], 9, 3), compare = bits(s[0], 12, 3), mode = bits(s[0], 29, 2);
    uint32_t mag = bits(s[2], 20, 2), min = bits(s[2], 22, 2), mip = bits(s[2], 26, 2);
    uint32_t reduction = compare ? 1 : mode == 1 ? 2 : mode == 2 ? 3 : 0;
    if ((mag | min) & 2) {
        d.filter = 0x55 | (reduction << 7);  // ANISOTROPIC
        d.max_anisotropy = 1u << std::min(ratio, 4u);
    } else {
        d.filter = (mip == 2 ? 1 : 0) | ((mag & 1) << 2) | ((min & 1) << 4) | (reduction << 7);
        d.max_anisotropy = 1;
    }
    d.comparison = compare ? compare + 1 : 1;
    d.min_lod = bits(s[1], 0, 12) / 256.0f;
    d.max_lod = mip == 0 ? d.min_lod : bits(s[1], 12, 12) / 256.0f;
    d.mip_lod_bias = static_cast<float>(static_cast<int32_t>(bits(s[2], 0, 14) << 18) >> 18) / 256.0f;
    switch (bits(s[3], 30, 2)) {
        case 1: d.border[3] = 1; break;
        case 2: d.border[0] = d.border[1] = d.border[2] = d.border[3] = 1; break;
        default: break;
    }
    return d;
}

}  // namespace rt::gpu
