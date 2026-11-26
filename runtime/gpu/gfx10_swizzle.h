// Where each element of a console texture lives (GFX10 swizzle equations, gfx10_swizzle.inc) and copies between a
// tiled surface and linear rows: what the console's XG library does for titles, and what the runtime needs to turn
// title textures into PC ones.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gfx10_layout.h"

namespace rt::gfx10 {

// The address equation of a mode: per address bit inside a block, the x, y and z element-coordinate bits XORed in.
struct Equation {
    uint32_t mode;
    bool thick;
    uint32_t elem_log2, block_log2;
    uint16_t x[16], y[16], z[16];
};
// Null for linear and modes the tables don't have.
const Equation* equation(uint32_t mode, bool thick, uint32_t elem_log2);

// Elements of level `mip` (width, height, depth: 1 unless 3D) of a texture whose mip 0 is `width` x `height` x `depth`
// pixels.
void level_size(const Surface& s, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip, uint32_t* w,
                uint32_t* h, uint32_t* d);

// Byte offset of element (x, y) of level `mip` in array slice `slice` (or depth slice `slice` of a 3D texture) from
// the surface's start. UINT64_MAX for a mode without an equation.
uint64_t element_offset(const Surface& s, uint32_t mip, uint32_t x, uint32_t y, uint32_t slice);

// Copies one subresource (level `mip` of slice `slice`, all its depth slices for 3D) between `surface` (the tiled
// memory, from the surface's start) and linear rows (`row_pitch` bytes apart, depth slices `depth_pitch` apart).
// `width` x `height` x `depth`: mip 0 in pixels. False for a mode without an equation.
bool tile(const Surface& s, uint8_t* surface, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip,
          uint32_t slice, const uint8_t* linear, size_t row_pitch, size_t depth_pitch);
bool detile(const Surface& s, const uint8_t* surface, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip,
            uint32_t slice, uint8_t* linear, size_t row_pitch, size_t depth_pitch);

}  // namespace rt::gfx10
