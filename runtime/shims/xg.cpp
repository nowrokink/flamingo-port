// xg_xs.dll: the console's texture layout library, answered from our GFX10 model (gpu/gfx10_layout.h).
//
//   HRESULT XGSuggestSwizzleMode(const D3D12_RESOURCE_DESC*, UINT, D3D12_TEXTURE_LAYOUT* out): the explicit layout
//       (0x100 | mode) the console takes for Layout 0
//   HRESULT XGCreateTextureComputer(const D3D12_RESOURCE_DESC*, XGTextureAddressComputer** out)
//
// The computer (no QueryInterface): 0 AddRef, 1 Release, 2 GetResourceLayout(XG_RESOURCE_LAYOUT*),
// 3 GetResourceSizeBytes, 4 GetResourceBaseAlignmentBytes, 5 GetMipLevelOffsetBytes(plane, mip),
// 6 GetTexelElementOffsetBytes(plane, mip, x, y, slice, sample, bit*), 7 GetTexelCoordinate,
// 8 CopyIntoSubresource(tiled, plane, subresource, linear, row pitch, depth pitch), 9 CopyFromSubresource(linear,
// plane, subresource, sample, tiled, row pitch, depth pitch), 10 GetResourceTiling, 11 (unknown). 1.0.2.1 calls 2, 6,
// 8, 9 and 10 (the slots and their arguments as the VIctory project records them).
//
// XG_RESOURCE_LAYOUT (0x1B40 bytes): size @0, alignment @8, mips @0x10, planes @0x14, then planes of 0x6C8 bytes from
// 0x18 (usage @0, size @8, slice size @0x10, offset @0x18, alignment @0x20, bytes per element @0x28, first tail mip
// @0x2C, mips in the tail @0x30, then 15 mips of 0x70 from 0x38: size @0, offset @8, slice size @0x10, pitch in
// pixels @0x18 and bytes @0x1C, alignment @0x20, padded width / height / depth @0x24 / 0x28 / 0x2C, width / height in
// elements @0x30 / 0x34 and in pixels @0x38 / 0x3C, depth @0x40, samples @0x44, swizzle mode @0x48, 1 @0x60, in the
// tail @0x64), dimension @0x1B38. Facts as the xcl project records them from the console's library.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "../gpu/gfx10_layout.h"
#include "../gpu/gfx10_swizzle.h"
#include "shims.h"

namespace {

using rt::gfx10::ResourceDesc;
using rt::gfx10::Result;
using rt::gfx10::Texture;

constexpr size_t kLayoutBytes = 0x1B40, kPlane = 0x18, kPlaneBytes = 0x6C8, kMip = 0x38, kMipBytes = 0x70;

struct Computer {
    void** vtbl;
    std::atomic<uint32_t> refs;
    ResourceDesc desc;
    Texture tex;
};

void put32(uint8_t* b, size_t at, uint64_t v) {
    uint32_t x = static_cast<uint32_t>(v);
    std::memcpy(b + at, &x, 4);
}
void put64(uint8_t* b, size_t at, uint64_t v) { std::memcpy(b + at, &v, 8); }

uint32_t WINAPI add_ref(Computer* c) { return ++c->refs; }
uint32_t WINAPI release(Computer* c) {
    uint32_t n = --c->refs;
    if (!n) delete c;
    return n;
}

HRESULT WINAPI get_resource_layout(Computer* c, uint8_t* out) {
    if (!out) return E_POINTER;
    std::memset(out, 0, kLayoutBytes);
    const Texture& t = c->tex;
    put64(out, 0, t.size);
    put64(out, 8, t.align);
    uint32_t mips = t.main >= 0 ? t.surface[t.main].mips : 1;
    put32(out, 0x10, mips);
    put32(out, 0x14, t.planes);
    put32(out, 0x1B38, c->desc.dimension);
    for (int k = 0; k < t.planes; ++k) {
        uint8_t* p = out + kPlane + k * kPlaneBytes;
        const rt::gfx10::Plane& pl = t.plane[k];
        put32(p, 0, pl.usage);
        put64(p, 8, pl.bytes);
        put64(p, 0x18, pl.offset);
        put64(p, 0x20, pl.align);
        if (pl.surface < 0) {  // metadata: its size only
            put64(p, 0x10, pl.bytes);
            put64(p, kMip, pl.bytes);
            put32(p, kMip + 0x20, static_cast<uint32_t>(pl.align));
            continue;
        }
        const rt::gfx10::Surface& s = t.surface[pl.surface];
        put64(p, 0x10, s.slice_bytes);
        put32(p, 0x28, uint64_t{1} << s.elem_log2);
        put32(p, 0x2C, s.first_tail);
        put32(p, 0x30, s.mips - s.first_tail);
        for (uint32_t i = 0; i < s.mips; ++i) {
            uint8_t* m = p + kMip + i * kMipBytes;
            const rt::gfx10::Mip& mi = s.mip[i];
            uint64_t level = mi.in_tail ? 256 : uint64_t{mi.width / s.block_w} * (mi.height / s.block_h) * s.block_bytes;
            uint32_t w = static_cast<uint32_t>(c->desc.width >> i), h = c->desc.height >> i;
            w = w ? w : 1, h = h ? h : 1;
            uint32_t depth = c->desc.dimension == 4 ? (c->desc.depth_or_array >> i ? c->desc.depth_or_array >> i : 1) : 1;
            put64(m, 0, level * s.slices);
            put64(m, 8, mi.offset);
            put64(m, 0x10, level);
            put32(m, 0x18, mi.width * s.side);
            put32(m, 0x1C, mi.width << s.elem_log2);
            put32(m, 0x20, s.block_bytes);
            put32(m, 0x24, mi.width * s.side);
            put32(m, 0x28, mi.height * s.side);
            put32(m, 0x2C, c->desc.dimension == 4 ? depth : s.slices);
            put32(m, 0x30, (w + s.side - 1) / s.side);
            put32(m, 0x34, (h + s.side - 1) / s.side);
            put32(m, 0x38, w);
            put32(m, 0x3C, h);
            put32(m, 0x40, depth);
            put32(m, 0x44, 1);
            put32(m, 0x48, s.mode);
            put32(m, 0x60, 1);
            put32(m, 0x64, mi.in_tail ? 1 : 0);
        }
    }
    return S_OK;
}

uint64_t WINAPI get_size(Computer* c) { return c->tex.size; }
uint64_t WINAPI get_alignment(Computer* c) { return c->tex.align; }

uint64_t WINAPI get_mip_offset(Computer* c, uint32_t plane, uint32_t mip) {
    const Texture& t = c->tex;
    if (plane >= static_cast<uint32_t>(t.planes) || t.plane[plane].surface < 0) return 0;
    const rt::gfx10::Surface& s = t.surface[t.plane[plane].surface];
    return t.plane[plane].offset + (mip < s.mips ? s.mip[mip].offset : 0);
}

const rt::gfx10::Surface* plane_surface(Computer* c, uint32_t plane, uint64_t* offset) {
    const Texture& t = c->tex;
    if (plane >= static_cast<uint32_t>(t.planes) || t.plane[plane].surface < 0) return nullptr;
    *offset = t.plane[plane].offset;
    return &t.surface[t.plane[plane].surface];
}
uint32_t depth_of(const Computer* c) { return c->desc.dimension == 4 ? c->desc.depth_or_array : 1; }

// 6 GetTexelElementOffsetBytes(plane, mip, x, y, slice or z, sample, bit*): an element's byte offset from the
// texture's start; 0 when outside (the title reads texels through it, and must not get an error code as an offset).
uint64_t WINAPI texel_offset(Computer* c, uint32_t plane, uint32_t mip, uint64_t x, uint32_t y, uint32_t slice,
                             uint32_t, uint32_t* bit) {
    if (bit) *bit = 0;
    uint64_t base;
    const rt::gfx10::Surface* s = plane_surface(c, plane, &base);
    if (!s || mip >= s->mips || x > UINT32_MAX) return 0;
    uint64_t at = rt::gfx10::element_offset(*s, mip, static_cast<uint32_t>(x), y, slice);
    return at == UINT64_MAX ? 0 : base + at;
}

// 8 CopyIntoSubresource(tiled base, plane, subresource, linear, row pitch, depth pitch)
HRESULT WINAPI copy_into(Computer* c, uint8_t* tiled, uint32_t plane, uint32_t sub, const uint8_t* linear,
                         uint32_t row, uint32_t depth) {
    uint64_t base;
    const rt::gfx10::Surface* s = plane_surface(c, plane, &base);
    if (!s || !tiled || !linear || !s->mips) return E_INVALIDARG;
    bool ok = rt::gfx10::tile(*s, tiled + base, static_cast<uint32_t>(c->desc.width), c->desc.height, depth_of(c),
                              sub % s->mips, sub / s->mips, linear, row, depth);
    return ok ? S_OK : E_NOTIMPL;
}

// 9 CopyFromSubresource(linear, plane, subresource, sample, tiled base, row pitch, depth pitch)
HRESULT WINAPI copy_from(Computer* c, uint8_t* linear, uint32_t plane, uint32_t sub, uint32_t, const uint8_t* tiled,
                         uint32_t row, uint32_t depth) {
    uint64_t base;
    const rt::gfx10::Surface* s = plane_surface(c, plane, &base);
    if (!s || !tiled || !linear || !s->mips) return E_INVALIDARG;
    bool ok = rt::gfx10::detile(*s, tiled + base, static_cast<uint32_t>(c->desc.width), c->desc.height, depth_of(c),
                                sub % s->mips, sub / s->mips, linear, row, depth);
    return ok ? S_OK : E_NOTIMPL;
}

// 10 GetResourceTiling(total tiles*, packed mips*, tile shape*, in: room / out: count, first, subresource tilings*),
// in 64 KiB tiles of plane 0, for 2D 64 KiB modes only (others fail, as the console's library does). The mip tail is
// the packed part (tile 0 of each slice); tilings list the standard levels of every slice in turn, and with a tail
// two slices at most, all but the first zeroed (as the VIctory project records the library's answers).
struct PackedMips {
    uint8_t standard, packed;
    uint32_t tiles, start;
};
struct SubresourceTiling {
    uint32_t width;
    uint16_t height, depth;
    uint32_t start;
};
HRESULT WINAPI resource_tiling(Computer* c, uint32_t* tiles, PackedMips* packed, uint32_t* shape, uint32_t* count,
                               uint32_t first, SubresourceTiling* out) {
    constexpr uint64_t kTile = 64 * 1024;
    uint64_t base;
    const rt::gfx10::Surface* s = plane_surface(c, 0, &base);
    if (!s || s->block_bytes != kTile) return E_FAIL;
    if (s->thick()) return E_NOTIMPL;
    uint32_t standard = s->first_tail, mips = s->mips;
    bool tail = standard < mips;
    uint64_t tail_tiles = standard == 0 ? s->slice_bytes / kTile : tail ? s->mip[standard - 1].offset / kTile : 0;
    if (tiles) *tiles = static_cast<uint32_t>(c->tex.size / kTile);
    if (packed) *packed = {static_cast<uint8_t>(standard), static_cast<uint8_t>(mips - standard),
                           static_cast<uint32_t>(tail_tiles), 0};
    if (shape) {
        shape[0] = s->block_w * s->side;
        shape[1] = s->block_h * s->side;
        shape[2] = s->block_d;
    }
    if (count) {
        uint32_t slices = tail ? std::min(s->slices, 2u) : s->slices;
        uint32_t total = standard * slices;
        uint32_t wanted = total > first ? std::min(total - first, *count) : 0;
        for (uint32_t i = 0; i < wanted && out; ++i) {
            uint32_t slice = (first + i) / standard, mip = (first + i) % standard;
            const rt::gfx10::Mip& m = s->mip[mip];
            out[i] = tail && slice > 0
                         ? SubresourceTiling{0, 0, 0, 0}
                         : SubresourceTiling{m.width / s->block_w, static_cast<uint16_t>(m.height / s->block_h), 1,
                                             static_cast<uint32_t>((uint64_t{slice} * s->slice_bytes + m.offset) / kTile)};
        }
        *count = wanted;
    }
    return S_OK;
}

HRESULT WINAPI not_yet(Computer*) {
    static std::atomic<int> logged{0};
    if (logged++ < 4)
        std::fprintf(stderr, "[rt] xg: a texture-computer method that copies or addresses texels (not written yet)\n");
    return E_NOTIMPL;
}

void** computer_table() {
    static void* t[12] = {
        reinterpret_cast<void*>(&add_ref),       reinterpret_cast<void*>(&release),
        reinterpret_cast<void*>(&get_resource_layout), reinterpret_cast<void*>(&get_size),
        reinterpret_cast<void*>(&get_alignment), reinterpret_cast<void*>(&get_mip_offset),
        reinterpret_cast<void*>(&texel_offset),  reinterpret_cast<void*>(&not_yet),
        reinterpret_cast<void*>(&copy_into),     reinterpret_cast<void*>(&copy_from),
        reinterpret_cast<void*>(&resource_tiling), reinterpret_cast<void*>(&not_yet),
    };
    return t;
}

HRESULT WINAPI XGCreateTextureComputer(const ResourceDesc* desc, Computer** out) {
    if (!desc || !out) return E_POINTER;
    *out = nullptr;
    auto* c = new Computer{computer_table(), {1}, *desc, {}};
    Result r = rt::gfx10::layout_texture(*desc, &c->tex);
    if (r != Result::ok) {
        static std::atomic<int> logged{0};
        if (logged++ < 8)
            std::fprintf(stderr, "[rt] xg: XGCreateTextureComputer(dim %u %llux%ux%u format %u layout %#x flags %#x): %s\n",
                         desc->dimension, static_cast<unsigned long long>(desc->width), desc->height,
                         desc->depth_or_array, desc->format, desc->layout, desc->flags,
                         r == Result::invalid ? "invalid" : "not modelled");
        delete c;
        return r == Result::invalid ? E_INVALIDARG : E_NOTIMPL;
    }
    static std::atomic<uint64_t> made{0};  // the first computers and their count, to set against a reference run
    uint64_t n = ++made;
    if (n <= 600)
        std::fprintf(stderr, "[rt] xg: from %p texture computer %llu: %llux%ux%u format %u mips %u layout %#x: %llu bytes\n",
                     _ReturnAddress(), static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(desc->width), desc->height, desc->depth_or_array, desc->format,
                     desc->mips, desc->layout,
                     static_cast<unsigned long long>(c->tex.size));
    *out = c;
    return S_OK;
}

HRESULT WINAPI XGSuggestSwizzleMode(const ResourceDesc* desc, UINT, uint32_t* out) {
    if (!desc || !out) return E_POINTER;
    ResourceDesc d = *desc;
    d.layout = 0;
    if (d.dimension == 2) {
        *out = 0x100;  // a 1D texture: linear
        return S_OK;
    }
    Texture t;
    uint32_t mode;
    if (rt::gfx10::layout_texture(d, &t) == Result::ok && t.main >= 0) {
        mode = t.surface[t.main].mode;
    } else {
        uint32_t e = 2, side = 1;
        rt::gfx10::element_of(d.format, &e, &side);
        mode = rt::gfx10::default_mode(0, d.flags, 8ull << e, d.width, d.height);
    }
    *out = 0x100 | mode;
    return S_OK;
}

[[maybe_unused]] void** const g_early = computer_table();

}  // namespace

SHIM_TABLE(xg,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
