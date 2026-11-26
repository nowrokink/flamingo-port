// The upscale the title asks FidelityFX for (ffx.cpp: ffxDispatch, type 0x10001), as a plain resize.
//
// 1.0.2.1 renders the world at the render size (1280x720 with the Series S profile) and hands the lit, pre-exposed
// HDR colour to the upscaler, which writes the image at the display size (1920x1080); everything after it (tone
// mapping, the interface's backdrop, the scene plane PresentX shows) reads that output. With no upscaler the output
// is never written and the scene plane stays as it was cleared. Here one compute dispatch of the runtime's own
// samples the colour with a linear filter into the output: no anti-aliasing, no history, no sharpening, but the
// image the title lit reaches the screen. The dispatch is recorded on the title's list, where the SDK records its own.
#include <windows.h>

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "d3d12x_internal.h"

namespace rt::d3d12x {

namespace {

constexpr UINT kSlots = 64;  // pairs of views (colour, output) a list may still name when the next is made

struct Upscale {
    std::mutex m;
    bool failed = false;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;  // 2 * kSlots views, shader visible
    UINT step = 0;
    UINT next = 0;
};
Upscale g_up;
std::atomic<ID3D12Resource*> g_last[2];
std::atomic<uint64_t> g_done{0}, g_refused{0};

// The typed format a view of `f` reads or writes as (the colour and the output are HDR or display formats).
DXGI_FORMAT typed(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
    }
}

// Compiled with the system's d3dcompiler_47.dll on first use, as the planes' shaders are (the runtime ships none).
bool ready_locked(ID3D12Device* dev) {
    Upscale& u = g_up;
    if (u.pso) return true;
    if (u.failed) return false;
    u.failed = true;
    static const char kSource[] =
        "Texture2D<float4> t : register(t0); RWTexture2D<float4> o : register(u0); SamplerState s : register(s0);\n"
        "cbuffer c : register(b0) { float2 scale; float2 last; }\n"
        "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
        "  o[id.xy] = t.SampleLevel(s, min((id.xy + 0.5) * scale, last), 0); }\n";
    using Compile = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    Compile compile = lib ? reinterpret_cast<Compile>(GetProcAddress(lib, "D3DCompile")) : nullptr;
    ID3DBlob *cs = nullptr, *sig = nullptr;
    if (!compile || FAILED(compile(kSource, sizeof kSource - 1, "upscale", nullptr, nullptr, "cs", "cs_5_0", 0, 0, &cs, nullptr))) {
        std::fprintf(stderr, "[rt] ffx: the resize shader didn't compile (d3dcompiler_47.dll %s)\n", lib ? "loaded" : "missing");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
    D3D12_ROOT_PARAMETER params[3]{};
    for (int i = 0; i < 2; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable = {1, &ranges[i]};
    }
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = {0, 0, 4};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_ROOT_SIGNATURE_DESC rd{3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&u.root))) ||
        FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&u.heap)))) {
        std::fprintf(stderr, "[rt] ffx: no root signature or view heap for the resize\n");
        return false;
    }
    pd.pRootSignature = u.root;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&u.pso)))) {
        u.pso = nullptr;
        std::fprintf(stderr, "[rt] ffx: no pipeline for the resize\n");
        return false;
    }
    u.step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    u.failed = false;
    return true;
}

}  // namespace

ID3D12Resource* last_upscale(int k) { return k == 0 || k == 1 ? g_last[k].load() : nullptr; }

bool upscale(void* list, void* colour, void* output, uint32_t render_w, uint32_t render_h, uint32_t out_w,
             uint32_t out_h) {
    ID3D12GraphicsCommandList* host = host_list_of(list);
    Resource* c = as_resource(colour);
    Resource* o = as_resource(output);
    ID3D12Device* dev = host_device();
    const uint64_t n = g_done.load() + g_refused.load();
    const bool say = (n & (n - 1)) == 0 && n <= 4096;
    if (!host || !c || !o || !c->host || !o->host || !dev) {
        ++g_refused;
        if (say)
            std::fprintf(stderr, "[rt] ffx: not resized: list %s, colour %s, output %s\n", host ? "ours" : "NOT a list of ours",
                         !c ? "NOT a resource of ours" : c->host ? "a PC texture" : "no PC texture",
                         !o ? "NOT a resource of ours" : o->host ? "a PC texture" : "no PC texture");
        return false;
    }
    const D3D12_RESOURCE_DESC cd = c->host->GetDesc(), od = o->host->GetDesc();
    const bool fits = cd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && od.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                      cd.SampleDesc.Count == 1 && !(cd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                      (od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!render_w || render_w > cd.Width) render_w = static_cast<uint32_t>(cd.Width);
    if (!render_h || render_h > cd.Height) render_h = cd.Height;
    if (!out_w || out_w > od.Width) out_w = static_cast<uint32_t>(od.Width);
    if (!out_h || out_h > od.Height) out_h = od.Height;
    if (say)
        std::fprintf(stderr,
                     "[rt] ffx: dispatch %llu: colour %p %llux%u format %d flags %#x (render size %ux%u) -> output %p "
                     "%llux%u format %d flags %#x (display size %ux%u)%s\n",
                     static_cast<unsigned long long>(n + 1), static_cast<void*>(c->host),
                     static_cast<unsigned long long>(cd.Width), cd.Height, static_cast<int>(cd.Format),
                     static_cast<unsigned>(cd.Flags), render_w, render_h, static_cast<void*>(o->host),
                     static_cast<unsigned long long>(od.Width), od.Height, static_cast<int>(od.Format),
                     static_cast<unsigned>(od.Flags), out_w, out_h, fits ? "" : ": NOT resized (the views can't be made)");
    if (!fits) {
        ++g_refused;
        return false;
    }
    UINT slot;
    {
        std::lock_guard lock(g_up.m);
        if (!ready_locked(dev)) {
            ++g_refused;
            return false;
        }
        slot = g_up.next++ % kSlots;
    }
    Upscale& u = g_up;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = u.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T{2} * slot * u.step;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = typed(cd.Format);
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (cd.DepthOrArraySize > 1) {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
    } else {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
    }
    dev->CreateShaderResourceView(c->host, &sd, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = typed(od.Format);
    if (od.DepthOrArraySize > 1) {
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.ArraySize = 1;
    } else {
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }
    cpu.ptr += u.step;
    dev->CreateUnorderedAccessView(o->host, nullptr, &ud, cpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = u.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64{2} * slot * u.step;
    // the textures stay in the common state between the runtime's commands
    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {c->host, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    b[1].Transition = {o->host, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    const bool same = c->host == o->host;  // (never seen; a texture can't be read and written by this pass)
    if (same) {
        ++g_refused;
        return false;
    }
    host->ResourceBarrier(2, b);
    host->SetDescriptorHeaps(1, &u.heap);
    host->SetComputeRootSignature(u.root);
    host->SetPipelineState(u.pso);
    host->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += u.step;
    host->SetComputeRootDescriptorTable(1, gpu);
    // an output texel's centre in the colour's texture coordinates, and the last texel centre of the rendered part
    const float k[4] = {static_cast<float>(render_w) / static_cast<float>(out_w) / static_cast<float>(cd.Width),
                        static_cast<float>(render_h) / static_cast<float>(out_h) / static_cast<float>(cd.Height),
                        (static_cast<float>(render_w) - 0.5f) / static_cast<float>(cd.Width),
                        (static_cast<float>(render_h) - 0.5f) / static_cast<float>(cd.Height)};
    host->SetComputeRoot32BitConstants(2, 4, k, 0);
    host->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    host->ResourceBarrier(2, b);
    g_last[0] = c->host;
    g_last[1] = o->host;
    ++g_done;
    return true;
}

}  // namespace rt::d3d12x
