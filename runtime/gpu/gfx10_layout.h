// Where a texture's bytes go in memory on the console's GPU (AMD GFX10 / RDNA2), so the runtime can answer
// GetResourceAllocationInfo with the console's sizes: the title places resources in its own heaps by those sizes and
// streams console-tiled texture data into them. Detiling for the PC copies reads the same layout.
//
// Rules from AMD's addrlib (Gfx10Lib, as published in Mesa under the MIT licence): block dimensions, the mip chain
// stored smallest level first with a shared mip-tail block, linear rows padded to 256 bytes; the default swizzle-mode
// choice and the metadata planes (HTILE of a depth buffer, CMASK of a single-level render target) as the console's
// driver makes them. Checked against sizes of retail console textures (gfx10_layout_test).
#pragma once
#include <cstdint>

namespace rt::gfx10 {

// D3D12_RESOURCE_DESC as the console's (0x38 bytes; Layout carries console swizzle modes as 0x100 + mode).
struct ResourceDesc {
    uint32_t dimension;  // 1 buffer, 2 / 3 / 4 texture 1D / 2D / 3D
    uint32_t pad;
    uint64_t alignment;
    uint64_t width;
    uint32_t height;
    uint16_t depth_or_array;
    uint16_t mips;
    uint32_t format;
    uint32_t samples, quality;
    uint32_t layout;
    uint32_t flags;  // 1 render target, 2 depth / stencil, 4 unordered access
    uint32_t pad2;
};
static_assert(sizeof(ResourceDesc) == 0x38);

// Swizzle modes (addrlib AddrSwizzleMode).
enum : uint32_t {
    kLinear = 0,
    k4KB_S = 5,
    k64KB_D = 10,
    k4KB_S_X = 21,
    k64KB_Z_X = 24,
    k64KB_R_X = 27,
};

constexpr int kMaxMips = 15;

struct Mip {
    uint64_t offset;      // from the start of a slice; levels in the tail: inside the tail block, which is at 0
    uint32_t width, height;  // in elements, padded to whole blocks (tail levels: the tail block)
    bool in_tail;
    uint32_t origin_x, origin_y;  // a tail level's place in the tail block, in elements
};

// One tiled surface: a mode, an element size and its mip chain.
struct Surface {
    uint32_t mode;
    uint32_t elem_log2;  // bytes per element, log2
    uint32_t side;       // pixels per element side: 4 for BCn
    uint32_t block_w, block_h, block_d;  // in elements; block_d > 1: a thick 3D block
    uint32_t block_bytes;
    uint32_t mips;
    uint32_t first_tail;  // first level in the mip tail (mips: none)
    uint32_t max_tail;    // levels the tail block can hold
    uint32_t slices;      // array slices, depth slices (thin 3D) or thick layers
    uint64_t slice_bytes;
    Mip mip[kMaxMips];

    bool thick() const { return block_d > 1; }
    uint64_t bytes() const { return slice_bytes * slices; }
};

struct Plane {
    uint32_t usage;  // 1 color, 2 CMASK, 4 HTILE, 5 / 6 depth / stencil, 7 / 8 the same of a depth buffer
    uint64_t offset, bytes, align;
    int surface;  // index into Texture::surface, -1: metadata or left empty
};

struct Texture {
    Plane plane[4];
    int planes;
    Surface surface[2];
    int main;  // the surface the texels are in, -1: none
    uint64_t size, align;
};

enum class Result { ok, not_modelled, invalid };

// The console's layout of a texture (dimension 2 / 3 / 4); buffers and MSAA aren't modelled.
Result layout_texture(const ResourceDesc& d, Texture* out);

// (bytes per element log2, pixels per element side) of a DXGI format; false for ones that aren't tiled (96-bit,
// packed 4:2:2) and unknown ones. The console's 116 / 117 (R10G10B10_7E3 / 6E4_A2_FLOAT) are 32 bits a pixel.
bool element_of(uint32_t format, uint32_t* elem_log2, uint32_t* side);

// The mode the console's driver picks for Layout 0 / 2 (2D): 64KB_D for 2, depth 64KB_Z_X, render targets 64KB_R_X,
// UAVs 4KB_S_X up to 64 KB of texels, else 64KB_R_X; otherwise 4KB_S.
uint32_t default_mode(uint32_t layout, uint32_t flags, uint64_t bits_per_pixel, uint64_t width, uint32_t height);

// A surface of `mode` (thick when `volume` and the mode tiles 3D blocks). False for modes this model doesn't know.
bool make_surface(uint32_t mode, uint32_t elem_log2, uint32_t side, uint32_t width, uint32_t height, uint32_t depth,
                  uint32_t array, uint32_t mips, bool volume, Surface* out);

}  // namespace rt::gfx10
