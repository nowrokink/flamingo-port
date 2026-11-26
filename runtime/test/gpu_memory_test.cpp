// gpu_memory_test: the title's memory on the GPU as translated shaders read it (shims/gpu_memory.cpp). Commits pages
// in uma's R (more than 512 MiB, so pool pages sit past half a chunk: a raw buffer view over a whole 1 GiB chunk is
// beyond D3D12's 2^27-element limit on paper), writes them from the CPU, lets gpumem::prepare() open the chunks and
// upload the page table, and has a compute shader (gpu_memory_test.hlsl, compiled to gpu_memory_test.cso next to this
// program) read them back through the page table: every value must match.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "../shims/gpu_memory.h"
#include "../shims/uma.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

ID3D12Device* g_dev = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_inc = 0;
constexpr UINT kHeapSize = 256;
constexpr UINT kResultSlot = 200;

}  // namespace

// What gpu_memory.cpp takes from the runtime, given here by the test.
namespace rt::d3d12x {
ID3D12Device* host_device() { return g_dev; }
D3D12_CPU_DESCRIPTOR_HANDLE fixed_view(uint32_t slot) {
    return {g_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{slot} * g_inc};
}
}  // namespace rt::d3d12x

int main() {
    CHECK(rt::uma::init());
    if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev)) != S_OK) {
        std::printf("gpu_memory_test: no D3D12 device; skipped\n");
        return 0;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSize,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    CHECK(SUCCEEDED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap))));
    g_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ---- title memory: 640 MiB committed in R, a value every 4 KiB (pool pages run past 512 MiB of chunk 0)
    constexpr uint64_t kBytes = 640ull << 20;
    uintptr_t va = rt::uma::reserve(kBytes + rt::uma::kPage, rt::uma::kPage);
    CHECK(va != 0);
    CHECK(rt::uma::commit(va, kBytes, PAGE_READWRITE));
    constexpr uint32_t kStride = 4096, kCount = static_cast<uint32_t>(kBytes / kStride);
    for (uint32_t i = 0; i < kCount; ++i) *reinterpret_cast<uint32_t*>(va + uint64_t{i} * kStride) = 0x5EED0000u ^ i;

    // ---- the shader
    std::ifstream f("gpu_memory_test.cso", std::ios::binary);
    std::vector<char> cso((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(!cso.empty());
    if (cso.empty()) return 1;
    D3D12_DESCRIPTOR_RANGE1 ranges[3] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 8, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
         rt::gpumem::kSlotPageTable},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, rt::gpumem::kChunkViews, 9, 0,
         D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, rt::gpumem::kSlotChunks},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 9, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, kResultSlot},
    };
    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 6;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 3;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{D3D_ROOT_SIGNATURE_VERSION_1_1};
    rsd.Desc_1_1.NumParameters = 2;
    rsd.Desc_1_1.pParameters = params;
    ID3DBlob *blob = nullptr, *err = nullptr;
    CHECK(SUCCEEDED(D3D12SerializeVersionedRootSignature(&rsd, &blob, &err)));
    ID3D12RootSignature* root = nullptr;
    CHECK(SUCCEEDED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.CS = {cso.data(), cso.size()};
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    std::printf("gpu_memory_test: compute pipeline -> %08lX\n", static_cast<unsigned long>(hr));
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) return 1;

    // ---- result buffer (one dword per value read) and a readback copy
    const uint32_t n = kCount;
    auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{type};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, uint64_t{n} * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                              {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
        ID3D12Resource* r = nullptr;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    };
    ID3D12Resource* result = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* readback = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
    u.Buffer.NumElements = n;
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(result, nullptr, &u, rt::d3d12x::fixed_view(kResultSlot));

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    CHECK(rt::gpumem::prepare(list));
    uint64_t base = rt::gpumem::r_base();
    list->SetDescriptorHeaps(1, &g_heap);
    list->SetComputeRootSignature(root);
    list->SetPipelineState(pso);
    D3D12_GPU_DESCRIPTOR_HANDLE table = g_heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(1, table);
    // the committed values, then one dword of the unmapped page right after them
    uint32_t args[6] = {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32), static_cast<uint32_t>(va),
                        static_cast<uint32_t>(uint64_t{va} >> 32), kCount, kStride};
    list->SetComputeRoot32BitConstants(0, 6, args, 0);
    list->Dispatch((kCount + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {result, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &b);
    list->CopyResource(readback, result);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 10000);
    CHECK(g_dev->GetDeviceRemovedReason() == S_OK);

    uint32_t* out = nullptr;
    D3D12_RANGE all{0, uint64_t{n} * 4};
    readback->Map(0, &all, reinterpret_cast<void**>(&out));
    uint32_t wrong = 0, first_wrong = UINT32_MAX;
    for (uint32_t i = 0; i < kCount; ++i)
        if (out[i] != (0x5EED0000u ^ i)) {
            if (first_wrong == UINT32_MAX) first_wrong = i;
            ++wrong;
        }
    std::printf("gpu_memory_test: %u values read through the page table, %u wrong%s\n", kCount, wrong,
                wrong ? "" : " (including pool pages past 512 MiB of the chunk)");
    if (wrong)
        std::printf("gpu_memory_test:   first wrong at %u (offset %llu MiB): %08x\n", first_wrong,
                    static_cast<unsigned long long>(uint64_t{first_wrong} * kStride >> 20), out[first_wrong]);
    CHECK(wrong == 0);
    std::printf(g_failures ? "gpu_memory_test: FAILED\n" : "gpu_memory_test: ok\n");
    return g_failures ? 1 : 0;
}
