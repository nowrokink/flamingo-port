#include "uma.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace rt::uma {
namespace {

// R's size: the largest of these with room from below 4 GiB (1.0.2.1 reserves 3+ GiB ranges at a time; thread stacks
// and heaps that ASLR places can be in the way of the larger ones).
constexpr uint64_t kRangeSizes[] = {256ull << 30, 192ull << 30, 128ull << 30, 96ull << 30, 64ull << 30};
constexpr uint64_t kLowStart = 0x10000000;            // R's start lies in [256 MiB, 4 GiB)
constexpr uint64_t kLowLimit = 0x100000000ull;

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct Api {
    VirtualAlloc2Fn valloc2 = nullptr;
    MapViewOfFile3Fn map3 = nullptr;
    UnmapViewOfFile2Fn unmap2 = nullptr;
    Api() {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        valloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2"));
        map3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3"));
        unmap2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kb, "UnmapViewOfFile2"));
    }
};

struct Page {
    uint32_t pool = 0;  // pool index + 1; 0 = unmapped (placeholder)
    bool owned = false; // committed here (returned to the pool when unmapped)
    ULONG protect = 0;
};

struct State {
    std::mutex m;
    Api api;
    uintptr_t base = 0;
    uint64_t range = 0;                     // R's size
    std::vector<Page> pages;                // one per page of R
    std::vector<uint32_t> table;            // the GPU page table: pool index + 1
    size_t dirty_begin = 0, dirty_end = 0;
    std::map<uintptr_t, uintptr_t> holes;   // placeholder pieces: start -> end
    std::map<uintptr_t, uint64_t> reserved; // reservations: start -> bytes
    std::map<uintptr_t, uint64_t> free_va;  // unreserved runs of R: start -> bytes
    std::vector<HANDLE> chunks;
    std::vector<uint8_t*> windows;          // per chunk: a view of all of it for the runtime's own access (lazy)
    std::vector<uint32_t> free_pool;        // free pool indices (a stack; new chunks push theirs in order)
    // Per pool page: how many places map it, and whether the title freed it as a physical page. A freed page goes
    // back to the pool only once nothing maps it (as a section's pages stay valid while a view maps them).
    std::vector<uint32_t> refs;
    std::vector<uint8_t> freed;
    size_t pool_used = 0, mapped = 0;
};

State& st() {
    static State s;
    return s;
}

size_t index_of(State& s, uintptr_t va) { return (va - s.base) / kPage; }

void mark_dirty(State& s, size_t i) {
    if (s.dirty_begin == s.dirty_end) {
        s.dirty_begin = i;
        s.dirty_end = i + 1;
    } else {
        s.dirty_begin = std::min(s.dirty_begin, i);
        s.dirty_end = std::max(s.dirty_end, i + 1);
    }
}

bool add_chunk(State& s) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                  static_cast<DWORD>(kChunk >> 32), static_cast<DWORD>(kChunk), nullptr);
    if (!h) {
        std::fprintf(stderr, "[rt] uma: no memory for another 1 GiB chunk (error %lu)\n", GetLastError());
        return false;
    }
    uint32_t first = static_cast<uint32_t>(s.chunks.size() * kPagesPerChunk);
    s.chunks.push_back(h);
    s.refs.resize(s.chunks.size() * kPagesPerChunk, 0);
    s.freed.resize(s.chunks.size() * kPagesPerChunk, 0);
    for (uint32_t i = static_cast<uint32_t>(kPagesPerChunk); i-- > 0;) s.free_pool.push_back(first + i);
    std::fprintf(stderr, "[rt] uma: chunk %zu (%zu GiB of pool)\n", s.chunks.size() - 1, s.chunks.size());
    return true;
}

bool take_pool(State& s, uint32_t* out) {
    if (s.free_pool.empty() && !add_chunk(s)) return false;
    *out = s.free_pool.back();
    s.free_pool.pop_back();
    ++s.pool_used;
    return true;
}

void give_pool(State& s, uint32_t index) {
    s.freed[index] = 0;
    s.free_pool.push_back(index);
    --s.pool_used;
}

// Makes [va, va + kPage) a placeholder of its own (splits only cut a prefix off a placeholder).
bool isolate(State& s, uintptr_t va) {
    auto it = s.holes.upper_bound(va);
    if (it == s.holes.begin()) return false;
    --it;
    uintptr_t b = it->first, e = it->second;
    if (va < b || va + kPage > e) return false;
    const DWORD split = MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER;
    if (va > b && !VirtualFree(reinterpret_cast<void*>(b), va - b, split)) return false;
    if (va + kPage < e && !VirtualFree(reinterpret_cast<void*>(va), kPage, split)) return false;
    s.holes.erase(it);
    if (va > b) s.holes[b] = va;
    if (va + kPage < e) s.holes[va + kPage] = e;
    return true;
}

ULONG view_protect(ULONG protect) { return (protect & 0xFF) == PAGE_READONLY ? PAGE_READONLY : PAGE_READWRITE; }

void (*g_on_unmap)(uintptr_t va) = nullptr;

bool unmap(State& s, uintptr_t va) {
    Page& p = s.pages[index_of(s, va)];
    if (!p.pool) return true;
    if (!s.api.unmap2(GetCurrentProcess(), reinterpret_cast<void*>(va), MEM_PRESERVE_PLACEHOLDER)) {
        std::fprintf(stderr, "[rt] uma: unmapping %p failed (error %lu)\n", reinterpret_cast<void*>(va), GetLastError());
        return false;
    }
    s.holes[va] = va + kPage;
    uint32_t pool = p.pool - 1;
    --s.refs[pool];
    if (p.owned || (s.freed[pool] && !s.refs[pool])) give_pool(s, pool);
    p = Page{};
    s.table[index_of(s, va)] = 0;
    mark_dirty(s, index_of(s, va));
    --s.mapped;
    if (g_on_unmap) g_on_unmap(va);
    return true;
}

bool map(State& s, uintptr_t va, uint32_t pool, bool owned, ULONG protect) {
    if (!unmap(s, va) || !isolate(s, va)) return false;
    HANDLE section = s.chunks[pool / kPagesPerChunk];
    uint64_t offset = (pool % kPagesPerChunk) * kPage;
    void* v = s.api.map3(section, GetCurrentProcess(), reinterpret_cast<void*>(va), offset, kPage,
                         MEM_REPLACE_PLACEHOLDER, view_protect(protect), nullptr, 0);
    if (v != reinterpret_cast<void*>(va)) {
        std::fprintf(stderr, "[rt] uma: mapping pool page %u at %p failed (error %lu)\n", pool,
                     reinterpret_cast<void*>(va), GetLastError());
        s.holes[va] = va + kPage;
        return false;
    }
    s.holes.erase(va);
    ++s.refs[pool];
    size_t i = index_of(s, va);
    s.pages[i] = Page{pool + 1, owned, protect};
    s.table[i] = pool + 1;
    mark_dirty(s, i);
    ++s.mapped;
    return true;
}

bool in_range(State& s, uintptr_t va, uint64_t bytes) {
    return s.base && va >= s.base && bytes <= s.range && va - s.base <= s.range - bytes;
}

// Carves [start, start + bytes) out of the free runs; false if it isn't wholly free.
bool take_va(State& s, uintptr_t start, uint64_t bytes) {
    auto it = s.free_va.upper_bound(start);
    if (it == s.free_va.begin()) return false;
    --it;
    uintptr_t b = it->first, e = it->first + it->second;
    if (start < b || start + bytes > e) return false;
    s.free_va.erase(it);
    if (start > b) s.free_va[b] = start - b;
    if (start + bytes < e) s.free_va[start + bytes] = e - (start + bytes);
    return true;
}

void give_va(State& s, uintptr_t start, uint64_t bytes) {
    auto it = s.free_va.emplace(start, bytes).first;
    auto next = std::next(it);
    if (next != s.free_va.end() && start + bytes == next->first) {
        it->second += next->second;
        s.free_va.erase(next);
    }
    if (it != s.free_va.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            s.free_va.erase(it);
        }
    }
}

}  // namespace

bool init() {
    State& s = st();
    std::lock_guard lock(s.m);
    if (s.base) return true;
    if (!s.api.valloc2 || !s.api.map3 || !s.api.unmap2) return false;
    // At the first 1 GiB boundary from 256 MiB up where the whole range is free, so its start is below 4 GiB.
    // (Address requirements together with MEM_RESERVE_PLACEHOLDER are refused with ERROR_INVALID_PARAMETER.)
    void* r = nullptr;
    uint64_t kRangeSize = 0;
    for (uint64_t size : kRangeSizes) {
        for (uintptr_t at = kLowStart; at < kLowLimit && !r; at = (at + kChunk) & ~(kChunk - 1))
            r = s.api.valloc2(GetCurrentProcess(), reinterpret_cast<void*>(at), size,
                              MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (r) {
            kRangeSize = size;
            break;
        }
    }
    if (!r) {
        std::fprintf(stderr, "[rt] uma: can't reserve even %llu GiB starting below 4 GiB (error %lu)\n",
                     kRangeSizes[std::size(kRangeSizes) - 1] >> 30, GetLastError());
        return false;
    }
    s.range = kRangeSize;
    if (kRangeSize < kRangeSizes[0]) {  // what's in the way of the largest
        uintptr_t end = reinterpret_cast<uintptr_t>(r) + kRangeSizes[0];
        for (uintptr_t p = reinterpret_cast<uintptr_t>(r) + kRangeSize; p < end;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
            if (mbi.State != MEM_FREE) {
                std::fprintf(stderr, "[rt] uma: in the way of %llu GiB: %p (%llu KiB, type %lx, allocated at %p)\n",
                             kRangeSizes[0] >> 30, mbi.BaseAddress,
                             static_cast<unsigned long long>(mbi.RegionSize >> 10), mbi.Type, mbi.AllocationBase);
                break;
            }
            p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
    }
    std::fprintf(stderr, "[rt] uma: %llu GiB of GPU-visible address space at %p\n", kRangeSize >> 30, r);
    s.base = reinterpret_cast<uintptr_t>(r);
    s.pages.assign(kRangeSize / kPage, Page{});
    s.table.assign(kRangeSize / kPage, 0);
    s.holes[s.base] = s.base + kRangeSize;
    s.free_va[s.base] = kRangeSize;
    return true;
}

uintptr_t base() { return st().base; }
uint64_t size() { return st().base ? st().range : 0; }
bool contains(uintptr_t va, uint64_t bytes) { return in_range(st(), va, bytes); }

namespace {

// First fit in [lo, hi) of the free runs; 0 if nothing fits.
uintptr_t first_fit(State& s, uint64_t bytes, uint64_t align, uintptr_t lo, uintptr_t hi) {
    for (auto& [b, n] : s.free_va) {
        uintptr_t start = (std::max<uintptr_t>(b, lo) + align - 1) & ~(align - 1);
        if (start + bytes <= b + n && start + bytes <= hi) {
            take_va(s, start, bytes);
            s.reserved[start] = bytes;
            return start;
        }
    }
    return 0;
}

}  // namespace

uintptr_t reserve(uint64_t bytes, uint64_t align, uintptr_t at) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (!s.base || !bytes) return 0;
    bytes = (bytes + kPage - 1) & ~(kPage - 1);
    align = std::max(align, kPage);
    if (at) {
        if (at % kPage || !in_range(s, at, bytes) || !take_va(s, at, bytes)) return 0;
        s.reserved[at] = bytes;
        return at;
    }
    // The part of R below 4 GiB is kept for what must sit there (descriptor heaps, reserve_low); everything
    // else goes above it, and only falls back to it when R is otherwise full.
    uintptr_t end = s.base + s.range;
    if (uintptr_t a = first_fit(s, bytes, align, std::max<uintptr_t>(s.base, kLowLimit), end)) return a;
    return first_fit(s, bytes, align, s.base, end);
}

uintptr_t reserve_low(uint64_t bytes, uint64_t align) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (!s.base || !bytes) return 0;
    bytes = (bytes + kPage - 1) & ~(kPage - 1);
    return first_fit(s, bytes, std::max(align, kPage), s.base, kLowLimit);
}

bool reservation(uintptr_t va, uintptr_t* start, uint64_t* bytes) {
    State& s = st();
    std::lock_guard lock(s.m);
    auto it = s.reserved.upper_bound(va);
    if (it == s.reserved.begin()) return false;
    --it;
    if (va >= it->first + it->second) return false;
    if (start) *start = it->first;
    if (bytes) *bytes = it->second;
    return true;
}

void on_unmap(void (*hook)(uintptr_t va)) { g_on_unmap = hook; }

bool release(uintptr_t start) {
    State& s = st();
    std::lock_guard lock(s.m);
    auto it = s.reserved.find(start);
    if (it == s.reserved.end()) return false;
    bool ok = true;
    for (uintptr_t va = start; va < start + it->second; va += kPage) ok &= unmap(s, va);
    give_va(s, start, it->second);
    s.reserved.erase(it);
    return ok;
}

bool commit(uintptr_t va, uint64_t bytes, ULONG protect) {
    State& s = st();
    std::lock_guard lock(s.m);
    uintptr_t first = va & ~(kPage - 1), last = (va + bytes + kPage - 1) & ~(kPage - 1);
    if (!in_range(s, first, last - first)) return false;
    for (uintptr_t p = first; p < last; p += kPage) {
        Page& pg = s.pages[index_of(s, p)];
        if (pg.pool) {
            pg.protect = protect;
            continue;
        }
        uint32_t pool;
        if (!take_pool(s, &pool)) return false;
        if (!map(s, p, pool, true, protect)) {
            give_pool(s, pool);
            return false;
        }
        // A fresh page reads as zero, as committed memory does (sections are zero-filled; reused pages aren't).
        std::memset(reinterpret_cast<void*>(p), 0, kPage);
    }
    return true;
}

bool decommit(uintptr_t va, uint64_t bytes) {
    State& s = st();
    std::lock_guard lock(s.m);
    uintptr_t first = va & ~(kPage - 1), last = (va + bytes + kPage - 1) & ~(kPage - 1);
    if (!in_range(s, first, last - first)) return false;
    bool ok = true;
    for (uintptr_t p = first; p < last; p += kPage) ok &= unmap(s, p);
    return ok;
}

bool valid_frame(uint64_t frame) {
    State& s = st();
    return frame && frame % 16 == 0 && frame / 16 - 1 < s.chunks.size() * kPagesPerChunk;
}

bool map_frame(uintptr_t va, uint64_t frame, ULONG protect) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (va % kPage || !in_range(s, va, kPage)) return false;
    if (!frame) return unmap(s, va);
    if (!valid_frame(frame)) return false;
    return map(s, va, static_cast<uint32_t>(frame / 16 - 1), false, protect);
}

uintptr_t alias_low(uintptr_t va, uint64_t bytes) {
    uintptr_t first = va & ~(kPage - 1), last = (va + bytes + kPage - 1) & ~(kPage - 1);
    uintptr_t low = reserve_low(last - first, kPage);
    if (!low) return 0;
    State& s = st();
    std::lock_guard lock(s.m);
    for (uintptr_t p = first; p < last; p += kPage) {
        const Page& src = s.pages[index_of(s, p)];
        if (!src.pool || !map(s, low + (p - first), src.pool - 1, false, src.protect)) {
            std::fprintf(stderr, "[rt] uma: can't alias %p below 4 GiB\n", reinterpret_cast<void*>(p));
            return 0;
        }
    }
    return low + (va - first);
}

uint64_t pool_bytes_used() {
    State& s = st();
    std::lock_guard lock(s.m);
    return uint64_t{s.pool_used} * kPage;
}

bool alloc_frames(size_t count, uint64_t* frames) {
    State& s = st();
    std::lock_guard lock(s.m);
    for (size_t i = 0; i < count; ++i) {
        uint32_t pool;
        if (!take_pool(s, &pool)) {
            while (i--) give_pool(s, static_cast<uint32_t>(frames[i] / 16 - 1));
            return false;
        }
        frames[i] = (uint64_t{pool} + 1) * 16;
    }
    return true;
}

void free_frames(size_t count, const uint64_t* frames) {
    State& s = st();
    std::lock_guard lock(s.m);
    for (size_t i = 0; i < count; ++i) {
        if (!valid_frame(frames[i])) continue;
        auto pool = static_cast<uint32_t>(frames[i] / 16 - 1);
        if (s.refs[pool]) s.freed[pool] = 1;  // back to the pool when its last mapping goes
        else give_pool(s, pool);
    }
}

bool query(uintptr_t va, MEMORY_BASIC_INFORMATION* info) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (!info || !in_range(s, va, 1)) return false;
    uintptr_t page = va & ~(kPage - 1);
    *info = MEMORY_BASIC_INFORMATION{};
    info->BaseAddress = reinterpret_cast<void*>(page);
    auto r = s.reserved.upper_bound(page);
    bool reserved = r != s.reserved.begin() && page < std::prev(r)->first + std::prev(r)->second;
    uintptr_t run_end = s.base + s.range, alloc_base = 0;
    if (reserved) {
        --r;
        alloc_base = r->first;
        run_end = r->first + r->second;
    } else if (r != s.reserved.end()) {
        run_end = r->first;
    }
    const Page& p = s.pages[index_of(s, page)];
    auto alike = [&](const Page& q) { return (q.pool != 0) == (p.pool != 0) && q.protect == p.protect; };
    uintptr_t end = page + kPage;
    while (end < run_end && alike(s.pages[index_of(s, end)])) end += kPage;
    info->RegionSize = end - page;
    if (!reserved) {
        info->State = MEM_FREE;
        info->Protect = PAGE_NOACCESS;
        return true;
    }
    info->AllocationBase = reinterpret_cast<void*>(alloc_base);
    info->AllocationProtect = PAGE_READWRITE;
    info->Type = MEM_PRIVATE;
    info->State = p.pool ? MEM_COMMIT : MEM_RESERVE;
    info->Protect = p.pool ? (p.protect ? p.protect : PAGE_READWRITE) : 0;
    return true;
}

// The whole of chunk `index` mapped once, anywhere (held with s.m).
static uint8_t* window(State& s, size_t index) {
    if (index >= s.chunks.size()) return nullptr;
    if (s.windows.size() < s.chunks.size()) s.windows.resize(s.chunks.size(), nullptr);
    if (!s.windows[index])
        s.windows[index] = static_cast<uint8_t*>(MapViewOfFile(s.chunks[index], FILE_MAP_WRITE, 0, 0, kChunk));
    return s.windows[index];
}

uint8_t* frame_pointer(uint64_t frame) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (!frame || frame % 16) return nullptr;
    uint64_t pool = frame / 16 - 1;
    uint8_t* w = window(s, pool / kPagesPerChunk);
    return w ? w + (pool % kPagesPerChunk) * kPage : nullptr;
}

size_t chunk_count() {
    State& s = st();
    std::lock_guard lock(s.m);
    return s.chunks.size();
}
void* chunk_view(size_t index) {
    State& s = st();
    std::lock_guard lock(s.m);
    return window(s, index);
}
const uint32_t* page_table() { return st().table.data(); }
size_t page_count() { return st().table.size(); }
void dirty(size_t* begin, size_t* end) {
    State& s = st();
    std::lock_guard lock(s.m);
    *begin = s.dirty_begin;
    *end = s.dirty_end;
}
void clear_dirty() {
    State& s = st();
    std::lock_guard lock(s.m);
    s.dirty_begin = s.dirty_end = 0;
}

Stats stats() {
    State& s = st();
    std::lock_guard lock(s.m);
    return {s.chunks.size(), s.pool_used, s.mapped, s.reserved.size()};
}

// Made at startup (the runtime has no thread-safe statics): R is reserved before the title runs, while the low
// addresses are still free.
[[maybe_unused]] const bool g_ready = init();

}  // namespace rt::uma
