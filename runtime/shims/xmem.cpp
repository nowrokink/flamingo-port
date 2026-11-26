// xmem.dll: the GDK's memory API.
//
// Signatures come from the call sites in game_scarlett_master.exe 1.0.2.1 (registers set before each call, how the
// result is tested), since the console header isn't public. Where a field's meaning is a guess, it says so.
//
// GPU-visible memory is unified memory (uma.h, docs/gpu-memory.md): XMemVirtualAlloc places every allocation in the
// range R, committed pages are pool pages, and physical pages are pool pages too, numbered as the console numbers
// them (4 KiB frames, 16 per 64 KiB page). Mapping a page splits its placeholder in R and maps the pool page there,
// so the same page can show at several addresses, as on the console, and the GPU sees what the CPU writes.
// Executable memory can't be a section view, so it (and anything asked for outside R) stays ordinary host memory
// the GPU doesn't see.
//
// XMemAlloc / XMemFree go through the title's allocation hooks, as the console's do: the exe exports
// __XMemAllocationHooks__ ({alloc, free}), and its hooks hand each request to handlers registered per allocator ID
// (XALLOC_ATTRIBUTES bits 16-23), falling back to XMemAllocDefault / XMemFreeDefault. The system's own allocations
// (D3D12 resource memory: allocator ID 128) take the same path, which is how 1.0.2.1 budgets its render targets.
// XMemAllocDefault: attributes with a page size (bits 14-15) are graphics memory, given whole 64 KiB pages in the
// GPU-visible range; the rest is CPU memory, 64-byte aligned. TODO: the alignment (bits 24-28) and memory-type
// (29-31) fields.
#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"
#include "uma.h"

namespace {

constexpr size_t kAlign = 64;
constexpr uint64_t kPage = rt::uma::kPage;     // console physical page, also the Windows allocation granularity
constexpr uint64_t kLargeAlign = 2ull << 20;   // MEM_LARGE_PAGES reservations: 2 MiB aligned on the console

// Allocation-type bits seen at the call sites. The low ones are the Windows values.
constexpr DWORD kMemPhysical = 0x00400000;     // MEM_PHYSICAL
constexpr DWORD kMemLargePages = 0x20000000;   // MEM_LARGE_PAGES (needs a privilege on desktop: dropped)
constexpr DWORD kMemKnown = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH |
                            MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER;

void warn(const char* fmt, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    std::fprintf(stderr, "[rt] xmem: ");
    std::fprintf(stderr, fmt, a, b, c);
    std::fprintf(stderr, " (error %lu)\n", GetLastError());
}

// Keeps the standard protection bits; the console adds GPU and caching bits above them (0x800 and up).
ULONG host_protect(DWORD protect) {
    ULONG p = protect & 0xFF;
    return p ? p : PAGE_READWRITE;
}

bool executable(DWORD protect) { return (protect & 0xF0) != 0; }  // PAGE_EXECUTE*

// ---------------------------------------------------------------- heap

struct Hooks {
    void*(__stdcall* alloc)(size_t size, uint64_t attributes);
    void(__stdcall* free)(void* p, uint64_t attributes);
};
const Hooks* g_hooks = nullptr;

bool graphics(uint64_t attributes) { return (attributes >> 14) & 3; }

// The title's CPU memory from XMemAllocDefault, live (for the memory report).
std::atomic<uint64_t> g_cpu_heap{0};

// The allocator behind the hooks.
void* __stdcall XMemAllocDefault(size_t size, uint64_t attributes) {
    if (!graphics(attributes)) {
        void* p = _aligned_malloc(size ? size : 1, kAlign);
        if (p) g_cpu_heap += _aligned_msize(p, kAlign, 0);
        return p;
    }
    uint64_t bytes = (static_cast<uint64_t>(size ? size : 1) + kPage - 1) & ~(kPage - 1);
    uintptr_t va = rt::uma::reserve(bytes, kPage);
    if (!va || !rt::uma::commit(va, bytes, PAGE_READWRITE)) {
        if (va) rt::uma::release(va);
        warn("no GPU-visible memory for %llx bytes (attributes %llx)", bytes, attributes);
        return nullptr;
    }
    return reinterpret_cast<void*>(va);
}

void __stdcall XMemFreeDefault(void* p, uint64_t) {
    if (!p) return;
    auto va = reinterpret_cast<uintptr_t>(p);
    uintptr_t start;
    uint64_t bytes;
    if (rt::uma::contains(va) && rt::uma::reservation(va, &start, &bytes) && start == va) {
        rt::uma::release(va);
    } else {
        g_cpu_heap -= std::min<uint64_t>(g_cpu_heap.load(), _aligned_msize(p, kAlign, 0));
        _aligned_free(p);
    }
}

void* __stdcall XMemAlloc(size_t size, uint64_t attributes) {
    if (g_hooks && g_hooks->alloc) return g_hooks->alloc(size, attributes);
    return XMemAllocDefault(size, attributes);
}

void __stdcall XMemFree(void* p, uint64_t attributes) {
    if (g_hooks && g_hooks->free) return g_hooks->free(p, attributes);
    XMemFreeDefault(p, attributes);
}

// ---------------------------------------------------------------- virtual memory

// Ordinary host memory, outside R: what the console's flags mean on a PC (large pages need a privilege and are
// dropped, so is MEM_PHYSICAL).
void* host_alloc(void* address, SIZE_T size, DWORD type, DWORD protect) {
    DWORD t = (type & kMemKnown & ~(MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER)) | (address ? 0 : MEM_RESERVE);
    void* p = VirtualAlloc(address, size, t, host_protect(protect));
    if (!p) warn("host VirtualAlloc of %llx bytes at %llx failed", size, reinterpret_cast<uintptr_t>(address));
    return p;
}

// PVOID XMemVirtualAlloc(PVOID address, SIZE_T size, DWORD allocationType, ULONG64 attributes, DWORD protect).
// The exe passes MEM_* in r8 (0x20003000, 0x20402000, 0x20406000, 0x42000), a small attribute value in r9 (3, 7,
// 0x21, 0x183, ...), and PAGE_* plus console bits on the stack (4, 0x2004, 0x22004, 0x801, ...).
//
// In R: MEM_RESERVE (with or without MEM_RESERVE_PLACEHOLDER) makes a reservation, at `address` or anywhere
// (2 MiB-aligned for MEM_LARGE_PAGES); MEM_REPLACE_PLACEHOLDER only claims part of a reservation the title made
// before (the placeholder stays ours); MEM_COMMIT maps fresh pool pages. MEM_PHYSICAL regions are reservations
// pages get mapped into (XMemMapPhysicalPages).
void* __stdcall XMemVirtualAlloc(void* address, SIZE_T size, DWORD type, uint64_t /*attributes*/, DWORD protect) {
    auto a = reinterpret_cast<uintptr_t>(address);
    if (!rt::uma::base() || executable(protect) || (address && !rt::uma::contains(a, size))) {
        if (executable(protect)) warn("executable memory (%llx bytes) isn't GPU-visible", size);
        return host_alloc(address, size, type, protect);
    }
    if (type & MEM_REPLACE_PLACEHOLDER) {
        if (!address || !rt::uma::reservation(a, nullptr, nullptr)) {
            warn("MEM_REPLACE_PLACEHOLDER at %llx outside a reservation", a);
            return nullptr;
        }
    } else if ((type & MEM_RESERVE) || !address) {  // MEM_COMMIT with no address reserves too, as on Windows
        uint64_t align = (type & kMemLargePages) ? kLargeAlign : kPage;
        a = rt::uma::reserve(size, align, a);
        if (!a) {
            warn("no room for %llx bytes at %llx in unified memory", size, reinterpret_cast<uintptr_t>(address));
            return nullptr;
        }
    } else if (!address || !rt::uma::reservation(a, nullptr, nullptr)) {
        warn("commit at %llx outside a reservation", a);
        return nullptr;
    }
    // MEM_PHYSICAL | MEM_COMMIT is ordinary committed memory.
    if ((type & MEM_COMMIT) && !rt::uma::commit(a, size, host_protect(protect))) {
        warn("committing %llx bytes at %llx failed", size, a);
        if (((type & MEM_RESERVE) || !address) && !(type & MEM_REPLACE_PLACEHOLDER)) rt::uma::release(a);
        return nullptr;
    }
    return reinterpret_cast<void*>(a);
}

// ---------------------------------------------------------------- physical pages

// BOOL XMemAllocatePhysicalPages(DWORD flags, ULONG_PTR* pageCount, ULONG_PTR* pageArray). Flags seen: 0x180,
// 0x280, 0x380 (meaning unknown; ignored). On return *pageCount is the number granted; callers check it equals
// the request. Page numbers are 4 KiB frames (16 apart).
BOOL __stdcall XMemAllocatePhysicalPages(DWORD /*flags*/, ULONG_PTR* count, ULONG_PTR* pages) {
    if (!count || !pages) return FALSE;
    uint64_t n = *count;
    *count = 0;
    if (!n) return TRUE;
    static_assert(sizeof(ULONG_PTR) == sizeof(uint64_t));
    if (!rt::uma::alloc_frames(n, reinterpret_cast<uint64_t*>(pages))) {
        warn("allocating %llu physical pages failed", n);
        return FALSE;
    }
    *count = static_cast<ULONG_PTR>(n);
    return TRUE;
}

// BOOL XMemFreePhysicalPages(ULONG_PTR pageCount, ULONG_PTR* pageArray). Pages still mapped somewhere keep their
// memory until they're unmapped (the pool page is reused only once it's back in the pool; see uma.cpp).
BOOL __stdcall XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pages) {
    if (!pages) return count == 0;
    rt::uma::free_frames(count, reinterpret_cast<const uint64_t*>(pages));
    return TRUE;
}

bool map_one(uintptr_t va, uint64_t frame) {
    if (!rt::uma::contains(va, kPage) || !rt::uma::reservation(va, nullptr, nullptr)) {
        warn("%llx is not a page in unified memory", va);
        return false;
    }
    if (frame && !rt::uma::valid_frame(frame)) {
        warn("unknown physical page %llx", frame);
        return false;
    }
    return rt::uma::map_frame(va, frame, PAGE_READWRITE);
}

// PVOID XMemMapPhysicalPages(PVOID address, ULONG_PTR pageCount, ULONG_PTR* pageArray): maps pageCount pages at
// address, or unmaps them when pageArray is NULL. Returns address, or NULL on failure (callers test rax).
void* __stdcall XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* pages) {
    auto a = reinterpret_cast<uintptr_t>(address);
    for (ULONG_PTR i = 0; i < count; ++i)
        if (!map_one(a + i * kPage, pages ? pages[i] : 0)) return nullptr;
    return address;
}

// BOOL XMemMapPhysicalPagesScatter(PVOID* addresses, ULONG_PTR pageCount, ULONG_PTR* pageArray): as above, one
// address per page; pageArray NULL unmaps.
BOOL __stdcall XMemMapPhysicalPagesScatter(void** addresses, ULONG_PTR count, ULONG_PTR* pages) {
    if (!addresses && count) return FALSE;
    BOOL ok = TRUE;
    for (ULONG_PTR i = 0; i < count; ++i)
        if (!map_one(reinterpret_cast<uintptr_t>(addresses[i]), pages ? pages[i] : 0)) ok = FALSE;
    return ok;
}

// ---------------------------------------------------------------- queries

// Same shape as VirtualQuery on the console (MEMORY_BASIC_INFORMATION out, returns bytes written).
SIZE_T __stdcall XMemVirtualQuery(const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T length) {
    if (length >= sizeof(MEMORY_BASIC_INFORMATION) && rt::uma::query(reinterpret_cast<uintptr_t>(address), info))
        return sizeof(MEMORY_BASIC_INFORMATION);
    return VirtualQuery(address, info, length);
}

bool read_console() {
    char v[32] = {};
    DWORD n = GetEnvironmentVariableA("RT_CONSOLE", v, sizeof v);
    return n && n < sizeof v && _stricmp(v, "seriess") == 0;
}
const bool g_series_s = read_console();  // at startup (no function-local statics: see docs/design.md)

// BOOL XMemGetWorkingSetStatistics(DWORD kind, STATS* stats). kind is 0x20000001 (title) or 0x2000000A (tools).
// One caller zeroes 120 bytes first; the struct is at least that big. Known from the call sites: the caller
// checks qword[0] - qword[8] > 100 MiB, so [0] is taken as the budget and [8] as what's in use. The other fields
// (qwords at 16 and 24, dwords at 72 and 76) are left alone: their meaning isn't known yet.
// The budget is the console's title memory (rt::shims::title_memory_bytes), not the PC's RAM: given 32 GB the
// title sized its caches and streaming for it (25 GB working set on the loading screen).
BOOL __stdcall XMemGetWorkingSetStatistics(DWORD /*kind*/, void* stats) {
    if (!stats) return FALSE;
    uint64_t total = rt::shims::title_memory_bytes();
    // in use: the title's own memory (uma's pool), not our process' private bytes (the GPU driver's, ours)
    uint64_t used = rt::uma::pool_bytes_used();
    std::memcpy(static_cast<uint8_t*>(stats) + 0, &total, sizeof(total));
    std::memcpy(static_cast<uint8_t*>(stats) + 8, &used, sizeof(used));
    return TRUE;
}

// BOOL XMemIsToolsMemAvailable(void). Its only caller breaks (int3) when this returns 0, so it reports TRUE.
BOOL __stdcall XMemIsToolsMemAvailable() { return TRUE; }

// BOOL XMemTransferMemory(DWORD direction, SIZE_T bytes): moves budget between title and tools memory on a
// devkit. There's no such split on desktop. Its caller breaks when this returns 0.
BOOL __stdcall XMemTransferMemory(DWORD /*direction*/, SIZE_T /*bytes*/) { return TRUE; }

}  // namespace

namespace rt::shims {

void set_xmem_hooks(const void* hooks) {
    g_hooks = static_cast<const Hooks*>(hooks);
    if (g_hooks)
        std::fprintf(stderr, "[rt] xmem: the title's allocation hooks: alloc %p, free %p\n",
                     reinterpret_cast<void*>(g_hooks->alloc), reinterpret_cast<void*>(g_hooks->free));
}

void* xmem_alloc(size_t size, uint64_t attributes) { return XMemAlloc(size, attributes); }
uint64_t xmem_cpu_bytes() { return g_cpu_heap.load(); }
void xmem_free(void* p, uint64_t attributes) { XMemFree(p, attributes); }

bool series_s() { return g_series_s; }
uint64_t title_memory_bytes() { return g_series_s ? 7776ull << 20 : (27ull << 30) / 2; }

}  // namespace rt::shims

SHIM_TABLE(xmem,
    {"xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&XMemAlloc)},
    {"xmem.dll", "XMemFree", reinterpret_cast<void*>(&XMemFree)},
    {"xmem.dll", "XMemAllocDefault", reinterpret_cast<void*>(&XMemAllocDefault)},
    {"xmem.dll", "XMemFreeDefault", reinterpret_cast<void*>(&XMemFreeDefault)},
    {"xmem.dll", "XMemVirtualAlloc", reinterpret_cast<void*>(&XMemVirtualAlloc)},
    {"xmem.dll", "XMemAllocatePhysicalPages", reinterpret_cast<void*>(&XMemAllocatePhysicalPages)},
    {"xmem.dll", "XMemFreePhysicalPages", reinterpret_cast<void*>(&XMemFreePhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPages", reinterpret_cast<void*>(&XMemMapPhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPagesScatter", reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter)},
    {"xmem.dll", "XMemVirtualQuery", reinterpret_cast<void*>(&XMemVirtualQuery)},
    {"xmem.dll", "XMemGetWorkingSetStatistics", reinterpret_cast<void*>(&XMemGetWorkingSetStatistics)},
    {"xmem.dll", "XMemIsToolsMemAvailable", reinterpret_cast<void*>(&XMemIsToolsMemAvailable)},
    {"xmem.dll", "XMemTransferMemory", reinterpret_cast<void*>(&XMemTransferMemory)},
);
