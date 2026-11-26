// Textures on the PC: the title writes texels (decoded video, streamed data) into console memory in the console's
// tiled layout; the PC texture made beside it gets them here, detiled on the CPU (gfx10::detile) and copied in before
// the commands that read it run.
//
// Which textures: the ones a draw's shaders read through their descriptor tables (draw() notes them on the list,
// note_textures), uploaded when the list is submitted (upload_textures, recorded into the submission's first list).
// When: the first time, and whenever a sampled hash of the texture's memory changes. Render targets, depth buffers
// and UAVs are left alone: the PC's GPU writes those, and their console memory is stale.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../gpu/gfx10_swizzle.h"
#include "d3d12x_internal.h"

namespace rt::d3d12x {
namespace {

struct Known {
    uint64_t hash = 0;
    bool uploaded = false;
    bool failed = false;
};

struct State {
    std::mutex m;
    std::vector<Resource*> views;  // the resource behind each view-heap index
    std::unordered_map<Resource*, Known> known;
    uint64_t uploads = 0, bytes = 0;
};
State g;  // a global: the runtime has no thread-safe statics

// A hash of the texture's memory; 0 if it can't be read (`*fault`: the offset that couldn't). Textures up to 8 MiB
// (font caches, UI images: small changes, such as a glyph the title adds to its cache) are hashed whole, four
// 64-bit lanes at a time; bigger ones (video frames, which change everywhere) by 64-byte samples spread over them.
constexpr uint64_t kWholeHash = 8ull << 20;

uint64_t sample_hash(const uint8_t* p, uint64_t size, uint64_t* fault) {
    uint64_t at = 0;
    __try {
        if (size <= kWholeHash) {
            uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull,
                     d = 0x27D4EB2F165667C5ull;
            const uint64_t* q = reinterpret_cast<const uint64_t*>(p);
            uint64_t words = size / 8;
            for (uint64_t i = 0; i + 4 <= words; i += 4, at = i * 8) {
                a = (a ^ q[i]) * 0x100000001B3ull;
                b = (b ^ q[i + 1]) * 0x100000001B3ull;
                c = (c ^ q[i + 2]) * 0x100000001B3ull;
                d = (d ^ q[i + 3]) * 0x100000001B3ull;
            }
            uint64_t h = a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ size;
            return h ? h : 1;
        }
        uint64_t h = 1469598103934665603ull;
        uint64_t step = std::max<uint64_t>(4096, (size / 1024 + 63) & ~uint64_t{63});
        for (at = 0; at + 64 <= size; at += step) {
            const uint64_t* q = reinterpret_cast<const uint64_t*>(p + at);
            for (int i = 0; i < 8; ++i) h = (h ^ q[i]) * 1099511628211ull;
        }
        if (size >= 64) {
            at = size - 64;
            const uint64_t* q = reinterpret_cast<const uint64_t*>(p + at);
            for (int i = 0; i < 8; ++i) h = (h ^ q[i]) * 1099511628211ull;
        }
        return h ? h : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = at;
        return 0;
    }
}

bool uploadable(const Resource* r) {
    const gfx10::ResourceDesc& d = r->desc.desc;
    return r->host && r->va && d.dimension >= 2 && d.dimension <= 4 && !(d.flags & 7) && d.samples <= 1;
}

// Detiles every subresource of `r` into `dst` (mapped upload memory laid out as `fp`); false if the layout or a mode
// isn't modelled, or the memory can't be read.
bool detile_all(const Resource* r, const gfx10::Surface& s, const uint8_t* surface, uint8_t* dst,
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, UINT subresources, UINT mips) {
    const gfx10::ResourceDesc& d = r->desc.desc;
    bool volume = d.dimension == 4;
    uint32_t depth = volume ? d.depth_or_array : 1;
    __try {
        for (UINT i = 0; i < subresources; ++i) {
            UINT mip = i % mips + r->first_mip, slice = i / mips;
            const auto& f = fp[i];
            size_t depth_pitch = size_t{f.Footprint.RowPitch} * (f.Footprint.Height + s.side - 1) / s.side;
            if (!gfx10::detile(s, surface, static_cast<uint32_t>(d.width), d.height, depth, mip, slice, dst + f.Offset,
                               f.Footprint.RowPitch, depth_pitch))
                return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

}  // namespace

void remember_view(uint32_t index, Resource* r) {
    if (!index || !r) return;
    std::lock_guard lock(g.m);
    if (g.views.size() <= index) g.views.resize(index + 1, nullptr);
    g.views[index] = r;
}

void forget_upload(Resource* r) {
    std::lock_guard lock(g.m);
    g.known.erase(r);
}

Resource* view_resource(uint32_t index) {
    std::lock_guard lock(g.m);
    return index < g.views.size() ? g.views[index] : nullptr;
}

ID3D12Resource* upload_textures(ID3D12GraphicsCommandList* list, const std::vector<Resource*>& used) {
    // what changed
    struct Job {
        Resource* r;
        gfx10::Texture t;
        uint64_t hash;
    };
    std::vector<Job> jobs;
    {
        std::lock_guard lock(g.m);
        for (Resource* r : used) {
            if (!uploadable(r)) continue;
            Known& k = g.known[r];
            if (k.failed) continue;
            Job j{r, {}, 0};
            if (gfx10::layout_texture(r->desc.desc, &j.t) != gfx10::Result::ok || j.t.main < 0) {
                k.failed = true;
                std::fprintf(stderr, "[rt] textures: layout not modelled (dim %u %llux%u format %u layout %#x)\n",
                             r->desc.desc.dimension, static_cast<unsigned long long>(r->desc.desc.width),
                             r->desc.desc.height, r->desc.desc.format, r->desc.desc.layout);
                continue;
            }
            uint64_t fault = 0;
            // a placed texture with only its smaller levels mapped: what is there ends where level first_mip - 1 starts
            uint64_t there = j.t.size;
            if (r->first_mip) {
                uint64_t plane = 0;
                for (int p = 0; p < j.t.planes; ++p)
                    if (j.t.plane[p].surface == j.t.main) plane = j.t.plane[p].offset;
                there = plane + j.t.surface[j.t.main].mip[r->first_mip - 1].offset;
            }
            j.hash = sample_hash(reinterpret_cast<const uint8_t*>(r->va), there, &fault);
            if (!j.hash) {
                MEMORY_BASIC_INFORMATION mi{};
                VirtualQuery(reinterpret_cast<void*>(r->va + fault), &mi, sizeof mi);
                std::fprintf(stderr,
                             "[rt] textures: %p (%llux%u format %u, %llu bytes, %llu allocated) unreadable at +%#llx: "
                             "state %#lx protect %#lx type %#lx, region %p + %#llx\n",
                             reinterpret_cast<void*>(r->va), static_cast<unsigned long long>(r->desc.desc.width),
                             r->desc.desc.height, r->desc.desc.format, static_cast<unsigned long long>(j.t.size),
                             static_cast<unsigned long long>(r->bytes), static_cast<unsigned long long>(fault),
                             mi.State, mi.Protect, mi.Type, mi.BaseAddress, static_cast<unsigned long long>(mi.RegionSize));
                k.failed = true;  // reported once: an unmapped texture stays a bug to look at
                continue;
            }
            if (k.uploaded && k.hash == j.hash) continue;
            jobs.push_back(j);
        }
    }
    if (jobs.empty()) return nullptr;

    // one upload buffer for all of them
    ID3D12Device* dev = host_device();
    std::vector<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>> fps(jobs.size());
    std::vector<UINT64> base(jobs.size());
    UINT64 total = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        D3D12_RESOURCE_DESC hd = jobs[i].r->host->GetDesc();
        UINT n = hd.MipLevels * (hd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : hd.DepthOrArraySize);
        fps[i].resize(n);
        UINT64 bytes = 0;
        dev->GetCopyableFootprints(&hd, 0, n, 0, fps[i].data(), nullptr, nullptr, &bytes);
        total = (total + 511) & ~UINT64{511};
        base[i] = total;
        total += bytes;
    }
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource* buf = nullptr;
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&buf))) ||
        FAILED(buf->Map(0, &none, reinterpret_cast<void**>(&mapped)))) {
        std::fprintf(stderr, "[rt] textures: no upload buffer of %llu bytes\n", static_cast<unsigned long long>(total));
        if (buf) buf->Release();
        return nullptr;
    }

    for (size_t i = 0; i < jobs.size(); ++i) {
        Job& j = jobs[i];
        const gfx10::Surface& s = j.t.surface[j.t.main];
        uint64_t plane_offset = 0;
        for (int p = 0; p < j.t.planes; ++p)
            if (j.t.plane[p].surface == j.t.main) plane_offset = j.t.plane[p].offset;
        D3D12_RESOURCE_DESC hd = j.r->host->GetDesc();
        UINT n = static_cast<UINT>(fps[i].size());
        for (auto& f : fps[i]) f.Offset += base[i];
        bool ok = detile_all(j.r, s, reinterpret_cast<const uint8_t*>(j.r->va + plane_offset), mapped, fps[i].data(), n,
                             hd.MipLevels);
        std::lock_guard lock(g.m);
        Known& k = g.known[j.r];
        if (!ok) {
            k.failed = true;
            std::fprintf(stderr, "[rt] textures: couldn't detile (%llux%u format %u mode %u)\n",
                         static_cast<unsigned long long>(hd.Width), hd.Height, hd.Format, s.mode);
            continue;
        }
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {j.r->host, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(1, &b);
        for (UINT sub = 0; sub < n; ++sub) {
            D3D12_TEXTURE_COPY_LOCATION dst{j.r->host, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = sub;
            D3D12_TEXTURE_COPY_LOCATION src{buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint = fps[i][sub];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        if (!k.uploaded)
            std::fprintf(stderr, "[rt] textures: first upload of %llux%ux%u format %u, %u mip(s), mode %u\n",
                         static_cast<unsigned long long>(hd.Width), hd.Height, hd.DepthOrArraySize, hd.Format,
                         hd.MipLevels, s.mode);
        k.uploaded = true;
        k.hash = j.hash;
        uint64_t u = ++g.uploads;
        g.bytes += j.t.size;
        if ((u & (u - 1)) == 0)
            std::fprintf(stderr, "[rt] textures: %llu upload(s), %llu MiB detiled\n", static_cast<unsigned long long>(u),
                         static_cast<unsigned long long>(g.bytes >> 20));
    }
    buf->Unmap(0, nullptr);
    return buf;  // released by the caller once the GPU is done with it
}

}  // namespace rt::d3d12x
