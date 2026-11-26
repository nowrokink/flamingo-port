// Draws on the PC: the title's graphics state on a console list becomes PC commands at each draw.
//
// Per draw: the PC pipeline for the bound slot-57 pipeline and render target formats (d3d12x_pipelines.cpp); each
// stage's 36 user-data dwords built from its metadata records (which user SGPR gets which descriptor table address
// or root constants, docs/gpu-memory.md) plus R's base, through a ring of upload memory as root CBVs; the ABI root
// signature's tables over the bindless heaps; vertex and index buffers at their pool chunk's GPU address.
//
// Not yet: depth, the pipeline's own blend / rasterizer state, the SGPRs the console driver fills (fetch shader,
// directly indexed heaps), static samplers, indexed rectangle lists (drawn as triangle lists).
#include <map>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "d3d12x_internal.h"
#include "gpu_memory.h"

namespace rt::d3d12x {
namespace {

constexpr uint32_t kUserDataDwords = 36;
constexpr uint64_t kRing = 64ull << 20;     // user data: 512 bytes a draw, so ~130k draws before it wraps

struct Ring {
    std::mutex m;
    bool tried = false;
    ID3D12Resource* buffer = nullptr;
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    std::atomic<uint64_t> next{0};
};
Ring g_ring;  // a global: the runtime has no thread-safe statics

bool ring_ready() {
    std::lock_guard lock(g_ring.m);
    if (!g_ring.tried) {
        g_ring.tried = true;
        D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, kRing, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        if (SUCCEEDED(host_device()->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&g_ring.buffer)))) {
            D3D12_RANGE none{0, 0};
            g_ring.buffer->Map(0, &none, reinterpret_cast<void**>(&g_ring.cpu));
            g_ring.gpu = g_ring.buffer->GetGPUVirtualAddress();
        }
    }
    return g_ring.cpu != nullptr;
}

// The root parameter a binding of this kind and register comes from: a table with a matching range, or a root
// descriptor / root constants with this register. -1 if none.
int find_param(const D3D12_ROOT_SIGNATURE_DESC1& d, uint32_t kind, uint32_t reg, bool* root_descriptor) {
    D3D12_DESCRIPTOR_RANGE_TYPE range;
    D3D12_ROOT_PARAMETER_TYPE direct;
    switch (kind) {
    case gpu::StageBinding::kSrv: range = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; direct = D3D12_ROOT_PARAMETER_TYPE_SRV; break;
    case gpu::StageBinding::kCbv: range = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; direct = D3D12_ROOT_PARAMETER_TYPE_CBV; break;
    case 0x301: range = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; direct = D3D12_ROOT_PARAMETER_TYPE_UAV; break;
    case gpu::StageBinding::kSampler:
        range = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        direct = static_cast<D3D12_ROOT_PARAMETER_TYPE>(-1);
        break;
    default: return -1;
    }
    for (UINT i = 0; i < d.NumParameters && i < GfxState::kParams; ++i) {
        const D3D12_ROOT_PARAMETER1& p = d.pParameters[i];
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                const D3D12_DESCRIPTOR_RANGE1& g = p.DescriptorTable.pDescriptorRanges[r];
                if (g.RangeType == range && reg >= g.BaseShaderRegister &&
                    (g.NumDescriptors == UINT_MAX || reg < g.BaseShaderRegister + g.NumDescriptors)) {
                    *root_descriptor = false;
                    return static_cast<int>(i);
                }
            }
        } else if (p.ParameterType == direct && p.Descriptor.ShaderRegister == reg) {
            *root_descriptor = true;
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The root signature's one descriptor table parameter, or -1 when it has none or several. A binding whose register
// no range names still takes its table's address in its SGPR and finds its descriptor at its own dword offset: the
// title's "sga" shaders (e.g. sga_dr_vs_pos_color_tex) bind b0 / t7 / t277 through a table whose only range says b27,
// and on the console ranges don't bound what a shader reads.
int only_table(const D3D12_ROOT_SIGNATURE_DESC1& d) {
    int found = -1;
    for (UINT i = 0; i < d.NumParameters && i < GfxState::kParams; ++i) {
        if (d.pParameters[i].ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
        if (found >= 0) return -1;
        found = static_cast<int>(i);
    }
    return found;
}

// ---- the console's rectangle lists (topology 17): three corners a rectangle, the fourth v0 + v2 - v1 (the right angle
// at v1). The PC has no such topology: a non-indexed rect list with its vertices in slot 0 becomes a triangle list of
// two triangles a rectangle, its fourth corner made on the CPU attribute by attribute (exact for the affine vertex
// shaders blits use), in upload memory.
bool readable(uint64_t p, size_t n);

constexpr D3D12_PRIMITIVE_TOPOLOGY kRectList = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(17);
constexpr uint32_t kMaxRectBytes = 256u << 10;

float half_f(uint16_t h) {
    uint32_t s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 1024), int(e) - 25);
    return s ? -v : v;
}
uint16_t f_half(float f) {  // round toward zero, enough for coordinates
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000;
    int e = static_cast<int>((x >> 23) & 255) - 127 + 15;
    if (e <= 0) return static_cast<uint16_t>(s);
    if (e >= 31) return static_cast<uint16_t>(s | 0x7C00);
    return static_cast<uint16_t>(s | (e << 10) | ((x >> 13) & 1023));
}

// The fourth corner's bytes for one element: a + c - b per component.
void corner_element(uint32_t format, const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: {
        int n = format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4 : format == DXGI_FORMAT_R32G32B32_FLOAT ? 3
                : format == DXGI_FORMAT_R32G32_FLOAT ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            float x, y, z;
            std::memcpy(&x, a + 4 * i, 4), std::memcpy(&y, b + 4 * i, 4), std::memcpy(&z, c + 4 * i, 4);
            float r = x + z - y;
            std::memcpy(out + 4 * i, &r, 4);
        }
        return;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16_FLOAT: {
        int n = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 4 : format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            uint16_t x, y, z;
            std::memcpy(&x, a + 2 * i, 2), std::memcpy(&y, b + 2 * i, 2), std::memcpy(&z, c + 2 * i, 2);
            uint16_t r = f_half(half_f(x) + half_f(z) - half_f(y));
            std::memcpy(out + 2 * i, &r, 2);
        }
        return;
    }
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UINT:
        for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(std::clamp(int(a[i]) + int(c[i]) - int(b[i]), 0, 255));
        return;
    default: return;  // left as v0's
    }
}

// The rect list's vertices expanded into `vb` (upload memory); false when this draw can't be (indexed, other slots,
// unreadable, too big), and it's drawn as the triangle list it was.
bool expand_rects(const GfxState& g, UINT count, UINT start, D3D12_VERTEX_BUFFER_VIEW* vb, UINT* out_count) {
    const D3D12_VERTEX_BUFFER_VIEW& src = g.vb[0];
    uint32_t stride = src.StrideInBytes;
    if (!g.vb_count || !stride || count < 3 || count % 3 || !g.pso) return false;
    for (const gpu::InputElement& e : g.pso->input)
        if (e.slot != 0 || e.per_instance || e.offset >= stride) return false;
    uint64_t rects = count / 3, bytes = rects * 6 * stride;
    uint64_t first = src.BufferLocation + uint64_t{start} * stride;
    if (bytes > kMaxRectBytes || !readable(first, count * stride)) return false;
    uint64_t span = (bytes + 255) & ~uint64_t{255};
    uint64_t at = g_ring.next.fetch_add(span) % kRing;
    if (at + span > kRing) at = g_ring.next.fetch_add(span) % kRing;
    uint8_t* out = g_ring.cpu + at;
    std::vector<uint8_t> d(stride);
    for (uint64_t r = 0; r < rects; ++r) {
        const uint8_t* v0 = reinterpret_cast<const uint8_t*>(first + r * 3 * stride);
        const uint8_t* v1 = v0 + stride;
        const uint8_t* v2 = v1 + stride;
        std::memcpy(d.data(), v0, stride);
        for (const gpu::InputElement& e : g.pso->input)
            if (e.offset < stride) corner_element(e.format, v0 + e.offset, v1 + e.offset, v2 + e.offset, d.data() + e.offset);
        const uint8_t* order[6] = {v0, v1, v2, v0, v2, d.data()};
        for (int k = 0; k < 6; ++k) std::memcpy(out + (r * 6 + k) * stride, order[k], stride);
    }
    *vb = src;
    vb->BufferLocation = g_ring.gpu + at;
    vb->SizeInBytes = static_cast<UINT>(bytes);
    *out_count = static_cast<UINT>(rects * 6);
    return true;
}

// A stage's user SGPRs as its metadata says, and R's base after them.
void user_data(const GfxState& g, const gpu::StageMeta& m, uint32_t ud[kUserDataDwords]) {
    std::memset(ud, 0, kUserDataDwords * 4);
    const uint64_t base = gpumem::r_base();
    ud[32] = static_cast<uint32_t>(base);
    ud[33] = static_cast<uint32_t>(base >> 32);
    if (!g.root || !g.root->desc) return;
    const D3D12_ROOT_SIGNATURE_DESC1& d = *g.root->desc;
    if (g.pso && g.pso->from_stream) {  // slot 86: no metadata; the root parameters in order from s8 (seen: a root SRV
        uint32_t s = 8;            // at s[8:9])
        for (UINT i = 0; i < d.NumParameters && i < GfxState::kParams && s < 32; ++i) {
            const D3D12_ROOT_PARAMETER1& q = d.pParameters[i];
            if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                ud[s++] = static_cast<uint32_t>(g.tables[i]);
            } else if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                for (UINT k = 0; k < q.Constants.Num32BitValues && k < 64 && s < 32; ++k) ud[s++] = g.constants[i][k];
            } else {
                ud[s++] = static_cast<uint32_t>(g.root_va[i]);
                if (s < 32) ud[s++] = static_cast<uint32_t>(g.root_va[i] >> 32);
            }
        }
        return;
    }
    for (const gpu::StageBinding& b : m.bindings) {
        if (b.sgpr >= 32) continue;
        if (b.kind == gpu::StageBinding::kRootConstants) {
            for (UINT i = 0; i < d.NumParameters && i < GfxState::kParams; ++i) {
                const D3D12_ROOT_PARAMETER1& p = d.pParameters[i];
                if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS || p.Constants.ShaderRegister != b.reg)
                    continue;
                uint32_t n = b.count ? b.count : p.Constants.Num32BitValues;
                for (uint32_t k = 0; k < n && b.sgpr + k < 32 && k < 64; ++k) ud[b.sgpr + k] = g.constants[i][k];
                break;
            }
            continue;
        }
        if (b.kind == gpu::StageBinding::kSampler && b.sgpr == 0) continue;  // a static sampler (not yet)
        if ((b.kind >> 8) == 1) {
            // A root view (the kind's second byte is the root type: 0 constants, 1 a root view, 3 a table, 4 a
            // static sampler, 5 and 6 the directly indexed heaps; the VIctory project's reading of the shader's
            // own table, which these records repeat). The record's third packed byte, read above as a count, is
            // the root parameter's number: its address goes to two user dwords.
            const uint32_t slot = b.count;
            if (slot < d.NumParameters && slot < GfxState::kParams) {
                ud[b.sgpr] = static_cast<uint32_t>(g.root_va[slot]);
                if (b.sgpr + 1 < 32) ud[b.sgpr + 1] = static_cast<uint32_t>(g.root_va[slot] >> 32);
            }
            continue;
        }
        bool direct = false;
        int p = find_param(d, b.kind, b.reg, &direct);
        if (p < 0 && b.kind != gpu::StageBinding::kSampler) p = only_table(d);  // registers the ranges don't name
        if (p < 0) continue;
        if (direct) {
            ud[b.sgpr] = static_cast<uint32_t>(g.root_va[p]);
            if (b.sgpr + 1 < 32) ud[b.sgpr + 1] = static_cast<uint32_t>(g.root_va[p] >> 32);
        } else {
            ud[b.sgpr] = static_cast<uint32_t>(g.tables[p]);  // shaders zero the high half: tables sit below 4 GiB
        }
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(D3D12_PRIMITIVE_TOPOLOGY t) {
    switch (t) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

// Counts, logged as they grow.
std::atomic<uint64_t> g_draws{0}, g_drawn{0}, g_no_pso{0}, g_no_target{0}, g_no_buffer{0}, g_no_shader{0};

// Draws skipped for want of a translated shader, per missing digest ("-" for a draw with no pipeline, "compute" for
// one whose pipeline is a compute one): the shaders the translator owes most are the ones at the top.
SRWLOCK g_owed_lock = SRWLOCK_INIT;
std::vector<std::pair<std::array<uint8_t, 16>, uint64_t>>* g_owed = new std::vector<std::pair<std::array<uint8_t, 16>, uint64_t>>;
uint64_t g_owed_none = 0, g_owed_compute = 0, g_owed_unnamed = 0;

void note_owed(const PipelineState* p) {
    AcquireSRWLockExclusive(&g_owed_lock);
    if (!p) ++g_owed_none;
    else if (p->compute) ++g_owed_compute;
    else if (p->missing.empty()) ++g_owed_unnamed;
    else
        for (const auto& d : p->missing) {
            auto it = std::find_if(g_owed->begin(), g_owed->end(), [&](const auto& e) { return e.first == d; });
            if (it == g_owed->end()) g_owed->push_back({d, 1});
            else ++it->second;
        }
    ReleaseSRWLockExclusive(&g_owed_lock);
}

void log_owed() {
    AcquireSRWLockExclusive(&g_owed_lock);
    auto v = *g_owed;
    const uint64_t none = g_owed_none, compute = g_owed_compute, unnamed = g_owed_unnamed;
    ReleaseSRWLockExclusive(&g_owed_lock);
    std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    shader_cache::write_owed(v);
    std::fprintf(stderr, "[rt] draws owed a shader: %zu digests; %llu draws with no pipeline, %llu with a compute one, "
                         "%llu with no stage named\n",
                 v.size(), (unsigned long long)none, (unsigned long long)compute, (unsigned long long)unnamed);
    for (size_t i = 0; i < v.size() && i < 24; ++i) {
        std::fprintf(stderr, "[rt] draws owed:   ");
        for (uint8_t x : v[i].first) std::fprintf(stderr, "%02x", x);
        std::fprintf(stderr, " %llu\n", (unsigned long long)v[i].second);
    }
}

void count_and_log() {
    uint64_t n = ++g_draws;
    if (((n & (n - 1)) == 0 && n >= 4096) || n % 20000 == 0) log_owed();
    if ((n & (n - 1)) == 0 || n % 20000 == 0)
        std::fprintf(stderr,
                     "[rt] draws: %llu seen, %llu drawn; not drawn: %llu no translated shader, %llu no PC pipeline, "
                     "%llu targets the PC lacks, %llu buffers not on the GPU\n",
                     (unsigned long long)n, (unsigned long long)g_drawn.load(), (unsigned long long)g_no_shader.load(),
                     (unsigned long long)g_no_pso.load(), (unsigned long long)g_no_target.load(),
                     (unsigned long long)g_no_buffer.load());
}

// One draw of a reported frame (diag::listing()), as the log line says it.
void note(const GfxState& g, const char* status, bool indexed, UINT count, UINT instances, ID3D12Resource* target) {
    char vs[13] = "-", ps[13] = "-";
    if (g.pso) {
        for (int i = 0; i < 6; ++i) {
            std::snprintf(vs + i * 2, 3, "%02x", g.pso->meta[0].digest[i]);
            std::snprintf(ps + i * 2, 3, "%02x", g.pso->meta[1].digest[i]);
        }
        if (!g.pso->meta[0].has_digest) std::memcpy(vs, "none", 5);
        if (!g.pso->meta[1].has_digest) std::memcpy(ps, "none", 5);
    }
    std::fprintf(stderr,
                 "[rt] diag:   draw %-11s topo %d %s %u x%u, %u vb, %zu input elems, %u rt (%p), dsv %s, viewport %.0fx%.0f, "
                 "vs %s ps %s\n",
                 status, g.topology, indexed ? "indexed" : "vertices", count, instances, g.vb_count,
                 g.pso ? g.pso->input.size() : 0, g.rtv_count, static_cast<void*>(target), g.dsv ? "set" : "none",
                 g.viewport_count ? g.viewports[0].Width : 0.f, g.viewport_count ? g.viewports[0].Height : 0.f, vs, ps);
}

// Whether [p, p + n) is committed, readable memory of this process.
bool readable(uint64_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!p || !VirtualQuery(reinterpret_cast<void*>(p), &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return p + n <= reinterpret_cast<uint64_t>(mi.BaseAddress) + mi.RegionSize;
}

std::mutex g_detail_m;
const PipelineState* g_detailed[8] = {};
uint64_t g_detailed_frame = 0;  // the frame they were for: each reported frame details its own pipelines

// A stage's descriptor tables: per SRV / sampler binding, the first T# / S# at its table address and the PC heap
// index it resolves to (0: never bound, so translated shaders read the fallback).
void detail_tsharp(const uint32_t* w);
void detail_tables(const char* stage, const gpu::StageMeta& m, const uint32_t* ud) {
    std::fprintf(stderr, "[rt] diag:     %s user data:", stage);
    for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %x", ud[i]);
    std::fprintf(stderr, "\n");
    for (const gpu::StageBinding& b : m.bindings) {
        std::fprintf(stderr, "[rt] diag:     %s binding kind %#x reg %u -> s%u (count %u, table dword %u, static %u)\n",
                     stage, b.kind, b.reg, b.sgpr, b.count, b.table_dwords, b.static_sampler);
        if (b.sgpr >= 32) continue;
        uint64_t t = ud[b.sgpr] ? ud[b.sgpr] + uint64_t{b.table_dwords} * 4 : 0;  // its descriptor in the table
        if (b.kind == gpu::StageBinding::kSrv && readable(t, 32)) {
            detail_tsharp(reinterpret_cast<const uint32_t*>(t));
        } else if (b.kind == gpu::StageBinding::kSampler && readable(t, 16)) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(t);
            std::fprintf(stderr, "[rt] diag:       S# %08x %08x %08x %08x -> sampler %u\n", w[0], w[1], w[2], w[3],
                         sampler_index(w));
        }
    }
    if (m.bindings.empty() && ud[8]) {  // a slot-86 pipeline: no metadata; its table at s8, in 32-byte slots
        for (uint32_t k = 0; k < 4; ++k) {
            const uint64_t t = ud[8] + uint64_t{k} * 32;
            if (!readable(t, 32)) break;
            std::fprintf(stderr, "[rt] diag:     %s table slot %u:\n", stage, k);
            detail_tsharp(reinterpret_cast<const uint32_t*>(t));
        }
    }
}

// One T#: the PC view it resolves to and the resource behind it.
void detail_tsharp(const uint32_t* w) {
    uint32_t view = view_index(w);
    std::fprintf(stderr, "[rt] diag:       T# %08x %08x %08x %08x %08x %08x %08x %08x -> view %u\n", w[0], w[1], w[2],
                 w[3], w[4], w[5], w[6], w[7], view);
    if (Resource* r = view_resource(view)) {
        const gfx10::ResourceDesc& d = r->desc.desc;
        std::fprintf(stderr,
                     "[rt] diag:         resource %p: dim %u %llux%ux%u format %u mips %u flags %#x (%s) at %llx, "
                     "PC texture %p\n",
                     static_cast<void*>(r), d.dimension, static_cast<unsigned long long>(d.width), d.height,
                     d.depth_or_array, d.format, d.mips, d.flags,
                     d.flags & 7 ? "written by the GPU: not uploaded" : "uploaded from title memory",
                     static_cast<unsigned long long>(r->va), static_cast<void*>(r->host));
        diag::sampled(r->host);
    } else if (view) {
        std::fprintf(stderr, "[rt] diag:         a view with no resource remembered\n");
    } else {
        std::fprintf(stderr, "[rt] diag:         no view: the shader reads the null texture\n");
    }
}

// The root signature bound at a draw: its parameters with what the title set, and the first descriptors of its tables.
void print_root(const GfxState& g) {
    if (g.root && g.root->desc) {
        const D3D12_ROOT_SIGNATURE_DESC1& d = *g.root->desc;
        std::fprintf(stderr, "[rt] diag:     root signature %p: %u parameter(s), %u static sampler(s), flags %#x\n",
                     static_cast<void*>(g.root), d.NumParameters, d.NumStaticSamplers, d.Flags);
        for (UINT i = 0; i < d.NumParameters; ++i) {
            const D3D12_ROOT_PARAMETER1& q = d.pParameters[i];
            std::fprintf(stderr, "[rt] diag:       param %u type %d visibility %d: table %llx, root va %llx", i,
                         q.ParameterType, q.ShaderVisibility, i < GfxState::kParams ? (unsigned long long)g.tables[i] : 0ull,
                         i < GfxState::kParams ? (unsigned long long)g.root_va[i] : 0ull);
            if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                for (UINT r = 0; r < q.DescriptorTable.NumDescriptorRanges; ++r) {
                    const D3D12_DESCRIPTOR_RANGE1& x = q.DescriptorTable.pDescriptorRanges[r];
                    std::fprintf(stderr, " [range type %d reg %u space %u count %d]", x.RangeType, x.BaseShaderRegister,
                                 x.RegisterSpace, static_cast<int>(x.NumDescriptors));
                }
            else if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
                std::fprintf(stderr, " [b%u space %u, %u dwords]", q.Constants.ShaderRegister, q.Constants.RegisterSpace,
                             q.Constants.Num32BitValues);
            else
                std::fprintf(stderr, " [reg %u space %u]", q.Descriptor.ShaderRegister, q.Descriptor.RegisterSpace);
            std::fprintf(stderr, "\n");
            if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && i < GfxState::kParams &&
                readable(g.tables[i], 96)) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(g.tables[i]);
                for (int k = 0; k < 24; k += 8)
                    std::fprintf(stderr, "[rt] diag:         [dword %2d] %08x %08x %08x %08x %08x %08x %08x %08x\n", k,
                                 w[k], w[k + 1], w[k + 2], w[k + 3], w[k + 4], w[k + 5], w[k + 6], w[k + 7]);
            }
            // a root CBV / SRV / UAV: the first 32 dwords of what it points at (words, and as floats)
            if ((q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV || q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                 q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV) &&
                i < GfxState::kParams && readable(g.root_va[i], 128)) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(g.root_va[i]);
                for (int k = 0; k < 32; k += 8) {
                    std::fprintf(stderr, "[rt] diag:         [dword %2d]", k);
                    for (int j = 0; j < 8; ++j) std::fprintf(stderr, " %08x", w[k + j]);
                    std::fprintf(stderr, "  |");
                    for (int j = 0; j < 8; ++j) {
                        float f;
                        std::memcpy(&f, &w[k + j], 4);
                        std::fprintf(stderr, " %g", f);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
        }
    } else {
        std::fprintf(stderr, "[rt] diag:     no root signature set\n");
    }
}

// RT_DIAG_STATES=1: each (pipeline, state block) pair drawn with, once: the shaders' digests, the draw's target
// format, and the first time each is seen, the pipeline's +8 block and the state block's entry, as words.
struct StatesSeen {
    std::mutex m;
    bool init = false, on = false;
    std::vector<std::pair<const void*, const void*>> pairs;
    std::vector<const void*> shown;
};
StatesSeen g_states;

void dump_words(const char* what, const void* id, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i += 32) {
        std::fprintf(stderr, "[rt] states: %s %p +%03zx:", what, id, i);
        for (size_t k = i; k < i + 32 && k + 4 <= n; k += 4) {
            uint32_t w;
            std::memcpy(&w, b + k, 4);
            std::fprintf(stderr, " %08x", w);
        }
        std::fprintf(stderr, "\n");
    }
}

void note_states(const GfxState& g, DXGI_FORMAT rt0) {
    std::lock_guard lock(g_states.m);
    if (!g_states.init) {
        g_states.init = true;
        char v[4];
        g_states.on = GetEnvironmentVariableA("RT_DIAG_STATES", v, sizeof v) && v[0] == '1';
    }
    if (!g_states.on) return;
    std::pair<const void*, const void*> key{g.pso, g.state_block};
    if (std::find(g_states.pairs.begin(), g_states.pairs.end(), key) != g_states.pairs.end()) return;
    g_states.pairs.push_back(key);
    char vs[13] = "-", ps[13] = "-";
    for (int i = 0; i < 6; ++i) {
        std::snprintf(vs + i * 2, 3, "%02x", g.pso->meta[0].digest[i]);
        std::snprintf(ps + i * 2, 3, "%02x", g.pso->meta[1].digest[i]);
    }
    // the depth target the title bound (not used on the PC yet): its texture's format, and the view's flags
    TargetView dv{};
    const bool depth = g.dsv && target_view(g.dsv, &dv) && dv.resource;
    std::fprintf(stderr,
                 "[rt] states: pipeline %p (%s %s ps %s) with state block %p, %u target(s), first format %d, topology %d; "
                 "depth target: %s, format %d, view format %d flags %#x\n",
                 static_cast<const void*>(g.pso), g.pso->mesh ? "ms" : "vs", vs, ps, g.state_block, g.rtv_count, rt0,
                 g.topology, !g.dsv ? "none" : depth ? "a texture" : "not a view of ours",
                 depth ? static_cast<int>(dv.resource->desc.desc.format) : 0,
                 depth && dv.has_dsv ? static_cast<int>(dv.dsv.Format) : 0,
                 depth && dv.has_dsv ? static_cast<unsigned>(dv.dsv.Flags) : 0u);
    if (std::find(g_states.shown.begin(), g_states.shown.end(), g.pso) == g_states.shown.end()) {
        g_states.shown.push_back(g.pso);
        dump_words("pipeline", g.pso, g.pso->state.data(), g.pso->state.size());
    }
    if (g.state_block && std::find(g_states.shown.begin(), g_states.shown.end(), g.state_block) == g_states.shown.end()) {
        g_states.shown.push_back(g.state_block);
        if (const std::vector<uint8_t>* e = state_block_entries(g.state_block))
            dump_words("block", g.state_block, e->data(), e->size());
    }
}

// The first drawn draw of each pipeline (up to 8) in a reported frame, in full: input layout, the first vertices as
// the title wrote them, the vertex stage's user data and the buffers its V#s name with their first rows, and the
// pixel stage's T# / S#.
void detail(const GfxState& g, const uint32_t* vs_ud, const uint32_t* ps_ud, uint64_t first_vertex) {
    bool seen = false;
    {
        std::lock_guard lock(g_detail_m);
        if (uint64_t f = diag::frame(); f != g_detailed_frame) {
            g_detailed_frame = f;
            for (auto& d : g_detailed) d = nullptr;
        }
        int free_slot = -1;
        for (int i = 0; i < 8; ++i) {
            if (g_detailed[i] == g.pso) seen = true;
            if (!g_detailed[i] && free_slot < 0) free_slot = i;
        }
        if (!seen && free_slot < 0) return;
        if (!seen) g_detailed[free_slot] = g.pso;
    }
    if (seen) {
        // one pipeline draws different things through its root arguments (slot 86's fill primitive takes its
        // vertices and colour from a root SRV) and its vertices (Scaleform's solid fills carry their colour), so
        // each of its draws shows them
        if (g.pso->from_stream) print_root(g);
        const uint64_t v = g.vb_count ? g.vb[0].BufferLocation + first_vertex * g.vb[0].StrideInBytes : 0;
        if (g.vb_count && g.vb[0].StrideInBytes && readable(v, g.vb[0].StrideInBytes)) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(v);
            std::fprintf(stderr, "[rt] diag:     vb 0 %llx, the draw's first vertex (%llu):",
                         (unsigned long long)g.vb[0].BufferLocation, (unsigned long long)first_vertex);
            for (UINT j = 0; j < g.vb[0].StrideInBytes / 4 && j < 8; ++j) std::fprintf(stderr, " %08x", w[j]);
            std::fprintf(stderr, "\n");
        }
        return;
    }
    const PipelineState& p = *g.pso;
    print_root(g);
    std::fprintf(stderr, "[rt] diag:     the pipeline's root signature: %p\n", static_cast<void*>(p.root));
    // the pipeline's state block (+8 of its desc) and the list's state block, as words
    auto words = [](const char* what, const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; i += 32) {
            std::fprintf(stderr, "[rt] diag:     %s +%03zx:", what, i);
            for (size_t k = i; k < i + 32 && k + 4 <= n; k += 4) {
                uint32_t w;
                std::memcpy(&w, b + k, 4);
                std::fprintf(stderr, " %08x", w);
            }
            std::fprintf(stderr, "\n");
        }
    };
    words("pso state", p.state.data(), p.state.size());
    if (const std::vector<uint8_t>* e = state_block_entries(g.state_block))
        words("list state block", e->data(), e->size());
    else
        std::fprintf(stderr, "[rt] diag:     no state block on the list\n");
    for (const gpu::InputElement& e : p.input)
        std::fprintf(stderr, "[rt] diag:     input %s%u: format %u, slot %u, offset %u%s\n", e.semantic.c_str(), e.index,
                     e.format, e.slot, e.offset, e.per_instance ? " (per instance)" : "");
    for (const shader_cache::VsInput& v : p.vs_in)
        std::fprintf(stderr, "[rt] diag:     vs input %s%u <- register %u\n", v.semantic, v.index, v.reg);
    for (UINT i = 0; i < g.vb_count; ++i) {
        const D3D12_VERTEX_BUFFER_VIEW& v = g.vb[i];
        std::fprintf(stderr, "[rt] diag:     vb %u: %llx, %u bytes, stride %u\n", i,
                     (unsigned long long)v.BufferLocation, v.SizeInBytes, v.StrideInBytes);
        for (UINT k = 0; k < 3 && v.StrideInBytes && readable(v.BufferLocation, (k + 1) * v.StrideInBytes); ++k) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(v.BufferLocation + uint64_t{k} * v.StrideInBytes);
            std::fprintf(stderr, "[rt] diag:       vertex %u:", k);
            for (UINT j = 0; j < v.StrideInBytes / 4 && j < 8; ++j) std::fprintf(stderr, " %08x", w[j]);
            std::fprintf(stderr, "\n");
        }
    }
    std::fprintf(stderr, "[rt] diag:     vs user data:");
    for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %x", vs_ud[i]);
    std::fprintf(stderr, "\n");
    for (const gpu::StageBinding& b : p.meta[0].bindings)
        std::fprintf(stderr, "[rt] diag:     vs binding kind %#x reg %u -> s%u (count %u)\n", b.kind, b.reg, b.sgpr,
                     b.count);
    // each user SGPR that holds an address below 4 GiB with a V# there
    for (int s = 0; s < 32; ++s) {
        uint64_t t = vs_ud[s];
        if (t < 0x10000 || !readable(t, 16)) continue;
        const uint32_t* vs = reinterpret_cast<const uint32_t*>(t);
        uint64_t base = vs[0] | (uint64_t{vs[1] & 0xFFFF} << 32);
        uint32_t stride = (vs[1] >> 16) & 0x3FFF, records = vs[2];
        std::fprintf(stderr, "[rt] diag:     s%d -> %llx: V# %08x %08x %08x %08x (base %llx, stride %u, records %u)\n", s,
                     (unsigned long long)t, vs[0], vs[1], vs[2], vs[3], (unsigned long long)base, stride, records);
        for (uint32_t r = 0; r < 6 && stride && readable(base + uint64_t{r} * stride, 16); ++r) {
            const float* f = reinterpret_cast<const float*>(base + uint64_t{r} * stride);
            std::fprintf(stderr, "[rt] diag:       row %u: %g %g %g %g\n", r, f[0], f[1], f[2], f[3]);
        }
    }
    detail_tables("ps", p.meta[1], ps_ud);
}

// The textures a stage reads through its descriptor tables (its SRV bindings' T#s), noted on the list for upload.
void note_textures(GfxState& g, const gpu::StageMeta& m, const uint32_t* ud) {
    if (g.pso && g.pso->from_stream && g.root && g.root->desc) {
        // slot 86: no binding metadata. Each table param sits in one SGPR from s8 (user_data()); the shader loads
        // its T#s from the table at 32-byte steps (seen: PS_BlitCalibration, T#s at +0 and +0x20, a V# at +0x40).
        // Any slot whose 8 dwords hash to a known view is taken; the rest miss the hash table.
        const D3D12_ROOT_SIGNATURE_DESC1& d = *g.root->desc;
        uint32_t s = 8;
        for (UINT i = 0; i < d.NumParameters && i < GfxState::kParams && s < 32; ++i) {
            const D3D12_ROOT_PARAMETER1& q = d.pParameters[i];
            if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                s += q.Constants.Num32BitValues;
                continue;
            }
            if (q.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                s += 2;
                continue;
            }
            const uint64_t table = ud[s++];
            for (uint32_t k = 0; table && k < 16; ++k) {
                const uint64_t at = table + uint64_t{k} * 32;
                if (!readable(at, 32)) break;
                Resource* r = used_view(reinterpret_cast<const uint32_t*>(at));
                if (r && r->host && std::find(g.used.begin(), g.used.end(), r) == g.used.end()) g.used.push_back(r);
            }
        }
        return;
    }
    for (const gpu::StageBinding& b : m.bindings) {
        if (b.kind != gpu::StageBinding::kSrv || b.sgpr >= 32 || !ud[b.sgpr]) continue;
        uint64_t at = ud[b.sgpr] + uint64_t{b.table_dwords} * 4;
        if (!readable(at, 32)) continue;
        Resource* r = used_view(reinterpret_cast<const uint32_t*>(at));
        if (r && r->host && std::find(g.used.begin(), g.used.end(), r) == g.used.end()) g.used.push_back(r);
    }
}

}  // namespace

void release_targets(GfxState& g, ID3D12GraphicsCommandList* host) {
    if (!g.held_count) return;
    D3D12_RESOURCE_BARRIER b[GfxState::kHeld];
    for (UINT i = 0; i < g.held_count; ++i) {
        b[i] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b[i].Transition = {g.held[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, g.held_state[i],
                           D3D12_RESOURCE_STATE_COMMON};
    }
    host->ResourceBarrier(g.held_count, b);
    g.held_count = 0;
}

void hold_targets(GfxState& g, ID3D12GraphicsCommandList* host, ID3D12Resource* const* r,
                  const D3D12_RESOURCE_STATES* s, UINT n) {
    n = std::min(n, GfxState::kHeld);
    if (n == g.held_count && std::equal(r, r + n, g.held) && std::equal(s, s + n, g.held_state)) return;
    // One batch: the held ones that change go back to common, the new ones leave it (a target held in another
    // state goes back first, then out again).
    D3D12_RESOURCE_BARRIER b[2 * GfxState::kHeld];
    UINT count = 0;
    UINT kept = 0;
    ID3D12Resource* keep[GfxState::kHeld];
    for (UINT i = 0; i < g.held_count; ++i) {
        bool same = false;
        for (UINT k = 0; k < n; ++k) same = same || (r[k] == g.held[i] && s[k] == g.held_state[i]);
        if (same) {
            keep[kept++] = g.held[i];
            continue;
        }
        b[count] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b[count++].Transition = {g.held[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, g.held_state[i],
                                 D3D12_RESOURCE_STATE_COMMON};
    }
    if (count) host->ResourceBarrier(count, b);  // back to common before any of them is taken out again
    count = 0;
    for (UINT k = 0; k < n; ++k) {
        if (std::find(keep, keep + kept, r[k]) != keep + kept) continue;
        b[count] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b[count++].Transition = {r[k], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, s[k]};
    }
    if (count) host->ResourceBarrier(count, b);
    std::copy(r, r + n, g.held);
    std::copy(s, s + n, g.held_state);
    g.held_count = n;
}

void prepare_submission(ID3D12GraphicsCommandList* pre) {
    gpumem::prepare(pre);
    upload_descriptor_table(pre);
}

// Whether a depth texture and a colour target are the same size (what the PC asks of targets bound together); the
// pairs that aren't are counted, and the first few logged: their draws go without depth.
std::atomic<uint64_t> g_depth_size_mismatch{0};
bool same_size(ID3D12Resource* depth, ID3D12Resource* colour) {
    const D3D12_RESOURCE_DESC d = depth->GetDesc(), c = colour->GetDesc();
    if (d.Width == c.Width && d.Height == c.Height && d.SampleDesc.Count == c.SampleDesc.Count) return true;
    const uint64_t n = ++g_depth_size_mismatch;
    if (n <= 4 || (n & (n - 1)) == 0)
        std::fprintf(stderr,
                     "[rt] draws: a depth target of %llux%u (%u samples) with a colour target of %llux%u (%u): drawn "
                     "without depth; %llu such draws so far\n",
                     static_cast<unsigned long long>(d.Width), d.Height, d.SampleDesc.Count,
                     static_cast<unsigned long long>(c.Width), c.Height, c.SampleDesc.Count,
                     static_cast<unsigned long long>(n));
    return false;
}

// ---- the arguments of indirect commands, limited on the GPU
//
// An indirect command's counts are in the title's memory and its GPU passes write them (culling, tile
// classification) after the list is recorded, so only the GPU can look at what a command will really get. A record
// no pass filled, or one a pass that didn't run on the PC left stale, can ask for billions of vertices or groups:
// the GPU then sits in that one command, the submission never returns, and the title's 30 s watchdog ends the game
// (seen as soon as indirect draws and dispatches ran). So each indirect command reads a copy: one compute thread of
// the runtime's copies the record into a scratch buffer, emptied when its counts are beyond what a pass could mean
// (as VIctory limits them): a draw of 2^23 vertices or more, more than 65,536 instances or 2^26 vertices over the
// instances; a dispatch or a mesh draw with a count over 65,535 (the PC's own limit).
struct ArgumentLimit {
    std::mutex m;
    bool failed = false;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource* scratch[2] = {};  // lists of the direct queue, lists of the compute queue
    std::atomic<uint32_t> next[2];
};
ArgumentLimit g_limit;
constexpr uint32_t kLimitSlots = 1u << 17, kLimitSlotBytes = 32;

bool limit_ready() {
    ArgumentLimit& a = g_limit;
    std::lock_guard lock(a.m);
    if (a.pso) return true;
    if (a.failed) return false;
    a.failed = true;
    static const char kSource[] =
        "ByteAddressBuffer src : register(t0); RWByteAddressBuffer dst : register(u0);\n"
        "cbuffer c : register(b0) { uint kind; uint at; }\n"
        "[numthreads(1, 1, 1)] void cs() {\n"
        "  uint a = src.Load(0), b = src.Load(4), c2 = src.Load(8), d = 0, e = 0;\n"
        "  if (kind <= 1) d = src.Load(12);\n"
        "  if (kind == 1) e = src.Load(16);\n"
        "  bool ok = kind <= 1 ? a < (1u << 23) && b <= 65536 && (a == 0 || b <= (1u << 26) / a)\n"
        "                      : a <= 65535 && b <= 65535 && c2 <= 65535;\n"
        "  if (!ok) { a = 0; b = 0; c2 = 0; d = 0; e = 0; }\n"
        "  dst.Store(at, a); dst.Store(at + 4, b); dst.Store(at + 8, c2); dst.Store(at + 12, d); dst.Store(at + 16, e); }\n";
    using Compile = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    Compile compile = lib ? reinterpret_cast<Compile>(GetProcAddress(lib, "D3DCompile")) : nullptr;
    ID3DBlob *cs = nullptr, *sig = nullptr;
    ID3D12Device* dev = host_device();
    if (!compile || FAILED(compile(kSource, sizeof kSource - 1, "limit", nullptr, nullptr, "cs", "cs_5_0", 0, 0, &cs, nullptr))) {
        std::fprintf(stderr, "[rt] indirect: the shader that limits arguments didn't compile: they go unlimited\n");
        return false;
    }
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = {0, 0, 2};
    D3D12_ROOT_SIGNATURE_DESC rd{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = uint64_t{kLimitSlots} * kLimitSlotBytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&a.root))) ||
        FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&a.scratch[0]))) ||
        FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&a.scratch[1])))) {
        std::fprintf(stderr, "[rt] indirect: no root signature or scratch for limiting arguments: they go unlimited\n");
        return false;
    }
    pd.pRootSignature = a.root;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&a.pso)))) {
        a.pso = nullptr;
        std::fprintf(stderr, "[rt] indirect: no pipeline for limiting arguments: they go unlimited\n");
        return false;
    }
    a.failed = false;
    return true;
}

// Records the limited copy of the record at `*at` of `*buffer` (a chunk's buffer) and gives the scratch and the
// copy's place instead; `kind`: 0 a draw, 1 an indexed draw, 2 a dispatch, 3 a mesh draw. It sets the list's compute
// root signature and pipeline: the caller sets its own after. False (the record left as it is) when it can't be.
// After the indirect command, limit_done puts the scratch back in the common state.
bool limit_arguments(ID3D12GraphicsCommandList* host, int kind, ID3D12Resource** buffer, uint64_t* at) {
    if (!limit_ready() || (*at & 3)) return false;
    const int q = host->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : 0;
    ArgumentLimit& a = g_limit;
    const uint32_t slot = a.next[q].fetch_add(1) % kLimitSlots;
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {a.scratch[q], 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    host->ResourceBarrier(1, &b);
    host->SetComputeRootSignature(a.root);
    host->SetPipelineState(a.pso);
    host->SetComputeRootShaderResourceView(0, (*buffer)->GetGPUVirtualAddress() + *at);
    host->SetComputeRootUnorderedAccessView(1, a.scratch[q]->GetGPUVirtualAddress());
    const uint32_t k[2] = {static_cast<uint32_t>(kind), slot * kLimitSlotBytes};
    host->SetComputeRoot32BitConstants(2, 2, k, 0);
    host->Dispatch(1, 1, 1);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    host->ResourceBarrier(1, &b);
    *buffer = a.scratch[q];
    *at = uint64_t{slot} * kLimitSlotBytes;
    return true;
}

void limit_done(ID3D12GraphicsCommandList* host) {
    const int q = host->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : 0;
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {g_limit.scratch[q], 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON};
    host->ResourceBarrier(1, &b);
}

// The command signatures of the title's indirect draws on the PC: one argument, the draw's own.
ID3D12CommandSignature* draw_signature(bool indexed) {
    static std::mutex m;
    static ID3D12CommandSignature* made[2] = {};
    static bool tried[2] = {};
    std::lock_guard lock(m);
    if (!tried[indexed]) {
        tried[indexed] = true;
        D3D12_INDIRECT_ARGUMENT_DESC a{};
        a.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        const D3D12_COMMAND_SIGNATURE_DESC d{
            static_cast<UINT>(indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS)), 1, &a, 0};
        if (FAILED(host_device()->CreateCommandSignature(&d, nullptr, IID_PPV_ARGS(&made[indexed])))) {
            made[indexed] = nullptr;
            std::fprintf(stderr, "[rt] draws: no command signature for indirect %sdraws\n", indexed ? "indexed " : "");
        }
    }
    return made[indexed];
}

// Rectangle lists the runtime can't expand itself (no vertex buffer to rewrite: the vertex shader makes the
// corners; or an indirect draw, whose count the GPU writes), drawn through the geometry shader that completes each
// rectangle (d3d12x_rects.cpp). The title's tiled lighting draws them indirectly, as VIctory found.
std::atomic<uint64_t> g_shader_rects{0}, g_indirect_rects{0};

// Whether expand_rects can take a draw (its checks, without the copy).
bool rects_expandable(const GfxState& g, UINT count, UINT start) {
    const D3D12_VERTEX_BUFFER_VIEW& src = g.vb[0];
    const uint32_t stride = src.StrideInBytes;
    if (!g.vb_count || !stride || count < 3 || count % 3 || !g.pso) return false;
    for (const gpu::InputElement& e : g.pso->input)
        if (e.slot != 0 || e.per_instance || e.offset >= stride) return false;
    return uint64_t{count} / 3 * 6 * stride <= kMaxRectBytes &&
           readable(src.BufferLocation + uint64_t{start} * stride, count * stride);
}

void draw(GfxState& g, ID3D12GraphicsCommandList* host, bool indexed, UINT count, UINT instances, UINT start,
          INT base_vertex, UINT start_instance, uint64_t arguments) {
    count_and_log();
    const bool listed = diag::listing();
    if (!g.pso || g.pso->compute || g.pso->code[0].empty()) {
        ++g_no_shader;
        note_owed(g.pso);
        diag::skipped(diag::kNoShader);
        if (listed) note(g, "no-shader", indexed, count, instances, nullptr);
        return;
    }
    // targets
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[8];
    ID3D12Resource* tex[8];
    DXGI_FORMAT fmt[8];
    for (UINT i = 0; i < g.rtv_count; ++i)
        if (!host_rtv(g.rtv[i], &rtv[i], &tex[i], &fmt[i])) {
            ++g_no_target;
            diag::skipped(diag::kNoTarget);
            if (listed) note(g, "no-target", indexed, count, instances, nullptr);
            return;
        }
    D3D12_PRIMITIVE_TOPOLOGY topology = g.topology;
    if (topology > D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ && topology < D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
        topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  // the console's rectangle list (17): see expand_rects
    // the depth target, when the title bound one the PC has: tested and written as the pipeline's registers say
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    ID3D12Resource* depth = nullptr;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    bool depth_read_only = false;
    bool has_depth = g.dsv && host_dsv(g.dsv, &dsv, &depth, &depth_format, &depth_read_only);
    // Bound on the PC only for a pipeline that tests or writes it: the title keeps a depth target bound through
    // passes that don't (the interface, the loading screen), and the PC wants a bound depth target to be the colour
    // targets' size, which the console doesn't (with every depth target bound, the loading screen went black).
    if (has_depth && !title_uses_depth(*g.pso, depth_format)) has_depth = false;
    if (has_depth && g.rtv_count && !same_size(depth, tex[0])) has_depth = false;
    if (!has_depth) depth_format = DXGI_FORMAT_UNKNOWN;
    const bool shader_rects = g.topology == kRectList && !indexed && (arguments || !rects_expandable(g, count, start));
    ID3D12PipelineState* pso =
        host_pso(*g.pso, g.rtv_count, fmt, depth_format,
                 static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(topology_type(topology) | (shader_rects ? kRectTopology : 0)),
                 g.state_block);
    if (!pso || !ensure_heaps() || !ring_ready()) {
        ++g_no_pso;
        diag::skipped(diag::kNoPipeline);
        if (listed) {
            note(g, "no-pipeline", indexed, count, instances, g.rtv_count ? tex[0] : nullptr);
            if (!g.pso->meta[0].has_digest) {  // a pipeline that came as PC code (slot 86): its shaders and root signature
                wchar_t name[64];
                for (int k = 0; k < 2; ++k) {
                    swprintf(name, 64, L"stream_%p_%ls.dxbc", static_cast<const void*>(g.pso), k ? L"ps" : L"vs");
                    if (const auto b = g.pso->code[k].bytes()) diag::save(name, b->data(), b->size());
                }
                print_root(g);
            }
        }
        return;
    }
    ID3D12Resource* args = nullptr;
    uint64_t args_at = 0;
    ID3D12CommandSignature* signature = nullptr;
    if (arguments && (!(signature = draw_signature(indexed)) ||
                      !gpumem::host_buffer(arguments, indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS),
                                           &args, &args_at))) {
        ++g_no_buffer;
        diag::skipped(diag::kNoBuffer);
        return;
    }
    if (shader_rects) {
        if (arguments) ++g_indirect_rects;
        const uint64_t n = ++g_shader_rects;
        if ((n & (n - 1)) == 0)
            std::fprintf(stderr, "[rt] draws: %llu draws of rectangle lists through the geometry shader (%llu indirect)\n",
                         static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_indirect_rects.load()));
    }
    // buffers at their chunk's GPU address
    D3D12_VERTEX_BUFFER_VIEW vb[16] = {};
    UINT rect_count = 0;
    const bool rects = !arguments && g.topology == kRectList && !indexed &&
                       expand_rects(g, count, start, &vb[0], &rect_count);
    for (UINT i = rects ? 1 : 0; i < g.vb_count; ++i) {
        vb[i] = g.vb[i];
        if (g.vb[i].BufferLocation && !gpumem::host_va(g.vb[i].BufferLocation, g.vb[i].SizeInBytes, &vb[i].BufferLocation)) {
            ++g_no_buffer;
            diag::skipped(diag::kNoBuffer);
            if (listed) note(g, "no-buffer", indexed, count, instances, nullptr);
            return;
        }
    }
    D3D12_INDEX_BUFFER_VIEW ib = g.ib;
    if (indexed && (!g.ib.BufferLocation || !gpumem::host_va(g.ib.BufferLocation, g.ib.SizeInBytes, &ib.BufferLocation))) {
        ++g_no_buffer;
        diag::skipped(diag::kNoBuffer);
            if (listed) note(g, "no-buffer", indexed, count, instances, nullptr);
        return;
    }
    // user data: vertex then pixel, 256 bytes apart (CBV alignment)
    uint64_t at = g_ring.next.fetch_add(512) % kRing;
    if (at + 512 > kRing) at = g_ring.next.fetch_add(512) % kRing;
    user_data(g, g.pso->meta[0], reinterpret_cast<uint32_t*>(g_ring.cpu + at));
    user_data(g, g.pso->meta[1], reinterpret_cast<uint32_t*>(g_ring.cpu + at + 256));
    note_textures(g, g.pso->meta[0], reinterpret_cast<const uint32_t*>(g_ring.cpu + at));
    note_textures(g, g.pso->meta[1], reinterpret_cast<const uint32_t*>(g_ring.cpu + at + 256));
    note_states(g, g.rtv_count ? fmt[0] : DXGI_FORMAT_UNKNOWN);
    if (listed) {
        // the draw's first vertex: its first index (read from the title's index buffer) plus the base vertex
        uint64_t first = start;
        if (indexed) {
            const bool wide = g.ib.Format == DXGI_FORMAT_R32_UINT;
            const uint64_t ia = g.ib.BufferLocation + uint64_t{start} * (wide ? 4 : 2);
            first = !readable(ia, 4) ? 0
                    : wide       ? *reinterpret_cast<const uint32_t*>(ia)
                                 : *reinterpret_cast<const uint16_t*>(ia);
            first = static_cast<uint64_t>(static_cast<int64_t>(first) + base_vertex);
        }
        detail(g, reinterpret_cast<const uint32_t*>(g_ring.cpu + at),
               reinterpret_cast<const uint32_t*>(g_ring.cpu + at + 256), first);
    }

    ID3D12DescriptorHeap* heaps[] = {view_heap(), sampler_heap()};
    host->SetDescriptorHeaps(2, heaps);
    host->SetGraphicsRootSignature(abi_root(false));
    host->SetGraphicsRootConstantBufferView(0, g_ring.gpu + at);
    host->SetGraphicsRootConstantBufferView(1, g_ring.gpu + at + 256);
    host->SetGraphicsRootDescriptorTable(2, heaps[0]->GetGPUDescriptorHandleForHeapStart());
    host->SetGraphicsRootDescriptorTable(3, heaps[1]->GetGPUDescriptorHandleForHeapStart());
    host->SetPipelineState(pso);
    host->IASetPrimitiveTopology(topology);
    if (g.vb_count) host->IASetVertexBuffers(0, g.vb_count, vb);
    if (indexed) host->IASetIndexBuffer(&ib);
    if (g.viewport_count) host->RSSetViewports(g.viewport_count, g.viewports);
    if (g.scissor_count) {
        host->RSSetScissorRects(g.scissor_count, g.scissors);
    } else if (g.viewport_count) {
        D3D12_RECT r{static_cast<LONG>(g.viewports[0].TopLeftX), static_cast<LONG>(g.viewports[0].TopLeftY),
                     static_cast<LONG>(g.viewports[0].TopLeftX + g.viewports[0].Width),
                     static_cast<LONG>(g.viewports[0].TopLeftY + g.viewports[0].Height)};
        host->RSSetScissorRects(1, &r);
    }
    // the targets leave the common state for as long as the next draws use them too (hold_targets)
    ID3D12Resource* targets[GfxState::kHeld];
    D3D12_RESOURCE_STATES states[GfxState::kHeld];
    UINT barriers = 0;
    for (UINT i = 0; i < g.rtv_count && barriers < GfxState::kHeld; ++i) {
        targets[barriers] = tex[i];
        states[barriers++] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (has_depth && barriers < GfxState::kHeld) {
        // a view that leaves the depth as it is can be tested while the same draw's shaders read the texture
        targets[barriers] = depth;
        states[barriers++] = depth_read_only ? D3D12_RESOURCE_STATE_DEPTH_READ |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                             : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    hold_targets(g, host, targets, states, barriers);
    host->OMSetRenderTargets(g.rtv_count, rtv, FALSE, has_depth ? &dsv : nullptr);
    if (has_depth) host->OMSetStencilRef(g.stencil_ref_set ? g.stencil_ref : title_stencil_ref(*g.pso));
    if (arguments) {
        const bool limited = limit_arguments(host, indexed ? 1 : 0, &args, &args_at);
        if (limited) host->SetPipelineState(pso);  // (the limiting pass set its own)
        host->ExecuteIndirect(signature, 1, args, args_at, nullptr, 0);
        if (limited) limit_done(host);
    } else if (indexed) host->DrawIndexedInstanced(count, instances, start, base_vertex, start_instance);
    else if (rects) host->DrawInstanced(rect_count, instances, 0, start_instance);
    else host->DrawInstanced(count, instances, start, start_instance);
    ++g_drawn;
    diag::drawn(tex, g.rtv_count);
    if (listed) note(g, "drawn", indexed, count, instances, g.rtv_count ? tex[0] : nullptr);
}

// ---- mesh draws
//
// The title's mesh pipelines draw with DispatchMesh(groups, 1, 1), most of them through ExecuteIndirect with the
// three dwords in its own memory (written by its culling passes on the GPU: zero when the list is recorded). The
// PC draws the same way: the translated mesh shader and the pipeline's pixel shader, the user data of each as a
// draw's, and for an indirect one the chunk's buffer as the argument buffer, read when the list runs. No vertex or
// index buffers: the mesh shader reads the title's memory itself.
std::atomic<uint64_t> g_mesh_seen{0}, g_mesh_drawn{0}, g_mesh_no_shader{0}, g_mesh_no_target{0}, g_mesh_no_pso{0},
    g_mesh_no_args{0};

std::atomic<ID3D12Resource*> g_mesh_targets[8];
std::atomic<UINT> g_mesh_target_count{0};

ID3D12Resource* last_mesh_target(int k) {
    return k >= 0 && static_cast<UINT>(k) < g_mesh_target_count.load() ? g_mesh_targets[k].load() : nullptr;
}

// The mesh draws recorded lately, per PC list in order (as the dispatches are kept): for a device removed in one.
struct RecentMesh {
    ID3D12GraphicsCommandList* list;
    uint8_t ms[16], ps[16];
    UINT groups;
    uint64_t arguments;
    uint32_t now[3];  // the argument dwords when the draw was recorded
};
constexpr uint64_t kRecentMeshes = 4096;
RecentMesh g_recent_meshes[kRecentMeshes];
std::atomic<uint64_t> g_recent_mesh_next{0};

void report_mesh_draws(ID3D12GraphicsCommandList* list, unsigned ran) {
    const uint64_t end = g_recent_mesh_next.load();
    const uint64_t begin = end > kRecentMeshes ? end - kRecentMeshes : 0;
    std::vector<const RecentMesh*> on;
    for (uint64_t i = begin; i < end; ++i)
        if (g_recent_meshes[i % kRecentMeshes].list == list) on.push_back(&g_recent_meshes[i % kRecentMeshes]);
    std::fprintf(stderr, "[rt] dred: %zu mesh draw(s) remembered on that list; %u ran before the stop\n", on.size(), ran);
    for (size_t i = ran > 2 ? ran - 2 : 0; i < on.size() && i < size_t{ran} + 3; ++i) {
        const RecentMesh& d = *on[i];
        std::fprintf(stderr, "[rt] dred:   %s mesh draw %zu: ", i == ran ? ">" : " ", i);
        if (d.arguments) {
            uint32_t w[3] = {};
            const bool ok = readable(d.arguments, 12);
            if (ok) std::memcpy(w, reinterpret_cast<const void*>(d.arguments), 12);
            std::fprintf(stderr, "indirect at %llx (recorded as %u %u %u, now %u %u %u%s)",
                         static_cast<unsigned long long>(d.arguments), d.now[0], d.now[1], d.now[2], w[0], w[1], w[2],
                         ok ? "" : ": unreadable");
        } else {
            std::fprintf(stderr, "%u group(s)", d.groups);
        }
        std::fprintf(stderr, ", ms ");
        for (uint8_t v : d.ms) std::fprintf(stderr, "%02x", v);
        std::fprintf(stderr, " ps ");
        for (uint8_t v : d.ps) std::fprintf(stderr, "%02x", v);
        std::fprintf(stderr, "\n");
    }
}

ID3D12CommandSignature* mesh_signature() {
    static std::mutex m;
    static ID3D12CommandSignature* made = nullptr;
    static std::atomic<bool> tried{false};
    if (tried.load(std::memory_order_acquire)) return made;  // made once; every indirect mesh draw asks
    std::lock_guard lock(m);
    if (!tried.load()) {
        D3D12_INDIRECT_ARGUMENT_DESC a{};
        a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        const D3D12_COMMAND_SIGNATURE_DESC d{sizeof(D3D12_DISPATCH_MESH_ARGUMENTS), 1, &a, 0};
        if (FAILED(host_device()->CreateCommandSignature(&d, nullptr, IID_PPV_ARGS(&made)))) {
            made = nullptr;
            std::fprintf(stderr, "[rt] draws: no command signature for indirect mesh draws\n");
        }
        tried.store(true, std::memory_order_release);
    }
    return made;
}

// ---- what the card does with a mesh draw, on request (the trigger file <diag>\meshstats, see mesh_stats_frame)
//
// The next kMeshStats mesh draws are each put inside a pipeline-statistics query: the primitives that reach the
// clipper (what the mesh shader gave out and didn't cull), the primitives left after clipping, and the pixel
// shader's invocations. With the draw's viewport and counts, that says where the geometry of a draw that leaves
// its targets empty is lost: in the shader's outputs, outside the view, or in the pixel stage.
constexpr int kMeshStats = 48;
struct MeshStat {
    uint8_t ms[16], ps[16];
    D3D12_VIEWPORT viewport;
    UINT viewports, scissors, targets, groups;
    uint32_t args[3];
    D3D12_RECT scissor;
};
struct MeshStats {
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* buffer = nullptr;
    std::atomic<int> next{kMeshStats};  // the next slot; kMeshStats and past it: not asked for
    std::atomic<uint64_t> asked_at{0};  // the frame it was asked at (0: nothing to report)
    MeshStat stat[kMeshStats];
};
MeshStats g_mesh_stats;  // a global: the runtime has no thread-safe statics

void mesh_stats_frame(uint64_t frame, bool asked) {
    MeshStats& s = g_mesh_stats;
    if (asked && !s.asked_at.load()) {
        ID3D12Device* dev = host_device();
        if (!s.heap && dev) {
            D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, kMeshStats, 0};
            D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0,
                                   kMeshStats * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), 1, 1, 1,
                                   DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&s.heap))) ||
                FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&s.buffer)))) {
                std::fprintf(stderr, "[rt] mesh stats: the query heap or its buffer couldn't be made\n");
                return;
            }
        }
        if (s.heap) {
            s.asked_at = frame ? frame : 1;
            s.next = 0;
            std::fprintf(stderr, "[rt] mesh stats: the next %d mesh draws are measured\n", kMeshStats);
        }
        return;
    }
    const uint64_t at = s.asked_at.load();
    if (!at || frame < at + 6) return;  // their lists have run by now
    const int n = std::min(s.next.exchange(kMeshStats), kMeshStats);
    s.asked_at = 0;
    D3D12_QUERY_DATA_PIPELINE_STATISTICS* q = nullptr;
    D3D12_RANGE all{0, kMeshStats * sizeof *q};
    if (FAILED(s.buffer->Map(0, &all, reinterpret_cast<void**>(&q)))) return;
    std::fprintf(stderr, "[rt] mesh stats: %d mesh draws\n", n);
    uint64_t in = 0, out = 0, px = 0;
    for (int i = 0; i < n; ++i) {
        const MeshStat& m = s.stat[i];
        char ms[13], ps[13];
        for (int k = 0; k < 6; ++k) std::snprintf(ms + 2 * k, 3, "%02x", m.ms[k]), std::snprintf(ps + 2 * k, 3, "%02x", m.ps[k]);
        in += q[i].CInvocations, out += q[i].CPrimitives, px += q[i].PSInvocations;
        std::fprintf(stderr,
                     "[rt] mesh stats:  #%d ms %s ps %s: %llu primitives to the clipper, %llu after it, %llu pixel "
                     "shader runs; groups %u, arguments %u %u %u, %u target(s), viewport %.0fx%.0f at %.0f,%.0f depth "
                     "%.2f..%.2f (%u set), scissor %ld,%ld-%ld,%ld (%u set)\n",
                     i, ms, ps, static_cast<unsigned long long>(q[i].CInvocations),
                     static_cast<unsigned long long>(q[i].CPrimitives),
                     static_cast<unsigned long long>(q[i].PSInvocations), m.groups, m.args[0], m.args[1], m.args[2],
                     m.targets, m.viewport.Width, m.viewport.Height, m.viewport.TopLeftX, m.viewport.TopLeftY,
                     m.viewport.MinDepth, m.viewport.MaxDepth, m.viewports, m.scissor.left, m.scissor.top,
                     m.scissor.right, m.scissor.bottom, m.scissors);
    }
    std::fprintf(stderr, "[rt] mesh stats: in all %llu primitives to the clipper, %llu after it, %llu pixel shader runs\n",
                 static_cast<unsigned long long>(in), static_cast<unsigned long long>(out),
                 static_cast<unsigned long long>(px));
    D3D12_RANGE none{0, 0};
    s.buffer->Unmap(0, &none);
}

void draw_mesh(GfxState& g, ID3D12GraphicsCommandList* host, UINT groups, uint64_t arguments) {
    const uint64_t n = ++g_mesh_seen;
    if ((n & (n - 1)) == 0 || n % 50000 == 0)
        std::fprintf(stderr,
                     "[rt] mesh draws: %llu seen, %llu drawn; not drawn: %llu no translated shader, %llu no PC "
                     "pipeline (or not ready), %llu targets the PC lacks, %llu arguments not on the GPU\n",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_mesh_drawn.load()),
                     static_cast<unsigned long long>(g_mesh_no_shader.load()),
                     static_cast<unsigned long long>(g_mesh_no_pso.load()),
                     static_cast<unsigned long long>(g_mesh_no_target.load()),
                     static_cast<unsigned long long>(g_mesh_no_args.load()));
    if (!g.pso || !g.pso->mesh || g.pso->code[0].empty()) {
        ++g_mesh_no_shader;
        note_owed(g.pso);
        static std::atomic<int> told{0};
        if (g.pso && told++ < 4) {  // what such a pipeline was made from
            const PipelineState& p = *g.pso;
            std::fprintf(stderr, "[rt] mesh draws: pipeline %p has no mesh shader (mesh %d, compute %d, from a stream %d, "
                                 "code %zu / %zu bytes, %zu stage(s) missing); its desc (%zu bytes kept):",
                         static_cast<const void*>(&p), p.mesh, p.compute, p.from_stream, p.code[0].size(),
                         p.code[1].size(), p.missing.size(), p.stream.size());
            for (size_t i = 0; i + 8 <= p.stream.size() && i < 384; i += 8) {
                uint64_t w;
                std::memcpy(&w, p.stream.data() + i, 8);
                std::fprintf(stderr, "%s%llx", i % 64 ? " " : "\n[rt] mesh draws:   ", static_cast<unsigned long long>(w));
            }
            std::fprintf(stderr, "\n");
        }
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[8];
    ID3D12Resource* tex[8];
    DXGI_FORMAT fmt[8];
    for (UINT i = 0; i < g.rtv_count; ++i)
        if (!host_rtv(g.rtv[i], &rtv[i], &tex[i], &fmt[i])) {
            ++g_mesh_no_target;
            return;
        }
    // the depth target, when the title bound one the PC has: tested and written as the pipeline's registers say
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    ID3D12Resource* depth = nullptr;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    bool depth_read_only = false;
    bool has_depth = g.dsv && host_dsv(g.dsv, &dsv, &depth, &depth_format, &depth_read_only);
    // Bound on the PC only for a pipeline that tests or writes it: the title keeps a depth target bound through
    // passes that don't (the interface, the loading screen), and the PC wants a bound depth target to be the colour
    // targets' size, which the console doesn't (with every depth target bound, the loading screen went black).
    if (has_depth && !title_uses_depth(*g.pso, depth_format)) has_depth = false;
    if (has_depth && g.rtv_count && !same_size(depth, tex[0])) has_depth = false;
    if (!has_depth) depth_format = DXGI_FORMAT_UNKNOWN;
    ID3D12PipelineState* pso = host_mesh_pso(*g.pso, g.rtv_count, fmt, depth_format, g.state_block);
    if (!pso || !ensure_heaps() || !ring_ready()) {
        ++g_mesh_no_pso;
        return;
    }
    ID3D12Resource* args = nullptr;
    uint64_t args_at = 0;
    ID3D12CommandSignature* signature = nullptr;
    if (arguments && (!(signature = mesh_signature()) ||
                      !gpumem::host_buffer(arguments, sizeof(D3D12_DISPATCH_MESH_ARGUMENTS), &args, &args_at))) {
        ++g_mesh_no_args;
        return;
    }
    ID3D12GraphicsCommandList6* list = nullptr;
    if (FAILED(host->QueryInterface(IID_PPV_ARGS(&list)))) {
        ++g_mesh_no_pso;
        return;
    }
    // user data: mesh then pixel, 256 bytes apart (CBV alignment)
    uint64_t at = g_ring.next.fetch_add(512) % kRing;
    if (at + 512 > kRing) at = g_ring.next.fetch_add(512) % kRing;
    user_data(g, g.pso->meta[0], reinterpret_cast<uint32_t*>(g_ring.cpu + at));
    user_data(g, g.pso->meta[1], reinterpret_cast<uint32_t*>(g_ring.cpu + at + 256));
    {
        // the first mesh draws of a few mesh shaders: the stage's bindings as read from its block, and the user
        // SGPRs they gave (a mesh program that finds a table address or a constant missing reads zeros for its
        // meshlet and gives out one vertex and one primitive)
        static std::mutex m;
        static uint8_t seen[6][16];
        static int count = 0;
        static std::atomic<bool> full{false};  // all six seen: nothing to look at (no lock on every mesh draw)
        std::unique_lock lock(m, std::defer_lock);
        if (!full.load(std::memory_order_relaxed)) lock.lock();
        if (full.load(std::memory_order_relaxed) || count >= 6) full = true;
        bool known = full.load(std::memory_order_relaxed);
        for (int i = 0; i < count; ++i) known = known || std::memcmp(seen[i], g.pso->meta[0].digest, 16) == 0;
        if (!known && count < 6) {
            std::memcpy(seen[count++], g.pso->meta[0].digest, 16);
            const gpu::StageMeta& sm = g.pso->meta[0];
            const uint32_t* ud = reinterpret_cast<const uint32_t*>(g_ring.cpu + at);
            std::fprintf(stderr, "[rt] mesh draws: ms ");
            for (int i = 0; i < 6; ++i) std::fprintf(stderr, "%02x", sm.digest[i]);
            std::fprintf(stderr, ": %zu binding(s) (kind/register/sgpr/count/table dwords):", sm.bindings.size());
            for (const gpu::StageBinding& b : sm.bindings)
                std::fprintf(stderr, " %x/%u/s%u/%u/%x", b.kind, b.reg, b.sgpr, b.count, b.table_dwords);
            std::fprintf(stderr, "\n[rt] mesh draws:   user SGPRs:");
            for (int i = 0; i < 32; ++i) std::fprintf(stderr, " s%d=%x", i, ud[i]);
            std::fprintf(stderr, "\n[rt] mesh draws:   root signature %p", static_cast<void*>(g.root));
            if (g.root && g.root->desc) {
                const D3D12_ROOT_SIGNATURE_DESC1& d = *g.root->desc;
                std::fprintf(stderr, ", %u parameter(s):", d.NumParameters);
                for (UINT i = 0; i < d.NumParameters && i < 24; ++i) {
                    const D3D12_ROOT_PARAMETER1& q = d.pParameters[i];
                    if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                        std::fprintf(stderr, " [%u table of %u range(s), first type %d reg %u; address %llx]", i,
                                     q.DescriptorTable.NumDescriptorRanges,
                                     q.DescriptorTable.NumDescriptorRanges ? q.DescriptorTable.pDescriptorRanges[0].RangeType : -1,
                                     q.DescriptorTable.NumDescriptorRanges ? q.DescriptorTable.pDescriptorRanges[0].BaseShaderRegister : 0,
                                     static_cast<unsigned long long>(g.tables[i]));
                    else if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
                        std::fprintf(stderr, " [%u constants reg %u x%u: %x %x ...]", i, q.Constants.ShaderRegister,
                                     q.Constants.Num32BitValues, g.constants[i][0], g.constants[i][1]);
                    else
                        std::fprintf(stderr, " [%u view type %d reg %u at %llx]", i, q.ParameterType,
                                     q.Descriptor.ShaderRegister, static_cast<unsigned long long>(g.root_va[i]));
                }
            }
            std::fprintf(stderr, "\n");
        }
    }
    note_textures(g, g.pso->meta[0], reinterpret_cast<const uint32_t*>(g_ring.cpu + at));
    note_textures(g, g.pso->meta[1], reinterpret_cast<const uint32_t*>(g_ring.cpu + at + 256));
    note_states(g, g.rtv_count ? fmt[0] : DXGI_FORMAT_UNKNOWN);
    {
        // A mesh program's user data starts at s8: s0-s7 are the stage's own inputs (s3 the wave's index, ...). A
        // record's user dword n is SGPR n + 8 there (MSNC_VS_Cable loads its meshlet buffer from the table whose
        // record says dword 9 through s17, and takes the root view of dwords 11-12 from s19 and s20). Left at
        // s8, every table address reached the program as zero: it read zeros for its meshlet and gave out one
        // vertex and one primitive a group (measured: N primitives to the clipper for N groups, none on screen).
        uint32_t* ud = reinterpret_cast<uint32_t*>(g_ring.cpu + at);
        std::memmove(ud + 8, ud, 24 * sizeof(uint32_t));
        std::memset(ud, 0, 8 * sizeof(uint32_t));
    }
    ID3D12DescriptorHeap* heaps[] = {view_heap(), sampler_heap()};
    host->SetDescriptorHeaps(2, heaps);
    host->SetGraphicsRootSignature(abi_mesh_root());
    host->SetGraphicsRootConstantBufferView(0, g_ring.gpu + at);
    host->SetGraphicsRootConstantBufferView(1, g_ring.gpu + at + 256);
    host->SetGraphicsRootDescriptorTable(2, heaps[0]->GetGPUDescriptorHandleForHeapStart());
    host->SetGraphicsRootDescriptorTable(3, heaps[1]->GetGPUDescriptorHandleForHeapStart());
    host->SetPipelineState(pso);
    if (g.viewport_count) host->RSSetViewports(g.viewport_count, g.viewports);
    if (g.scissor_count) {
        host->RSSetScissorRects(g.scissor_count, g.scissors);
    } else if (g.viewport_count) {
        D3D12_RECT r{static_cast<LONG>(g.viewports[0].TopLeftX), static_cast<LONG>(g.viewports[0].TopLeftY),
                     static_cast<LONG>(g.viewports[0].TopLeftX + g.viewports[0].Width),
                     static_cast<LONG>(g.viewports[0].TopLeftY + g.viewports[0].Height)};
        host->RSSetScissorRects(1, &r);
    }
    // the targets leave the common state for as long as the next draws use them too (hold_targets)
    ID3D12Resource* targets[GfxState::kHeld];
    D3D12_RESOURCE_STATES states[GfxState::kHeld];
    UINT barriers = 0;
    for (UINT i = 0; i < g.rtv_count && barriers < GfxState::kHeld; ++i) {
        targets[barriers] = tex[i];
        states[barriers++] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (has_depth && barriers < GfxState::kHeld) {
        // a view that leaves the depth as it is can be tested while the same draw's shaders read the texture
        targets[barriers] = depth;
        states[barriers++] = depth_read_only ? D3D12_RESOURCE_STATE_DEPTH_READ |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                             : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    hold_targets(g, host, targets, states, barriers);
    host->OMSetRenderTargets(g.rtv_count, rtv, FALSE, has_depth ? &dsv : nullptr);
    if (has_depth) host->OMSetStencilRef(g.stencil_ref_set ? g.stencil_ref : title_stencil_ref(*g.pso));
    {
        RecentMesh& d = g_recent_meshes[g_recent_mesh_next.fetch_add(1) % kRecentMeshes];
        d.list = host;
        std::memcpy(d.ms, g.pso->meta[0].digest, 16);
        std::memcpy(d.ps, g.pso->meta[1].digest, 16);
        d.groups = groups;
        d.arguments = arguments;
        d.now[0] = d.now[1] = d.now[2] = 0;
        if (arguments && readable(arguments, 12)) std::memcpy(d.now, reinterpret_cast<const void*>(arguments), 12);
    }
    int stat = g_mesh_stats.next.load() < kMeshStats ? g_mesh_stats.next.fetch_add(1) : kMeshStats;
    if (stat < kMeshStats) {
        MeshStat& m = g_mesh_stats.stat[stat];
        std::memcpy(m.ms, g.pso->meta[0].digest, 16);
        std::memcpy(m.ps, g.pso->meta[1].digest, 16);
        m.viewport = g.viewport_count ? g.viewports[0] : D3D12_VIEWPORT{};
        m.scissor = g.scissor_count ? g.scissors[0] : D3D12_RECT{};
        m.viewports = g.viewport_count, m.scissors = g.scissor_count, m.targets = g.rtv_count, m.groups = groups;
        m.args[0] = m.args[1] = m.args[2] = 0;
        if (arguments && readable(arguments, 12)) std::memcpy(m.args, reinterpret_cast<const void*>(arguments), 12);
        list->BeginQuery(g_mesh_stats.heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, stat);
    }
    if (arguments) {
        const bool limited = limit_arguments(host, 3, &args, &args_at);
        if (limited) host->SetPipelineState(pso);  // (the limiting pass set its own)
        list->ExecuteIndirect(signature, 1, args, args_at, nullptr, 0);
        if (limited) limit_done(host);
    } else {
        list->DispatchMesh(groups, 1, 1);
    }
    if (stat < kMeshStats) {
        list->EndQuery(g_mesh_stats.heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, stat);
        list->ResolveQueryData(g_mesh_stats.heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, stat, 1, g_mesh_stats.buffer,
                               stat * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
    }
    list->Release();
    if (g.rtv_count >= g_mesh_target_count.load()) {  // the widest set of targets: the geometry buffers
        for (UINT i = 0; i < g.rtv_count; ++i) g_mesh_targets[i] = tex[i];
        g_mesh_target_count = g.rtv_count;
    }
    ++g_mesh_drawn;
    diag::drawn(tex, g.rtv_count);
}

// Dispatches on the PC: the compute pipeline's translated shader with the ABI compute root signature (param 0 its 36
// user-data dwords from its metadata, 2 / 3 the bindless heaps), then a UAV barrier so what it wrote is there for
// what follows. Not run: no compute pipeline bound, or one whose shader isn't translated.
std::atomic<uint64_t> g_dispatches{0}, g_dispatched{0}, g_no_cpso{0}, g_not_compute{0}, g_no_cs{0}, g_no_hpso{0};

// The dispatches recorded lately, per PC list in order: when the device is removed in one (DRED names the list and
// how many of its operations ran), which compute shader it was.
struct RecentDispatch {
    ID3D12GraphicsCommandList* list;
    uint8_t digest[16];
    bool has_digest;
    UINT x, y, z;
    size_t dxil_bytes;
};
constexpr uint64_t kRecentDispatches = 1024;
RecentDispatch g_recent_dispatches[kRecentDispatches];
std::atomic<uint64_t> g_recent_dispatch_next{0};

void report_dispatches(ID3D12GraphicsCommandList* list, unsigned ran) {
    // the newest run of entries on this list: walk back to its first, then forward
    const uint64_t end = g_recent_dispatch_next.load();
    const uint64_t begin = end > kRecentDispatches ? end - kRecentDispatches : 0;
    std::vector<const RecentDispatch*> on;
    for (uint64_t i = begin; i < end; ++i)
        if (g_recent_dispatches[i % kRecentDispatches].list == list) on.push_back(&g_recent_dispatches[i % kRecentDispatches]);
    std::fprintf(stderr, "[rt] dred: %zu dispatch(es) remembered on that list; %u ran before the stop\n", on.size(), ran);
    for (size_t i = 0; i < on.size(); ++i) {
        const RecentDispatch& d = *on[i];
        std::fprintf(stderr, "[rt] dred:   %s dispatch %zu: %u x %u x %u, DXIL %zu bytes, cs ", i == ran ? ">" : " ", i,
                     d.x, d.y, d.z, d.dxil_bytes);
        if (d.has_digest)
            for (uint8_t v : d.digest) std::fprintf(stderr, "%02x", v);
        else std::fprintf(stderr, "(no digest)");
        std::fprintf(stderr, "\n");
    }
}

// The dispatches not run, by compute shader: [0] no translated shader, [1] no PC pipeline (being made, or refused).
// A shader with no digest is PC code the title brought itself (CreateComputePipelineState with a DXIL container).
std::mutex g_owed_dispatch_lock;
std::map<std::array<uint8_t, 16>, std::array<uint64_t, 2>> g_owed_dispatches;

void note_owed_dispatch(const PipelineState& p, int why) {
    std::array<uint8_t, 16> d{};
    if (why == 0 && !p.missing.empty()) d = p.missing[0];
    else if (p.meta[0].has_digest) std::memcpy(d.data(), p.meta[0].digest, 16);
    std::lock_guard lock(g_owed_dispatch_lock);
    if (g_owed_dispatches.size() < 4096 || g_owed_dispatches.count(d)) ++g_owed_dispatches[d][why];
}

void report_owed_dispatches() {
    std::vector<std::pair<uint64_t, std::array<uint8_t, 16>>> v[2];
    {
        std::lock_guard lock(g_owed_dispatch_lock);
        for (const auto& [d, n] : g_owed_dispatches)
            for (int k = 0; k < 2; ++k)
                if (n[k]) v[k].push_back({n[k], d});
    }
    for (int k = 0; k < 2; ++k) {
        std::sort(v[k].begin(), v[k].end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::fprintf(stderr, "[rt] dispatches owed: %zu compute shader(s) with %s; the most asked:\n", v[k].size(),
                     k ? "no PC pipeline" : "no translation");
        for (size_t i = 0; i < v[k].size() && i < 40; ++i) {
            std::fprintf(stderr, "[rt] dispatches owed:   %s ", k ? "p" : "t");
            for (uint8_t b : v[k][i].second) std::fprintf(stderr, "%02x", b);
            std::fprintf(stderr, " %llu\n", static_cast<unsigned long long>(v[k][i].first));
        }
    }
}

ID3D12CommandSignature* dispatch_signature() {
    static std::mutex m;
    static ID3D12CommandSignature* made = nullptr;
    static bool tried = false;
    std::lock_guard lock(m);
    if (!tried) {
        tried = true;
        D3D12_INDIRECT_ARGUMENT_DESC a{};
        a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        const D3D12_COMMAND_SIGNATURE_DESC d{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &a, 0};
        if (FAILED(host_device()->CreateCommandSignature(&d, nullptr, IID_PPV_ARGS(&made)))) {
            made = nullptr;
            std::fprintf(stderr, "[rt] dispatches: no command signature for indirect dispatches\n");
        }
    }
    return made;
}

std::atomic<uint64_t> g_indirect_dispatches{0}, g_indirect_dispatched{0};

// RT_NO_DISPATCH=1 (a diagnostic): no compute dispatch runs. The title's CPU memory is in the pool every translated
// shader can write through the chunks' UAVs: with it on, a crash that goes away points at a shader writing out of
// bounds.
const bool g_no_dispatch = [] {
    char v[4] = {};
    return GetEnvironmentVariableA("RT_NO_DISPATCH", v, sizeof v) && v[0] == '1';
}();

void dispatch(GfxState& c, ID3D12GraphicsCommandList* host, UINT x, UINT y, UINT z, uint64_t arguments) {
    if (g_no_dispatch) return;
    uint64_t n = ++g_dispatches;
    if (arguments) ++g_indirect_dispatches;
    bool ran = false;
    if (c.pso && c.pso->compute && c.pso->code[0].empty()) note_owed_dispatch(*c.pso, 0);
    if (!c.pso) ++g_no_cpso;
    else if (!c.pso->compute) ++g_not_compute;
    else if (c.pso->code[0].empty() && ++g_no_cs <= 1 && !c.pso->missing.empty()) {
        std::fprintf(stderr, "[rt] dispatches: a compute shader not in the cache: ");
        for (uint8_t v : c.pso->missing[0]) std::fprintf(stderr, "%02x", v);
        std::fprintf(stderr, "\n");
    }
    if (c.pso && c.pso->compute && !c.pso->code[0].empty() && x && y && z) {
        ID3D12PipelineState* pso = host_compute_pso(*c.pso);
        ID3D12Resource* args = nullptr;
        uint64_t args_at = 0;
        ID3D12CommandSignature* signature = nullptr;
        if (arguments && (!(signature = dispatch_signature()) ||
                          !gpumem::host_buffer(arguments, sizeof(D3D12_DISPATCH_ARGUMENTS), &args, &args_at)))
            pso = nullptr;  // (counted with the dispatches that have no PC pipeline)
        if (pso && ensure_heaps() && ring_ready()) {
            const bool limited = arguments && limit_arguments(host, 2, &args, &args_at);
            uint64_t at = g_ring.next.fetch_add(256) % kRing;
            if (at + 256 > kRing) at = g_ring.next.fetch_add(256) % kRing;
            auto* ud = reinterpret_cast<uint32_t*>(g_ring.cpu + at);
            user_data(c, c.pso->meta[0], ud);
            note_textures(c, c.pso->meta[0], ud);
            ID3D12DescriptorHeap* heaps[] = {view_heap(), sampler_heap()};
            host->SetDescriptorHeaps(2, heaps);
            host->SetComputeRootSignature(abi_root(true));
            host->SetComputeRootConstantBufferView(0, g_ring.gpu + at);
            host->SetComputeRootConstantBufferView(1, g_ring.gpu + at);  // the unused placeholder, set all the same
            host->SetComputeRootDescriptorTable(2, heaps[0]->GetGPUDescriptorHandleForHeapStart());
            host->SetComputeRootDescriptorTable(3, heaps[1]->GetGPUDescriptorHandleForHeapStart());
            host->SetPipelineState(pso);
            {
                RecentDispatch& d = g_recent_dispatches[g_recent_dispatch_next.fetch_add(1) % kRecentDispatches];
                d.list = host;
                d.has_digest = c.pso->meta[0].has_digest;
                std::memcpy(d.digest, c.pso->meta[0].digest, 16);
                d.x = x;
                d.y = y;
                d.z = z;
                d.dxil_bytes = c.pso->code[0].size();
            }
            if (arguments) {
                host->ExecuteIndirect(signature, 1, args, args_at, nullptr, 0);
                if (limited) limit_done(host);
                ++g_indirect_dispatched;
            } else {
                host->Dispatch(x, y, z);
            }
            D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_UAV};
            host->ResourceBarrier(1, &b);
            ran = true;
            ++g_dispatched;
        } else {
            ++g_no_hpso;
            note_owed_dispatch(*c.pso, 1);
        }
    }
    if ((n & (n - 1)) == 0 && n >= 16384) report_owed_dispatches();
    if ((n & (n - 1)) == 0)
        std::fprintf(stderr,
                     "[rt] dispatches: %llu seen, %llu run; not run: %llu no pipeline, %llu not a compute one, %llu "
                     "no translated shader, %llu no PC pipeline\n",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_dispatched.load()),
                     static_cast<unsigned long long>(g_no_cpso.load()), static_cast<unsigned long long>(g_not_compute.load()),
                     static_cast<unsigned long long>(g_no_cs.load()), static_cast<unsigned long long>(g_no_hpso.load()));
    (void)ran;
}

}  // namespace rt::d3d12x
