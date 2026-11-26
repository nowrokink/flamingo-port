#include "gpu_memory.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "d3d12x.h"
#include "d3d12x_internal.h"
#include "uma.h"

namespace rt::gpumem {
namespace {

struct State {
    std::mutex m;
    bool tried = false, ok = false;
    ID3D12Device3* dev = nullptr;
    std::vector<ID3D12Heap*> heaps;
    std::vector<ID3D12Resource*> chunks;
    ID3D12Resource* table = nullptr;        // default heap, the page table the shaders read
    ID3D12Resource* upload = nullptr;       // upload heap, the same size: changed entries go through it
    uint8_t* upload_cpu = nullptr;
    size_t entries = 0;
    bool table_is_uav = false, first = true;
};

State g_state;  // a global: the runtime has no thread-safe statics

ID3D12Resource* buffer(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* props, ID3D12Heap* heap, uint64_t bytes,
                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    HRESULT hr = heap ? dev->CreatePlacedResource(heap, 0, &d, state, nullptr, IID_PPV_ARGS(&r))
                      : dev->CreateCommittedResource(props, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[rt] gpumem: a %llu-byte buffer couldn't be made (%08lX)\n",
                     static_cast<unsigned long long>(bytes), static_cast<unsigned long>(hr));
        return nullptr;
    }
    return r;
}

void raw_uav(ID3D12Device* dev, ID3D12Resource* r, uint64_t bytes, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = static_cast<UINT>(bytes / 4);
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(r, nullptr, &u, at);
}

bool setup(State& s) {
    s.tried = true;
    ID3D12Device* dev = d3d12x::host_device();
    if (!dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&s.dev)))) return false;
    s.entries = uma::page_count();
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD};
    s.table = buffer(dev, &def, nullptr, s.entries * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COMMON);
    s.upload = buffer(dev, &up, nullptr, s.entries * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!s.table || !s.upload) return false;
    D3D12_RANGE none{0, 0};
    if (FAILED(s.upload->Map(0, &none, reinterpret_cast<void**>(&s.upload_cpu)))) return false;
    raw_uav(dev, s.table, s.entries * 4, d3d12x::fixed_view(kSlotPageTable));
    // every chunk view starts null (reads zero), kNullChunk stays so
    for (uint32_t i = 0; i < kChunkViews; ++i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = 1;
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev->CreateUnorderedAccessView(nullptr, nullptr, &u, d3d12x::fixed_view(kSlotChunks + i));
    }
    std::fprintf(stderr, "[rt] gpumem: page table of %zu entries on the GPU\n", s.entries);
    return true;
}

// Chunks uma made since the last call become heaps with a buffer each, and their views.
void open_chunks(State& s) {
    for (size_t i = s.chunks.size(); i < uma::chunk_count() && i < kNullChunk; ++i) {
        ID3D12Heap* heap = nullptr;
        void* view = uma::chunk_view(i);
        HRESULT hr = view ? s.dev->OpenExistingHeapFromAddress(view, IID_PPV_ARGS(&heap)) : E_OUTOFMEMORY;
        // a heap opened from an address is a cross-adapter heap: its resources must say so
        ID3D12Resource* r = SUCCEEDED(hr) ? buffer(s.dev, nullptr, heap, uma::kChunk,
                                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                                       D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER,
                                                   D3D12_RESOURCE_STATE_COMMON)
                                          : nullptr;
        if (!r) {
            std::fprintf(stderr, "[rt] gpumem: chunk %zu couldn't be opened for the GPU (%08lX)\n", i,
                         static_cast<unsigned long>(hr));
            if (heap) heap->Release();
            return;
        }
        s.heaps.push_back(heap);
        s.chunks.push_back(r);
        raw_uav(s.dev, r, uma::kChunk, d3d12x::fixed_view(kSlotChunks + static_cast<uint32_t>(i)));
        std::fprintf(stderr, "[rt] gpumem: chunk %zu visible to the GPU\n", i);
    }
}

}  // namespace

uint64_t r_base() { return uma::base(); }

bool host_va(uint64_t va, uint64_t bytes, D3D12_GPU_VIRTUAL_ADDRESS* out) {
    State& s = g_state;
    if (!bytes || !uma::contains(va, bytes)) return false;
    const uint32_t* table = uma::page_table();
    uint64_t first = (va - uma::base()) / uma::kPage, last = (va + bytes - 1 - uma::base()) / uma::kPage;
    uint32_t e = table[first];
    if (!e) return false;
    for (uint64_t p = first + 1; p <= last; ++p)
        if (table[p] != e + (p - first)) return false;
    uint64_t pool = e - 1, chunk = pool / uma::kPagesPerChunk;
    if ((pool + (last - first)) / uma::kPagesPerChunk != chunk) return false;
    std::lock_guard lock(s.m);
    if (chunk >= s.chunks.size()) return false;
    *out = s.chunks[chunk]->GetGPUVirtualAddress() + (pool % uma::kPagesPerChunk) * uma::kPage + (va % uma::kPage);
    return true;
}

bool host_buffer(uint64_t va, uint64_t bytes, ID3D12Resource** buffer, uint64_t* offset) {
    State& s = g_state;
    if (!bytes || !uma::contains(va, bytes)) return false;
    const uint32_t* table = uma::page_table();
    uint64_t first = (va - uma::base()) / uma::kPage, last = (va + bytes - 1 - uma::base()) / uma::kPage;
    uint32_t e = table[first];
    if (!e) return false;
    for (uint64_t p = first + 1; p <= last; ++p)
        if (table[p] != e + (p - first)) return false;
    uint64_t pool = e - 1, chunk = pool / uma::kPagesPerChunk;
    if ((pool + (last - first)) / uma::kPagesPerChunk != chunk) return false;
    std::lock_guard lock(s.m);
    if (chunk >= s.chunks.size()) return false;
    *buffer = s.chunks[chunk];
    *offset = (pool % uma::kPagesPerChunk) * uma::kPage + (va % uma::kPage);
    return true;
}

bool prepare(ID3D12GraphicsCommandList* list) {
    State& s = g_state;
    std::lock_guard lock(s.m);
    if (!s.tried) s.ok = setup(s);
    if (!s.ok || !list) return false;
    open_chunks(s);
    size_t begin = 0, end = s.entries;
    if (!s.first) uma::dirty(&begin, &end);
    end = end < s.entries ? end : s.entries;
    if (begin < end) {
        std::memcpy(s.upload_cpu + begin * 4, uma::page_table() + begin, (end - begin) * 4);
        uma::clear_dirty();
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = s.table;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (s.table_is_uav) {
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &b);
        }
        list->CopyBufferRegion(s.table, begin * 4, s.upload, begin * 4, (end - begin) * 4);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &b);
        s.table_is_uav = true;
        s.first = false;
    }
    return true;
}

}  // namespace rt::gpumem
