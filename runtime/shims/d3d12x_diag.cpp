// Frame diagnostics (see d3d12x_internal.h): what a frame's commands did on the PC and what the textures hold.
//
// Counting is per frame as commands are recorded (lists are recorded and submitted within the frame they're
// presented in, near enough for a table). The read back happens after the frame's present, on the presenting queue,
// with the textures in the common state they're kept in between our commands.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "com_probe.h"
#include "d3d12x_internal.h"

namespace rt::d3d12x::diag {
namespace {

struct Target {
    ID3D12Resource* tex;
    uint64_t draws, clears;
};

struct State {
    std::mutex m;
    bool init = false, on = false;
    std::vector<uint64_t> frames;  // the frames to report
    uint64_t armed_until = 0;      // the trigger file: frames are listed until one with 3+ draws, reported whole
    uint64_t frame_draws = 0;      // draws recorded in the frame so far
    uint64_t current = 1;          // the frame being recorded
    std::vector<Target> targets;   // this frame's
    std::vector<ID3D12Resource*> sampled;  // textures its shaders read (held)
    uint64_t skips[kSkips] = {};
    uint64_t dispatches = 0;
    std::wstring dir;
};
State g;  // a global: the runtime has no thread-safe statics

void init_locked() {
    if (g.init) return;
    g.init = true;
    char list[256];
    DWORD n = GetEnvironmentVariableA("RT_DIAG_FRAMES", list, sizeof list);
    if (!n || n >= sizeof list) return;
    for (char* p = list; *p;) {
        char* end = nullptr;
        unsigned long long f = std::strtoull(p, &end, 10);
        if (end == p) break;
        if (f) g.frames.push_back(f);
        p = *end ? end + 1 : end;
    }
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    g.dir = std::wstring(local) + L"\\Flamingo";
    CreateDirectoryW(g.dir.c_str(), nullptr);
    g.dir += L"\\diag";
    CreateDirectoryW(g.dir.c_str(), nullptr);
    g.on = true;  // RT_DIAG_FRAMES=0: only frames asked for through the trigger file
    std::fprintf(stderr, "[rt] diag: reporting %zu frame(s), images in %ls; creating %ls\\now reports the next frame\n",
                 g.frames.size(), g.dir.c_str(), g.dir.c_str());
}

Target& target_locked(ID3D12Resource* t) {
    for (Target& x : g.targets)
        if (x.tex == t) return x;
    t->AddRef();  // kept alive until the frame's report
    g.targets.push_back({t, 0, 0});
    return g.targets.back();
}

// ---- pixels

float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(static_cast<float>(m), -24)
              : e == 31 ? (m ? NAN : INFINITY)
                        : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}
float f11(uint32_t v) {  // 6-bit exponent, 6-bit mantissa (5 for the 10-bit one, handled by the caller's shift)
    uint32_t e = (v >> 6) & 31, m = v & 63;
    if (e == 0) return std::ldexp(static_cast<float>(m), -20);
    if (e == 31) return INFINITY;
    return std::ldexp(static_cast<float>(m | 64), static_cast<int>(e) - 21);
}
float f10(uint32_t v) {
    uint32_t e = (v >> 5) & 31, m = v & 31;
    if (e == 0) return std::ldexp(static_cast<float>(m), -19);
    if (e == 31) return INFINITY;
    return std::ldexp(static_cast<float>(m | 32), static_cast<int>(e) - 20);
}

// RGBA of one texel as floats; false for a format this doesn't read.
bool texel(DXGI_FORMAT f, const uint8_t* p, float out[4]) {
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.f;
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        out[0] = p[2] / 255.f, out[1] = p[1] / 255.f, out[2] = p[0] / 255.f, out[3] = p[3] / 255.f;
        return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t v;
        std::memcpy(&v, p, 4);
        out[0] = (v & 1023) / 1023.f, out[1] = ((v >> 10) & 1023) / 1023.f, out[2] = ((v >> 20) & 1023) / 1023.f;
        out[3] = (v >> 30) / 3.f;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        uint16_t h[4];
        std::memcpy(h, p, 8);
        for (int i = 0; i < 4; ++i) out[i] = half_to_float(h[i]);
        return true;
    }
    case DXGI_FORMAT_R11G11B10_FLOAT: {
        uint32_t v;
        std::memcpy(&v, p, 4);
        out[0] = f11(v & 2047), out[1] = f11((v >> 11) & 2047), out[2] = f10(v >> 22);
        return true;
    }
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        std::memcpy(out, p, 16);
        return true;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: {
        uint16_t h[2];
        std::memcpy(h, p, 4);
        out[0] = half_to_float(h[0]), out[1] = half_to_float(h[1]);
        return true;
    }
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: {
        uint16_t h;
        std::memcpy(&h, p, 2);
        out[0] = out[1] = out[2] = half_to_float(h);
        return true;
    }
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
        std::memcpy(out, p, 4);
        out[1] = out[2] = out[0];
        return true;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM:
        out[0] = out[1] = out[2] = p[0] / 255.f;
        return true;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM:
        out[0] = p[0] / 255.f, out[1] = p[1] / 255.f;
        return true;
    default: return false;
    }
}

uint32_t bytes_per_texel(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: return 2;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: return 2;
    default: return 4;
    }
}

// A 24-bit .bmp at most 960 wide (nearest texel), values clamped to [0, 1].
void write_bmp(const std::wstring& path, const uint8_t* data, uint32_t pitch, uint32_t w, uint32_t h, DXGI_FORMAT f) {
    uint32_t step = (w + 959) / 960, ow = w / step, oh = h / step, row = (ow * 3 + 3) & ~3u;
    std::vector<uint8_t> out(54 + static_cast<size_t>(row) * oh, 0);
    uint8_t* hd = out.data();
    hd[0] = 'B', hd[1] = 'M';
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(hd + at, &v, 4); };
    put32(2, static_cast<uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, ow);
    put32(22, oh);
    hd[26] = 1, hd[28] = 24;
    uint32_t bpt = bytes_per_texel(f);
    for (uint32_t y = 0; y < oh; ++y) {
        uint8_t* o = hd + 54 + static_cast<size_t>(oh - 1 - y) * row;
        const uint8_t* src = data + static_cast<size_t>(y * step) * pitch;
        for (uint32_t x = 0; x < ow; ++x) {
            float c[4];
            texel(f, src + static_cast<size_t>(x * step) * bpt, c);
            for (int i = 0; i < 3; ++i) {
                float v = std::isfinite(c[2 - i]) ? std::clamp(c[2 - i], 0.f, 1.f) : 1.f;
                o[x * 3 + i] = static_cast<uint8_t>(v * 255.f + 0.5f);
            }
        }
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
    CloseHandle(file);
}

// Subresource 0 of `tex` read back and reported; `name` for the file.
void read_back(ID3D12CommandQueue* queue, ID3D12Resource* tex, const std::wstring& name) {
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1) {
        std::fprintf(stderr, "[rt] diag:   (not read: dimension %d, %u samples)\n", d.Dimension, d.SampleDesc.Count);
        return;
    }
    float probe[4];
    uint8_t zero[16] = {};
    if (!texel(d.Format, zero, probe)) {
        std::fprintf(stderr, "[rt] diag:   (not read: format %d)\n", d.Format);
        return;
    }
    ID3D12Device* dev = nullptr;
    tex->GetDevice(IID_PPV_ARGS(&dev));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource* buf = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&buf))) &&
              SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    if (ok) {
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {tex, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* l = list;
        queue->ExecuteCommandLists(1, &l);
        queue->Signal(fence, 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 10000);
        CloseHandle(ev);
        ok = fence->GetCompletedValue() == 1;
    }
    uint8_t* p = nullptr;
    D3D12_RANGE all{0, static_cast<SIZE_T>(total)};
    if (ok && SUCCEEDED(buf->Map(0, &all, reinterpret_cast<void**>(&p)))) {
        // statistics over the whole texture (every 4th texel each way)
        uint32_t w = static_cast<uint32_t>(d.Width), h = d.Height, bpt = bytes_per_texel(d.Format);
        double sum[4] = {}, mx[4] = {-1e30, -1e30, -1e30, -1e30};
        uint64_t n = 0, lit = 0, odd = 0;
        for (uint32_t y = 0; y < h; y += 4)
            for (uint32_t x = 0; x < w; x += 4) {
                float c[4];
                texel(d.Format, p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch + static_cast<size_t>(x) * bpt, c);
                ++n;
                if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) {
                    ++odd;
                    continue;
                }
                if (c[0] != 0 || c[1] != 0 || c[2] != 0) ++lit;
                for (int i = 0; i < 4; ++i) sum[i] += c[i], mx[i] = std::max(mx[i], static_cast<double>(c[i]));
            }
        std::fprintf(stderr,
                     "[rt] diag:   pixels: %.1f%% not black, %.2f%% NaN/inf; mean rgba %.3f %.3f %.3f %.3f, max %.3f %.3f "
                     "%.3f %.3f -> %ls.bmp\n",
                     n ? 100.0 * lit / n : 0.0, n ? 100.0 * odd / n : 0.0, sum[0] / std::max<uint64_t>(n, 1),
                     sum[1] / std::max<uint64_t>(n, 1), sum[2] / std::max<uint64_t>(n, 1),
                     sum[3] / std::max<uint64_t>(n, 1), mx[0], mx[1], mx[2], mx[3], name.c_str());
        write_bmp(g.dir + L"\\" + name + L".bmp", p + fp.Offset, fp.Footprint.RowPitch, w, h, d.Format);
        D3D12_RANGE none{0, 0};
        buf->Unmap(0, &none);
    } else {
        std::fprintf(stderr, "[rt] diag:   (read back failed)\n");
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (buf) buf->Release();
    dev->Release();
}

}  // namespace

bool enabled() {
    std::lock_guard lock(g.m);
    init_locked();
    return g.on;
}

uint64_t frame() {
    std::lock_guard lock(g.m);
    return g.current;
}

bool listing() {
    if (!enabled()) return false;
    std::lock_guard lock(g.m);
    return g.current < g.armed_until || std::find(g.frames.begin(), g.frames.end(), g.current) != g.frames.end();
}

void drawn(ID3D12Resource* const* targets, UINT count) {
    if (!enabled()) return;
    std::lock_guard lock(g.m);
    ++g.frame_draws;
    for (UINT i = 0; i < count; ++i)
        if (targets[i]) ++target_locked(targets[i]).draws;
}

void cleared(ID3D12Resource* target) {
    if (!target || !enabled()) return;
    std::lock_guard lock(g.m);
    ++target_locked(target).clears;
}

void sampled(ID3D12Resource* t) {
    if (!t || !enabled()) return;
    std::lock_guard lock(g.m);
    if (std::find(g.sampled.begin(), g.sampled.end(), t) != g.sampled.end() || g.sampled.size() >= 24) return;
    t->AddRef();
    g.sampled.push_back(t);
}

void skipped(Skip why) {
    if (!enabled()) return;
    std::lock_guard lock(g.m);
    ++g.skips[why];
}

void save(const wchar_t* name, const void* bytes, size_t size) {
    if (!enabled() || !bytes || !size) return;
    std::lock_guard lock(g.m);
    std::wstring path = g.dir + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;  // there already
    DWORD wrote = 0;
    WriteFile(file, bytes, static_cast<DWORD>(size), &wrote, nullptr);
    CloseHandle(file);
    std::fprintf(stderr, "[rt] diag: saved %ls (%zu bytes)\n", name, size);
}

void dispatched() {
    if (!enabled()) return;
    std::lock_guard lock(g.m);
    ++g.dispatches;
}

void frame_end(uint64_t frame, ID3D12CommandQueue* queue, ID3D12Resource* presented) {
    if (!enabled()) return;
    std::lock_guard lock(g.m);
    bool armed = frame < g.armed_until, wanted = std::find(g.frames.begin(), g.frames.end(), frame) != g.frames.end();
    if (armed && g.frame_draws >= 3) g.armed_until = 0;  // this one: reported, the trigger spent
    if (wanted || (armed && g.armed_until == 0)) {
        std::fprintf(stderr,
                     "[rt] diag: frame %llu: %zu target(s) written; draws not drawn: %llu no shader, %llu no target, "
                     "%llu no pipeline, %llu no buffer; %llu dispatch(es) (not run)\n",
                     (unsigned long long)frame, g.targets.size(), (unsigned long long)g.skips[kNoShader],
                     (unsigned long long)g.skips[kNoTarget], (unsigned long long)g.skips[kNoPipeline],
                     (unsigned long long)g.skips[kNoBuffer], (unsigned long long)g.dispatches);
        bool presented_listed = false;
        std::vector<std::pair<ID3D12Resource*, std::wstring>> reads;
        for (size_t i = 0; i < g.targets.size(); ++i) {
            const Target& t = g.targets[i];
            D3D12_RESOURCE_DESC d = t.tex->GetDesc();
            bool is_presented = t.tex == presented;
            presented_listed |= is_presented;
            std::fprintf(stderr, "[rt] diag:  #%zu %p %llux%u fmt %d mips %u: %llu draw(s), %llu clear(s)%s\n", i,
                         static_cast<void*>(t.tex), (unsigned long long)d.Width, d.Height, d.Format, d.MipLevels,
                         (unsigned long long)t.draws, (unsigned long long)t.clears, is_presented ? "  <- PRESENTED" : "");
            wchar_t name[64];
            swprintf(name, 64, L"f%llu_t%02zu_%llux%u_fmt%d", (unsigned long long)frame, i,
                     (unsigned long long)d.Width, d.Height, d.Format);
            reads.push_back({t.tex, name});
        }
        if (presented && !presented_listed) {
            D3D12_RESOURCE_DESC d = presented->GetDesc();
            std::fprintf(stderr, "[rt] diag:  presented %p %llux%u fmt %d: no draw or clear wrote it this frame\n",
                         static_cast<void*>(presented), (unsigned long long)d.Width, d.Height, d.Format);
            wchar_t name[64];
            swprintf(name, 64, L"f%llu_presented", (unsigned long long)frame);
            reads.push_back({presented, name});
        }
        for (size_t i = 0; i < g.sampled.size(); ++i) {
            D3D12_RESOURCE_DESC d = g.sampled[i]->GetDesc();
            wchar_t name[64];
            swprintf(name, 64, L"f%llu_s%02zu_%llux%u_fmt%d", (unsigned long long)frame, i, (unsigned long long)d.Width,
                     d.Height, d.Format);
            std::fprintf(stderr, "[rt] diag:  sampled #%zu %p %llux%u fmt %d mips %u\n", i,
                         static_cast<void*>(g.sampled[i]), (unsigned long long)d.Width, d.Height, d.Format, d.MipLevels);
            reads.push_back({g.sampled[i], name});
        }
        for (auto& [tex, name] : reads) {
            std::fprintf(stderr, "[rt] diag:  reading %ls\n", name.c_str());
            read_back(queue, tex, name);
        }
    }
    for (Target& t : g.targets) t.tex->Release();
    g.targets.clear();
    g.frame_draws = 0;
    for (ID3D12Resource* t : g.sampled) t->Release();
    g.sampled.clear();
    g.current = frame + 1;
    // the trigger file "meshstats": the next mesh draws are measured (d3d12x_draw.cpp, mesh_stats_frame)
    mesh_stats_frame(frame, frame % 15 == 0 && DeleteFileW((g.dir + L"\\meshstats").c_str()));
    // the trigger file "mesh": the targets of the last mesh draw are read back now (their statistics in the log, each
    // as a .bmp). A handful of images: quick enough inside the open world, where a whole frame's report holds the
    // frame past the title's 30 s watchdog.
    if (frame % 15 == 0 && DeleteFileW((g.dir + L"\\mesh").c_str())) {
        std::fprintf(stderr, "[rt] diag: the last mesh draw's targets, at frame %llu:\n",
                     static_cast<unsigned long long>(frame));
        for (int k = 0; k < 8; ++k)
            if (ID3D12Resource* t = last_mesh_target(k)) {
                const D3D12_RESOURCE_DESC d = t->GetDesc();
                std::fprintf(stderr, "[rt] diag:  target %d: %llux%u format %d\n", k,
                             static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format));
                read_back(queue, t, L"mesh_target" + std::to_wstring(k));
            }
    }
    // the trigger file "ffx": the last upscale's colour (what the title lit, at the render size) and its output
    if (frame % 15 == 0 && DeleteFileW((g.dir + L"\\ffx").c_str())) {
        std::fprintf(stderr, "[rt] diag: the last upscale's colour and output, at frame %llu:\n",
                     static_cast<unsigned long long>(frame));
        for (int k = 0; k < 2; ++k)
            if (ID3D12Resource* t = last_upscale(k)) {
                const D3D12_RESOURCE_DESC d = t->GetDesc();
                std::fprintf(stderr, "[rt] diag:  %s: %llux%u format %d\n", k ? "output" : "colour",
                             static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format));
                read_back(queue, t, k ? L"ffx_output" : L"ffx_colour");
            } else {
                std::fprintf(stderr, "[rt] diag:  %s: none yet\n", k ? "output" : "colour");
            }
    }
    // the trigger file: the next frame is reported (checked every 15 frames)
    if (frame % 15 == 0 && DeleteFileW((g.dir + L"\\now").c_str())) {
        g.armed_until = frame + 2 + 300;  // from the next whole frame, the first one that draws 3+ times (10 s at most)
        std::fprintf(stderr, "[rt] diag: armed: the next frame with 3 or more draws is reported\n");
    }
    std::memset(g.skips, 0, sizeof g.skips);
    g.dispatches = 0;
}

}  // namespace rt::d3d12x::diag
