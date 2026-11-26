// gfx10_layout against sizes of retail console textures (the room each takes in a console texture archive; the
// numbers as the xcl project's tests record them, used here as facts only), plus the rules the model applies to planes.
#include "../gpu/gfx10_layout.h"
#include "../gpu/gfx10_swizzle.h"
#include "../gpu/sharp.h"

#include <vector>

#include <cstdio>

using namespace rt::gfx10;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%-60s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

ResourceDesc texture(uint32_t dim, uint64_t w, uint32_t h, uint16_t depth, uint16_t mips, uint32_t format,
                     uint32_t layout, uint32_t flags = 0) {
    ResourceDesc d{};
    d.dimension = dim;
    d.width = w;
    d.height = h;
    d.depth_or_array = depth;
    d.mips = mips;
    d.format = format;
    d.samples = 1;
    d.layout = layout;
    d.flags = flags;
    return d;
}

}  // namespace

int main() {
    constexpr uint32_t BC1 = 71, BC7 = 98, BGRA8 = 87, R16 = 56, RGBA16F = 10, D32 = 40, RGBA8 = 28, D24S8 = 45;
    char what[128];

    // 2D: (mode, format, width, height, mips, bytes of one slice)
    struct { uint32_t mode, format, w, h, mips; uint64_t bytes; } flat[] = {
        {5, BC7, 128, 1024, 5, 192512},  {5, BGRA8, 128, 128, 7, 90112},   {5, BC7, 512, 64, 9, 65536},
        {5, BC1, 64, 512, 9, 61440},     {10, BC7, 2048, 2048, 1, 4194304}, {10, BC7, 512, 512, 8, 393216},
        {5, BC7, 16, 16, 5, 4096},  // every level in the tail
    };
    for (auto& t : flat) {
        Texture x;
        Result r = layout_texture(texture(3, t.w, t.h, 1, static_cast<uint16_t>(t.mips), t.format, 0x100 + t.mode), &x);
        std::snprintf(what, sizeof what, "2D %ux%u format %u mode %u mips %u: %llu bytes", t.w, t.h, t.format, t.mode,
                      t.mips, static_cast<unsigned long long>(t.bytes));
        expect(r == Result::ok && x.size == t.bytes, what);
    }
    {
        Texture x;
        layout_texture(texture(3, 512, 512, 32, 8, BC7, 0x100 + 10), &x);
        expect(x.size == 32ull * 393216, "array of 32: 32 slices");
    }

    // 3D, 4KB_S (thick): (format, w, h, d, bytes)
    struct { uint32_t format, w, h, d; uint64_t bytes; } volume[] = {
        {R16, 256, 32, 8, 262144}, {RGBA16F, 32, 64, 32, 524288}, {RGBA16F, 32, 32, 32, 262144}};
    for (auto& t : volume) {
        Texture x;
        Result r = layout_texture(texture(4, t.w, t.h, static_cast<uint16_t>(t.d), 1, t.format, 0), &x);
        std::snprintf(what, sizeof what, "3D %ux%ux%u format %u: %llu bytes", t.w, t.h, t.d, t.format,
                      static_cast<unsigned long long>(t.bytes));
        expect(r == Result::ok && x.size == t.bytes, what);
    }
    {
        Surface s;
        make_surface(k4KB_S, 1, 1, 256, 32, 8, 1, 1, true, &s);
        expect(s.block_w == 8 && s.block_h == 16 && s.block_d == 16, "thick 4KB block of 2-byte elements: 8x16x16");
    }

    // default modes
    {
        Texture x;
        layout_texture(texture(3, 256, 256, 1, 1, RGBA8, 0), &x);
        expect(x.surface[0].mode == k4KB_S && x.align == 4096, "plain texture: 4KB_S, 4 KB aligned");
        layout_texture(texture(3, 1920, 1080, 1, 1, RGBA8, 0, 1), &x);
        expect(x.surface[0].mode == k64KB_R_X, "render target: 64KB_R_X");
        expect(x.planes == 2 && x.plane[1].usage == 2 && x.plane[1].offset % 0x2000 == 0 &&
                   x.plane[1].offset >= x.plane[0].bytes && x.plane[1].bytes == 2048ull * 2048 / 128,
               "render target: CMASK after the texels");
        layout_texture(texture(3, 128, 128, 1, 1, RGBA8, 0, 4), &x);
        expect(x.surface[0].mode == k4KB_S_X, "small UAV: 4KB_S_X");
        layout_texture(texture(3, 1024, 1024, 1, 1, RGBA8, 0, 4), &x);
        expect(x.surface[0].mode == k64KB_R_X, "large UAV: 64KB_R_X");
    }

    // depth buffers: HTILE first, then depth (then stencil)
    {
        Texture x;
        Result r = layout_texture(texture(3, 1920, 1080, 1, 1, D32, 0, 2), &x);
        expect(r == Result::ok && x.surface[0].mode == k64KB_Z_X && x.planes == 2 && x.plane[0].usage == 4 &&
                   x.plane[0].bytes == 2048ull * 2048 / 16 && x.plane[1].offset % 0x10000 == 0 &&
                   x.size == x.plane[1].offset + x.plane[1].bytes,
               "D32 depth buffer: HTILE, depth");
        r = layout_texture(texture(3, 1920, 1080, 1, 1, D24S8, 0, 2), &x);
        expect(r == Result::ok && x.planes == 3 && x.plane[1].usage == 7 && x.plane[2].usage == 8 &&
                   x.surface[1].elem_log2 == 0 && x.plane[2].offset >= x.plane[1].offset + x.plane[1].bytes,
               "D24S8 depth buffer: HTILE, depth, stencil");
    }

    // linear rows are 256 bytes
    {
        Texture x;
        layout_texture(texture(3, 100, 10, 1, 1, RGBA8, 1), &x);
        expect(x.size == 512ull * 10, "linear 100x10 RGBA8: 512-byte rows");
    }

    // not modelled
    {
        Texture x;
        ResourceDesc d = texture(3, 64, 64, 1, 1, RGBA8, 0);
        d.samples = 4;
        expect(layout_texture(d, &x) == Result::not_modelled, "MSAA: not modelled");
    }

    // element addresses (gpu/gfx10_swizzle.h): 4KB_S offsets as the asset exporter's detiling (verified on retail
    // textures) gives them, for a 64x64 texture of 4- and 16-byte elements
    {
        struct { uint32_t format, x, y; uint64_t at; } want[] = {
            {RGBA8, 1, 0, 4},        {RGBA8, 0, 1, 16},        {RGBA8, 5, 3, 180},       {RGBA8, 31, 7, 2812},
            {RGBA8, 33, 40, 12548},  {RGBA8, 63, 63, 16380},   {2, 1, 0, 64},            {2, 5, 3, 624},
            {2, 31, 7, 7152},        {2, 33, 40, 42048},       {2, 63, 63, 65520}};
        bool ok = true;
        for (auto& w : want) {
            Texture x;
            layout_texture(texture(3, 64, 64, 1, 1, w.format, 0x100 + k4KB_S), &x);
            ok &= element_offset(x.surface[0], 0, w.x, w.y, 0) == w.at;
        }
        expect(ok, "4KB_S element offsets match the asset exporter's");
    }
    // every element of every level and slice gets its own bytes, and tile / detile round-trip
    for (uint32_t mode : {1u, 5u, 6u, 9u, 10u, 21u, 22u, 24u, 25u, 26u, 27u}) {
        for (uint32_t format : {61u, 56u, RGBA8, 16u, 2u}) {  // 1, 2, 4, 8, 16 bytes
            if (mode == 24 && format == 2) continue;  // no 16-byte depth
            Texture x;
            if (layout_texture(texture(3, 72, 40, 3, 6, format, 0x100 + mode), &x) != Result::ok) {
                std::snprintf(what, sizeof what, "mode %u format %u: a layout", mode, format);
                expect(false, what);
                continue;
            }
            const Surface& s = x.surface[0];
            std::vector<uint8_t> tiled(x.size, 0), seen(x.size, 0);
            uint32_t bpe = 1u << s.elem_log2;
            bool unique = true, round = true;
            uint32_t stamp = 1;
            for (uint32_t slice = 0; slice < 3; ++slice)
                for (uint32_t mip = 0; mip < s.mips; ++mip) {
                    uint32_t w, h, d;
                    level_size(s, 72, 40, 1, mip, &w, &h, &d);
                    std::vector<uint8_t> lin(size_t{w} * h * bpe), back(lin.size());
                    for (auto& b : lin) b = static_cast<uint8_t>(stamp = stamp * 1103515245u + 12345u) | 1;
                    for (uint32_t yy = 0; yy < h; ++yy)
                        for (uint32_t xx = 0; xx < w; ++xx) {
                            uint64_t at = element_offset(s, mip, xx, yy, slice);
                            if (at + bpe > tiled.size() || seen[at]) unique = false;
                            else seen[at] = 1;
                        }
                    tile(s, tiled.data(), 72, 40, 1, mip, slice, lin.data(), size_t{w} * bpe, 0);
                    detile(s, tiled.data(), 72, 40, 1, mip, slice, back.data(), size_t{w} * bpe, 0);
                    round &= back == lin;
                }
            std::snprintf(what, sizeof what, "mode %u, %u-byte elements: own bytes, round trip", mode, bpe);
            expect(unique && round, what);
        }
    }

    // descriptors (gpu/sharp.h)
    {
        using namespace rt::gpu;
        const uint32_t literal[4] = {0x92, 0xC00000, 0x14500000, 0};  // a literal S# seen in console shaders
        SamplerDesc s = decode_ssharp(literal);
        expect(s.filter == 0x14 && s.address_u == 3 && s.address_v == 3 && s.address_w == 3 && s.min_lod == 0 &&
                   s.max_lod == 12,
               "literal S#: bilinear, point mips, clamp, LOD 0..12");
        uint32_t again[4];
        encode_ssharp(s, again);
        expect(again[0] == literal[0] && again[1] == literal[1] && again[2] == literal[2] && again[3] == literal[3],
               "S# encode(decode(x)) == x");

        uint32_t f;
        uint8_t sel[4];
        expect(gfx10_format(87, &f, sel) && f == 56 && sel[0] == kSelZ && sel[2] == kSelX,
               "B8G8R8A8: 8_8_8_8 read as ZYXW");
        ImageDesc d{0x123456700ull, 56, {kSelX, kSelY, kSelZ, kSelW}, 1920, 1080, 1, kImg2D, 0, 0, 0, 0, 27};
        uint32_t t[8];
        encode_tsharp(d, t);
        uint32_t width = ((t[1] >> 30) | ((t[2] & 0xFFF) << 2)) + 1, height = ((t[2] >> 14) & 0x3FFF) + 1;
        expect(t[0] == 0x1234567 && ((t[1] >> 20) & 0x1FF) == 56 && width == 1920 && height == 1080 &&
                   ((t[3] >> 20) & 0x1F) == 27 && (t[3] >> 28) == kImg2D,
               "T#: address, format, size, swizzle mode, type");
    }

    std::printf(failures ? "%d FAILED\n" : "all layout checks passed\n", failures);
    return failures ? 1 : 0;
}
