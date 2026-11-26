#include "gfx10_swizzle.h"

#include <bit>
#include <cstring>
#include <vector>

namespace rt::gfx10 {
namespace {

const Equation kEquations[] = {
#include "gfx10_swizzle.inc"
};

// The in-block offset is linear over XOR in the coordinates: eq(x, y, z) = eq(x, 0, 0) ^ eq(0, y, 0) ^ eq(0, 0, z).
uint32_t part(const Equation& q, const uint16_t* masks, uint32_t c) {
    uint32_t o = 0;
    for (uint32_t b = 0; b < q.block_log2; ++b) o |= static_cast<uint32_t>(std::popcount(c & masks[b]) & 1) << b;
    return o;
}

struct Place {
    uint64_t base;      // the level's start in this slice (or depth layer)
    uint32_t bw, bh;    // block size in elements
    uint32_t per_row;   // blocks per row of the level
    uint32_t ox, oy;    // origin in the tail block
    bool tail;
};

Place place(const Surface& s, uint32_t mip, uint32_t slice) {
    const Mip& m = s.mip[mip];
    uint64_t layer = s.thick() ? slice / s.block_d : slice;
    Place p{layer * s.slice_bytes + (m.in_tail ? 0 : m.offset), s.block_w, s.block_h, 0, m.origin_x, m.origin_y,
            m.in_tail};
    p.per_row = m.in_tail ? 1 : m.width / s.block_w;
    return p;
}

template <typename Copy>
bool walk(const Surface& s, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip, uint32_t slice,
          size_t row_pitch, size_t depth_pitch, Copy copy) {
    if (mip >= s.mips) return false;
    uint32_t w, h, d;
    level_size(s, width, height, depth, mip, &w, &h, &d);
    uint32_t bpe = 1u << s.elem_log2;
    bool volume = depth > 1 || s.thick();
    if (s.mode == kLinear) {
        for (uint32_t z = 0; z < d; ++z) {
            uint32_t sl = volume ? z : slice;
            uint64_t base = uint64_t{sl} * s.slice_bytes + s.mip[mip].offset;
            for (uint32_t y = 0; y < h; ++y)
                copy(base + uint64_t{y} * (s.mip[mip].width << s.elem_log2), z * depth_pitch + y * row_pitch,
                     size_t{w} * bpe);
        }
        return true;
    }
    const Equation* q = equation(s.mode, s.thick(), s.elem_log2);
    if (!q) return false;
    std::vector<uint32_t> ex(w);
    for (uint32_t z = 0; z < d; ++z) {
        uint32_t sl = volume ? z : slice;
        Place p = place(s, mip, sl);
        uint32_t ez = part(*q, q->z, sl);
        for (uint32_t x = 0; x < w; ++x) ex[x] = part(*q, q->x, x + p.ox);
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t ey = part(*q, q->y, y + p.oy) ^ ez;
            uint64_t row = p.base + (p.tail ? 0 : uint64_t{y / p.bh} * p.per_row * s.block_bytes);
            for (uint32_t x = 0; x < w; ++x) {
                uint64_t at = row + (p.tail ? 0 : uint64_t{x / p.bw} * s.block_bytes) + (ex[x] ^ ey);
                copy(at, z * depth_pitch + y * row_pitch + size_t{x} * bpe, bpe);
            }
        }
    }
    return true;
}

}  // namespace

const Equation* equation(uint32_t mode, bool thick, uint32_t elem_log2) {
    for (const Equation& q : kEquations)
        if (q.mode == mode && q.thick == thick && q.elem_log2 == elem_log2) return &q;
    return nullptr;
}

void level_size(const Surface& s, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip, uint32_t* w,
                uint32_t* h, uint32_t* d) {
    auto at = [&](uint32_t v) { return v >> mip ? v >> mip : 1; };
    *w = (at(width) + s.side - 1) / s.side;
    *h = (at(height) + s.side - 1) / s.side;
    *d = depth > 1 ? at(depth) : 1;
}

uint64_t element_offset(const Surface& s, uint32_t mip, uint32_t x, uint32_t y, uint32_t slice) {
    if (mip >= s.mips) return UINT64_MAX;
    if (s.mode == kLinear)
        return uint64_t{slice} * s.slice_bytes + s.mip[mip].offset + uint64_t{y} * (s.mip[mip].width << s.elem_log2) +
               (uint64_t{x} << s.elem_log2);
    const Equation* q = equation(s.mode, s.thick(), s.elem_log2);
    if (!q) return UINT64_MAX;
    Place p = place(s, mip, slice);
    uint32_t in = part(*q, q->x, x + p.ox) ^ part(*q, q->y, y + p.oy) ^ part(*q, q->z, slice);
    if (p.tail) return p.base + in;
    return p.base + (uint64_t{y / p.bh} * p.per_row + x / p.bw) * s.block_bytes + in;
}

bool tile(const Surface& s, uint8_t* surface, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip,
          uint32_t slice, const uint8_t* linear, size_t row_pitch, size_t depth_pitch) {
    return walk(s, width, height, depth, mip, slice, row_pitch, depth_pitch,
                [&](uint64_t at, size_t from, size_t n) { std::memcpy(surface + at, linear + from, n); });
}

bool detile(const Surface& s, const uint8_t* surface, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip,
            uint32_t slice, uint8_t* linear, size_t row_pitch, size_t depth_pitch) {
    return walk(s, width, height, depth, mip, slice, row_pitch, depth_pitch,
                [&](uint64_t at, size_t to, size_t n) { std::memcpy(linear + to, surface + at, n); });
}

}  // namespace rt::gfx10
