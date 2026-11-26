// Prototype for docs/gpu-memory.md: pool chunks of title memory as D3D12 heaps.
//
// Checks that (1) a pagefile-backed section opens as a D3D12 heap (OpenExistingHeapFromAddress on a view of it) with a buffer
// over all of it, (2) its pages, mapped by the CPU anywhere through placeholders (one page at two addresses too),
// are the memory the GPU reads and writes, coherent after a fence, and (3) measures GPU copy bandwidth from pool
// memory (system RAM) against video memory.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr uint64_t kPage = 64 * 1024;
constexpr uint64_t kChunk = 1ull << 30;

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*,
                                        ULONG);

struct Gpu {
    ID3D12Device3* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    void submit_and_wait() {
        list->Close();
        ID3D12CommandList* l = list;
        queue->ExecuteCommandLists(1, &l);
        queue->Signal(fence, ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, INFINITE);
        alloc->Reset();
        list->Reset(alloc, nullptr);
    }
};

ID3D12Resource* buffer_on(ID3D12Device* dev, ID3D12Heap* heap, uint64_t size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // A heap opened from an address is a cross-adapter heap: its resources must say so.
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreatePlacedResource(heap, 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) std::fprintf(stderr, "CreatePlacedResource: %08lX\n", static_cast<unsigned long>(hr));
    return r;
}

ID3D12Resource* vram_buffer(ID3D12Device* dev, uint64_t size) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    return r;
}

// GB/s of a GPU copy of `size` bytes, from timestamps around it (best of 3).
double copy_rate(Gpu& g, ID3D12Resource* dst, ID3D12Resource* src, uint64_t size) {
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    ID3D12QueryHeap* qh = nullptr;
    g.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&qh));
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 16;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    g.dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                   IID_PPV_ARGS(&readback));
    uint64_t freq = 0;
    g.queue->GetTimestampFrequency(&freq);
    double best = 0;
    for (int i = 0; i < 3; ++i) {
        g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        g.list->CopyBufferRegion(dst, 0, src, 0, size);
        g.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        g.list->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback, 0);
        g.submit_and_wait();
        uint64_t* t = nullptr;
        readback->Map(0, nullptr, reinterpret_cast<void**>(&t));
        double seconds = static_cast<double>(t[1] - t[0]) / static_cast<double>(freq);
        readback->Unmap(0, nullptr);
        double rate = static_cast<double>(size) / seconds / 1e9;
        if (rate > best) best = rate;
    }
    readback->Release();
    qh->Release();
    return best;
}

}  // namespace

int main() {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto valloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2"));
    auto map3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3"));

    // ---- device
    Gpu g;
    IDXGIFactory6* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&g.dev)))) {
        std::fprintf(stderr, "FAIL: no ID3D12Device3\n");
        return 1;
    }
    std::printf("uma_test: %ls\n", ad.Description);
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
    g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list));
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));

    // ---- (1) two 1 GiB pool chunks, each a D3D12 heap with one buffer over it
    HANDLE section[2];
    ID3D12Heap* heap[2] = {};
    ID3D12Resource* buf[2] = {};
    for (int c = 0; c < 2; ++c) {
        section[c] = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        static_cast<DWORD>(kChunk >> 32), static_cast<DWORD>(kChunk), nullptr);
        CHECK(section[c] != nullptr);
        // From a view of the section, not OpenExistingHeapFromFileMapping: with the NVIDIA driver that one keeps
        // the section's commit after the process ends (see uma::chunk_view).
        void* view = MapViewOfFile(section[c], FILE_MAP_WRITE, 0, 0, kChunk);
        CHECK(view != nullptr);
        HRESULT hr = g.dev->OpenExistingHeapFromAddress(view, IID_PPV_ARGS(&heap[c]));
        std::printf("uma_test: OpenExistingHeapFromAddress(chunk %d) -> %08lX\n", c, static_cast<unsigned long>(hr));
        CHECK(SUCCEEDED(hr));
        if (FAILED(hr)) return 1;
        D3D12_HEAP_DESC hd = heap[c]->GetDesc();
        std::printf("uma_test:   heap %llu bytes, type %d, cpu page %d, pool %d, flags %x\n", hd.SizeInBytes,
                    hd.Properties.Type, hd.Properties.CPUPageProperty, hd.Properties.MemoryPoolPreference, hd.Flags);
        buf[c] = buffer_on(g.dev, heap[c], kChunk);
        CHECK(buf[c] != nullptr);
    }
    if (!buf[0] || !buf[1]) return 1;

    // ---- (2) pages mapped anywhere through placeholders: chunk 0 page 0 at R+0 and again at R+128K (an alias),
    // chunk 1 page 5 at R+64K.
    auto* r = static_cast<uint8_t*>(valloc2(GetCurrentProcess(), nullptr, 8 * kPage, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                            PAGE_NOACCESS, nullptr, 0));
    CHECK(r != nullptr);
    for (int i = 0; i < 7; ++i) VirtualFree(r + i * kPage, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    auto map_page = [&](int slot, int chunk, uint64_t page) {
        void* v = map3(section[chunk], GetCurrentProcess(), r + slot * kPage, page * kPage, kPage,
                       MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        CHECK(v == r + slot * kPage);
        return static_cast<uint32_t*>(v);
    };
    uint32_t* a = map_page(0, 0, 0);
    uint32_t* b = map_page(1, 1, 5);
    uint32_t* alias = map_page(2, 0, 0);
    for (uint32_t i = 0; i < kPage / 4; ++i) {
        a[i] = 0xA0000000u + i;
        b[i] = 0xB0000000u + i;
    }
    CHECK(alias[123] == 0xA0000000u + 123);  // the same page, two addresses

    // ---- (3) the GPU copies between the chunks; the CPU sees the result after the fence
    g.list->CopyBufferRegion(buf[1], 7 * kPage, buf[0], 0, kPage);       // chunk 1 page 7 <- chunk 0 page 0
    g.list->CopyBufferRegion(buf[0], 3 * kPage, buf[1], 5 * kPage, kPage);  // chunk 0 page 3 <- chunk 1 page 5
    g.submit_and_wait();
    uint32_t* c1p7 = map_page(3, 1, 7);
    uint32_t* c0p3 = map_page(4, 0, 3);
    bool same_a = true, same_b = true;
    for (uint32_t i = 0; i < kPage / 4; ++i) {
        same_a &= c1p7[i] == 0xA0000000u + i;
        same_b &= c0p3[i] == 0xB0000000u + i;
    }
    CHECK(same_a);
    CHECK(same_b);
    std::printf("uma_test: GPU copies between chunks seen by the CPU: %s\n", same_a && same_b ? "yes" : "NO");

    // A GPU write into a page the CPU maps at two addresses shows at both.
    g.list->CopyBufferRegion(buf[0], 0, buf[1], 5 * kPage, 256);
    g.submit_and_wait();
    CHECK(a[0] == 0xB0000000u && alias[0] == 0xB0000000u);

    // ---- bandwidth: GPU copies of 256 MiB
    constexpr uint64_t kSize = 256ull << 20;
    ID3D12Resource* vram_a = vram_buffer(g.dev, kSize);
    ID3D12Resource* vram_b = vram_buffer(g.dev, kSize);
    if (vram_a && vram_b) {
        double pool_to_vram = copy_rate(g, vram_a, buf[0], kSize);
        double vram_to_pool = copy_rate(g, buf[1], vram_a, kSize);
        double pool_to_pool = copy_rate(g, buf[1], buf[0], kSize);
        double vram_to_vram = copy_rate(g, vram_b, vram_a, kSize);
        std::printf("uma_test: copy GB/s  pool->vram %.1f  vram->pool %.1f  pool->pool %.1f  vram->vram %.1f\n",
                    pool_to_vram, vram_to_pool, pool_to_pool, vram_to_vram);
        CHECK(pool_to_vram > 1.0);
        vram_a->Release();
        vram_b->Release();
    }

    if (g_failures) {
        std::fprintf(stderr, "uma_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("uma_test: ok\n");
    return 0;
}
