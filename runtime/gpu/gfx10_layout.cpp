#include "gfx10_layout.h"

#include <algorithm>

namespace rt::gfx10 {
namespace {

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
uint64_t align_up(uint64_t a, uint64_t b) { return div_up(a, b) * b; }

uint32_t log2_of(uint64_t v) {
    uint32_t n = 0;
    while ((uint64_t{1} << n) < v) ++n;
    return n;
}

// Block size log2 of a mode: 256 B (1-3), 4 KB (4-7, 20-23), 64 KB (8-11, 16-19, 24-27); 0 for linear, reserved
// and the VAR modes.
uint32_t block_log2(uint32_t mode) {
    if (mode >= 1 && mode <= 3) return 8;
    if ((mode >= 4 && mode <= 7) || (mode >= 20 && mode <= 23)) return 12;
    if ((mode >= 8 && mode <= 11) || (mode >= 16 && mode <= 19) || (mode >= 24 && mode <= 27)) return 16;
    return 0;
}

// 3D textures: the S modes and 64KB_D_X tile x, y and z together (thick); 64KB_Z_X / R_X and linear lay out each
// depth slice as a 2D surface (addrlib's 3D mode mask).
bool thick_3d(uint32_t mode) { return mode == 5 || mode == 9 || mode == 17 || mode == 21 || mode == 25 || mode == 26; }
bool thin_3d(uint32_t mode) { return mode == 0 || mode == 24 || mode == 27; }

}  // namespace

bool element_of(uint32_t f, uint32_t* e, uint32_t* side) {
    uint32_t el, s = 1;
    if (f >= 1 && f <= 4) el = 4;
    else if (f >= 9 && f <= 22) el = 3;
    else if ((f >= 23 && f <= 47) || f == 67 || (f >= 87 && f <= 93) || f == 116 || f == 117) el = 2;
    else if ((f >= 48 && f <= 59) || f == 85 || f == 86 || f == 115) el = 1;
    else if (f >= 60 && f <= 65) el = 0;
    else if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) el = 3, s = 4;          // BC1, BC4
    else if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) el = 4, s = 4;  // BC2/3/5/6H/7
    else return false;
    *e = el;
    *side = s;
    return true;
}

uint32_t default_mode(uint32_t layout, uint32_t flags, uint64_t bits_per_pixel, uint64_t width, uint32_t height) {
    if (flags & 2) return k64KB_Z_X;
    if (layout == 2) return k64KB_D;
    if (flags & 1) return k64KB_R_X;
    if (!(flags & 4)) return k4KB_S;
    return bits_per_pixel * width * height <= 0x80000 ? k4KB_S_X : k64KB_R_X;
}

bool make_surface(uint32_t mode, uint32_t e, uint32_t side, uint32_t width, uint32_t height, uint32_t depth,
                  uint32_t array, uint32_t mips, bool volume, Surface* out) {
    if (e > 4 || mips == 0 || mips > kMaxMips) return false;
    Surface s{};
    s.mode = mode;
    s.elem_log2 = e;
    s.side = side;
    s.mips = mips;
    bool thick = volume && thick_3d(mode);
    if (volume && !thick && !thin_3d(mode)) return false;
    uint32_t blk = mode == kLinear ? 8 : block_log2(mode);
    if (!blk) return false;
    if (mode == kLinear) {
        // rows padded to 256 bytes: a "block" one row high
        s.block_w = 256 >> e, s.block_h = 1, s.block_d = 1;
    } else if (thick) {
        // a 1 KB micro block grown evenly in w, h and d (addrlib Block1K_3d, ComputeThickBlockDimension)
        static const uint8_t w1k[5] = {4, 3, 3, 3, 2}, h1k[5] = {3, 3, 3, 2, 2}, d1k[5] = {3, 3, 2, 2, 2};
        uint32_t avg = (blk - 10) / 3, rest = (blk - 10) % 3;
        s.block_w = 1u << (w1k[e] + avg);
        s.block_h = 1u << (h1k[e] + avg + rest / 2);
        s.block_d = 1u << (d1k[e] + avg + (rest != 0));
    } else {
        // a 256-byte micro block (Block256_2d) grown in w then h
        static const uint8_t w256[5] = {4, 4, 3, 3, 2}, h256[5] = {4, 3, 3, 2, 2};
        uint32_t amp = blk - 8;
        s.block_w = 1u << (w256[e] + amp / 2);
        s.block_h = 1u << (h256[e] + amp - amp / 2);
        s.block_d = 1;
    }
    s.block_bytes = 1u << blk;

    // The largest level the tail block holds (GetMipTailDim) and how many levels it takes (GetMaxNumMipsInTail).
    uint32_t tw, th;
    if (!thick) {
        bool z = mode != kLinear && mode % 4 == 0;
        if (z && e <= 1) tw = s.block_w / 4, th = s.block_h >> (1 - e);  // Z modes of 1- / 2-byte elements
        else tw = s.block_w / 2, th = s.block_h;
    } else {
        switch (blk % 3) {
            case 0: tw = s.block_w, th = s.block_h / 2; break;
            case 1: tw = s.block_w / 2, th = s.block_h; break;
            default: tw = s.block_w, th = s.block_h; break;  // the depth halves
        }
    }
    uint32_t eff = thick ? blk - (blk - 8) / 3 : blk;
    uint32_t max_tail = mode == kLinear || eff <= 8 ? 0 : eff <= 11 ? 1 + (1u << (eff - 9)) : eff - 4;

    // Levels are padded from mip 0's elements halved rounding up.
    uint64_t w0 = div_up(width, side), h0 = div_up(height, side);
    auto pw = [&](uint32_t i) { return div_up(w0, uint64_t{1} << i); };
    auto ph = [&](uint32_t i) { return div_up(h0, uint64_t{1} << i); };
    s.first_tail = mips;
    if (mips > 1 && max_tail)
        for (uint32_t i = 0; i < mips; ++i)
            if (pw(i) <= tw && ph(i) <= th && mips - i <= max_tail) {
                s.first_tail = i;
                break;
            }

    uint64_t offset = s.first_tail < mips ? s.block_bytes : 0;  // the tail block comes first
    // A level in the tail sits at its own offset inside the tail block (largest level first, 256-byte units; the
    // larger slots grow by powers of two).
    for (uint32_t i = s.first_tail; i < mips; ++i) {
        uint32_t m = max_tail - 1 - (i - s.first_tail);
        uint64_t at = m > 6 ? uint64_t{16} << m : uint64_t{m} << 8;
        // its origin: x from the odd and y from the even address bits above 256 bytes, in 256-byte micro blocks
        static const uint8_t mw[5] = {4, 4, 3, 3, 2}, mh[5] = {4, 3, 3, 2, 2};       // Block256_2d, log2
        static const uint8_t tw3[5] = {3, 2, 2, 2, 1}, th3[5] = {2, 2, 2, 1, 1};      // Block256_3d, log2
        uint32_t ux = 0, uy = 0;
        for (int b = 0; b < 6; ++b) {
            ux |= static_cast<uint32_t>((at >> (9 + 2 * b)) & 1) << b;
            uy |= static_cast<uint32_t>((at >> (8 + 2 * b)) & 1) << b;
        }
        uint32_t w256 = 1u << (thick ? tw3[e] : mw[e]), h256 = 1u << (thick ? th3[e] : mh[e]);
        s.mip[i] = {at, tw, th, true, ux * w256, uy * h256};
    }
    s.max_tail = max_tail;
    for (uint32_t i = s.first_tail; i-- > 0;) {
        uint64_t w = align_up(pw(i), s.block_w), h = align_up(ph(i), s.block_h);
        s.mip[i] = {offset, static_cast<uint32_t>(w), static_cast<uint32_t>(h), false, 0, 0};
        offset += (w / s.block_w) * (h / s.block_h) * s.block_bytes;
    }
    s.slice_bytes = offset;
    s.slices = volume ? static_cast<uint32_t>(div_up(depth, s.block_d)) : array;
    *out = s;
    return true;
}

Result layout_texture(const ResourceDesc& d, Texture* out) {
    *out = Texture{};
    out->main = -1;
    uint32_t dim = d.dimension, height = d.height, layout = d.layout, flags = d.flags, format = d.format;
    uint64_t width = d.width;
    uint32_t array = d.depth_or_array, mips = d.mips;
    if (dim < 2 || dim > 4 || d.samples > 1) return Result::not_modelled;
    if (width == 0 || height == 0 || array == 0) return Result::invalid;

    if (dim == 4) {  // a volume: one plane of texels
        if (flags & 3) return Result::not_modelled;
        if (width > 2048 || height > 2048 || array > 2048) return Result::invalid;
        uint32_t e, side;
        if (!element_of(format, &e, &side)) return Result::invalid;
        uint32_t mode;
        if (layout == 1) mode = kLinear;
        else if (layout >= 0x100 && layout <= 0x11F) mode = layout & 0x1F;
        else if (layout == 0) mode = (flags & 4) ? default_mode(0, flags, (8ull << e) / (side * side) * array, width, height) : k4KB_S;
        else return Result::not_modelled;
        if (!mips) mips = log2_of(std::max<uint64_t>({width, height, array}) + 1);
        Surface& s = out->surface[0];
        if (!make_surface(mode, e, side, static_cast<uint32_t>(width), height, array, 1, mips, true, &s))
            return Result::not_modelled;
        out->plane[0] = {1, 0, s.bytes(), s.block_bytes, 0};
        out->planes = 1;
        out->main = 0;
        out->size = s.bytes();
        out->align = s.block_bytes;
        return Result::ok;
    }

    bool line = dim == 2;
    if (line && (height != 1 || (flags & 3))) return Result::invalid;
    if (width > 16384 || height > 16384) return Result::invalid;
    // depth + stencil formats: a 4-byte depth plane (the console's D16_UNORM_S8_UINT family 118-120: 2-byte) and a
    // 1-byte stencil plane; 119 (its R16_UNORM_X8 view format) is a plain 2-byte format except as a depth buffer
    bool d16s8 = format == 118 || format == 120 || (format == 119 && (flags & 3) == 2 && mips == 1);
    bool planar = (format >= 19 && format <= 22) || (format >= 44 && format <= 47) || d16s8;
    uint32_t e, side = 1;
    if (d16s8 || format == 119) e = 1;
    else if (planar) e = 2;
    else if (!element_of(format, &e, &side)) return Result::invalid;
    uint32_t mode;
    if (layout == 1 || (layout == 0 && line)) mode = kLinear;
    else if (layout >= 0x100 && layout <= 0x11F) mode = layout & 0x1F;
    else if (layout == 0 || layout == 2) {
        uint64_t bpp = (format >= 19 && format <= 22) ? 64 : (8ull << e) / (side * side);
        mode = default_mode(layout, flags, bpp, width, height);
    } else return Result::not_modelled;
    if (line && mode != kLinear && mode != k64KB_R_X) return Result::invalid;
    // the 2D modes this model has: not the VAR ones, Z only for uncompressed elements up to 8 bytes
    switch (mode) {
        case 0: case 1: case 2: case 5: case 6: case 9: case 10: case 17: case 18: case 21: case 22: case 25:
        case 26: case 27: break;
        case 24: if (e <= 3 && side == 1) break; return Result::invalid;
        default: return Result::not_modelled;
    }
    if (planar && layout == 2 && format != 119) return Result::invalid;
    if (!mips) mips = log2_of(std::max<uint64_t>(width, height) + 1);
    if (mips > kMaxMips) return Result::invalid;

    bool rt = flags & 1, ds = flags & 2;
    // a depth buffer (one level; it gets an HTILE) is in a Z mode; in a color format its texels are left out
    bool depth_buffer = ds && mips == 1 && (planar || !rt);
    if (depth_buffer && mode != k64KB_Z_X) return Result::invalid;
    bool depth_format = planar || format == 39 || format == 40 || format == 53 || format == 55;
    bool blank = (depth_buffer && !depth_format) || ((rt || ds) && side > 1 && mips == 1);
    bool empty = blank && ds;
    bool stencil_only = format == 120 && !depth_buffer;

    auto w32 = static_cast<uint32_t>(width);
    // metadata, per slice: HTILE 1 byte per 16 pixels, CMASK per 128, of the size padded to 1024 x 1024
    uint64_t meta_px = align_up(width, 1024) * align_up(height, 1024) * array;
    struct Want { uint32_t usage; int surface; uint64_t bytes, align; };
    Want want[4];
    int n = 0;
    if (depth_buffer) want[n++] = {4, -1, meta_px / 16, 0x10000};
    if (!blank && !stencil_only) {
        if (!make_surface(mode, e, side, w32, height, 1, array, mips, false, &out->surface[0])) return Result::not_modelled;
        out->main = 0;
        uint32_t usage = ds ? (planar || depth_buffer ? 7u : 1u) : planar ? 5u : 1u;
        want[n++] = {usage, 0, out->surface[0].bytes(), out->surface[0].block_bytes};
    } else {
        want[n++] = {ds ? 7u : planar ? 5u : 1u, -1, 0, 0};
    }
    if (planar) {
        if (!make_surface(mode, 0, 1, w32, height, 1, array, mips, false, &out->surface[1])) return Result::not_modelled;
        want[n++] = {ds ? 8u : 6u, 1, out->surface[1].bytes(), out->surface[1].block_bytes};
    }
    // a single-level render target gets a CMASK (not with the 64KB_D choice or the _T modes)
    if (rt && !depth_buffer && !planar && !empty && mips == 1 && layout != 2 && !(layout >= 0x110 && layout <= 0x113))
        want[n++] = {2, -1, meta_px / 128, 0x2000};

    uint64_t end = 0, align = 0x100;
    for (int i = 0; i < n; ++i) {
        uint64_t at = want[i].bytes ? align_up(end, want[i].align) : 0;
        out->plane[i] = {want[i].usage, at, want[i].bytes, want[i].align, want[i].surface};
        end = std::max(end, at + want[i].bytes);
        align = std::max(align, want[i].align);
    }
    out->planes = n;
    out->size = empty ? 0 : end;
    out->align = align;
    return Result::ok;
}

}  // namespace rt::gfx10
