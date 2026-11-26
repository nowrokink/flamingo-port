// More device objects and methods: heaps and placed resources, resources at an address, page pools, query heaps,
// command signatures, pipeline-state streams, console samplers and descriptor copies.
//
// Device slots (console table): 23 CopyDescriptors, 24 CopyDescriptorsSimple, 28 CreateHeap, 29 CreatePlacedResource,
// 39 CreateQueryHeap, 41 CreateCommandSignature, 61 CreateSamplerX, 74 / 75 register / unregister a page pool,
// 86 CreatePipelineState (a stream), 133 a resource at an address the title mapped itself.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "com_probe.h"
#include "d3d12x_internal.h"
#include "shader_cache.h"
#include "shims.h"
#include "uma.h"

namespace rt::d3d12x {
namespace {

void note(const char* what) {
    static std::mutex m;
    static std::vector<const char*> seen;
    std::lock_guard lock(m);
    for (const char* s : seen)
        if (s == what) return;
    seen.push_back(what);
    std::fprintf(stderr, "[rt] d3d12: %s\n", what);
}

constexpr uint64_t kResourceAttributes = (128ull << 16) | (2ull << 14);  // as committed resources (d3d12x.cpp)

// ---- heaps

struct Heap {
    Header header;
    D3D12_HEAP_DESC desc;
    uintptr_t va;
};

void** memory_heap_table();
D3D12_HEAP_DESC* WINAPI heap_get_desc(void* a, void* b) {
    auto [h, out] = this_and_out<Heap, D3D12_HEAP_DESC>(a, b, memory_heap_table());
    if (out) *out = h->desc;
    return out;
}

void** memory_heap_table() {
    static void** vt = [] {
        void** t = shims::make_probe_vtable("D3D12Heap");
        shims::set_probe_method(t, 8, reinterpret_cast<void*>(&heap_get_desc));
        return t;
    }();
    return vt;
}

Heap* as_heap(void* p) {
    auto* h = static_cast<Heap*>(p);
    return h && h->header.vtbl == memory_heap_table() ? h : nullptr;
}

Resource* resource_on(uintptr_t va, uint64_t bytes, const ResourceDesc1& desc, const D3D12_HEAP_PROPERTIES& props,
                      D3D12_HEAP_FLAGS flags, const D3D12_CLEAR_VALUE* clear) {
    auto* r = new Resource{{resource_table(), 0, 1}, desc, va, bytes, props, flags, nullptr};
    if (desc.desc.dimension != 1) r->host = make_host_texture(desc.desc, clear);
    return r;
}

// ---- query heaps and command signatures

struct QueryHeap {
    Header header;
    D3D12_QUERY_HEAP_DESC desc;
    ID3D12QueryHeap* host;
};
void** query_heap_table() {
    static void** vt = shims::make_probe_vtable("D3D12QueryHeap");
    return vt;
}

struct CommandSignature {
    Header header;
    uint32_t stride;
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
    RootSignature* root;
};
void** command_signature_table() {
    static void** vt = shims::make_probe_vtable("D3D12CommandSignature");
    return vt;
}

}  // namespace

bool indirect_signature(void* p, IndirectSignature* out) {
    if (!is_object(p, command_signature_table())) return false;
    auto* s = static_cast<CommandSignature*>(p);
    *out = {s->stride, s->arguments.data(), s->arguments.size(), s->root};
    return true;
}

namespace {

// ---- page pools: 64 KiB pages at an address the title maps into tiled resources by page index

std::mutex g_pools_lock;
std::vector<std::pair<uint64_t, uint64_t>> g_pools;  // (address, pages); a removed one is (0, 0)

// ---- pipeline-state streams
//
// Subobjects are a D3D12_PIPELINE_STATE_SUBOBJECT_TYPE then the value, each pointer-aligned; the value starts at 8
// when it holds a pointer, else at 4.
struct Subobject {
    uint32_t bytes;    // of the value
    bool pointer;      // aligned to 8
};
bool subobject(uint32_t type, Subobject* s) {
    switch (type) {
        case 0: *s = {8, true}; return true;                   // root signature
        case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25: *s = {16, true}; return true;  // shaders
        case 7: *s = {32, true}; return true;                  // stream output
        case 8: *s = {328, false}; return true;                // blend
        case 9: case 13: case 14: case 16: case 18: case 20: *s = {4, false}; return true;
        case 10: *s = {44, false}; return true;                // rasterizer (11 fields)
        case 11: *s = {52, false}; return true;                // depth-stencil
        case 21: *s = {56, false}; return true;                // depth-stencil 1 (+ depth bounds)
        case 12: *s = {16, true}; return true;                 // input layout
        case 15: *s = {36, false}; return true;                // render-target formats
        case 17: *s = {8, false}; return true;                 // sample desc
        case 19: *s = {16, true}; return true;                 // cached PSO
        case 22: *s = {24, true}; return true;                 // view instancing
        case 26: *s = {60, false}; return true;                // depth-stencil 2
        case 27: *s = {44, false}; return true;                // rasterizer 1
        case 28: *s = {40, false}; return true;                // rasterizer 2
        default: return false;
    }
}

std::vector<uint8_t> code_of(const uint8_t* value) {
    D3D12_SHADER_BYTECODE b;
    std::memcpy(&b, value, sizeof b);
    if (!b.pShaderBytecode || !b.BytecodeLength) return {};
    auto* p = static_cast<const uint8_t*>(b.pShaderBytecode);
    return std::vector<uint8_t>(p, p + b.BytecodeLength);
}

}  // namespace

// HRESULT CreateHeap(const D3D12_HEAP_DESC*, iid, out) (slot 28): memory from XMemAlloc as for committed resources.
HRESULT WINAPI create_heap(void*, const D3D12_HEAP_DESC* desc, REFIID, void** out) {
    if (!desc || !desc->SizeInBytes) return E_INVALIDARG;
    if (!out) return S_FALSE;
    uint64_t bytes = (desc->SizeInBytes + uma::kPage - 1) & ~uint64_t{uma::kPage - 1};
    auto va = reinterpret_cast<uintptr_t>(shims::xmem_alloc(bytes, kResourceAttributes));
    if (!va) return E_OUTOFMEMORY;
    static std::atomic<int> logged{0};
    if (logged++ < 8)
        std::fprintf(stderr, "[rt] d3d12: CreateHeap(%llu KiB, type %u, flags %#x) at %p\n",
                     static_cast<unsigned long long>(bytes >> 10), desc->Properties.Type, desc->Flags,
                     reinterpret_cast<void*>(va));
    *out = new Heap{{memory_heap_table(), 0, 1}, *desc, va};
    return S_OK;
}

// HRESULT CreatePlacedResource(heap, offset, desc, state, clear, iid, out) (slot 29).
HRESULT WINAPI create_placed_resource(void*, void* heap, uint64_t offset, const gfx10::ResourceDesc* desc,
                                      D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE* clear, REFIID, void** out) {
    Heap* h = as_heap(heap);
    if (!h || !desc) return E_INVALIDARG;
    if (!out) return S_FALSE;
    ResourceDesc1 d{};
    d.desc = *desc;
    uint64_t bytes = offset < h->desc.SizeInBytes ? h->desc.SizeInBytes - offset : 0;
    const char* was = texture_source("CreatePlacedResource (slot 29)");
    *out = resource_on(h->va + offset, bytes, d, h->desc.Properties, h->desc.Flags, clear);
    texture_source(was);
    return S_OK;
}

// Slot 133: a resource at an address the title mapped itself (address, desc1, initial layout, clear, iid, out).
HRESULT WINAPI create_resource_at(void*, uint64_t address, const ResourceDesc1* desc, UINT, const D3D12_CLEAR_VALUE* clear,
                                  REFIID, void** out) {
    if (!desc || !address) return E_INVALIDARG;
    if (!out) return S_FALSE;
    if (!uma::contains(address)) note("a resource at an address outside the GPU-visible range");
    static std::atomic<int> logged{0};
    if (logged++ < 4) {
        uint64_t q[6];
        std::memcpy(q, desc, sizeof q);
        std::fprintf(stderr, "[rt] d3d12: resource at %p (slot 133): desc %llx %llx %llx %llx %llx %llx\n",
                     reinterpret_cast<void*>(address), q[0], q[1], q[2], q[3], q[4], q[5]);
    }
    const char* was = texture_source("resources at an address (slot 133)");
    const gfx10::ResourceDesc& d = desc->desc;
    if (d.dimension != 1 && !(d.flags & 7) && d.samples <= 1) {
        // a texture the title streams: its PC texture follows what is mapped (d3d12x_views.cpp)
        auto* r = new Resource{{resource_table(), 0, 1}, *desc, address, 0, {D3D12_HEAP_TYPE_DEFAULT},
                               D3D12_HEAP_FLAG_NONE, nullptr};
        adopt_streamed(r);
        *out = r;
    } else {
        *out = resource_on(address, 0, *desc, {D3D12_HEAP_TYPE_DEFAULT}, D3D12_HEAP_FLAG_NONE, clear);
    }
    texture_source(was);
    return S_OK;
}

// Slots 74 / 75: (address, page count, handle out) / (handle).
HRESULT WINAPI register_page_pool(void*, uint64_t address, uint64_t pages, uint64_t* handle) {
    std::lock_guard lock(g_pools_lock);
    g_pools.push_back({address, pages});
    if (handle) *handle = g_pools.size();
    std::fprintf(stderr, "[rt] d3d12: page pool %zu: %llu pages at %p\n", g_pools.size(),
                 static_cast<unsigned long long>(pages), reinterpret_cast<void*>(address));
    return S_OK;
}
HRESULT WINAPI unregister_page_pool(void*, uint64_t handle) {
    std::lock_guard lock(g_pools_lock);
    if (handle && handle <= g_pools.size()) g_pools[handle - 1] = {0, 0};
    return S_OK;
}

// HRESULT CreateQueryHeap(desc, iid, out) (slot 39): the PC's query heap when the type is the PC's.
HRESULT WINAPI create_query_heap(void*, const D3D12_QUERY_HEAP_DESC* desc, REFIID, void** out) {
    if (!desc) return E_INVALIDARG;
    if (!out) return S_FALSE;
    ID3D12QueryHeap* host = nullptr;
    D3D12_QUERY_HEAP_DESC d = *desc;
    d.NodeMask = 0;
    if (desc->Type <= D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS && host_device())
        host_device()->CreateQueryHeap(&d, IID_PPV_ARGS(&host));
    if (!host) note("a query heap the PC doesn't make (a console type?)");
    *out = new QueryHeap{{query_heap_table(), 0, 1}, *desc, host};
    return S_OK;
}

// HRESULT CreateCommandSignature(desc, root signature, iid, out) (slot 41): kept for ExecuteIndirect.
HRESULT WINAPI create_command_signature(void*, const D3D12_COMMAND_SIGNATURE_DESC* desc, void* root, REFIID,
                                        void** out) {
    if (!desc) return E_INVALIDARG;
    if (!out) return S_FALSE;
    auto* s = new CommandSignature{{command_signature_table(), 0, 1}, desc->ByteStride, {}, as_root_signature(root)};
    if (desc->pArgumentDescs) s->arguments.assign(desc->pArgumentDescs, desc->pArgumentDescs + desc->NumArgumentDescs);
    *out = s;
    return S_OK;
}

// HRESULT CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC*, iid, out) (slot 86).
HRESULT WINAPI create_pipeline_stream(void*, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID, void** out) {
    if (!desc || !desc->pPipelineStateSubobjectStream) return E_INVALIDARG;
    if (!out) return S_FALSE;
    auto* p = new PipelineState{{pipeline_table(), 0, 1}, false, nullptr, {}};
    auto* s = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    static std::atomic<int> dumped{0};
    if (dumped++ < 2) {
        std::fprintf(stderr, "[rt] d3d12: pipeline stream of %zu bytes:", static_cast<size_t>(desc->SizeInBytes));
        for (size_t i = 0; i < desc->SizeInBytes && i < 256; i += 8) {
            uint64_t q;
            std::memcpy(&q, s + i, 8);
            std::fprintf(stderr, "%s%llx", i % 64 ? " " : "\n[rt]   ", static_cast<unsigned long long>(q));
        }
        std::fprintf(stderr, "\n");
    }
    size_t at = 0;
    while (at + 4 <= desc->SizeInBytes) {
        uint32_t type;
        std::memcpy(&type, s + at, 4);
        Subobject so;
        if (!subobject(type, &so)) {
            std::fprintf(stderr, "[rt] d3d12: pipeline stream: subobject type %u isn't known; the rest is skipped\n", type);
            break;
        }
        const uint8_t* value = s + at + (so.pointer ? 8 : 4);
        switch (type) {
            case 0: {
                void* root;
                std::memcpy(&root, value, 8);
                p->root = as_root_signature(root);
                break;
            }
            case 1: p->code[0] = code_of(value); break;  // VS
            case 2: p->code[1] = code_of(value); break;  // PS
            case 3: p->code[2] = code_of(value); break;  // DS
            case 4: p->code[3] = code_of(value); break;  // HS
            case 5: p->code[4] = code_of(value); break;  // GS
            case 6: p->code[0] = code_of(value); p->compute = true; break;
            case 24: p->code[3] = code_of(value); break;  // AS (in the HS place)
            case 25: p->code[0] = code_of(value); break;  // MS (in the VS place)
            case 12: {  // the input layout: D3D12_INPUT_LAYOUT_DESC, its elements copied (their names too)
                D3D12_INPUT_LAYOUT_DESC il;
                std::memcpy(&il, value, sizeof il);
                for (UINT i = 0; il.pInputElementDescs && i < il.NumElements && i < 32; ++i) {
                    const D3D12_INPUT_ELEMENT_DESC& e = il.pInputElementDescs[i];
                    gpu::InputElement x;
                    x.semantic = e.SemanticName ? e.SemanticName : "";
                    x.index = e.SemanticIndex, x.format = e.Format, x.slot = e.InputSlot;
                    x.offset = e.AlignedByteOffset;
                    x.per_instance = e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                    x.step = e.InstanceDataStepRate;
                    p->input.push_back(x);
                }
                break;
            }
            default: break;
        }
        at += ((so.pointer ? 8 : 4) + so.bytes + 7) & ~size_t{7};
    }
    p->stream.assign(s, s + desc->SizeInBytes);
    attach_container_shaders(*p);
    static std::atomic<int> shown{0};
    for (int k = 0; k < 2 && shown < 4; ++k)
        if (const auto b = p->code[k].bytes(); b && b->size() >= 4) {
            ++shown;
            std::fprintf(stderr, "[rt] d3d12: stream pipeline %s: %zu bytes, starts %.4s\n", k ? "PS" : "VS",
                         b->size(), reinterpret_cast<const char*>(b->data()));
        }
    *out = p;
    return S_OK;
}

// void CreateSamplerX(const console sampler desc (27 dwords)*, dest) (slot 61): filters (1 mag, 2 min, 3 mip; 0 point,
// 1 linear, 2-3 anisotropic), 5 reduction mode, 6-8 addressing, 9-11 min / max LOD and bias, 13 anisotropy,
// 16 comparison, 17-20 border colour (facts as VIctory records them).
void WINAPI create_sampler_x(void* device, const uint32_t* w, uint64_t dest) {
    if (!w) return create_sampler(device, nullptr, dest);
    auto f = [&](int i) {
        float v;
        std::memcpy(&v, &w[i], 4);
        return v;
    };
    bool aniso = w[1] >= 2 || w[2] >= 2;
    bool compares = w[16] >= 2 && w[16] <= 7;
    uint32_t reduction = w[5] == 1 ? 2 : w[5] == 2 ? 3 : compares ? 1 : 0;
    auto linear = [](uint32_t v) { return v == 1 || v == 3 ? 1u : 0u; };
    uint32_t filter = aniso ? 0x55 : (linear(w[2]) << 4 | linear(w[1]) << 2 | (w[3] == 2 ? 1u : 0u));
    D3D12_SAMPLER_DESC d{};
    d.Filter = static_cast<D3D12_FILTER>(reduction << 7 | filter);
    d.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(w[6] ? w[6] : 1);
    d.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(w[7] ? w[7] : 1);
    d.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(w[8] ? w[8] : 1);
    d.MinLOD = f(9);
    d.MaxLOD = w[3] == 0 ? f(9) : f(10);
    d.MipLODBias = f(11);
    d.MaxAnisotropy = aniso ? (w[13] < 1 ? 1 : w[13] > 16 ? 16 : w[13]) : 1;
    d.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(w[16] ? w[16] : 1);
    for (int i = 0; i < 4; ++i) d.BorderColor[i] = f(17 + i);
    create_sampler(device, &d, dest);
}

// void CopyDescriptorsSimple(count, dest, src, type) (slot 24): descriptors are memory; so are our RTV / DSV tags.
void WINAPI copy_descriptors_simple(void*, UINT count, uint64_t dest, uint64_t src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (!dest || !src || !count) return;
    size_t step = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 16 : 32;
    std::memmove(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), count * step);
}

// void CopyDescriptors(dest ranges, dest starts, dest sizes, src ranges, src starts, src sizes, type) (slot 23).
void WINAPI copy_descriptors(void*, UINT dest_ranges, const uint64_t* dest_starts, const UINT* dest_sizes,
                             UINT src_ranges, const uint64_t* src_starts, const UINT* src_sizes,
                             D3D12_DESCRIPTOR_HEAP_TYPE type) {
    size_t step = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 16 : 32;
    UINT di = 0, si = 0, doff = 0, soff = 0;
    while (di < dest_ranges && si < src_ranges) {
        UINT dn = dest_sizes ? dest_sizes[di] : 1, sn = src_sizes ? src_sizes[si] : 1;
        UINT n = std::min(dn - doff, sn - soff);
        std::memmove(reinterpret_cast<void*>(dest_starts[di] + doff * step),
                     reinterpret_cast<const void*>(src_starts[si] + soff * step), n * step);
        doff += n, soff += n;
        if (doff == dn) ++di, doff = 0;
        if (soff == sn) ++si, soff = 0;
    }
}

// Slots 57 and 67 (console): more ways to make a pipeline, (desc, iid, out). 1.0.2.1 makes ten thousand pipelines
// through 57 at start (its desc starts with the root signature). Their descs aren't decoded yet: the pipeline keeps
// the first 512 bytes, and the first few are logged.
bool readable(uint64_t ptr, uint64_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    return ptr && size && VirtualQuery(reinterpret_cast<void*>(ptr), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           ptr + size <= reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// A pipeline the title made the PC's way, from shader containers (a stream, slot 86; a graphics or compute desc,
// slots 10, 11 and 62). The stages are console code in DXBC containers ("DXIL" without bitcode: XDAT -> FSSM), as
// in shaders.rpf: each takes its translation from the cache by its HASH digest, or is saved for the translator (the
// pipeline can't be used until then). There is no stage metadata on this path: the root parameters go to the user
// SGPRs in order (from_stream). The compute ones made through slots 11 and 62 were handed to the PC as they came
// (80070057): 24,922 of 32,512 dispatches with no PC pipeline in one run of the open world were theirs. (The
// VIctory project refuses the title's slots 57 and 67, and the title then makes every pipeline this way.)
// The thread-group size a translated compute shader was compiled with, from its DXIL container's PSV0 part: the
// part's data is a dword with the runtime info's size, then the info, and from its version 2 (48 bytes) on the
// three sizes are the dwords at +36 of it. False if the container has none.
bool dxil_group_size(const std::vector<uint8_t>& dxil, UINT size[3]) {
    if (dxil.size() < 32 || std::memcmp(dxil.data(), "DXBC", 4) != 0) return false;
    uint32_t parts;
    std::memcpy(&parts, dxil.data() + 28, 4);
    for (uint32_t k = 0; k < parts && 32 + 4 * k + 4 <= dxil.size(); ++k) {
        uint32_t at, bytes, info;
        std::memcpy(&at, dxil.data() + 32 + 4 * k, 4);
        if (at + 12 > dxil.size() || std::memcmp(dxil.data() + at, "PSV0", 4) != 0) continue;
        std::memcpy(&bytes, dxil.data() + at + 4, 4);
        std::memcpy(&info, dxil.data() + at + 8, 4);
        if (info < 48 || bytes < 4 + info || at + 12 + 48 > dxil.size()) return false;
        std::memcpy(size, dxil.data() + at + 12 + 36, 12);
        return size[0] && size[1] && size[2];
    }
    return false;
}

std::shared_ptr<const std::vector<uint8_t>> Code::bytes() const {
    if (!cached_) return bytes_;
    std::vector<uint8_t> dxil;
    int kind = 0;
    if (!shader_cache::find(digest_, dxil, kind)) return nullptr;
    return std::make_shared<const std::vector<uint8_t>>(std::move(dxil));
}

std::atomic<uint32_t> g_container_pipelines{0}, g_container_cached{0};
void attach_container_shaders(PipelineState& p) {
    p.from_stream = true;
    bool all = true;
    for (int k = 0; k < 2; ++k) {
        uint8_t digest[16];
        const auto title_code = p.code[k].bytes();
        if (!title_code || !shader_cache::container_digest(title_code->data(), title_code->size(), digest)) continue;
        std::memcpy(p.meta[k].digest, digest, 16);
        p.meta[k].has_digest = true;
        std::vector<uint8_t> dxil;
        int kind;
        if (shader_cache::find(digest, dxil, kind)) {
            if (k == 0 && p.compute) dxil_group_size(dxil, p.cs_group);
            p.code[k].set_cached(digest, dxil.size());
            if (k == 0) shader_cache::vs_inputs(digest, p.vs_in);
        } else {
            shader_cache::save_pending(digest, title_code->data(), title_code->size());
            std::array<uint8_t, 16> m;
            std::memcpy(m.data(), digest, 16);
            p.missing.push_back(m);
            shader_cache::note_missing(digest);
            all = false;
        }
    }
    if (!all) p.code[0].clear();  // nothing to draw or dispatch with yet
    const uint32_t n = ++g_container_pipelines, c = all ? ++g_container_cached : g_container_cached.load();
    if ((n & (n - 1)) == 0)
        std::fprintf(stderr, "[rt] shaders: %u pipelines made from shader containers, %u with every stage in the cache\n",
                     n, c);
}

// Pipelines made, stages named by a digest, stages found in the translated-shader cache (logged as they grow).
std::atomic<uint32_t> g_pipelines{0}, g_stages{0}, g_cached{0};

// A slot-57 pipeline's stages: (GPU code, metadata) pairs from +40, 32 bytes apart; each metadata block ends with the
// HASH digest of its shaders.rpf shader. The DXIL found for it goes to code[] by program kind.
void attach_cached_shaders(const uint8_t* desc, PipelineState& p) {
    uint32_t stages = 0, cached = 0;
    for (int at = 56; at <= 56 + 32 * 4; at += 32) {
        uint64_t ptr, size;
        std::memcpy(&ptr, desc + at, 8);
        std::memcpy(&size, desc + at + 8, 8);
        // a mesh stage's block is larger than the others' (its stage data alone is 0x45c bytes): up to 64 KiB
        // a pipeline doesn't have every stage: a mesh pipeline has the pixel stage in the first pair, nothing in the
        // second and the mesh stage in the third. An empty pair is passed over, not the end of the list
        if (!ptr || !size) continue;
        if (size > 65536 || !readable(ptr, size)) break;
        uint8_t digest[16];
        const bool named = shader_cache::digest_of(reinterpret_cast<const uint8_t*>(ptr), size, digest);
        if (size > 4096) {
            static std::atomic<int> told{0};
            if (told++ < 6)
                std::fprintf(stderr, "[rt] shaders: a stage block of %llu bytes at desc +%d: %s\n",
                             static_cast<unsigned long long>(size), at, named ? "named by a digest" : "no digest found");
        }
        if (!named) continue;
        ++stages;
        gpu::StageMeta meta;
        const bool parsed = gpu::parse_stage(reinterpret_cast<const uint8_t*>(ptr), size, meta);
        std::memcpy(meta.digest, digest, 16);  // the block's own digest: what the logs name the shader by
        meta.has_digest = true;
        {
            // RT_NO_WIDE_STAGE=1 (to tell what the stages with the longer header change): their bindings are dropped,
            // as when the longer header wasn't read
            static const bool drop = [] {
                char v[4];
                return GetEnvironmentVariableA("RT_NO_WIDE_STAGE", v, sizeof v) && v[0] == '1';
            }();
            uint32_t first = 0;
            std::memcpy(&first, reinterpret_cast<const void*>(ptr), 4);
            if (drop && first == 0x403) meta.bindings.clear();
        }
        // what a stage block gave, the first few of each pair: a stage whose bindings aren't read gets no user data
        // (every table address and root constant zero), and then reads nothing of the title's
        {
            static std::atomic<int> told[5];
            const int pair = (at - 56) / 32;
            if (pair >= 0 && pair < 5 && told[pair]++ < 3) {
                const auto* b = reinterpret_cast<const uint8_t*>(ptr);
                std::fprintf(stderr, "[rt] shaders: stage block at desc +%d: %llu bytes, %s, %zu binding(s); starts", at,
                             static_cast<unsigned long long>(size), parsed ? "read" : "NOT read as a stage block",
                             meta.bindings.size());
                for (size_t i = 0; i + 4 <= size && i < 64; i += 4) {
                    uint32_t w;
                    std::memcpy(&w, b + i, 4);
                    std::fprintf(stderr, " %x", w);
                }
                std::fprintf(stderr, "\n");
            }
        }
        std::vector<uint8_t> dxil;
        int kind;
        const bool found = shader_cache::find(digest, dxil, kind);
        if (size > 4096) {
            static std::atomic<int> told{0};
            if (told++ < 16) {
                std::fprintf(stderr, "[rt] shaders: the large stage block at desc +%d is ", at);
                for (uint8_t b : digest) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, ": %s (DXIL kind %d)\n", found ? "in the cache" : "not in the cache", found ? kind : -1);
            }
        }
        if (!found) {
            std::array<uint8_t, 16> m;
            std::memcpy(m.data(), digest, 16);
            p.missing.push_back(m);
            shader_cache::note_missing(digest);
            if (g_stages - g_cached < 8) {
                std::fprintf(stderr, "[rt] shaders: not in the cache: ");
                for (uint8_t b : digest) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, "\n");
            }
            continue;
        }
        // DXIL kinds: 0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute -> code[] VS, PS, DS, HS, GS
        static constexpr int kSlot[6] = {1, 0, 4, 3, 2, 0};
        if (kind == 13) {  // a mesh shader: the pipeline's first stage, in the vertex shader's place
            p.mesh = true;
            p.code[0].set_cached(digest, dxil.size());
            p.meta[0] = std::move(meta);
            ++cached;
            continue;
        }
        if (kind > 5) continue;
        if (kind == 5) {
            p.compute = true;
            dxil_group_size(dxil, p.cs_group);
        }
        p.code[kSlot[kind]].set_cached(digest, dxil.size());
        p.meta[kSlot[kind]] = std::move(meta);
        if (kind == 1) shader_cache::vs_inputs(digest, p.vs_in);
        ++cached;
    }
    if (!stages) {  // no stage found where the five pairs are: what the desc holds (the first few)
        static std::atomic<int> told{0};
        if (told++ < 4) {
            std::fprintf(stderr, "[rt] shaders: a pipeline with no stage named; its desc:");
            for (int i = 0; i < 48; ++i) {
                uint64_t w;
                std::memcpy(&w, desc + 8 * i, 8);
                std::fprintf(stderr, "%s%llx", i % 8 ? " " : "\n[rt] shaders:   ", static_cast<unsigned long long>(w));
            }
            std::fprintf(stderr, "\n");
        }
    }
    // the input layout block: the pair at +24
    uint64_t ptr, size;
    std::memcpy(&ptr, desc + 24, 8);
    std::memcpy(&size, desc + 32, 8);
    if (size && size <= 4096 && readable(ptr, size))
        gpu::parse_input_layout(reinterpret_cast<const uint8_t*>(ptr), size, p.input);
    uint32_t n = ++g_pipelines;
    uint32_t s = g_stages += stages, c = g_cached += cached;
    if ((n & (n - 1)) == 0 || n % 2048 == 0)
        std::fprintf(stderr, "[rt] shaders: %u pipelines, %u stages named, %u found in the cache\n", n, s, c);
}

HRESULT make_pipeline_from(const uint8_t* desc, void** out, int slot) {
    if (!out) return S_FALSE;
    auto* p = new PipelineState{{pipeline_table(), 0, 1}, false, nullptr, {}};
    if (desc) {
        void* root;
        std::memcpy(&root, desc, 8);
        p->root = as_root_signature(root);
        p->stream.assign(desc, desc + 512);
    }
    static std::atomic<int> logged[2]{};
    if (desc && logged[slot == 67]++ < 2) {
        std::fprintf(stderr, "[rt] d3d12: pipeline (slot %d) desc:", slot);
        for (int i = 0; i < 160; i += 8) {
            uint64_t q;
            std::memcpy(&q, desc + i, 8);
            std::fprintf(stderr, "%s%llx", i % 64 ? " " : "\n[rt]   ", static_cast<unsigned long long>(q));
        }
        std::fprintf(stderr, "\n");
        // what the (pointer, size) pairs after the root signature point at: 16 bytes of each
        for (int i = 1; i + 1 < 14; i += 2) {
            uint64_t ptr, size;
            std::memcpy(&ptr, desc + 8 * i, 8);
            std::memcpy(&size, desc + 8 * (i + 1), 8);
            if (!ptr || size > (1u << 24)) break;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(ptr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            auto* b = reinterpret_cast<const uint8_t*>(ptr);
            std::fprintf(stderr, "[rt]   +%d: %llx bytes at %llx:", 8 * i, static_cast<unsigned long long>(size),
                         static_cast<unsigned long long>(ptr));
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %02x", b[k]);
            std::fprintf(stderr, " |%.4s|\n", reinterpret_cast<const char*>(b));
        }
    }
    if (slot == 67) p->compute = true;  // the console's CreateComputePipelineState, translated or not
    if (desc && (slot == 57 || slot == 67)) {  // 67: compute pipelines, the same layout (code +40, metadata +56)
        uint64_t ptr, size;  // the state block at +8, kept for the frame diagnostics
        std::memcpy(&ptr, desc + 8, 8);
        std::memcpy(&size, desc + 16, 8);
        if (size && size <= 0x400 && readable(ptr, size))
            p->state.assign(reinterpret_cast<const uint8_t*>(ptr), reinterpret_cast<const uint8_t*>(ptr) + size);
        uint64_t mptr, msize;  // compute: the register block saved by the shader's digest, for the translator
        std::memcpy(&mptr, desc + 56, 8);
        std::memcpy(&msize, desc + 64, 8);
        uint8_t digest[16];
        // (a stage block can be larger than 4 KiB: the raytracing-resolve compute stages' are about 5.5 KiB)
        if (slot == 67 && !p->state.empty() && msize <= 65536 && readable(mptr, msize) &&
            shader_cache::digest_of(reinterpret_cast<const uint8_t*>(mptr), msize, digest))
            shader_cache::save_compute_regs(digest, p->state.data(), p->state.size());
        attach_cached_shaders(desc, *p);
        pso_trial(*p);
    }
    *out = p;
    return S_OK;
}
// RT_DUMP_SHADER_CODE=<dir>: the blocks of the first slot-57 pipelines (metadata and GPU code: the (address, size)
// pairs from +8) and their root signatures, as p<NN>_<offset>.bin and p<NN>_root.txt.
void dump_code(const uint8_t* desc) {
    static char dir[MAX_PATH];
    static std::atomic<int> n{0};
    if (!dir[0] && !GetEnvironmentVariableA("RT_DUMP_SHADER_CODE", dir, MAX_PATH)) return;
    const int idx = n++;                    // pipelines are made on several threads: one number per pipeline
    if (idx >= 64) return;
    // the pipeline's root signature as text, to read the metadata's user-data records against
    void* rootp;
    std::memcpy(&rootp, desc, 8);
    if (RootSignature* rs = as_root_signature(rootp); rs && rs->desc) {
        char name[MAX_PATH];
        std::snprintf(name, sizeof name, "%s/p%02d_root.txt", dir, idx);
        if (FILE* f = std::fopen(name, "w")) {
            const D3D12_ROOT_SIGNATURE_DESC1& d = *rs->desc;
            std::fprintf(f, "flags %x, %u parameters, %u static samplers\n", d.Flags, d.NumParameters,
                         d.NumStaticSamplers);
            for (UINT i = 0; i < d.NumParameters; ++i) {
                const D3D12_ROOT_PARAMETER1& p = d.pParameters[i];
                std::fprintf(f, "%2u: type %d vis %d", i, p.ParameterType, p.ShaderVisibility);
                if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
                    std::fprintf(f, " constants b%u space%u x%u", p.Constants.ShaderRegister, p.Constants.RegisterSpace,
                                 p.Constants.Num32BitValues);
                else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                    for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                        const auto& g = p.DescriptorTable.pDescriptorRanges[r];
                        std::fprintf(f, " [range type %d n %d base %u space %u off %u]", g.RangeType,
                                     (int)g.NumDescriptors, g.BaseShaderRegister, g.RegisterSpace,
                                     g.OffsetInDescriptorsFromTableStart);
                    }
                else
                    std::fprintf(f, " descriptor reg %u space %u", p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
    }
    for (int at : {8, 24, 40, 56, 72, 88}) {
        uint64_t ptr, size;
        std::memcpy(&ptr, desc + at, 8);
        std::memcpy(&size, desc + at + 8, 8);
        MEMORY_BASIC_INFORMATION mbi{};
        if (!ptr || !size || size > (1u << 20) || !VirtualQuery(reinterpret_cast<void*>(ptr), &mbi, sizeof mbi) ||
            mbi.State != MEM_COMMIT)
            continue;
        char name[MAX_PATH];
        std::snprintf(name, sizeof name, "%s/p%02d_%d.bin", dir, idx, at);
        if (FILE* f = std::fopen(name, "wb")) {
            std::fwrite(reinterpret_cast<const void*>(ptr), 1, size, f);
            std::fclose(f);
        }
    }
}

HRESULT WINAPI create_pipeline_57(void*, const uint8_t* desc, REFIID, void** out) {
    if (desc) dump_code(desc);
    return make_pipeline_from(desc, out, 57);
}
HRESULT WINAPI create_pipeline_67(void*, const uint8_t* desc, REFIID, void** out) {
    return make_pipeline_from(desc, out, 67);
}

// Slot 55 (console): a pipeline derived from another, (base pipeline, count, entries, iid, out): the base's shaders
// with fixed-function state replaced by the entries, each 332 bytes: a type, then the state (blend, rasterizer,
// depth-stencil and so on, the largest being a 328-byte blend desc). 1.0.2.1 caches the objects by state and sets
// them with SetPipelineState in place of the base (list_pipeline). The entries are kept as given; not applied yet.
struct StateBlock {
    Header header;
    void* first;
    std::vector<uint8_t> entries;
};
const std::vector<uint8_t>* state_block_entries(void* block) {
    return block && is_object(block, state_block_table()) ? &static_cast<StateBlock*>(block)->entries : nullptr;
}

void* state_block_first(void* block) {
    return block && is_object(block, state_block_table()) ? static_cast<StateBlock*>(block)->first : nullptr;
}

void** state_block_table() {
    static void** vt = shims::make_probe_vtable("D3D12StateBlock");
    return vt;
}

HRESULT WINAPI create_state_block(void*, void* first, UINT count, const uint8_t* entries, REFIID, void** out) {
    if (!out) return S_FALSE;
    auto* b = new StateBlock{{state_block_table(), 0, 1}, first, {}};
    if (entries && count) b->entries.assign(entries, entries + size_t{count} * 332);
    static std::atomic<int> logged{0};
    if (logged++ < 4) {
        uint32_t types[8] = {};
        for (UINT i = 0; i < count && i < 8; ++i) std::memcpy(&types[i], entries + i * 332, 4);
        std::fprintf(stderr, "[rt] d3d12: state block %p (first %p) of %u entries (types %u %u %u %u ...)\n",
                     static_cast<void*>(b), first, count, types[0], types[1], types[2], types[3]);
    }
    // what the first argument is, by kind, the first few of each (a pipeline the block is made from?)
    {
        const bool pointer = reinterpret_cast<uintptr_t>(first) > 0x10000 && !IsBadReadPtr(first, 16);
        const PipelineState* p = pointer ? as_pipeline(first) : nullptr;
        const char* name = !first ? "null" : !pointer ? "not a pointer"
                           : shims::probe_object_name(first) ? shims::probe_object_name(first) : "unknown";
        const int kind = !first ? 0 : p ? (p->compute ? 2 : 1) : 3;
        static std::atomic<int> shown[4];
        if (shown[kind]++ < 3)
            std::fprintf(stderr, "[rt] d3d12: state block %p made from %p: %s%s, %u entries\n", static_cast<void*>(b),
                         first, name, kind == 1 ? " (a graphics pipeline)" : kind == 2 ? " (a compute pipeline)" : "",
                         count);
    }
    *out = b;
    return S_OK;
}

// Made at startup: the runtime has no thread-safe statics.
[[maybe_unused]] const bool g_objects_early =
    (memory_heap_table(), query_heap_table(), command_signature_table(), state_block_table(), true);

}  // namespace rt::d3d12x
