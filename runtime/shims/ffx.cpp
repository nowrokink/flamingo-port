// amd_fidelityfx_loader_xbox.dll: the FidelityFX API (ffxCreateContext, ffxDestroyContext, ffxConfigure, ffxQuery,
// ffxDispatch) the title upscales through. The title's own loader would hand the calls to
// amd_fidelityfx_upscaler_xbox.dll, which drives the console's D3D12 directly; here the API is the runtime's, so the
// title's imports bind to these (shims come before a module's exports).
//
// Every call takes a chain of descriptions, each starting with {uint64 type, next}. The upscaler's types (effect
// 0x00010000) and their layouts are the FidelityFX SDK's (ffx_upscale.h):
//   0x00010000 create      0x00010001 dispatch                 0x00010002 query: upscale ratio of a quality mode
//   0x00010003 query: render resolution of a quality mode      0x00010004 query: jitter phase count
//   0x00010005 query: jitter offset                            0x00010006 dispatch: generate the reactive mask
//   0x00010007 configure: key / value                          0x00010008 query: GPU memory usage
// The queries are plain arithmetic and are answered as the SDK does. 1.0.2.1 divides its frame counter by the jitter
// phase count, so a query that fails (count left 0) is a divide by zero in the title. Dispatch (0x10001) resizes
// the colour into the output with a linear filter (d3d12x_upscale.cpp): the SDK's ffxDispatchDescUpscale has the
// list at +16, then seven resources of 48 bytes each ({resource, description of 8 dwords, state}): colour +24, depth
// +72, motion vectors +120, exposure +168, reactive +216, transparency and composition +264, output +312; then the
// jitter +360, the motion vector scale +368, the render size +376 and the display size +384.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "d3d12x.h"
#include "shims.h"

namespace {

constexpr uint32_t kOk = 0, kUnknownType = 2;

struct Header {
    uint64_t type;
    Header* next;
};

bool readable(const void* p, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    return p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           reinterpret_cast<uintptr_t>(p) + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

template <typename T>
T field(const Header* h, size_t offset) {
    T v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(h) + offset, sizeof v);
    return v;
}

template <typename T>
void put(void* to, T value) {
    if (readable(to, sizeof value)) std::memcpy(to, &value, sizeof value);
}

// The first calls of each kind, with the description's first words: the layouts are read as the SDK's, and a title
// built against another version would show here.
void log_chain(const char* what, const Header* h, std::atomic<int>& shown, int limit) {
    if (shown++ >= limit) return;
    for (int depth = 0; h && depth < 6; h = h->next, ++depth) {
        if (!readable(h, sizeof *h)) break;
        std::fprintf(stderr, "[rt] ffx: %s: type %#llx:", what, static_cast<unsigned long long>(h->type));
        for (int i = 4; i < 28 && readable(reinterpret_cast<const uint32_t*>(h) + i, 4); ++i)
            std::fprintf(stderr, " %x", reinterpret_cast<const uint32_t*>(h)[i]);
        std::fprintf(stderr, "\n");
    }
}

// The SDK's ffxFsr3UpscalerGetJitterPhaseCount: 8 phases at native resolution, more the more the image is upscaled.
int32_t jitter_phases(uint32_t render_width, uint32_t display_width) {
    if (!render_width) return 8;
    const float ratio = static_cast<float>(display_width) / static_cast<float>(render_width);
    const int32_t n = static_cast<int32_t>(8.0f * ratio * ratio);
    return n > 0 ? n : 8;
}

float halton(int32_t index, int32_t base) {
    float f = 1.0f, result = 0.0f;
    for (int32_t i = index; i > 0; i /= base) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(i % base);
    }
    return result;
}

float quality_ratio(uint32_t mode) {
    switch (mode) {
        case 0: return 1.0f;  // native AA
        case 1: return 1.5f;  // quality
        case 2: return 1.7f;  // balanced
        case 3: return 2.0f;  // performance
        case 4: return 3.0f;  // ultra performance
        default: return 1.0f;
    }
}

std::atomic<int> g_creates{0}, g_configures{0}, g_queries{0}, g_dispatches{0};
std::atomic<uint64_t> g_dispatch_count{0};

uint32_t ffxCreateContext(void** context, Header* desc, const void*) {
    log_chain("create", desc, g_creates, 4);
    static uint64_t g_context[8];  // the context is opaque to the title: a token that isn't null
    put<void*>(context, g_context);
    return kOk;
}

uint32_t ffxDestroyContext(void** context, const void*) {
    put<void*>(context, nullptr);
    return kOk;
}

uint32_t ffxConfigure(void**, const Header* desc) {
    log_chain("configure", desc, g_configures, 8);
    return kOk;
}

uint32_t ffxQuery(void**, Header* desc) {
    log_chain("query", desc, g_queries, 12);
    if (!readable(desc, 48)) return kUnknownType;
    switch (desc->type) {
        case 0x00010002:  // {quality mode @16, float* ratio @24}
            put(field<void*>(desc, 24), quality_ratio(field<uint32_t>(desc, 16)));
            return kOk;
        case 0x00010003: {  // {display width @16, height @20, quality mode @24, uint* width @32, uint* height @40}
            const float ratio = quality_ratio(field<uint32_t>(desc, 24));
            put(field<void*>(desc, 32), static_cast<uint32_t>(static_cast<float>(field<uint32_t>(desc, 16)) / ratio));
            put(field<void*>(desc, 40), static_cast<uint32_t>(static_cast<float>(field<uint32_t>(desc, 20)) / ratio));
            return kOk;
        }
        case 0x00010004:  // {render width @16, display width @20, int* count @24}
            put(field<void*>(desc, 24), jitter_phases(field<uint32_t>(desc, 16), field<uint32_t>(desc, 20)));
            return kOk;
        case 0x00010005: {  // {index @16, phase count @20, float* x @24, float* y @32}
            const int32_t phases = field<int32_t>(desc, 20) > 0 ? field<int32_t>(desc, 20) : 8;
            const int32_t i = (field<int32_t>(desc, 16) % phases + phases) % phases + 1;
            put(field<void*>(desc, 24), halton(i, 2) - 0.5f);
            put(field<void*>(desc, 32), halton(i, 3) - 0.5f);
            return kOk;
        }
        case 0x00010008: {  // {{uint64 total, uint64 aliasable}* usage @16}
            const uint64_t none[2] = {};
            if (void* out = field<void*>(desc, 16); readable(out, sizeof none)) std::memcpy(out, none, sizeof none);
            return kOk;
        }
        case 4: {  // the versions of an effect: {create type @16, device @24, uint64* count @32, ids @40, names @48}
            static const char kName[] = "runtime";
            uint64_t* count = field<uint64_t*>(desc, 32);
            if (!readable(count, 8)) return kOk;
            const uint64_t room = *count;
            if (room && readable(field<void*>(desc, 40), 8)) put<uint64_t>(field<void*>(desc, 40), 1);
            if (room && readable(field<void*>(desc, 48), 8)) put<const char*>(field<void*>(desc, 48), kName);
            *count = 1;
            return kOk;
        }
        default: return kUnknownType;
    }
}

uint32_t ffxDispatch(void**, const Header* desc) {
    log_chain("dispatch", desc, g_dispatches, 3);
    const uint64_t n = ++g_dispatch_count;
    static std::atomic<uint64_t> resized{0};
    for (const Header* h = desc; h && readable(h, sizeof *h); h = h->next) {
        if (h->type != 0x00010001 || !readable(h, 392)) continue;
        void* colour = field<void*>(h, 24);
        void* output = field<void*>(h, 312);
        // (checked in 1.0.2.1: the list at +16, the colour at +24 and the output at +312 are objects of the runtime)
        if (rt::d3d12x::upscale(field<void*>(h, 16), colour, output, field<uint32_t>(h, 376), field<uint32_t>(h, 380),
                                field<uint32_t>(h, 384), field<uint32_t>(h, 388)))
            ++resized;
    }
    if ((n & (n - 1)) == 0 && n >= 64)
        std::fprintf(stderr, "[rt] ffx: %llu dispatches asked for, %llu resized\n", static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(resized.load()));
    return kOk;
}

}  // namespace

SHIM_TABLE(ffx,
    {"amd_fidelityfx_loader_xbox.dll", "ffxCreateContext", reinterpret_cast<void*>(&ffxCreateContext)},
    {"amd_fidelityfx_loader_xbox.dll", "ffxDestroyContext", reinterpret_cast<void*>(&ffxDestroyContext)},
    {"amd_fidelityfx_loader_xbox.dll", "ffxConfigure", reinterpret_cast<void*>(&ffxConfigure)},
    {"amd_fidelityfx_loader_xbox.dll", "ffxQuery", reinterpret_cast<void*>(&ffxQuery)},
    {"amd_fidelityfx_loader_xbox.dll", "ffxDispatch", reinterpret_cast<void*>(&ffxDispatch)},
);
