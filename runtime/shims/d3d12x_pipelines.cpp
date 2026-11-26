// PC pipelines from translated shaders: the root signatures they are compiled against (the bindings of
// tools/rdna2spirv.py --target d3d12) and graphics pipelines for slot-57 pipelines.
//
// Translated shaders' bindings: user data b0 (36 dwords: the stage's user SGPRs, then R's base and the code base),
// the page table u8 and the chunks u9[] (gpu_memory.h), the descriptor hash table t2, bindless arrays each in a space
// of its own: textures t0 in spaces 1 (2D), 4 (2D arrays), 6 (1D), 7 (3D) over the views' SRV half, storage images
// u0 in spaces 5 (2D) and 8 (3D) over their UAV half (r32ui, for atomics), typed storage images u0 in spaces 12-15
// (float4: 2D, 3D, 2D array, 1D) and 16-19 (uint4, the same) over the UAV half too, samplers s0 in spaces 2 and 11 (comparison) over the sampler
// heap.
#include <algorithm>
#include <windows.h>

#include <atomic>
#include <vector>
#include <set>
#include <string>
#include <functional>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

#include "d3d12x_internal.h"
#include "gpu_memory.h"

namespace rt::d3d12x {
namespace {

struct Roots {
    std::mutex m;
    bool tried[3] = {};
    ID3D12RootSignature* root[3] = {};  // graphics, compute, mesh
};
Roots g_roots;  // a global: the runtime has no thread-safe statics

ID3D12RootSignature* make_root(bool compute, bool mesh = false) {
    ID3D12Device* dev = host_device();
    if (!dev || !ensure_heaps()) return nullptr;
    const UINT fixed = fixed_view_base(), uavs = uav_view_base();
    constexpr auto kVolatile = static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
    constexpr UINT kAll = UINT_MAX;  // unbounded
    const D3D12_DESCRIPTOR_RANGE1 views[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 8, 0, kVolatile, fixed + gpumem::kSlotPageTable},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, gpumem::kChunkViews, 9, 0, kVolatile, fixed + gpumem::kSlotChunks},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, kVolatile, fixed + gpumem::kSlotHashTable},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kAll, 0, 1, kVolatile, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kAll, 0, 4, kVolatile, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kAll, 0, 6, kVolatile, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kAll, 0, 7, kVolatile, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 5, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 8, kVolatile, uavs},
        // typed storage images (image_store): float4 in spaces 12-15, uint4 in 16-19 (2D, 3D, 2D array, 1D)
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 12, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 13, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 14, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 15, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 16, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 17, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 18, kVolatile, uavs},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kAll, 0, 19, kVolatile, uavs},
    };
    const D3D12_DESCRIPTOR_RANGE1 samplers[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kAll, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kAll, 0, 11, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0},
    };
    D3D12_ROOT_PARAMETER1 p[4] = {};
    p[0].ParameterType = p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[0].Descriptor.Flags = p[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[0].ShaderVisibility = compute ? D3D12_SHADER_VISIBILITY_ALL
                            : mesh  ? D3D12_SHADER_VISIBILITY_MESH
                                    : D3D12_SHADER_VISIBILITY_VERTEX;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable = {static_cast<UINT>(std::size(views)), views};
    p[3].DescriptorTable = {static_cast<UINT>(std::size(samplers)), samplers};
    // compute has no pixel parameter: keep the indices (tables at 2 and 3) by giving it a harmless pixel-only CBV.
    // Visibility doesn't separate ranges in a compute root signature (the runtime checks overlap across all
    // parameters), so that CBV takes a space no shader uses instead of b0 space 0, which param 0 has.
    if (compute) p[1].Descriptor.RegisterSpace = 99;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{D3D_ROOT_SIGNATURE_VERSION_1_1};
    d.Desc_1_1.NumParameters = 4;
    d.Desc_1_1.pParameters = p;
    d.Desc_1_1.Flags = compute || mesh ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                                       : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&d, &blob, &err);
    ID3D12RootSignature* r = nullptr;
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&r));
    if (FAILED(hr))
        std::fprintf(stderr, "[rt] pipelines: the %s root signature couldn't be made (%08lX)%s%s\n",
                     compute ? "compute" : "graphics", static_cast<unsigned long>(hr), err ? ": " : "",
                     err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    if (blob) blob->Release();
    if (err) err->Release();
    return r;
}

struct PsoKey {
    const PipelineState* p;
    UINT count;
    DXGI_FORMAT rt[8];
    DXGI_FORMAT dsv;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
    const void* block;  // the state block it was set through (its state over the pipeline's), or null
    bool operator<(const PsoKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};
std::mutex g_pso_lock;
std::map<PsoKey, ID3D12PipelineState*> g_psos;

// The title's input layout as the translated vertex shader takes it: an element whose semantic the shader reads as
// input register r becomes TEXCOORD<r>; elements the shader doesn't read are left out. False if the shader reads an
// input the layout doesn't give.
bool host_layout(const PipelineState& p, std::vector<D3D12_INPUT_ELEMENT_DESC>& out) {
    out.clear();
    for (const shader_cache::VsInput& in : p.vs_in) {
        const gpu::InputElement* e = nullptr;
        for (const gpu::InputElement& x : p.input)
            if (_stricmp(x.semantic.c_str(), in.semantic) == 0 && x.index == in.index) e = &x;
        if (!e) return false;
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName = "TEXCOORD";
        d.SemanticIndex = in.reg;
        d.Format = static_cast<DXGI_FORMAT>(e->format);
        d.InputSlot = e->slot;
        d.AlignedByteOffset = e->offset;
        d.InputSlotClass = e->per_instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                           : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = e->step;
        out.push_back(d);
    }
    return true;
}

std::once_flag g_trial_once;
std::atomic<int> g_trial_left{0};
std::atomic<int> g_trial_ok{0}, g_trial_failed{0}, g_trial_layout{0}, g_trial_logged{0};

}  // namespace

ID3D12RootSignature* abi_mesh_root() {
    std::lock_guard lock(g_roots.m);
    if (!g_roots.tried[2]) {
        g_roots.tried[2] = true;
        g_roots.root[2] = make_root(false, true);
    }
    return g_roots.root[2];
}

ID3D12RootSignature* abi_root(bool compute) {
    std::lock_guard lock(g_roots.m);
    if (!g_roots.tried[compute]) {
        g_roots.tried[compute] = true;
        g_roots.root[compute] = make_root(compute);
    }
    return g_roots.root[compute];
}

// ---- blending from the slot-57 pipeline's register block (the 0x184 bytes at +8 of its desc: GFX10 register values)
// +0xCC CB_TARGET_MASK (4 bits a target), +0xD0.. CB_BLEND0..7_CONTROL: color source factor [4:0], color function
// [7:5], color destination factor [12:8], alpha source [20:16], alpha function [23:21], alpha destination [28:24],
// separate alpha [29], enable [30]. Factors (AMD's BlendOp): 0 zero, 1 one, 2 src color, 3 1-src color, 4 src alpha,
// 5 1-src alpha, 6 dst alpha, 7 1-dst alpha, 8 dst color, 9 1-dst color, 10 src alpha saturate, 13 constant color,
// 14 1-constant color, 15 src1 color, 16 1-src1 color, 17 src1 alpha, 18 1-src1 alpha, 19 constant alpha,
// 20 1-constant alpha. Functions: 0 dst + src, 1 src - dst, 2 min, 3 max, 4 dst - src.
//
// The block of a pipeline whose first stage is a mesh shader is 0x54 bytes longer (0x1d8), and these registers are
// that much further in it: read at the graphics offsets, a mesh pipeline's target mask was another register's value
// (0x060201bc in the geometry-buffer pipelines: two of the six targets not written at all, two only in part).
// The other registers used, at the graphics offsets (found by listing the blocks of the pipelines a world load
// draws with, RT_DIAG_STATES=1, next to what each draws into):
//   +0xA0 PA_SU_SC_MODE_CNTL: cull front [0], cull back [1], front face clockwise [2]
//   +0xB8 DB_DEPTH_CONTROL: stencil enable [0], depth enable [1], depth write [2], depth bounds [3], depth
//         function [6:4], back face stencil [7], stencil function [10:8], back face's [22:20]. Functions: 0 never,
//         1 less, 2 equal, 3 less or equal, 4 greater, 5 not equal, 6 greater or equal, 7 always. The world's depth
//         is reversed (its geometry pipelines: 0x007007e7, greater or equal), the shadow maps' isn't (0x007007b6).
//   +0xBC DB_STENCIL_CONTROL: fail [3:0], pass [7:4], depth fail [11:8], then the back face's three. Operations:
//         0 keep, 1 zero, 2 ones, 3 replace with the reference, 4 replace with the operation value, 5 add clamped,
//         6 subtract clamped, 7 invert, 8 add, 9 subtract
//   +0xC0 DB_STENCILREFMASK (+0xC4 the back face's): reference [7:0], read mask [15:8], write mask [23:16],
//         operation value [31:24]
constexpr size_t kTargetMask = 0xCC, kBlendControl = 0xD0, kModeControl = 0xA0, kDepthControl = 0xB8,
                 kStencilControl = 0xBC, kStencilRef = 0xC0;
size_t register_shift(const PipelineState& p) { return p.mesh ? 0x54 : 0; }

D3D12_BLEND blend_factor(uint32_t f, bool alpha) {
    switch (f) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 2: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 3: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 6: return D3D12_BLEND_DEST_ALPHA;
    case 7: return D3D12_BLEND_INV_DEST_ALPHA;
    case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 10: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
    case 13: return D3D12_BLEND_BLEND_FACTOR;
    case 14: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 15: return alpha ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC1_COLOR;
    case 16: return alpha ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC1_COLOR;
    case 17: return D3D12_BLEND_SRC1_ALPHA;
    case 18: return D3D12_BLEND_INV_SRC1_ALPHA;
    case 19: return D3D12_BLEND_BLEND_FACTOR;  // the alpha of the blend factor: D3D12 has no separate one
    case 20: return D3D12_BLEND_INV_BLEND_FACTOR;
    default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP blend_op(uint32_t f) {
    switch (f) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
    }
}

// The title's blend state for `count` targets; false when the pipeline has no register block (defaults stay).
bool title_blend(const PipelineState& p, UINT count, D3D12_BLEND_DESC& out) {
    const size_t shift = register_shift(p);
    if (p.state.size() < shift + kBlendControl + 32) return false;
    uint32_t mask;
    std::memcpy(&mask, p.state.data() + shift + kTargetMask, 4);
    out = {};
    for (UINT i = 0; i < 8; ++i) {
        uint32_t c;
        std::memcpy(&c, p.state.data() + shift + kBlendControl + 4 * i, 4);
        D3D12_RENDER_TARGET_BLEND_DESC& t = out.RenderTarget[i];
        t.RenderTargetWriteMask = static_cast<UINT8>((mask >> (4 * i)) & 0xF);
        t.LogicOp = D3D12_LOGIC_OP_NOOP;
        t.BlendEnable = (c >> 30) & 1;
        bool separate = (c >> 29) & 1;
        uint32_t cs = c & 0x1F, cf = (c >> 5) & 7, cd = (c >> 8) & 0x1F;
        uint32_t as = separate ? (c >> 16) & 0x1F : cs, af = separate ? (c >> 21) & 7 : cf,
                 ad = separate ? (c >> 24) & 0x1F : cd;
        t.SrcBlend = blend_factor(cs, false), t.DestBlend = blend_factor(cd, false), t.BlendOp = blend_op(cf);
        t.SrcBlendAlpha = blend_factor(as, true), t.DestBlendAlpha = blend_factor(ad, true), t.BlendOpAlpha = blend_op(af);
        // D3D12: min / max take factors of one
        if (t.BlendOp == D3D12_BLEND_OP_MIN || t.BlendOp == D3D12_BLEND_OP_MAX) t.SrcBlend = t.DestBlend = D3D12_BLEND_ONE;
        if (t.BlendOpAlpha == D3D12_BLEND_OP_MIN || t.BlendOpAlpha == D3D12_BLEND_OP_MAX)
            t.SrcBlendAlpha = t.DestBlendAlpha = D3D12_BLEND_ONE;
        if (!t.BlendEnable) {  // what the PC validates even when off
            t.SrcBlend = t.SrcBlendAlpha = D3D12_BLEND_ONE;
            t.DestBlend = t.DestBlendAlpha = D3D12_BLEND_ZERO;
            t.BlendOp = t.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        if (i > 0 && i < count && std::memcmp(&t, &out.RenderTarget[0], sizeof t) != 0) out.IndependentBlendEnable = TRUE;
    }
    return true;
}

D3D12_STENCIL_OP stencil_op(uint32_t f) {
    switch (f) {
    case 1: return D3D12_STENCIL_OP_ZERO;
    case 2: case 3: case 4: return D3D12_STENCIL_OP_REPLACE;  // (ones and the operation value: the reference on the PC)
    case 5: return D3D12_STENCIL_OP_INCR_SAT;
    case 6: return D3D12_STENCIL_OP_DECR_SAT;
    case 7: return D3D12_STENCIL_OP_INVERT;
    case 8: return D3D12_STENCIL_OP_INCR;
    case 9: return D3D12_STENCIL_OP_DECR;
    default: return D3D12_STENCIL_OP_KEEP;
    }
}

// The title's depth and stencil state for a pipeline drawn into a depth target of format `dsv`: off when there is
// no depth target or no register block. What is off gets what the PC validates all the same.
D3D12_DEPTH_STENCIL_DESC title_depth(const PipelineState& p, DXGI_FORMAT dsv) {
    D3D12_DEPTH_STENCIL_DESC out{};
    out.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    out.FrontFace = out.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                    D3D12_COMPARISON_FUNC_ALWAYS};
    const size_t shift = register_shift(p);
    if (dsv == DXGI_FORMAT_UNKNOWN || p.state.size() < shift + kStencilRef + 8) return out;
    uint32_t dc, sc, ref;
    std::memcpy(&dc, p.state.data() + shift + kDepthControl, 4);
    std::memcpy(&sc, p.state.data() + shift + kStencilControl, 4);
    std::memcpy(&ref, p.state.data() + shift + kStencilRef, 4);
    if ((dc >> 1) & 1) {
        out.DepthEnable = TRUE;
        out.DepthWriteMask = (dc >> 2) & 1 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        out.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(((dc >> 4) & 7) + 1);
    }
    // Stencil is applied only with a depth test (the world's passes). The pipelines that test stencil alone are the
    // interface's masks (Scaleform: equal to the reference, add one on pass), and with those applied the loading
    // screen's pictures were all rejected: what clears or undoes the masks on the PC isn't right yet. RT_UI_STENCIL=1
    // applies them (to look into it); RT_NO_STENCIL=1 applies none.
    static const int stencil_mode = [] {
        char v[4];
        if (GetEnvironmentVariableA("RT_NO_STENCIL", v, sizeof v) && v[0] == '1') return 0;
        if (GetEnvironmentVariableA("RT_UI_STENCIL", v, sizeof v) && v[0] == '1') return 2;
        return 1;
    }();
    const bool has_stencil = (stencil_mode == 2 || (stencil_mode == 1 && out.DepthEnable)) &&
                             (dsv == DXGI_FORMAT_D24_UNORM_S8_UINT || dsv == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    if ((dc & 1) && has_stencil) {
        out.StencilEnable = TRUE;
        out.StencilReadMask = static_cast<UINT8>(ref >> 8);
        out.StencilWriteMask = static_cast<UINT8>(ref >> 16);
        out.FrontFace = {stencil_op(sc & 0xF), stencil_op((sc >> 8) & 0xF), stencil_op((sc >> 4) & 0xF),
                         static_cast<D3D12_COMPARISON_FUNC>(((dc >> 8) & 7) + 1)};
        out.BackFace = out.FrontFace;
        if ((dc >> 7) & 1)
            out.BackFace = {stencil_op((sc >> 12) & 0xF), stencil_op((sc >> 20) & 0xF), stencil_op((sc >> 16) & 0xF),
                            static_cast<D3D12_COMPARISON_FUNC>(((dc >> 20) & 7) + 1)};
    }
    return out;
}

bool title_uses_depth(const PipelineState& p, DXGI_FORMAT dsv) {
    const D3D12_DEPTH_STENCIL_DESC d = title_depth(p, dsv);
    return d.DepthEnable || d.StencilEnable;
}

UINT title_stencil_ref(const PipelineState& p) {
    const size_t shift = register_shift(p);
    if (p.state.size() < shift + kStencilRef + 4) return 0;
    uint32_t ref;
    std::memcpy(&ref, p.state.data() + shift + kStencilRef, 4);
    return ref & 0xFF;
}

// A state block's blend state over the pipeline's. An entry is a type and then the state as the PC lays it out;
// type 2 is a D3D12_BLEND_DESC (328 bytes: the entry's 332 less the type). Kept only if it reads as one (every
// factor, operation and mask in range); disabled targets get what the PC validates even when off.
std::atomic<int> g_block_blends{0}, g_block_odd{0};
bool block_blend(void* block, UINT count, D3D12_BLEND_DESC& out) {
    const std::vector<uint8_t>* entries = state_block_entries(block);
    if (!entries) return false;
    for (size_t at = 0; at + 332 <= entries->size(); at += 332) {
        uint32_t type;
        std::memcpy(&type, entries->data() + at, 4);
        if (type != 2) continue;
        D3D12_BLEND_DESC b;
        static_assert(sizeof b == 328);
        std::memcpy(&b, entries->data() + at + 4, sizeof b);
        bool ok = b.AlphaToCoverageEnable <= 1 && b.IndependentBlendEnable <= 1;
        for (UINT i = 0; i < 8 && ok; ++i) {
            D3D12_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[i];
            if (i >= count && !b.IndependentBlendEnable) t = b.RenderTarget[0];
            if (t.BlendEnable > 1 || t.LogicOpEnable > 1 || t.RenderTargetWriteMask > 0xF) ok = false;
            if (!t.BlendEnable) {
                t.SrcBlend = t.SrcBlendAlpha = D3D12_BLEND_ONE;
                t.DestBlend = t.DestBlendAlpha = D3D12_BLEND_ZERO;
                t.BlendOp = t.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            } else if (t.SrcBlend < 1 || t.SrcBlend > 19 || t.DestBlend < 1 || t.DestBlend > 19 || t.BlendOp < 1 ||
                       t.BlendOp > 5 || t.SrcBlendAlpha < 1 || t.SrcBlendAlpha > 19 || t.DestBlendAlpha < 1 ||
                       t.DestBlendAlpha > 19 || t.BlendOpAlpha < 1 || t.BlendOpAlpha > 5) {
                ok = false;
            }
            if (!t.LogicOpEnable) t.LogicOp = D3D12_LOGIC_OP_NOOP;
        }
        const D3D12_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[0];
        if (!ok) {
            if (g_block_odd++ < 4)
                std::fprintf(stderr,
                             "[rt] pipelines: a state block's blend entry doesn't read as a blend desc (%d %d | %d %d %d "
                             "%d %d %d %d %d %d %u): not applied\n",
                             b.AlphaToCoverageEnable, b.IndependentBlendEnable, t.BlendEnable, t.LogicOpEnable, t.SrcBlend,
                             t.DestBlend, t.BlendOp, t.SrcBlendAlpha, t.DestBlendAlpha, t.BlendOpAlpha, t.LogicOp,
                             t.RenderTargetWriteMask);
            return false;
        }
        if (g_block_blends++ < 12)
            std::fprintf(stderr,
                         "[rt] pipelines: state block %p blend: enable %d, colour %d %d op %d, alpha %d %d op %d, mask "
                         "%#x%s\n",
                         block, t.BlendEnable, t.SrcBlend, t.DestBlend, t.BlendOp, t.SrcBlendAlpha, t.DestBlendAlpha,
                         t.BlendOpAlpha, t.RenderTargetWriteMask, b.IndependentBlendEnable ? ", independent" : "");
        out = b;
        return true;
    }
    return false;
}

// ---- pipeline recipes: what the title's draws asked for, so it can be made again outside the game
//
// The driver compiles a translated pipeline in 2 to 100 s the first time and keeps the result on disk (a second
// CreatePipelineState of the same shaders takes 0.02 s, measured with --pso-time). A world load asks for hundreds at
// once, so the first load of an area draws almost nothing. Each PC pipeline asked for is written, once, as a line of
// <cache>\pipelines.txt: everything that makes it but the shaders, which are named by digest. FlamingoPort.exe
// --warm-pipelines makes them all, on every core, with the game closed; the game then finds them in the driver's cache.
//   g <vs> <ps|-> <targets> <formats|-> <dsv> <topology type> <blend desc, hex> <input elements|-> [<depth desc, hex>]
//   m <ms> <ps|-> <targets> <formats|-> <blend desc, hex> [<dsv> <depth desc, hex>]
//   c <cs>
// An input element is register:format:slot:offset:class:step. The depth-stencil desc (and a mesh pipeline's depth
// format) are there when the pipeline is drawn into a depth target.
std::mutex g_recipe_lock;
std::set<std::string>* g_recipes = nullptr;  // the file's lines (read at the first note)

void put_hex(std::string& s, const void* p, size_t n) {
    static const char kDigits[] = "0123456789abcdef";
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        s += kDigits[b[i] >> 4];
        s += kDigits[b[i] & 15];
    }
}

bool get_hex(const std::string& s, void* out, size_t n) {
    if (s.size() != 2 * n) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < n; ++i) {
        const int hi = digit(s[2 * i]), lo = digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        static_cast<uint8_t*>(out)[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

std::wstring recipes_path() { return std::wstring(shader_cache::folder()) + L"\\pipelines.txt"; }

void note_recipe(const std::string& line) {
    std::lock_guard lock(g_recipe_lock);
    FILE* f = nullptr;
    if (!g_recipes) {
        g_recipes = new std::set<std::string>;
        if (_wfopen_s(&f, recipes_path().c_str(), L"r") == 0 && f) {
            char buffer[4096];
            while (std::fgets(buffer, sizeof buffer, f)) {
                std::string l = buffer;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                if (!l.empty()) g_recipes->insert(l);
            }
            std::fclose(f);
        }
    }
    if (!g_recipes->insert(line).second) return;
    f = nullptr;
    if (_wfopen_s(&f, recipes_path().c_str(), L"a") != 0 || !f) return;
    std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
}

// The blend state a PC pipeline is made with: the defaults, then the title's register block, then the state block
// it was set through.
D3D12_BLEND_DESC final_blend(const PipelineState& p, UINT rt_count, bool every_target, const D3D12_BLEND_DESC* block) {
    D3D12_BLEND_DESC b{};
    b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT i = 0; i < (every_target ? 8u : rt_count); ++i)
        b.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    title_blend(p, rt_count, b);
    if (block) b = *block;
    return b;
}

std::string recipe_targets(const PsoKey& key) {
    std::string s = std::to_string(key.count) + " ";
    for (UINT i = 0; i < key.count; ++i) s += (i ? "," : "") + std::to_string(static_cast<int>(key.rt[i]));
    if (!key.count) s += "-";
    return s;
}

void note_drawn_recipe(const PipelineState& p, const PsoKey& key, const D3D12_BLEND_DESC& blend,
                       const std::vector<D3D12_INPUT_ELEMENT_DESC>* layout, const D3D12_DEPTH_STENCIL_DESC& depth) {
    if (!p.meta[0].has_digest || (!p.code[1].empty() && !p.meta[1].has_digest)) return;  // PC code with no digest
    std::string s = layout ? "g " : "m ";
    put_hex(s, p.meta[0].digest, 16);
    s += " ";
    if (p.code[1].empty()) s += "-";
    else put_hex(s, p.meta[1].digest, 16);
    s += " " + recipe_targets(key) + " ";
    if (layout) s += std::to_string(static_cast<int>(key.dsv)) + " " + std::to_string(static_cast<int>(key.topology)) + " ";
    put_hex(s, &blend, sizeof blend);
    if (layout) {
        s += " ";
        for (size_t i = 0; i < layout->size(); ++i) {
            const D3D12_INPUT_ELEMENT_DESC& e = (*layout)[i];
            s += (i ? "," : "") + std::to_string(e.SemanticIndex) + ":" + std::to_string(static_cast<int>(e.Format)) + ":" +
                 std::to_string(e.InputSlot) + ":" + std::to_string(e.AlignedByteOffset) + ":" +
                 std::to_string(static_cast<int>(e.InputSlotClass)) + ":" + std::to_string(e.InstanceDataStepRate);
        }
        if (layout->empty()) s += "-";
    }
    if (key.dsv != DXGI_FORMAT_UNKNOWN) {
        s += " ";
        if (!layout) s += std::to_string(static_cast<int>(key.dsv)) + " ";
        put_hex(s, &depth, sizeof depth);
    }
    note_recipe(s);
}

// ---- PC pipelines are made off the title's threads
//
// The driver compiles a pipeline's shaders when it is made, and that can take seconds (one translated shader took the
// NVIDIA compiler more than 30). The title makes its pipelines' PC counterparts at their first draw, on the threads
// that record its lists, and it stops itself (a watchdog thread that writes to null) when a frame's recording takes
// 30 s: the first frame of the open world did. So a pipeline is made by a few threads of ours; the draw that asked
// waits a little (most are ready in milliseconds, and the menus draw as before), within a budget a second so that a
// frame with hundreds of new pipelines doesn't add up, and a draw whose pipeline isn't ready yet is not drawn.
struct PsoJob {
    PsoKey key;
    std::function<ID3D12PipelineState*()> make;
    std::array<uint8_t, 16> digest[2];
    bool compute;
    size_t cost;  // the bytes of translated code the driver will compile: the smallest waiting job is made first
};
std::set<PsoKey> g_making;                 // asked for, not ready (g_pso_lock)
std::condition_variable g_pso_ready;       // a pipeline became ready (with g_pso_lock)
std::mutex g_job_lock;
std::condition_variable g_job_there;
// Two lines: what draws wait for, and compute pipelines. A translated compute shader can hold the driver's compiler
// for a minute (68 s seen), and in one line three makers busy with those left 145,000 of 160,000 draws of a
// five-minute run without their pipeline. Every maker but the first takes a draw's pipeline before a compute one.
std::deque<PsoJob> g_jobs, g_compute_jobs;
std::atomic<uint64_t> g_made_async{0}, g_made_slow{0}, g_refused_async{0};
std::atomic<uint64_t> g_driver_ms[2];      // the driver's time making them: draws', compute
constexpr DWORD kWaitEach = 40, kWaitSecond = 300;  // ms: one draw's wait, and all waits within one second

DWORD WINAPI pso_worker(void* index) {
    // Only the first maker takes compute pipelines (before the draws'), the others never do. With every maker free
    // to take one when no draw's pipeline waited, a load that met compute shaders the driver had never compiled
    // put all three on them for minutes, and the mesh pipelines the driver already had (0.02 s each) waited behind:
    // 64 mesh draws of 250,000 were drawn in a run where the one before, with the same cache, drew 97%.
    const bool takes_compute = index == nullptr;
    for (;;) {
        PsoJob job;
        {
            std::unique_lock lock(g_job_lock);
            g_job_there.wait(lock, [&] { return !g_jobs.empty() || (takes_compute && !g_compute_jobs.empty()); });
            std::deque<PsoJob>& from = takes_compute && !g_compute_jobs.empty() ? g_compute_jobs : g_jobs;
            // The smallest first: the driver's time grows with the code (a 1 MB pedestrian shader pair takes it
            // over a minute, the interface's a few milliseconds), and a world load queues hundreds of mesh
            // pipelines (398 seen) that the menus' and the interface's would otherwise wait behind.
            auto next = std::min_element(from.begin(), from.end(),
                                         [](const PsoJob& a, const PsoJob& b) { return a.cost < b.cost; });
            job = std::move(*next);
            from.erase(next);
        }
        const ULONGLONG t0 = GetTickCount64();
        ID3D12PipelineState* pso = job.make();
        const ULONGLONG ms = GetTickCount64() - t0;
        const uint64_t n = ++g_made_async;
        if (!pso) ++g_refused_async;
        g_driver_ms[job.compute ? 1 : 0] += ms;
        size_t waiting = 0;
        {
            std::lock_guard lock(g_pso_lock);
            g_psos[job.key] = pso;
            g_making.erase(job.key);
            waiting = g_making.size();
        }
        g_pso_ready.notify_all();
        if (ms >= 2000) {
            ++g_made_slow;
            std::fprintf(stderr, "[rt] pipelines: a %s pipeline took the driver %.1f s to make (%s); shaders ",
                         job.compute ? "compute" : "graphics", ms / 1000.0, pso ? "made" : "refused");
            for (int k = 0; k < (job.compute ? 1 : 2); ++k) {
                for (uint8_t b : job.digest[k]) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, " ");
            }
            std::fprintf(stderr, "\n");
        }
        if ((n & (n - 1)) == 0 && n >= 16)
            std::fprintf(stderr,
                         "[rt] pipelines: %llu made off the title's threads (%llu refused, %llu took 2 s or more), %zu "
                         "being made; the driver's time: %.1f s on draws', %.1f s on compute\n",
                         static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_refused_async.load()),
                         static_cast<unsigned long long>(g_made_slow.load()), waiting, g_driver_ms[0].load() / 1000.0,
                         g_driver_ms[1].load() / 1000.0);
    }
}

size_t pipelines_being_made() {
    std::lock_guard lock(g_pso_lock);
    return g_making.size();
}

// Hands a pipeline to the makers and waits for it within the budget; null if it isn't ready (or was refused).
ID3D12PipelineState* make_async(const PsoKey& key, PsoJob&& job) {
    static std::once_flag started;
    std::call_once(started, [] {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        DWORD n = si.dwNumberOfProcessors / 2;
        n = n < 2 ? 2 : n > 8 ? 8 : n;
        for (DWORD i = 0; i < n; ++i)
            if (HANDLE t = CreateThread(nullptr, 0, &pso_worker, reinterpret_cast<void*>(uintptr_t{i}), 0, nullptr))
                CloseHandle(t);
        std::fprintf(stderr, "[rt] pipelines: %lu thread(s) make the PC pipelines\n", n);
    });
    {
        std::lock_guard lock(g_job_lock);
        (job.compute ? g_compute_jobs : g_jobs).push_back(std::move(job));
    }
    g_job_there.notify_all();
    // the budget: waits within the same second add up to kWaitSecond at most
    static std::atomic<ULONGLONG> second{0};
    static std::atomic<DWORD> spent{0};
    const ULONGLONG now = GetTickCount64();
    if (second.exchange(now / 1000) != now / 1000) spent = 0;
    if (spent.load() >= kWaitSecond) return nullptr;
    std::unique_lock lock(g_pso_lock);
    const bool ready = g_pso_ready.wait_for(lock, std::chrono::milliseconds(kWaitEach),
                                            [&] { return g_psos.find(key) != g_psos.end(); });
    spent += static_cast<DWORD>(GetTickCount64() - now);
    return ready ? g_psos[key] : nullptr;
}

ID3D12PipelineState* host_pso(PipelineState& p, UINT rt_count, const DXGI_FORMAT* rt_formats, DXGI_FORMAT dsv_format,
                              D3D12_PRIMITIVE_TOPOLOGY_TYPE topology, void* state_block) {
    if (p.compute || p.code[0].empty() || rt_count > 8) return nullptr;
    PsoKey key{};
    key.p = &p;
    key.count = rt_count;
    for (UINT i = 0; i < rt_count; ++i) key.rt[i] = rt_formats[i];
    key.dsv = dsv_format;
    key.topology = topology;
    key.block = state_block;
    {
        std::lock_guard lock(g_pso_lock);
        if (auto it = g_psos.find(key); it != g_psos.end()) return it->second;
        if (!g_making.insert(key).second) return nullptr;  // being made
    }
    ID3D12RootSignature* root = abi_root(false);
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    if (!root || !host_layout(p, layout)) {
        std::lock_guard lock(g_pso_lock);
        g_making.erase(key);
        g_psos[key] = nullptr;
        return nullptr;
    }
    PsoJob job{};
    job.key = key;
    job.compute = false;
    job.cost = p.code[0].size() + p.code[1].size();
    std::memcpy(job.digest[0].data(), p.meta[0].digest, 16);
    std::memcpy(job.digest[1].data(), p.meta[1].digest, 16);
    PipelineState* pipeline = &p;  // the title's pipelines are never freed: the makers may read it later
    D3D12_BLEND_DESC block{};  // read now: the title may release the block before the makers get here
    const bool has_block = block_blend(state_block, rt_count, block);
    note_drawn_recipe(p, key, final_blend(p, rt_count, false, has_block ? &block : nullptr), &layout,
                      title_depth(p, dsv_format));
    job.make = [pipeline, key, root, has_block, block, layout = std::move(layout)]() -> ID3D12PipelineState* {
        const PipelineState& p = *pipeline;
        const UINT rt_count = key.count;
        // the translations, read from the cache for as long as the driver needs them
        const auto vs = p.code[0].bytes(), ps = p.code[1].bytes();
        if (!vs || (!p.code[1].empty() && !ps)) return nullptr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root;
        d.VS = {vs->data(), vs->size()};
        if (ps) d.PS = {ps->data(), ps->size()};
        // states: blending from the title's register block (title_blend); rasterizer and depth not yet: no culling
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
        d.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(static_cast<int>(key.topology) & 0xFF);
        if (static_cast<int>(key.topology) & kRectTopology)  // a rectangle list: its fourth vertices
            if (const std::vector<uint8_t>* gs = rect_geometry_shader(*vs)) d.GS = {gs->data(), gs->size()};
        d.NumRenderTargets = rt_count;
        for (UINT i = 0; i < rt_count; ++i) {
            d.RTVFormats[i] = static_cast<DXGI_FORMAT>(key.rt[i]);
            d.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        title_blend(p, rt_count, d.BlendState);  // the title's own, when its pipeline carries the register block
        if (has_block) d.BlendState = block;     // set through a state block: the block's blending, not the pipeline's
        d.DSVFormat = static_cast<DXGI_FORMAT>(key.dsv);
        d.DepthStencilState = title_depth(p, d.DSVFormat);  // off when the draw has no depth target
        d.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = host_device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
        if (FAILED(hr) && d.GS.pShaderBytecode) {
            // refused with the rectangles' geometry shader: without it (half of each rectangle, as before)
            static std::atomic<int> told{0};
            if (told++ < 4)
                std::fprintf(stderr, "[rt] pipelines: a pipeline with the rectangles' geometry shader was refused "
                                     "(%08lX): made without it\n", static_cast<unsigned long>(hr));
            d.GS = {};
            pso = nullptr;
            hr = host_device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
        }
        if (FAILED(hr)) {
            pso = nullptr;
            if (g_trial_logged++ < 6) {
                std::fprintf(stderr, "[rt] pipelines: a PC pipeline couldn't be made (%08lX); vs ",
                             static_cast<unsigned long>(hr));
                for (uint8_t b : p.meta[0].digest) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, " ps ");
                for (uint8_t b : p.meta[1].digest) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, ", %zu inputs\n", layout.size());
            }
        }
        return pso;
    };
    return make_async(key, std::move(job));
}

// A pipeline-state stream's subobject, as the PC lays them out: the type, then the value at its own alignment, the
// whole padded to a pointer.
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) StreamPart {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};
struct MeshStream {
    StreamPart<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
    StreamPart<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
    StreamPart<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    StreamPart<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    StreamPart<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask;
    StreamPart<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
    StreamPart<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
    StreamPart<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> depth_format;
    StreamPart<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> targets;
    StreamPart<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> samples;
};

// As host_pso, for a pipeline whose first stage is a mesh shader: made from a stream (the PC's graphics desc has
// no mesh stage). No culling, the title's blending and depth state.
ID3D12PipelineState* host_mesh_pso(PipelineState& p, UINT rt_count, const DXGI_FORMAT* rt_formats, DXGI_FORMAT dsv_format,
                                   void* state_block) {
    if (!p.mesh || p.code[0].empty() || rt_count > 8) return nullptr;
    PsoKey key{};
    key.p = &p;
    key.count = rt_count;
    for (UINT i = 0; i < rt_count; ++i) key.rt[i] = rt_formats[i];
    key.dsv = dsv_format;
    key.block = state_block;
    key.topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(0x4D);  // mesh: not a topology of a draw's
    {
        std::lock_guard lock(g_pso_lock);
        if (auto it = g_psos.find(key); it != g_psos.end()) return it->second;
        if (!g_making.insert(key).second) return nullptr;  // being made
    }
    ID3D12RootSignature* root = abi_mesh_root();
    if (!root) {
        std::lock_guard lock(g_pso_lock);
        g_making.erase(key);
        g_psos[key] = nullptr;
        return nullptr;
    }
    PsoJob job{};
    job.key = key;
    job.compute = false;
    job.cost = p.code[0].size() + p.code[1].size();
    std::memcpy(job.digest[0].data(), p.meta[0].digest, 16);
    std::memcpy(job.digest[1].data(), p.meta[1].digest, 16);
    PipelineState* pipeline = &p;
    D3D12_BLEND_DESC block{};
    const bool has_block = block_blend(state_block, rt_count, block);
    note_drawn_recipe(p, key, final_blend(p, rt_count, true, has_block ? &block : nullptr), nullptr,
                      title_depth(p, dsv_format));
    job.make = [pipeline, key, root, has_block, block]() -> ID3D12PipelineState* {
        const PipelineState& p = *pipeline;
        const auto ms = p.code[0].bytes(), ps = p.code[1].bytes();
        if (!ms || (!p.code[1].empty() && !ps)) return nullptr;
        MeshStream s;
        s.root.value = root;
        s.ms.value = {ms->data(), ms->size()};
        if (ps) s.ps.value = {ps->data(), ps->size()};
        for (UINT i = 0; i < 8; ++i) s.blend.value.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        title_blend(p, key.count, s.blend.value);
        if (has_block) s.blend.value = block;
        s.sample_mask.value = UINT_MAX;
        s.rasterizer.value.FillMode = D3D12_FILL_MODE_SOLID;
        s.rasterizer.value.CullMode = D3D12_CULL_MODE_NONE;
        s.rasterizer.value.DepthClipEnable = TRUE;
        s.targets.value.NumRenderTargets = key.count;
        for (UINT i = 0; i < key.count; ++i) s.targets.value.RTFormats[i] = key.rt[i];
        s.depth_format.value = static_cast<DXGI_FORMAT>(key.dsv);
        s.depth.value = title_depth(p, s.depth_format.value);
        s.samples.value.Count = 1;
        ID3D12Device2* dev = nullptr;
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = host_device()->QueryInterface(IID_PPV_ARGS(&dev));
        if (SUCCEEDED(hr)) {
            const D3D12_PIPELINE_STATE_STREAM_DESC d{sizeof s, &s};
            hr = dev->CreatePipelineState(&d, IID_PPV_ARGS(&pso));
            dev->Release();
        }
        if (FAILED(hr)) {
            pso = nullptr;
            static std::atomic<int> told{0};
            if (told++ < 8) {
                std::fprintf(stderr, "[rt] pipelines: a PC mesh pipeline couldn't be made (%08lX); ms ",
                             static_cast<unsigned long>(hr));
                for (uint8_t b : p.meta[0].digest) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, " ps ");
                for (uint8_t b : p.meta[1].digest) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, ", %u target(s)\n", key.count);
            }
        }
        return pso;
    };
    return make_async(key, std::move(job));
}

ID3D12PipelineState* host_compute_pso(PipelineState& p) {
    if (!p.compute || p.code[0].empty()) return nullptr;
    PsoKey key{};
    key.p = &p;
    key.count = 0xFFFF;  // compute: one pipeline a title pipeline
    {
        std::lock_guard lock(g_pso_lock);
        if (auto it = g_psos.find(key); it != g_psos.end()) return it->second;
        if (!g_making.insert(key).second) return nullptr;  // being made
    }
    ID3D12RootSignature* root = abi_root(true);
    if (!root) {
        std::lock_guard lock(g_pso_lock);
        g_making.erase(key);
        g_psos[key] = nullptr;
        return nullptr;
    }
    PsoJob job{};
    job.key = key;
    job.compute = true;
    if (p.meta[0].has_digest) {
        std::string recipe = "c ";
        put_hex(recipe, p.meta[0].digest, 16);
        note_recipe(recipe);
    }
    job.cost = p.code[0].size();
    std::memcpy(job.digest[0].data(), p.meta[0].digest, 16);
    PipelineState* pipeline = &p;
    job.make = [pipeline, root]() -> ID3D12PipelineState* {
        const PipelineState& p = *pipeline;
        const auto cs = p.code[0].bytes();
        if (!cs) return nullptr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root;
        d.CS = {cs->data(), cs->size()};
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = host_device()->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            pso = nullptr;
            static std::atomic<int> told{0};
            if (told++ < 6) {
                std::fprintf(stderr, "[rt] pipelines: a PC compute pipeline couldn't be made (%08lX); cs ",
                             static_cast<unsigned long>(hr));
                for (uint8_t b : p.meta[0].digest) std::fprintf(stderr, "%02x", b);
                std::fprintf(stderr, "\n");
            }
        }
        return pso;
    };
    return make_async(key, std::move(job));
}

void pso_trial(PipelineState& p) {
    std::call_once(g_trial_once, [] {
        char v[16] = {};
        g_trial_left = GetEnvironmentVariableA("RT_PSO_TRIAL", v, sizeof v) ? std::atoi(v) : 0;
    });
    if (p.compute || p.code[0].empty() || p.code[1].empty() || g_trial_left.fetch_sub(1) <= 0) return;
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    if (!host_layout(p, layout)) {
        ++g_trial_layout;
    } else {
        const DXGI_FORMAT rt = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (host_pso(p, 1, &rt, DXGI_FORMAT_UNKNOWN, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, nullptr)) ++g_trial_ok;
        else ++g_trial_failed;
    }
    int done = g_trial_ok + g_trial_failed + g_trial_layout;
    if ((done & (done - 1)) == 0 || g_trial_left == 0)
        std::fprintf(stderr, "[rt] pipelines: trial of %d: %d made, %d refused, %d with inputs the layout lacks\n",
                     done, g_trial_ok.load(), g_trial_failed.load(), g_trial_layout.load());
}

// FlamingoPort.exe --pso-time ms <mesh.dxil> [<pixel.dxil>] | cs <compute.dxil>: makes one PC pipeline from translated
// shaders, outside the game, and prints the time the driver took: for comparing ways of generating the code (a
// world load can't: there the pipelines wait in line for minutes). The driver keeps what it has compiled, so a
// shader timed twice is quick the second time: compare shaders it hasn't seen.
int pso_time(int argc, wchar_t** argv) {
    auto read = [](const wchar_t* path, std::vector<uint8_t>& out) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
        std::fseek(f, 0, SEEK_END);
        out.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    };
    if (argc < 4) {
        std::fprintf(stderr, "usage: FlamingoPort.exe --pso-time ms <mesh.dxil> [<pixel.dxil>] | cs <compute.dxil>\n");
        return 2;
    }
    const bool compute = !wcscmp(argv[2], L"cs");
    std::vector<uint8_t> first, second;
    if (!read(argv[3], first) || (argc > 4 && !read(argv[4], second))) {
        std::fprintf(stderr, "pso-time: a file couldn't be read\n");
        return 2;
    }
    ID3D12Device* device = host_device();
    ID3D12RootSignature* root = !device ? nullptr : compute ? abi_root(true) : abi_mesh_root();
    if (!root) {
        std::fprintf(stderr, "pso-time: no D3D12 device or root signature\n");
        return 1;
    }
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr;
    const ULONGLONG t0 = GetTickCount64();
    if (compute) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root;
        d.CS = {first.data(), first.size()};
        hr = device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
    } else {
        MeshStream s;
        s.root.value = root;
        s.ms.value = {first.data(), first.size()};
        if (!second.empty()) s.ps.value = {second.data(), second.size()};
        for (UINT i = 0; i < 8; ++i) s.blend.value.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        s.sample_mask.value = UINT_MAX;
        s.rasterizer.value.FillMode = D3D12_FILL_MODE_SOLID;
        s.rasterizer.value.CullMode = D3D12_CULL_MODE_NONE;
        s.rasterizer.value.DepthClipEnable = TRUE;
        s.targets.value.NumRenderTargets = 4;  // a G-buffer pass's worth; what the shaders write past them is dropped
        for (UINT i = 0; i < 4; ++i) s.targets.value.RTFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.samples.value.Count = 1;
        ID3D12Device2* dev2 = nullptr;
        hr = device->QueryInterface(IID_PPV_ARGS(&dev2));
        if (SUCCEEDED(hr)) {
            const D3D12_PIPELINE_STATE_STREAM_DESC d{sizeof s, &s};
            hr = dev2->CreatePipelineState(&d, IID_PPV_ARGS(&pso));
            dev2->Release();
        }
    }
    const ULONGLONG ms = GetTickCount64() - t0;
    std::printf("pso-time: %s, %zu + %zu bytes: %.2f s (%s, %08lX)\n", compute ? "compute" : "mesh", first.size(),
                second.size(), ms / 1000.0, SUCCEEDED(hr) ? "made" : "refused", static_cast<unsigned long>(hr));
    std::fflush(stdout);
    if (pso) pso->Release();
    return SUCCEEDED(hr) ? 0 : 1;
}


// FlamingoPort.exe --warm-pipelines [--jobs N]: makes every pipeline of <cache>\pipelines.txt (see "pipeline recipes"
// above) from the translated shaders, so the driver has them compiled before the game asks. Made ones are released
// at once: what stays is the driver's own cache on disk.
namespace {
struct Recipe {
    char kind = 0;
    uint8_t first[16]{}, second[16]{};
    bool has_second = false;
    UINT count = 0;
    DXGI_FORMAT rt[8]{};
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    int topology = 0;
    D3D12_BLEND_DESC blend{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    D3D12_DEPTH_STENCIL_DESC depth{};  // with a depth target (dsv): the title's; else off
    size_t cost = 0;  // the bytes of translated code: the driver's time grows with it, so the small go first
};

std::vector<std::string> split(const std::string& s, char by) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t next = s.find(by, at);
        out.push_back(s.substr(at, next == std::string::npos ? next : next - at));
        if (next == std::string::npos) return out;
        at = next + 1;
    }
}

bool parse_recipe(const std::string& line, Recipe& r) {
    const std::vector<std::string> t = split(line, ' ');
    if (t.empty() || t[0].size() != 1) return false;
    r.kind = t[0][0];
    if (r.kind == 'c') return t.size() == 2 && get_hex(t[1], r.first, 16);
    const size_t want = r.kind == 'g' ? 9 : r.kind == 'm' ? 6 : 0;
    const size_t more = r.kind == 'g' ? 1 : 2;  // with a depth target: the depth desc (a mesh line: its format too)
    if (!want || (t.size() != want && t.size() != want + more) || !get_hex(t[1], r.first, 16)) return false;
    r.depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    r.depth.FrontFace = r.depth.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                            D3D12_COMPARISON_FUNC_ALWAYS};
    if (t.size() != want) {
        if (r.kind == 'm') r.dsv = static_cast<DXGI_FORMAT>(std::atoi(t[want].c_str()));
        if (!get_hex(t.back(), &r.depth, sizeof r.depth)) return false;
    }
    r.has_second = t[2] != "-";
    if (r.has_second && !get_hex(t[2], r.second, 16)) return false;
    r.count = static_cast<UINT>(std::atoi(t[3].c_str()));
    if (r.count > 8) return false;
    if (r.count) {
        const std::vector<std::string> f = split(t[4], ',');
        if (f.size() != r.count) return false;
        for (UINT i = 0; i < r.count; ++i) r.rt[i] = static_cast<DXGI_FORMAT>(std::atoi(f[i].c_str()));
    }
    size_t at = 5;
    if (r.kind == 'g') {
        r.dsv = static_cast<DXGI_FORMAT>(std::atoi(t[at++].c_str()));
        r.topology = std::atoi(t[at++].c_str());
    }
    if (!get_hex(t[at++], &r.blend, sizeof r.blend)) return false;
    if (r.kind == 'g' && t[at] != "-")
        for (const std::string& e : split(t[at], ',')) {
            const std::vector<std::string> v = split(e, ':');
            if (v.size() != 6) return false;
            D3D12_INPUT_ELEMENT_DESC d{};
            d.SemanticName = "TEXCOORD";
            d.SemanticIndex = static_cast<UINT>(std::atoi(v[0].c_str()));
            d.Format = static_cast<DXGI_FORMAT>(std::atoi(v[1].c_str()));
            d.InputSlot = static_cast<UINT>(std::atoi(v[2].c_str()));
            d.AlignedByteOffset = static_cast<UINT>(std::atoi(v[3].c_str()));
            d.InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(std::atoi(v[4].c_str()));
            d.InstanceDataStepRate = static_cast<UINT>(std::atoi(v[5].c_str()));
            r.layout.push_back(d);
        }
    return true;
}

struct Warm {
    std::vector<Recipe> recipes;
    std::atomic<size_t> next{0};
    std::atomic<int> made{0}, refused{0}, missing{0}, slow{0};
    std::atomic<uint64_t> driver_ms{0};
    ID3D12RootSignature* root[3]{};  // graphics, compute, mesh
};
Warm* g_warm = nullptr;

// 0 made, 1 refused, 2 a shader isn't in the cache
int warm_one(const Recipe& r, ULONGLONG& ms) {
    std::vector<uint8_t> a, b;
    int kind = 0;
    if (!shader_cache::find(r.first, a, kind) || (r.has_second && !shader_cache::find(r.second, b, kind))) return 2;
    ID3D12Device* device = host_device();
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr;
    const ULONGLONG t0 = GetTickCount64();
    if (r.kind == 'c') {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = g_warm->root[1];
        d.CS = {a.data(), a.size()};
        hr = device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
    } else if (r.kind == 'g') {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};  // as host_pso makes it
        d.pRootSignature = g_warm->root[0];
        d.VS = {a.data(), a.size()};
        if (r.has_second) d.PS = {b.data(), b.size()};
        d.BlendState = r.blend;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.InputLayout = {r.layout.data(), static_cast<UINT>(r.layout.size())};
        d.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(r.topology & 0xFF);
        if (r.topology & kRectTopology)
            if (const std::vector<uint8_t>* gs = rect_geometry_shader(a)) d.GS = {gs->data(), gs->size()};
        d.NumRenderTargets = r.count;
        for (UINT i = 0; i < r.count; ++i) d.RTVFormats[i] = r.rt[i];
        d.DSVFormat = r.dsv;
        d.DepthStencilState = r.depth;
        d.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    } else {
        MeshStream s;  // as host_mesh_pso makes it
        s.root.value = g_warm->root[2];
        s.ms.value = {a.data(), a.size()};
        if (r.has_second) s.ps.value = {b.data(), b.size()};
        s.blend.value = r.blend;
        s.sample_mask.value = UINT_MAX;
        s.rasterizer.value.FillMode = D3D12_FILL_MODE_SOLID;
        s.rasterizer.value.CullMode = D3D12_CULL_MODE_NONE;
        s.rasterizer.value.DepthClipEnable = TRUE;
        s.targets.value.NumRenderTargets = r.count;
        for (UINT i = 0; i < r.count; ++i) s.targets.value.RTFormats[i] = r.rt[i];
        s.depth_format.value = r.dsv;
        s.depth.value = r.depth;
        s.samples.value.Count = 1;
        ID3D12Device2* dev2 = nullptr;
        hr = device->QueryInterface(IID_PPV_ARGS(&dev2));
        if (SUCCEEDED(hr)) {
            const D3D12_PIPELINE_STATE_STREAM_DESC d{sizeof s, &s};
            hr = dev2->CreatePipelineState(&d, IID_PPV_ARGS(&pso));
            dev2->Release();
        }
    }
    ms = GetTickCount64() - t0;
    if (FAILED(hr) || !pso) return 1;
    pso->Release();
    return 0;
}

DWORD WINAPI warm_worker(void*) {
    for (;;) {
        const size_t i = g_warm->next++;
        if (i >= g_warm->recipes.size()) return 0;
        ULONGLONG ms = 0;
        const int what = warm_one(g_warm->recipes[i], ms);
        g_warm->driver_ms += ms;
        if (ms >= 2000) ++g_warm->slow;
        ++(what == 0 ? g_warm->made : what == 1 ? g_warm->refused : g_warm->missing);
    }
}
}  // namespace

int warm_pipelines(int argc, wchar_t** argv) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    int jobs = static_cast<int>(si.dwNumberOfProcessors) - 1;
    for (int i = 2; i + 1 < argc; ++i)
        if (!wcscmp(argv[i], L"--jobs")) jobs = _wtoi(argv[i + 1]);
    jobs = jobs < 1 ? 1 : jobs > 32 ? 32 : jobs;
    g_warm = new Warm;
    FILE* f = nullptr;
    int unread = 0;
    if (_wfopen_s(&f, recipes_path().c_str(), L"r") == 0 && f) {
        char buffer[4096];
        while (std::fgets(buffer, sizeof buffer, f)) {
            std::string l = buffer;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.empty()) continue;
            Recipe r;
            if (parse_recipe(l, r)) g_warm->recipes.push_back(std::move(r));
            else ++unread;
        }
        std::fclose(f);
    }
    const size_t total = g_warm->recipes.size();
    // Smallest first: the many quick ones are all there early (a game started meanwhile finds most of what it asks
    // for), the few that hold the driver for a minute or more come last.
    for (Recipe& r : g_warm->recipes) {
        std::vector<uint8_t> code;
        int kind = 0;
        if (shader_cache::find(r.first, code, kind)) r.cost += code.size();
        if (r.has_second && shader_cache::find(r.second, code, kind)) r.cost += code.size();
    }
    std::stable_sort(g_warm->recipes.begin(), g_warm->recipes.end(),
                     [](const Recipe& a, const Recipe& b) { return a.cost < b.cost; });
    std::printf("warm: %zu pipelines in %ls (%d lines not understood), %d at a time, smallest first (%zu to %zu KiB "
                "of code)\n",
                total, recipes_path().c_str(), unread, jobs, total ? g_warm->recipes.front().cost >> 10 : size_t{0},
                total ? g_warm->recipes.back().cost >> 10 : size_t{0});
    std::fflush(stdout);
    if (!total) return 0;
    if (!host_device()) {
        std::fprintf(stderr, "warm: no D3D12 device\n");
        return 1;
    }
    g_warm->root[0] = abi_root(false);
    g_warm->root[1] = abi_root(true);
    g_warm->root[2] = abi_mesh_root();
    if (!g_warm->root[0] || !g_warm->root[1] || !g_warm->root[2]) {
        std::fprintf(stderr, "warm: a root signature couldn't be made\n");
        return 1;
    }
    const ULONGLONG t0 = GetTickCount64();
    std::vector<HANDLE> threads;
    for (int i = 0; i < jobs; ++i)
        if (HANDLE t = CreateThread(nullptr, 0, &warm_worker, nullptr, 0, nullptr)) threads.push_back(t);
    for (;;) {
        const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(threads.size()), threads.data(), TRUE, 5000);
        const int done = g_warm->made + g_warm->refused + g_warm->missing;
        std::printf("warm: %d of %zu (%d made, %d refused, %d with a shader not in the cache; %d took 2 s or more), "
                    "%.0f s\n",
                    done, total, g_warm->made.load(), g_warm->refused.load(), g_warm->missing.load(), g_warm->slow.load(),
                    (GetTickCount64() - t0) / 1000.0);
        std::fflush(stdout);
        if (w != WAIT_TIMEOUT) break;
    }
    for (HANDLE t : threads) CloseHandle(t);
    std::printf("warm: done in %.0f s; the driver's time over the threads: %.0f s\n", (GetTickCount64() - t0) / 1000.0,
                g_warm->driver_ms.load() / 1000.0);
    return 0;
}

}  // namespace rt::d3d12x
