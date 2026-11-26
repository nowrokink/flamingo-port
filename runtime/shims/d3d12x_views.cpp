// View creation: the console writes descriptors into heap memory at the destination handle, and so do we.
//
//   CBV / buffer SRV / buffer UAV   a V# over the buffer's memory (shaders read it by address; no PC descriptor)
//   texture SRV / UAV               a GFX10 T# of the console's layout, mapped through the descriptor hash table to a
//                                   PC SRV / UAV pair (d3d12x_bindless.cpp)
//   sampler                         a GFX10 S#, mapped to a PC sampler the same way
//   RTV / DSV                       a tag naming a record of ours (resource + PC view desc); the PC makes the view
//                                   when a command list binds it. Tags survive the title copying descriptors.
//
// Device slots (the PC's order): 17 CBV, 18 SRV, 19 UAV, 20 RTV, 21 DSV, 22 CreateSampler; console additions:
// 70 / 71 SRV / UAV of memory the title placed itself (resource desc, view desc + GPU address at +40, dest).
#include <windows.h>
#include <intrin.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "../gpu/desc_table.h"
#include "../gpu/sharp.h"
#include "d3d12x_internal.h"
#include "uma.h"

namespace rt::d3d12x {
namespace {

void write_words(uint64_t dest, const uint32_t* words, size_t count, size_t slot_words) {
    if (!dest) return;
    __try {
        auto* slot = reinterpret_cast<uint32_t*>(dest);
        for (size_t i = 0; i < slot_words; ++i) slot[i] = i < count ? words[i] : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[rt] d3d12: descriptor destination %p isn't writable\n", reinterpret_cast<void*>(dest));
    }
}
void write_view(uint64_t dest, const uint32_t* words, size_t count) { write_words(dest, words, count, 8); }

void note(const char* what) {
    static std::mutex m;
    static std::vector<const char*> seen;
    std::lock_guard lock(m);
    for (const char* s : seen)
        if (s == what) return;
    seen.push_back(what);
    std::fprintf(stderr, "[rt] d3d12: %s\n", what);
}

// ---- buffers

uint32_t raw_format() { return 22; }  // 32_FLOAT, as console code builds raw V#s

void buffer_view(uint64_t dest, uint64_t address, uint64_t limit, uint32_t format, uint64_t first, uint32_t count,
                 uint32_t stride, bool raw) {
    gpu::BufferDesc b{};
    uint8_t sel[4] = {gpu::kSelX, gpu::kSelY, gpu::kSelZ, gpu::kSelW};
    uint32_t f = raw_format();
    if (!raw && !stride && format && !gpu::gfx10_format(format, &f, sel)) note("a typed buffer view of a format with no GFX10 twin");
    uint32_t element = raw ? 4 : stride;
    b.address = address + first * (element ? element : 4);
    b.stride = raw ? 0 : stride;
    uint64_t records = raw ? uint64_t{count} * 4 : count;
    if (limit && b.address < address + limit && raw) records = std::min<uint64_t>(records, address + limit - b.address);
    b.num_records = static_cast<uint32_t>(records);
    b.format = f;
    std::memcpy(b.sel, sel, 4);
    uint32_t v[4];
    gpu::encode_vsharp(b, v);
    write_view(dest, v, 4);
}

// ---- textures

struct TextureView {
    uint32_t format;
    uint32_t dimension;  // D3D12_SRV_DIMENSION
    uint32_t first_mip, mips;
    uint32_t first_slice, slices;
};

// The PC view dimension numbers (SRV): 2 1D, 3 1D array, 4 2D, 5 2D array, 6 2D MS, 7 2D MS array, 8 3D, 9 cube,
// 10 cube array. UAV: 2 1D, 3 1D array, 4 2D, 5 2D array, 6 2D MS, 7 2D MS array, 8 3D.
TextureView srv_view(const gfx10::ResourceDesc& r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d) {
    uint32_t mips = r.mips ? r.mips : 1, slices = r.dimension == 4 ? 1 : r.depth_or_array;
    uint32_t def = r.dimension == 2 ? (slices > 1 ? 3 : 2) : r.dimension == 4 ? 8 : (slices > 1 ? 5 : 4);
    if (r.samples > 1) def = slices > 1 ? 7 : 6;
    TextureView v{r.format, def, 0, mips, 0, slices};
    if (!d) return v;
    if (d->Format) v.format = d->Format;
    v.dimension = d->ViewDimension;
    auto range = [&](uint32_t first, uint32_t count) {
        v.first_mip = first;
        v.mips = count == UINT32_MAX || !count ? mips - std::min(first, mips - 1) : count;
    };
    switch (d->ViewDimension) {
        case 2: range(d->Texture1D.MostDetailedMip, d->Texture1D.MipLevels); v.slices = 1; break;
        case 3: range(d->Texture1DArray.MostDetailedMip, d->Texture1DArray.MipLevels);
                v.first_slice = d->Texture1DArray.FirstArraySlice; v.slices = d->Texture1DArray.ArraySize; break;
        case 4: range(d->Texture2D.MostDetailedMip, d->Texture2D.MipLevels); v.slices = 1; break;
        case 5: range(d->Texture2DArray.MostDetailedMip, d->Texture2DArray.MipLevels);
                v.first_slice = d->Texture2DArray.FirstArraySlice; v.slices = d->Texture2DArray.ArraySize; break;
        case 6: v.mips = 1; v.slices = 1; break;
        case 7: v.mips = 1; v.first_slice = d->Texture2DMSArray.FirstArraySlice;
                v.slices = d->Texture2DMSArray.ArraySize; break;
        case 8: range(d->Texture3D.MostDetailedMip, d->Texture3D.MipLevels); break;
        case 9: range(d->TextureCube.MostDetailedMip, d->TextureCube.MipLevels); v.slices = 6; break;
        case 10: range(d->TextureCubeArray.MostDetailedMip, d->TextureCubeArray.MipLevels);
                 v.first_slice = d->TextureCubeArray.First2DArrayFace; v.slices = d->TextureCubeArray.NumCubes * 6; break;
        default: break;
    }
    return v;
}

TextureView uav_view(const gfx10::ResourceDesc& r, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d) {
    TextureView v = srv_view(r, nullptr);
    v.mips = 1;
    if (!d) return v;
    if (d->Format) v.format = d->Format;
    v.dimension = d->ViewDimension;
    switch (d->ViewDimension) {
        case 2: v.first_mip = d->Texture1D.MipSlice; v.slices = 1; break;
        case 3: v.first_mip = d->Texture1DArray.MipSlice; v.first_slice = d->Texture1DArray.FirstArraySlice;
                v.slices = d->Texture1DArray.ArraySize; break;
        case 4: v.first_mip = d->Texture2D.MipSlice; v.slices = 1; break;
        case 5: v.first_mip = d->Texture2DArray.MipSlice; v.first_slice = d->Texture2DArray.FirstArraySlice;
                v.slices = d->Texture2DArray.ArraySize; break;
        case 8: v.first_mip = d->Texture3D.MipSlice; v.first_slice = d->Texture3D.FirstWSlice;
                v.slices = d->Texture3D.WSize; break;
        default: break;
    }
    return v;
}

uint32_t image_type(uint32_t dimension) {
    switch (dimension) {
        case 2: return gpu::kImg1D;
        case 3: return gpu::kImg1DArray;
        case 5: return gpu::kImg2DArray;
        case 6: return gpu::kImg2DMsaa;
        case 7: return gpu::kImg2DMsaaArray;
        case 8: return gpu::kImg3D;
        case 9: case 10: return gpu::kImgCube;
        default: return gpu::kImg2D;
    }
}

// The T# of a view of a texture at `address` laid out as the console lays out `r`.
bool texture_tsharp(uint64_t address, const gfx10::ResourceDesc& r, const TextureView& v, uint32_t t[8]) {
    gfx10::Texture layout;
    if (gfx10::layout_texture(r, &layout) != gfx10::Result::ok) {
        note("a texture view of a layout the model doesn't have (its T# is approximate)");
        layout = {};
    }
    gpu::ImageDesc d{};
    if (!gpu::gfx10_format(v.format, &d.format, d.sel)) {
        note("a texture view of a format with no GFX10 twin");
        d.format = 0;
        d.sel[0] = gpu::kSelX, d.sel[1] = gpu::kSelY, d.sel[2] = gpu::kSelZ, d.sel[3] = gpu::kSelW;
    }
    uint64_t plane = 0;
    uint32_t mode = 0;
    if (layout.main >= 0) {
        for (int i = 0; i < layout.planes; ++i)
            if (layout.plane[i].surface == layout.main) plane = layout.plane[i].offset;
        mode = layout.surface[layout.main].mode;
    }
    d.address = address + plane;
    d.width = static_cast<uint32_t>(r.width);
    d.height = r.height;
    d.depth = r.dimension == 4 ? r.depth_or_array : v.slices;
    d.type = image_type(v.dimension);
    d.base_level = v.first_mip;
    d.last_level = v.first_mip + (v.mips ? v.mips - 1 : 0);
    d.base_array = v.first_slice;
    d.last_array = v.first_slice + (v.slices ? v.slices - 1 : 0);
    d.swizzle_mode = mode;
    gpu::encode_tsharp(d, t);
    return true;
}

// PC view descs: the console's are the PC's, but its formats 116-120 and our layouts aren't the PC's.
const D3D12_SHADER_RESOURCE_VIEW_DESC* pc_srv(const D3D12_SHADER_RESOURCE_VIEW_DESC* d) {
    return d && d->Format >= 116 ? nullptr : d;
}
const D3D12_UNORDERED_ACCESS_VIEW_DESC* pc_uav(const D3D12_UNORDERED_ACCESS_VIEW_DESC* d) {
    return d && d->Format >= 116 ? nullptr : d;
}

// A console resource desc that makes sense (dimension, sizes); logs the first few that don't, with where from.
bool plausible(const gfx10::ResourceDesc* r, const char* where) {
    bool ok = r && r->dimension >= 1 && r->dimension <= 4 && r->width && r->width < (1ull << 40) && r->height &&
              r->height <= 65536 && r->format < 256;
    if (!ok) {
        static std::atomic<int> logged{0};
        if (logged++ < 6) {
            uint64_t q[7] = {};
            if (r) std::memcpy(q, r, sizeof q);
            std::fprintf(stderr, "[rt] d3d12: %s with a desc that isn't one: %llx %llx %llx %llx %llx %llx %llx\n",
                         where, q[0], q[1], q[2], q[3], q[4], q[5], q[6]);
        }
    }
    return ok;
}

// ---- textures the title placed in its own memory: a PC texture per (address, description), made on first view

std::mutex g_placed_lock;
std::map<std::pair<uint64_t, std::vector<uint32_t>>, Resource*> g_placed;

// ---- placed textures and the title's streaming
//
// The title streams the world's textures through its own memory, and a placed texture is only partly there: the
// console lays a texture out with its mip tail first and larger levels after it, and the title maps the pages of the
// levels it has loaded (measured at the first open-world load, Series S profile: of 12.4 GiB of textures placed,
// 3.8 GiB were mapped). The PC texture holds only the levels whose memory is mapped when the texture is placed
// (Resource::first_mip: the first console level it has), so the PC's memory follows the title's.
//
// A PC texture goes away (retired: unbound from the PC's descriptors at once, released a few presents later, when no
// list recorded with it can still be waiting) when
//  - another texture is placed over its memory (another address range overlapping it, or another shape at its address;
//    two descriptions of the same memory and shape are one texture seen two ways, and both stay);
//  - its memory is unmapped down to the tail (the title freed it);
//  - the title places it again with more levels mapped than the PC texture has: a new PC texture replaces it.
struct PlacedSpan {
    uint64_t end;
    uint64_t width;
    uint32_t height, depth, mips;
    Resource* res;
    std::pair<uint64_t, std::vector<uint32_t>> key;
};
std::multimap<uint64_t, PlacedSpan> g_spans;  // by start address
std::vector<std::pair<uint64_t, ID3D12Resource*>> g_retired;  // (the present it was retired at, the PC texture)
std::atomic<uint64_t> g_present_now{0};
uint64_t g_retired_count = 0, g_retired_bytes = 0, g_partial = 0, g_absent = 0, g_regrown = 0;
uint64_t g_placed_live = 0, g_evicted = 0, g_revived = 0, g_deferred = 0;  // bytes of PC textures; counts

// What each placed SRV was made from, by its T#: enough to make the PC texture again when a draw uses a view whose
// texture was evicted, or never made (g_records_lock).
struct PlacedRecord {
    Resource* res;  // a streamed resource's view; null: a placed view (address, desc)
    uint64_t address;
    gfx10::ResourceDesc desc;
    bool has_srv;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
};
std::mutex g_records_lock;
std::map<gpu::DescKey, PlacedRecord> g_records;
std::vector<Resource*> g_streamed;  // the streamed resources alive (g_placed_lock)

// The video memory the placed textures' PC textures may take: RT_TEXTURE_BUDGET_MB, or 40% of the adapter's
// dedicated memory (targets, the title's other textures and the desktop need the rest), at least 1 GiB. Past it,
// new placed textures get no PC texture until a draw uses them, and the ones idle the longest are evicted.
uint64_t texture_budget() {
    static uint64_t budget = 0;  // set with g_placed_lock held
    if (budget) return budget;
    char v[16] = {};
    if (DWORD n = GetEnvironmentVariableA("RT_TEXTURE_BUDGET_MB", v, sizeof v); n && n < sizeof v)
        budget = std::strtoull(v, nullptr, 10) << 20;
    uint64_t dedicated = 0;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (ID3D12Device* dev = host_device(); dev && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(factory->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&d)))
            dedicated = d.DedicatedVideoMemory;
        if (adapter) adapter->Release();
        factory->Release();
    }
    if (!budget) budget = std::max<uint64_t>(1ull << 30, dedicated / 5 * 2);
    std::fprintf(stderr, "[rt] placed textures: a budget of %llu MiB of video memory (the adapter has %llu MiB)\n",
                 static_cast<unsigned long long>(budget >> 20), static_cast<unsigned long long>(dedicated >> 20));
    return budget;
}

// Pages of the title's memory unmapped since the last look (uma's hook only takes note, adjacent pages merged).
std::mutex g_unmapped_lock;
std::vector<std::pair<uint64_t, uint64_t>> g_unmapped;  // [start, end)

void note_unmapped(uintptr_t va) {
    std::lock_guard lock(g_unmapped_lock);
    if (!g_unmapped.empty() && g_unmapped.back().second == va) g_unmapped.back().second = va + uma::kPage;
    else g_unmapped.push_back({va, va + uma::kPage});
}

bool page_mapped(uint64_t va) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (uma::contains(va)) return uma::query(va, &mbi) && mbi.State == MEM_COMMIT;
    return VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT;
}

// Where the texels of a placed texture start (its main surface), or 0 if the layout isn't modelled.
uint64_t texels_at(uint64_t address, const gfx10::Texture& t) {
    if (t.main < 0) return 0;
    for (int k = 0; k < t.planes; ++k)
        if (t.plane[k].surface == t.main) return address + t.plane[k].offset;
    return address;
}

// The first level of a placed texture whose memory, and that of every smaller level, is mapped; `mips` if not even
// the smallest is. One slice only: the levels of an array's slices interleave, so an array is all there (0) or not.
uint32_t first_resident_mip(uint64_t address, const gfx10::ResourceDesc& r, const gfx10::Texture& t) {
    const uint64_t base = texels_at(address, t);
    if (!base) return 0;  // not modelled: as before, the whole texture
    const gfx10::Surface& s = t.surface[t.main];
    const uint64_t all = s.bytes();
    uint64_t mapped = 0;  // bytes mapped from `base` on
    for (uint64_t p = base & ~uint64_t{uma::kPage - 1}; p < base + all; p += uma::kPage) {
        if (!page_mapped(p)) break;
        mapped = p + uma::kPage - base;
    }
    if (mapped >= all) return 0;
    if (r.dimension != 3 || r.depth_or_array != 1) return r.mips;
    // levels first_tail.. are in the tail block at 0; level i < first_tail ends where level i - 1 starts
    const uint32_t last = s.first_tail < s.mips ? s.first_tail : s.mips - 1;
    for (uint32_t m = 1; m <= last; ++m) {
        if (s.mip[m - 1].offset > mapped) continue;
        // a block-compressed PC texture's first level is at least one block
        if (std::max<uint64_t>(1, r.width >> m) < s.side || std::max<uint32_t>(1, r.height >> m) < s.side) break;
        return m;
    }
    return r.mips;
}

ID3D12Resource* make_placed_host(const gfx10::ResourceDesc& r, uint32_t first) {
    if (first >= r.mips) return nullptr;
    gfx10::ResourceDesc d = r;
    d.width = std::max<uint64_t>(1, r.width >> first);
    d.height = std::max<uint32_t>(1, r.height >> first);
    d.mips = static_cast<uint16_t>(r.mips - first);
    const char* was = texture_source("placed views (slots 70 / 71)");
    ID3D12Resource* host = make_host_texture(d, nullptr);
    texture_source(was);
    return host;
}

// With g_placed_lock held: gives `res` its PC texture (levels first_mip on), counted against the budget.
void give_host(Resource* res) {
    res->host = make_placed_host(res->desc.desc, res->first_mip);
    res->host_bytes = 0;
    if (!res->host) return;
    const D3D12_RESOURCE_DESC d = res->host->GetDesc();
    res->host_bytes = host_device()->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
    g_placed_live += res->host_bytes;
    res->last_used = g_present_now.load();
}

// With g_placed_lock held: the PC texture of `res` is no more (its descriptors read as null from now on).
void retire_host(Resource* res, uint64_t bytes) {
    if (!res->host) return;
    g_placed_live -= std::min(g_placed_live, res->host_bytes);
    res->host_bytes = 0;
    unbind_texture(res->host);
    g_retired.push_back({g_present_now.load(), res->host});
    res->host = nullptr;
    ++g_retired_count;
    g_retired_bytes += bytes;
    if ((g_retired_count & (g_retired_count - 1)) == 0 && g_retired_count >= 256)
        std::fprintf(stderr, "[rt] placed textures: %zu live; so far %llu retired (%llu MiB of the title's memory freed "
                             "or reused), %llu made with only some levels, %llu with none mapped, %llu remade larger; PC textures %llu MiB "
                             "of a %llu MiB budget, %llu evicted, %llu made again for a draw, %llu put off\n",
                     g_placed.size(), static_cast<unsigned long long>(g_retired_count),
                     static_cast<unsigned long long>(g_retired_bytes >> 20), static_cast<unsigned long long>(g_partial),
                     static_cast<unsigned long long>(g_absent), static_cast<unsigned long long>(g_regrown),
                     static_cast<unsigned long long>(g_placed_live >> 20),
                     static_cast<unsigned long long>(texture_budget() >> 20), static_cast<unsigned long long>(g_evicted),
                     static_cast<unsigned long long>(g_revived), static_cast<unsigned long long>(g_deferred));
}

// With g_placed_lock held: a streamed resource's PC texture made to hold the levels mapped now. `want`: a draw
// uses it, so the budget doesn't hold it back.
void refresh_streamed_locked(Resource* r, bool want) {
    const gfx10::ResourceDesc& d = r->desc.desc;
    gfx10::Texture t;
    const bool modelled = gfx10::layout_texture(d, &t) == gfx10::Result::ok;
    const uint32_t first = modelled ? first_resident_mip(r->va, d, t) : 0;
    if (r->host && first == r->first_mip) return;
    if (!r->host && first >= d.mips) {
        r->first_mip = first;
        return;
    }
    if (!want && !r->host && g_placed_live >= texture_budget()) {
        ++g_deferred;
        return;
    }
    if (r->host) ++g_regrown;
    retire_host(r, 0);
    r->first_mip = first;
    give_host(r);
    forget_upload(r);
    if (first >= d.mips) ++g_absent;
    else if (first) ++g_partial;
}

void retire_span(std::multimap<uint64_t, PlacedSpan>::iterator& s) {
    retire_host(s->second.res, s->second.end - s->first);
    g_placed.erase(s->second.key);
    s = g_spans.erase(s);
}

// With g_placed_lock held: the textures whose memory was unmapped since the last look, tail and all, are gone.
void retire_unmapped() {
    std::vector<std::pair<uint64_t, uint64_t>> gone;
    {
        std::lock_guard lock(g_unmapped_lock);
        gone.swap(g_unmapped);
    }
    for (const auto& g : gone) {
        // spans start at most 512 MiB before the range's end (no texture is larger)
        auto s = g_spans.lower_bound(g.first > (512ull << 20) ? g.first - (512ull << 20) : 0);
        while (s != g_spans.end() && s->first < g.second)
            if (s->second.end > g.first && !page_mapped(s->first)) retire_span(s);
            else ++s;
    }
}

// `want`: a draw uses it now, so it gets its PC texture even over the budget (the present after evicts others).
Resource* placed_texture(uint64_t address, const gfx10::ResourceDesc& r, bool want = false) {
    std::vector<uint32_t> key(14);
    std::memcpy(key.data(), &r, sizeof r);
    std::lock_guard lock(g_placed_lock);
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        uma::on_unmap(&note_unmapped);
    }
    retire_unmapped();
    // No address yet (a streamed texture described before its memory is there): nothing to show, no PC texture.
    if (!address) {
        static Resource* none = new Resource{{resource_table(), 0, 1}, {r, {}, 0}, 0, 0, {D3D12_HEAP_TYPE_DEFAULT},
                                             D3D12_HEAP_FLAG_NONE, nullptr};
        return none;
    }
    gfx10::Texture t;
    const uint64_t size = gfx10::layout_texture(r, &t) == gfx10::Result::ok ? t.size : 0;
    const uint32_t first = size ? first_resident_mip(address, r, t) : 0;
    auto it = g_placed.find({address, key});
    if (it != g_placed.end()) {
        Resource* res = it->second;
        // placed again: the same texture, unless more of it is mapped now than the PC texture holds
        if (first < res->first_mip && (want || g_placed_live < texture_budget())) {
            retire_host(res, 0);
            res->first_mip = first;
            give_host(res);
            forget_upload(res);
            ++g_regrown;
        } else if (!res->host && first < r.mips && (want || g_placed_live < texture_budget())) {
            // evicted, or not made for want of room: made now that a draw uses it (or there is room)
            res->first_mip = first;
            give_host(res);
            forget_upload(res);
            if (want) ++g_revived;
        }
        return res;
    }
    if (size) {
        const uint64_t end = address + size;
        auto s = g_spans.lower_bound(address > (512ull << 20) ? address - (512ull << 20) : 0);
        while (s != g_spans.end() && s->first < end) {
            const PlacedSpan& o = s->second;
            const bool same = s->first == address && o.end == end && o.width == r.width && o.height == r.height &&
                              o.depth == r.depth_or_array && o.mips == r.mips;
            if (o.end > address && !same) retire_span(s);
            else ++s;
        }
    }
    if (first >= r.mips) ++g_absent;
    else if (first) ++g_partial;
    auto* res = new Resource{{resource_table(), 0, 1}, {r, {}, 0}, address, 0, {D3D12_HEAP_TYPE_DEFAULT}, D3D12_HEAP_FLAG_NONE,
                             nullptr};
    res->first_mip = first;
    // over the budget: no PC texture until a draw uses it
    if (want || g_placed_live < texture_budget()) give_host(res);
    else if (first < r.mips) ++g_deferred;
    if (size)
        g_spans.emplace(address, PlacedSpan{address + size, r.width, r.height, r.depth_or_array, r.mips, res,
                                            {address, key}});
    g_placed.emplace(std::make_pair(address, std::move(key)), res);
    return res;
}

}  // namespace

void retire_texture(ID3D12Resource* texture) {
    if (!texture) return;
    std::lock_guard lock(g_placed_lock);
    unbind_texture(texture);
    g_retired.push_back({g_present_now.load(), texture});
}

void release_retired_textures(uint64_t present) {
    std::vector<ID3D12Resource*> gone;
    {
        std::lock_guard lock(g_placed_lock);
        g_present_now = present;
        retire_unmapped();
        size_t keep = 0;
        for (auto& e : g_retired)
            if (e.first + 8 <= present) gone.push_back(e.second);
            else g_retired[keep++] = e;
        g_retired.resize(keep);
        // over the budget: the PC textures no draw has used for the longest go, down to 85% of it; a draw that
        // uses one again gets it made again (used_view)
        if (g_placed_live > texture_budget()) {
            std::vector<std::pair<uint64_t, Resource*>> idle;
            for (auto& e : g_placed)
                if (e.second->host && e.second->last_used + 30 < present) idle.push_back({e.second->last_used, e.second});
            for (Resource* r : g_streamed)
                if (r->host && r->last_used + 30 < present) idle.push_back({r->last_used, r});
            std::sort(idle.begin(), idle.end());
            const uint64_t goal = texture_budget() / 100 * 85;
            for (size_t i = 0; i < idle.size() && g_placed_live > goal; ++i) {
                retire_host(idle[i].second, 0);
                ++g_evicted;
            }
        }
        static uint64_t told = 0;
        if (present - told >= 600) {
            told = present;
            std::fprintf(stderr, "[rt] placed textures: PC textures %llu MiB of a %llu MiB budget; %llu evicted, %llu "
                                 "made again for a draw, %llu put off\n",
                         static_cast<unsigned long long>(g_placed_live >> 20),
                         static_cast<unsigned long long>(texture_budget() >> 20),
                         static_cast<unsigned long long>(g_evicted), static_cast<unsigned long long>(g_revived),
                         static_cast<unsigned long long>(g_deferred));
        }
    }
    for (ID3D12Resource* r : gone) {
        note_texture_released(r);
        r->Release();
    }
}

void adopt_streamed(Resource* r) {
    std::lock_guard lock(g_placed_lock);
    static bool hooked = false;  // as placed_texture does
    r->streamed = true;
    r->first_mip = r->desc.desc.mips;  // none yet
    g_streamed.push_back(r);
    refresh_streamed_locked(r, false);
    (void)hooked;
}

void refresh_streamed(Resource* r) {
    std::lock_guard lock(g_placed_lock);
    refresh_streamed_locked(r, false);
}

void drop_streamed(Resource* r) {
    std::lock_guard lock(g_placed_lock);
    retire_host(r, 0);
    auto it = std::find(g_streamed.begin(), g_streamed.end(), r);
    if (it != g_streamed.end()) {
        *it = g_streamed.back();
        g_streamed.pop_back();
    }
}

Resource* used_view(const uint32_t* t) {
    uint32_t index = view_index(t);
    Resource* r = index ? view_resource(index) : nullptr;
    if (!r || !r->host) {
        PlacedRecord rec;
        {
            std::lock_guard lock(g_records_lock);
            auto it = g_records.find(gpu::DescriptorHashTable::tsharp_key(t));
            if (it == g_records.end()) return r;
            rec = it->second;
        }
        Resource* p = rec.res;
        if (p) {
            std::lock_guard lock(g_placed_lock);
            if (std::find(g_streamed.begin(), g_streamed.end(), p) == g_streamed.end()) return r;  // released since
            const bool had = p->host != nullptr;
            refresh_streamed_locked(p, true);
            if (!had && p->host) ++g_revived;
        } else {
            p = placed_texture(rec.address, rec.desc, true);
        }
        if (!p->host) return r;
        remember_view(bind_texture(t, p->host, rec.has_srv ? &rec.srv : nullptr, nullptr), p);
        r = p;
    }
    r->last_used = g_present_now.load();
    return r;
}

namespace {

// ---- render targets and depth buffers: tagged records

struct TargetRecord {
    Resource* resource;
    bool depth;
    D3D12_RENDER_TARGET_VIEW_DESC rtv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
    bool has_desc;
};

std::mutex g_targets_lock;
std::vector<TargetRecord> g_targets;
constexpr uint32_t kRtvTag = 0x31565452, kDsvTag = 0x31565344;  // "RTV1", "DSV1"

void write_target(uint64_t dest, const TargetRecord& rec) {
    uint32_t index;
    {
        std::lock_guard lock(g_targets_lock);
        index = static_cast<uint32_t>(g_targets.size());
        g_targets.push_back(rec);
    }
    uint32_t w[2] = {rec.depth ? kDsvTag : kRtvTag, index};
    write_view(dest, w, 2);
}

}  // namespace

namespace {
bool read_tag(uint64_t handle, uint32_t w[2]) {
    __try {
        std::memcpy(w, reinterpret_cast<const void*>(handle), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool target_view(uint64_t handle, TargetView* out) {
    uint32_t w[2];
    if (!handle || !read_tag(handle, w) || (w[0] != kRtvTag && w[0] != kDsvTag)) return false;
    std::lock_guard lock(g_targets_lock);
    if (w[1] >= g_targets.size()) return false;
    const TargetRecord& r = g_targets[w[1]];
    *out = {r.resource, r.depth, r.has_desc && !r.depth, r.rtv, r.has_desc && r.depth, r.dsv};
    return true;
}

// void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, dest) (slot 17): a V# over the buffer's
// address with a 16-byte stride (num_records in float4s). Shaders read constants both by offset (s_buffer_load,
// where the stride doesn't matter) and as an indexed float4 array (buffer_load ... idxen: Scaleform's batched
// vertex shaders fetch per-batch matrix rows that way), which needs it: with stride 0 every row read was row 0.
void WINAPI create_cbv(void*, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, uint64_t dest) {
    if (!desc || !desc->BufferLocation) return write_view(dest, nullptr, 0);
    buffer_view(dest, desc->BufferLocation, desc->SizeInBytes, 0, 0, (desc->SizeInBytes + 15) / 16, 16, false);
}

// void CreateShaderResourceView(resource, desc, dest) (slot 18).
void WINAPI create_srv(void*, void* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, uint64_t dest) {
    Resource* r = as_resource(resource);
    if (!r) {
        if (resource) note("a shader resource view of an object that isn't one of our resources");
        return write_view(dest, nullptr, 0);
    }
    const gfx10::ResourceDesc& rd = r->desc.desc;
    if (rd.dimension == 1) {
        if (!desc) return buffer_view(dest, r->va, rd.width, 0, 0, static_cast<uint32_t>(rd.width / 4), 0, true);
        bool raw = desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW;
        if (desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
            note("an acceleration-structure view (written as a raw V#)");
            uint64_t at = desc->RaytracingAccelerationStructure.Location;
            return buffer_view(dest, at, 0, 0, 0, static_cast<uint32_t>(rd.width / 4), 0, true);
        }
        return buffer_view(dest, r->va, rd.width, desc->Format, desc->Buffer.FirstElement, desc->Buffer.NumElements,
                           desc->Buffer.StructureByteStride, raw);
    }
    uint32_t t[8];
    texture_tsharp(r->va, rd, srv_view(rd, desc), t);
    write_view(dest, t, 8);
    if (r->streamed) {
        refresh_streamed(r);  // the title makes a view when it has loaded more of the texture
        std::lock_guard lock(g_records_lock);
        PlacedRecord& rec = g_records[gpu::DescriptorHashTable::tsharp_key(t)];
        rec.res = r;
        rec.address = r->va;
        rec.desc = rd;
        rec.has_srv = pc_srv(desc) != nullptr;
        if (rec.has_srv) rec.srv = *desc;
    }
    remember_view(bind_texture(t, r->host, pc_srv(desc), nullptr), r);
}

// void CreateUnorderedAccessView(resource, counter, desc, dest) (slot 19). Counters aren't modelled.
void WINAPI create_uav(void*, void* resource, void* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                       uint64_t dest) {
    Resource* r = as_resource(resource);
    if (!r) {
        if (resource) note("an unordered access view of an object that isn't one of our resources");
        return write_view(dest, nullptr, 0);
    }
    if (counter) note("a UAV counter (not modelled)");
    const gfx10::ResourceDesc& rd = r->desc.desc;
    if (rd.dimension == 1) {
        if (!desc) return buffer_view(dest, r->va, rd.width, 0, 0, static_cast<uint32_t>(rd.width / 4), 0, true);
        bool raw = desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW;
        return buffer_view(dest, r->va, rd.width, desc->Format, desc->Buffer.FirstElement, desc->Buffer.NumElements,
                           desc->Buffer.StructureByteStride, raw);
    }
    uint32_t t[8];
    texture_tsharp(r->va, rd, uav_view(rd, desc), t);
    write_view(dest, t, 8);
    bind_texture(t, r->host, nullptr, pc_uav(desc));
}

// void CreateRenderTargetView(resource, desc, dest) (slot 20) / CreateDepthStencilView (slot 21).
void WINAPI create_rtv(void*, void* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, uint64_t dest) {
    TargetRecord rec{as_resource(resource), false, desc ? *desc : D3D12_RENDER_TARGET_VIEW_DESC{}, {}, desc != nullptr};
    write_target(dest, rec);
}
void WINAPI create_dsv(void*, void* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, uint64_t dest) {
    TargetRecord rec{as_resource(resource), true, {}, desc ? *desc : D3D12_DEPTH_STENCIL_VIEW_DESC{}, desc != nullptr};
    write_target(dest, rec);
}

// void CreateSampler(const D3D12_SAMPLER_DESC*, dest) (slot 22): an S# in the 16-byte sampler slot.
void WINAPI create_sampler(void*, const D3D12_SAMPLER_DESC* desc, uint64_t dest) {
    if (!desc) return write_words(dest, nullptr, 0, 4);
    gpu::SamplerDesc d{static_cast<uint32_t>(desc->Filter),
                       static_cast<uint32_t>(desc->AddressU),
                       static_cast<uint32_t>(desc->AddressV),
                       static_cast<uint32_t>(desc->AddressW),
                       desc->MipLODBias,
                       desc->MaxAnisotropy,
                       static_cast<uint32_t>(desc->ComparisonFunc),
                       {desc->BorderColor[0], desc->BorderColor[1], desc->BorderColor[2], desc->BorderColor[3]},
                       desc->MinLOD,
                       desc->MaxLOD};
    uint32_t s[4];
    gpu::encode_ssharp(d, s);
    write_words(dest, s, 4, 4);
    bind_sampler(s, *desc);
}

// Slots 70 / 71: views of memory the title placed itself: (resource desc, view desc whose GPU address is at +40,
// dest). Buffers become V#s at that address; textures get a PC texture of their own, made on first view.
void WINAPI create_placed_srv(void*, const gfx10::ResourceDesc* resource, const uint8_t* desc, uint64_t dest) {
    if (!desc || !plausible(resource, "a placed SRV (slot 70)")) {
        static std::atomic<int> logged{0};
        if (desc && logged++ < 3) {
            void* frames[6] = {};
            USHORT n = RtlCaptureStackBackTrace(0, 6, frames, nullptr);
            std::fprintf(stderr, "[rt]   stack:");
            for (USHORT i = 0; i < n; ++i) std::fprintf(stderr, " %p", frames[i]);
            std::fprintf(stderr, "\n");
            uint64_t q[6];
            std::memcpy(q, desc, sizeof q);
            std::fprintf(stderr, "[rt]   its view desc: %llx %llx %llx %llx %llx %llx; dest %llx; from %p\n", q[0], q[1],
                         q[2], q[3], q[4], q[5], static_cast<unsigned long long>(dest), _ReturnAddress());
        }
        return write_view(dest, nullptr, 0);
    }
    auto* view = reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc);
    uint64_t address;
    std::memcpy(&address, desc + 40, 8);
    if (resource->dimension == 1) {
        bool raw = view->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW;
        return buffer_view(dest, address, resource->width, view->Format, view->Buffer.FirstElement,
                           view->Buffer.NumElements, view->Buffer.StructureByteStride, raw);
    }
    uint32_t t[8];
    texture_tsharp(address, *resource, srv_view(*resource, view), t);
    write_view(dest, t, 8);
    {
        std::lock_guard lock(g_records_lock);
        PlacedRecord& rec = g_records[gpu::DescriptorHashTable::tsharp_key(t)];
        rec.res = nullptr;
        rec.address = address;
        rec.desc = *resource;
        rec.has_srv = pc_srv(view) != nullptr;
        if (rec.has_srv) rec.srv = *view;
    }
    Resource* placed = placed_texture(address, *resource);
    remember_view(bind_texture(t, placed->host, pc_srv(view), nullptr), placed);
}

void WINAPI create_placed_uav(void*, const gfx10::ResourceDesc* resource, const uint8_t* desc, uint64_t dest) {
    if (!desc || !plausible(resource, "a placed UAV (slot 71)")) return write_view(dest, nullptr, 0);
    auto* view = reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(desc);
    uint64_t address;
    std::memcpy(&address, desc + 40, 8);
    if (resource->dimension == 1) {
        bool raw = view->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW;
        return buffer_view(dest, address, resource->width, view->Format, view->Buffer.FirstElement,
                           view->Buffer.NumElements, view->Buffer.StructureByteStride, raw);
    }
    uint32_t t[8];
    texture_tsharp(address, *resource, uav_view(*resource, view), t);
    write_view(dest, t, 8);
    bind_texture(t, placed_texture(address, *resource)->host, nullptr, pc_uav(view));
}

}  // namespace rt::d3d12x
