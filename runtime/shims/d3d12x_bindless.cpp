// The PC side of console descriptors: shader-visible heaps for views and samplers, and the descriptor hash table that
// maps a console T# / S# to its index there (see d3d12x_internal.h). The table's words go to the GPU with the
// command lists that may read it (not yet: command lists aren't replayed).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../gpu/desc_table.h"
#include "d3d12x_internal.h"
#include "gpu_memory.h"

namespace rt::d3d12x {
namespace {

constexpr uint32_t kViews = 1u << 19;  // descriptors; within every tier-2+ GPU's limit for a shader-visible heap
constexpr uint32_t kSamplers = 2048;   // the PC's limit
// The view heap: SRVs at [0, kHalf), the UAV of SRV k at kHalf + k (a translated shader indexes both arrays with
// the same k), the runtime's fixed views at the end.
constexpr uint32_t kHalf = (kViews - kFixedViews) / 2;

struct Heap {
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    uint32_t increment = 0, next = 1, capacity = 0;  // index 0: the fallback
    std::vector<uint32_t> free;

    bool make(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count) {
        D3D12_DESCRIPTOR_HEAP_DESC d{type, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)))) return false;
        cpu = heap->GetCPUDescriptorHandleForHeapStart();
        increment = dev->GetDescriptorHandleIncrementSize(type);
        capacity = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? kHalf : count;
        return true;
    }
    uint32_t take() {
        if (!free.empty()) {
            uint32_t i = free.back();
            free.pop_back();
            return i;
        }
        return next < capacity ? next++ : 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE at(uint32_t i) const { return {cpu.ptr + SIZE_T{i} * increment}; }
    D3D12_CPU_DESCRIPTOR_HANDLE uav_at(uint32_t i) const { return {cpu.ptr + SIZE_T{kHalf + i} * increment}; }
};

struct State {
    std::mutex m;
    bool ready = false, failed = false;
    Heap views, samplers;
    gpu::DescriptorHashTable table{1u << 16};
    // per PC texture: the (T# key, index) pairs bound to it, for unbind_texture
    std::unordered_map<ID3D12Resource*, std::vector<std::pair<gpu::DescKey, uint32_t>>> bound;
};

State g_state;  // a global: the runtime has no thread-safe statics
State& st() { return g_state; }

void null_uav(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_UINT;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(nullptr, nullptr, &d, at);
}

// Made on first use, with the fallbacks at index 0.
bool ready(State& s) {
    if (s.ready || s.failed) return s.ready;
    ID3D12Device* dev = host_device();
    if (!dev || !s.views.make(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViews) ||
        !s.samplers.make(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplers)) {
        std::fprintf(stderr, "[rt] d3d12: the PC's shader-visible descriptor heaps couldn't be made\n");
        s.failed = true;
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC null{};
    null.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dev->CreateShaderResourceView(nullptr, &null, s.views.at(0));
    null_uav(dev, s.views.uav_at(0));
    D3D12_SAMPLER_DESC point{};
    point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    point.AddressU = point.AddressV = point.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point.MaxLOD = D3D12_FLOAT32_MAX;
    dev->CreateSampler(&point, s.samplers.at(0));
    s.ready = true;
    std::fprintf(stderr, "[rt] d3d12: shader-visible heaps: %u views, %u samplers\n", kViews, kSamplers);
    return true;
}

template <typename Make>
uint32_t bind(Heap State::*which, const gpu::DescKey& key, Make make) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (!ready(s)) return 0;
    if (uint32_t known = s.table.lookup(key)) return known;  // the same descriptor again
    Heap& h = s.*which;
    uint32_t i = h.take();
    if (!i) {
        static bool logged = false;
        if (!logged) std::fprintf(stderr, "[rt] d3d12: a shader-visible heap is full\n");
        logged = true;
        return 0;
    }
    make(host_device(), h.at(i));
    s.table.insert(key, i);
    return i;
}

// A format views of a PC texture can use: typeless ones and depth formats as the typed format shaders read.
DXGI_FORMAT readable(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        default: return f;
    }
}
// A view's format on a texture made in a stand-in format (make_host_texture: the console's targets and compute
// outputs in R9G9B9E5, B5G5R5A1 or B4G4R4A4 are RGBA16F or RGBA8 on the PC): the title's view names its own format,
// which the PC texture can't be viewed as, so the view takes the texture's.
DXGI_FORMAT fitted(DXGI_FORMAT view, DXGI_FORMAT texture) {
    const bool console_only = view == DXGI_FORMAT_R9G9B9E5_SHAREDEXP || view == DXGI_FORMAT_B5G5R5A1_UNORM ||
                              view == DXGI_FORMAT_B4G4R4A4_UNORM;
    return console_only && texture != view ? texture : view;
}
DXGI_FORMAT writable(DXGI_FORMAT f) {
    switch (f = readable(f)) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return f;
    }
}

// Views of the PC texture always get an explicit desc in a format it can be viewed as: the title's (made readable),
// else the whole texture. A view the PC can't take must never reach it (an invalid call removes the device).
D3D12_SHADER_RESOURCE_VIEW_DESC srv_for(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC* given) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    if (given) {
        d = *given;
        d.Format = fitted(readable(d.Format ? d.Format : r.Format), r.Format);
        if (!d.Shader4ComponentMapping) d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        // The title's view is of its own texture; the PC one can have fewer mips or slices (a streamed texture whose
        // header grew after the PC texture was made): the ranges are cut to what the PC texture has, and a view of
        // another shape than the texture gives way to the whole texture.
        const UINT mips = r.MipLevels ? r.MipLevels : 1, slices = r.DepthOrArraySize ? r.DepthOrArraySize : 1;
        auto cut_mips = [&](UINT& first, UINT& count) {
            if (first >= mips) first = mips - 1;
            if (count != UINT(-1) && count > mips - first) count = mips - first;
            if (!count) count = 1;
        };
        auto cut_slices = [&](UINT& first, UINT& count, UINT unit) {
            if (first >= slices) first = 0;
            UINT room = (slices - first) / unit;
            if (count == UINT(-1) || count > room) count = room;
        };
        const bool is3d = r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const bool is1d = r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        const bool ms = r.SampleDesc.Count > 1;
        const bool flat = !is3d && !is1d && !ms;  // a plain 2D texture or an array of them
        const bool cube = flat && slices >= 6 && r.Width == r.Height;
        bool fits = false;
        switch (d.ViewDimension) {
            case D3D12_SRV_DIMENSION_TEXTURE1D:
                if ((fits = is1d)) cut_mips(d.Texture1D.MostDetailedMip, d.Texture1D.MipLevels);
                break;
            case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
                if ((fits = is1d)) {
                    cut_mips(d.Texture1DArray.MostDetailedMip, d.Texture1DArray.MipLevels);
                    cut_slices(d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, 1);
                }
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2D:
                if ((fits = flat)) cut_mips(d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels);
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
                if ((fits = flat)) {
                    cut_mips(d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels);
                    cut_slices(d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, 1);
                }
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DMS: fits = !is3d && !is1d && ms; break;
            case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
                if ((fits = !is3d && !is1d && ms))
                    cut_slices(d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize, 1);
                break;
            case D3D12_SRV_DIMENSION_TEXTURE3D:
                if ((fits = is3d)) cut_mips(d.Texture3D.MostDetailedMip, d.Texture3D.MipLevels);
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBE:
                if ((fits = cube)) cut_mips(d.TextureCube.MostDetailedMip, d.TextureCube.MipLevels);
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
                if ((fits = cube)) {
                    cut_mips(d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.MipLevels);
                    cut_slices(d.TextureCubeArray.First2DArrayFace, d.TextureCubeArray.NumCubes, 6);
                    fits = d.TextureCubeArray.NumCubes != 0;
                }
                break;
            default: break;
        }
        if (fits) return d;
        const DXGI_FORMAT format = d.Format;
        const UINT mapping = d.Shader4ComponentMapping;
        d = srv_for(r, nullptr);
        d.Format = format;
        d.Shader4ComponentMapping = mapping;
        return d;
    }
    d.Format = readable(r.Format);
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bool array = r.DepthOrArraySize > 1;
    switch (r.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            d.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE1D;
            d.Texture1DArray = {0, UINT(-1), 0, r.DepthOrArraySize, 0};
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            d.Texture3D = {0, UINT(-1), 0};
            break;
        default:
            if (r.SampleDesc.Count > 1) {
                d.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
                d.Texture2DMSArray = {0, r.DepthOrArraySize};
            } else {
                d.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
                d.Texture2DArray = {0, UINT(-1), 0, r.DepthOrArraySize, 0, 0};
            }
    }
    return d;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC uav_for(const D3D12_RESOURCE_DESC& r, const D3D12_UNORDERED_ACCESS_VIEW_DESC* given) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    if (given) {
        d = *given;
        d.Format = fitted(writable(d.Format ? d.Format : r.Format), r.Format);
        // as for the shader resource views: cut to the PC texture, or the whole texture when the shape differs
        const UINT mips = r.MipLevels ? r.MipLevels : 1, slices = r.DepthOrArraySize ? r.DepthOrArraySize : 1;
        const bool is3d = r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const bool is1d = r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        auto cut = [&](UINT& first, UINT& count) {
            if (first >= slices) first = 0;
            if (count == UINT(-1) || count > slices - first) count = slices - first;
        };
        bool fits = false;
        switch (d.ViewDimension) {
            case D3D12_UAV_DIMENSION_TEXTURE1D: fits = is1d && d.Texture1D.MipSlice < mips; break;
            case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
                if ((fits = is1d && d.Texture1DArray.MipSlice < mips))
                    cut(d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize);
                break;
            case D3D12_UAV_DIMENSION_TEXTURE2D: fits = !is3d && !is1d && d.Texture2D.MipSlice < mips; break;
            case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
                if ((fits = !is3d && !is1d && d.Texture2DArray.MipSlice < mips))
                    cut(d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
                break;
            case D3D12_UAV_DIMENSION_TEXTURE3D:
                if ((fits = is3d && d.Texture3D.MipSlice < mips)) {
                    UINT depth = slices >> d.Texture3D.MipSlice;
                    if (!depth) depth = 1;
                    if (d.Texture3D.FirstWSlice >= depth) d.Texture3D.FirstWSlice = 0;
                    if (d.Texture3D.WSize != UINT(-1) && d.Texture3D.WSize > depth - d.Texture3D.FirstWSlice)
                        d.Texture3D.WSize = UINT(-1);
                }
                break;
            default: break;
        }
        if (fits) return d;
        const DXGI_FORMAT format = d.Format;
        d = uav_for(r, nullptr);
        d.Format = format;
        return d;
    }
    d.Format = writable(r.Format);
    bool array = r.DepthOrArraySize > 1;
    switch (r.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            d.ViewDimension = array ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY : D3D12_UAV_DIMENSION_TEXTURE1D;
            d.Texture1DArray = {0, 0, r.DepthOrArraySize};
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            d.Texture3D = {0, 0, UINT(-1)};
            break;
        default:
            d.ViewDimension = array ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
            d.Texture2DArray = {0, 0, r.DepthOrArraySize, 0};
    }
    return d;
}

}  // namespace

uint32_t bind_texture(const uint32_t t[8], ID3D12Resource* texture, const D3D12_SHADER_RESOURCE_VIEW_DESC* srv,
                      const D3D12_UNORDERED_ACCESS_VIEW_DESC* uav) {
    if (!texture) return 0;
    D3D12_RESOURCE_DESC rd = texture->GetDesc();
    bool can_write = rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    bool can_read = !(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = srv_for(rd, srv);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = uav_for(rd, uav);
    const gpu::DescKey key = gpu::DescriptorHashTable::tsharp_key(t);
    bool made = false;
    const uint32_t index = bind(&State::views, key,
                [&](ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE pair) {
                    D3D12_CPU_DESCRIPTOR_HANDLE second{pair.ptr + SIZE_T{kHalf} * st().views.increment};
                    bool was = FAILED(dev->GetDeviceRemovedReason());
                    if (can_read) dev->CreateShaderResourceView(texture, &sd, pair);
                    else dev->CreateShaderResourceView(nullptr, &sd, pair);
                    bool srv_broke = !was && FAILED(dev->GetDeviceRemovedReason());
                    if (can_write) dev->CreateUnorderedAccessView(texture, nullptr, &ud, second);
                    else null_uav(dev, second);
                    // a view the PC refused removes the device: say which, once
                    if (!was && FAILED(dev->GetDeviceRemovedReason()))
                        std::fprintf(stderr,
                                     "[rt] views: the %s view removed the device: texture dim %d %llux%ux%u mips %u "
                                     "format %d flags %#x samples %u; srv (%s) format %d dim %d words %u %u %u %u %u; "
                                     "uav (%s) format %d dim %d words %u %u %u\n",
                                     srv_broke ? "shader resource" : "unordered access", static_cast<int>(rd.Dimension),
                                     static_cast<unsigned long long>(rd.Width), rd.Height, rd.DepthOrArraySize,
                                     rd.MipLevels, static_cast<int>(rd.Format), static_cast<unsigned>(rd.Flags),
                                     rd.SampleDesc.Count, srv ? "the title's" : "ours", static_cast<int>(sd.Format),
                                     static_cast<int>(sd.ViewDimension), sd.Texture2DArray.MostDetailedMip,
                                     sd.Texture2DArray.MipLevels, sd.Texture2DArray.FirstArraySlice,
                                     sd.Texture2DArray.ArraySize, sd.Texture2DArray.PlaneSlice,
                                     uav ? "the title's" : "ours", static_cast<int>(ud.Format),
                                     static_cast<int>(ud.ViewDimension), ud.Texture2DArray.MipSlice,
                                     ud.Texture2DArray.FirstArraySlice, ud.Texture2DArray.ArraySize);
                    made = true;
                });
    if (made && index) {
        State& s = st();
        std::lock_guard lock(s.m);
        s.bound[texture].push_back({key, index});
    }
    return index;
}

void unbind_texture(ID3D12Resource* texture) {
    State& s = st();
    std::lock_guard lock(s.m);
    auto it = s.bound.find(texture);
    if (it == s.bound.end()) return;
    ID3D12Device* dev = host_device();
    D3D12_SHADER_RESOURCE_VIEW_DESC null{};
    null.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    for (const auto& [key, index] : it->second) {
        if (s.table.lookup(key) != index) continue;  // bound to another texture since
        s.table.remove(key);
        dev->CreateShaderResourceView(nullptr, &null, s.views.at(index));
        null_uav(dev, s.views.uav_at(index));
        s.views.free.push_back(index);
    }
    s.bound.erase(it);
}

uint32_t view_index(const uint32_t t[8]) {
    State& s = st();
    std::lock_guard lock(s.m);
    return s.ready ? s.table.lookup(gpu::DescriptorHashTable::tsharp_key(t)) : 0;
}
uint32_t sampler_index(const uint32_t ss[4]) {
    State& s = st();
    std::lock_guard lock(s.m);
    return s.ready ? s.table.lookup(gpu::DescriptorHashTable::ssharp_key(ss)) : 0;
}

uint32_t bind_sampler(const uint32_t s[4], const D3D12_SAMPLER_DESC& desc) {
    return bind(&State::samplers, gpu::DescriptorHashTable::ssharp_key(s),
                [&](ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE at) { dev->CreateSampler(&desc, at); });
}

// ---- render target views on the PC: a ring of CPU descriptors, made when a command records (clears and target
// bindings read them then)

namespace {
constexpr uint32_t kRtvRing = 4096;
std::mutex g_rtv_lock;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
uint32_t g_rtv_next = 0, g_rtv_increment = 0;
}  // namespace

// The descriptor hash table on the GPU (t2 of translated shaders, gpu_memory.h's kSlotHashTable): a default buffer
// whose changed words are copied in before a submission; made again when the table grows.
namespace {
ID3D12Resource* g_table_gpu = nullptr;
ID3D12Resource* g_table_upload = nullptr;
std::vector<ID3D12Resource*> g_table_old;  // the tables before it grew (see upload_descriptor_table)
uint8_t* g_table_cpu = nullptr;
uint32_t g_table_generation = UINT32_MAX;
bool g_table_readable = false;
}  // namespace

bool upload_descriptor_table(ID3D12GraphicsCommandList* list) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (!ready(s)) return false;
    ID3D12Device* dev = host_device();
    const std::vector<uint32_t>& words = s.table.words();
    const uint64_t bytes = words.size() * 4;
    uint32_t begin = s.table.dirty_begin(), end = s.table.dirty_end();
    if (s.table.generation() != g_table_generation) {
        // The old pair is kept, not released: submissions aren't waited for, and one still on the GPU may read it.
        // The table only grows, and rarely: what stays is less than the table itself.
        if (g_table_gpu) g_table_old.push_back(g_table_gpu);
        if (g_table_upload) g_table_old.push_back(g_table_upload);
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&g_table_gpu))) ||
            FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&g_table_upload)))) {
            g_table_gpu = g_table_upload = nullptr;
            return false;
        }
        D3D12_RANGE none{0, 0};
        g_table_upload->Map(0, &none, reinterpret_cast<void**>(&g_table_cpu));
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.NumElements = static_cast<UINT>(words.size());
        v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        dev->CreateShaderResourceView(g_table_gpu, &v, {s.views.cpu.ptr + SIZE_T{fixed_view_base() +
                                                                                  gpumem::kSlotHashTable} *
                                                                             s.views.increment});
        g_table_generation = s.table.generation();
        g_table_readable = false;
        begin = 0;
        end = static_cast<uint32_t>(words.size());
    }
    if (begin >= end) return true;
    std::memcpy(g_table_cpu + begin * 4ull, words.data() + begin, (end - begin) * 4ull);
    s.table.clear_dirty();
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {g_table_gpu, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST};
    if (g_table_readable) list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(g_table_gpu, begin * 4ull, g_table_upload, begin * 4ull, (end - begin) * 4ull);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    g_table_readable = true;
    return true;
}

bool ensure_heaps() {
    State& s = st();
    std::lock_guard lock(s.m);
    return ready(s);
}
ID3D12DescriptorHeap* view_heap() { return st().ready ? st().views.heap : nullptr; }
ID3D12DescriptorHeap* sampler_heap() { return st().ready ? st().samplers.heap : nullptr; }
uint32_t fixed_view_base() { return kViews - kFixedViews; }
uint32_t uav_view_base() { return kHalf; }
D3D12_CPU_DESCRIPTOR_HANDLE fixed_view(uint32_t slot) {
    ensure_heaps();
    const Heap& h = st().views;
    return {h.cpu.ptr + SIZE_T{fixed_view_base() + slot} * h.increment};
}

bool host_rtv(uint64_t handle, D3D12_CPU_DESCRIPTOR_HANDLE* out, ID3D12Resource** texture, DXGI_FORMAT* format) {
    TargetView t;
    if (!target_view(handle, &t) || t.depth || !t.resource || !t.resource->host) return false;
    ID3D12Device* dev = host_device();
    std::lock_guard lock(g_rtv_lock);
    if (!g_rtv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvRing, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtv_heap)))) return false;
        g_rtv_increment = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    D3D12_RESOURCE_DESC rd = t.resource->host->GetDesc();
    D3D12_RENDER_TARGET_VIEW_DESC v{};
    if (t.has_rtv) {
        v = t.rtv;
        v.Format = fitted(readable(v.Format ? v.Format : rd.Format), rd.Format);
        // the title's view cut to the PC texture (an invalid view removes the device)
        const UINT mips = rd.MipLevels ? rd.MipLevels : 1;
        if (v.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D && v.Texture2D.MipSlice >= mips) v.Texture2D.MipSlice = 0;
        if (v.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
            if (v.Texture2DArray.MipSlice >= mips) v.Texture2DArray.MipSlice = 0;
            if (v.Texture2DArray.FirstArraySlice >= rd.DepthOrArraySize) v.Texture2DArray.FirstArraySlice = 0;
            if (v.Texture2DArray.ArraySize > UINT(rd.DepthOrArraySize) - v.Texture2DArray.FirstArraySlice)
                v.Texture2DArray.ArraySize = rd.DepthOrArraySize - v.Texture2DArray.FirstArraySlice;
        }
    } else {
        v.Format = readable(rd.Format);
        v.ViewDimension = rd.DepthOrArraySize > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (rd.DepthOrArraySize > 1) v.Texture2DArray = {0, 0, rd.DepthOrArraySize, 0};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE at{g_rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                                   SIZE_T{g_rtv_next++ % kRtvRing} * g_rtv_increment};
    bool was = FAILED(dev->GetDeviceRemovedReason());
    dev->CreateRenderTargetView(t.resource->host, &v, at);
    if (!was && FAILED(dev->GetDeviceRemovedReason()))
        std::fprintf(stderr,
                     "[rt] views: a render target view removed the device: texture dim %d %llux%ux%u mips %u format "
                     "%d flags %#x; view (%s) format %d dim %d words %u %u %u\n",
                     static_cast<int>(rd.Dimension), static_cast<unsigned long long>(rd.Width), rd.Height,
                     rd.DepthOrArraySize, rd.MipLevels, static_cast<int>(rd.Format), static_cast<unsigned>(rd.Flags),
                     t.has_rtv ? "the title's" : "ours", static_cast<int>(v.Format), static_cast<int>(v.ViewDimension),
                     v.Texture2DArray.MipSlice, v.Texture2DArray.FirstArraySlice, v.Texture2DArray.ArraySize);
    *out = at;
    *texture = t.resource->host;
    if (format) *format = v.Format;
    return true;
}

// The depth format a PC depth texture is viewed as by a depth-stencil view (the textures are made typeless so that
// shaders can read them too: make_host_texture); unknown for anything that isn't a depth texture's format.
DXGI_FORMAT depth_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

namespace {
constexpr uint32_t kDsvRing = 1024;
ID3D12DescriptorHeap* g_dsv_heap = nullptr;
uint32_t g_dsv_next = 0, g_dsv_increment = 0;
}  // namespace

bool host_dsv(uint64_t handle, D3D12_CPU_DESCRIPTOR_HANDLE* out, ID3D12Resource** texture, DXGI_FORMAT* format,
              bool* read_only) {
    TargetView t;
    if (!target_view(handle, &t) || !t.depth || !t.resource || !t.resource->host) return false;
    const D3D12_RESOURCE_DESC rd = t.resource->host->GetDesc();
    const DXGI_FORMAT f = depth_format(rd.Format);
    if (!(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || f == DXGI_FORMAT_UNKNOWN) return false;
    ID3D12Device* dev = host_device();
    std::lock_guard lock(g_rtv_lock);
    if (!g_dsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvRing, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_dsv_heap)))) return false;
        g_dsv_increment = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    // The title's view cut to the PC texture, in the texture's own depth format (the title's names the console's:
    // the 16-bit depth with stencil formats have no PC counterpart). Its read-only flags are kept: a draw that
    // tests a depth it also samples binds it that way.
    D3D12_DEPTH_STENCIL_VIEW_DESC v{};
    const UINT mips = rd.MipLevels ? rd.MipLevels : 1;
    const bool array = rd.DepthOrArraySize > 1;
    if (t.has_dsv && (t.dsv.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D ||
                      t.dsv.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) && rd.SampleDesc.Count == 1) {
        v = t.dsv;
        if (v.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) {
            if (v.Texture2D.MipSlice >= mips) v.Texture2D.MipSlice = 0;
        } else {
            if (v.Texture2DArray.MipSlice >= mips) v.Texture2DArray.MipSlice = 0;
            if (v.Texture2DArray.FirstArraySlice >= rd.DepthOrArraySize) v.Texture2DArray.FirstArraySlice = 0;
            if (v.Texture2DArray.ArraySize > UINT(rd.DepthOrArraySize) - v.Texture2DArray.FirstArraySlice)
                v.Texture2DArray.ArraySize = rd.DepthOrArraySize - v.Texture2DArray.FirstArraySlice;
        }
    } else if (rd.SampleDesc.Count > 1) {
        v.ViewDimension = array ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DMS;
        if (array) v.Texture2DMSArray = {0, rd.DepthOrArraySize};
    } else {
        v.ViewDimension = array ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_DSV_DIMENSION_TEXTURE2D;
        if (array) v.Texture2DArray = {0, 0, rd.DepthOrArraySize};
    }
    v.Format = f;
    const bool stencil = f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    v.Flags &= D3D12_DSV_FLAG_READ_ONLY_DEPTH | (stencil ? D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_NONE);
    D3D12_CPU_DESCRIPTOR_HANDLE at{g_dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                                   SIZE_T{g_dsv_next++ % kDsvRing} * g_dsv_increment};
    dev->CreateDepthStencilView(t.resource->host, &v, at);
    *out = at;
    *texture = t.resource->host;
    if (format) *format = f;
    // read only: every plane the format has is (a view that writes either plane needs the write state)
    if (read_only)
        *read_only = (v.Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) && (!stencil || (v.Flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL));
    return true;
}

}  // namespace rt::d3d12x
