// Tests the xmem.dll shims the way game_scarlett_master.exe calls them (flags taken from its call sites).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../shims/shims.h"
#include "../shims/uma.h"

namespace {

constexpr uint64_t kPage = 64 * 1024;
int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

template <class F>
F shim(const char* name) {
    void* p = rt::shims::find("xmem.dll", name);
    if (!p) {
        std::fprintf(stderr, "FAIL: xmem.dll!%s is not registered\n", name);
        std::exit(1);
    }
    return reinterpret_cast<F>(p);
}

using VirtualAllocFn = void*(__stdcall*)(void*, SIZE_T, DWORD, uint64_t, DWORD);
using AllocPagesFn = BOOL(__stdcall*)(DWORD, ULONG_PTR*, ULONG_PTR*);
using FreePagesFn = BOOL(__stdcall*)(ULONG_PTR, ULONG_PTR*);
using MapFn = void*(__stdcall*)(void*, ULONG_PTR, ULONG_PTR*);
using ScatterFn = BOOL(__stdcall*)(void**, ULONG_PTR, ULONG_PTR*);
using StatsFn = BOOL(__stdcall*)(DWORD, void*);
using BoolFn = BOOL(__stdcall*)();
using TransferFn = BOOL(__stdcall*)(DWORD, SIZE_T);
using AllocFn = void*(__stdcall*)(size_t, uint64_t);
using FreeFn = void(__stdcall*)(void*, uint64_t);

uint32_t* word(void* base, uint64_t page) { return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(base) + page * kPage); }

}  // namespace

int main() {
    auto valloc = shim<VirtualAllocFn>("XMemVirtualAlloc");
    auto alloc_pages = shim<AllocPagesFn>("XMemAllocatePhysicalPages");
    auto free_pages = shim<FreePagesFn>("XMemFreePhysicalPages");
    auto map = shim<MapFn>("XMemMapPhysicalPages");
    auto scatter = shim<ScatterFn>("XMemMapPhysicalPagesScatter");

    // Heap.
    {
        auto a = shim<AllocFn>("XMemAllocDefault");
        auto f = shim<FreeFn>("XMemFreeDefault");
        void* p = a(393216, 0xF0820000);
        CHECK(p && reinterpret_cast<uintptr_t>(p) % 64 == 0);
        std::memset(p, 0xAB, 393216);
        f(p, 0xF0820000);
    }

    // Physical pages: 4 granted, as requested.
    ULONG_PTR pages[4] = {}, count = 4;
    CHECK(alloc_pages(0x280, &count, pages) == TRUE);
    CHECK(count == 4);
    CHECK(pages[0] && pages[0] % 16 == 0 && pages[1] == pages[0] + 16);  // 4 KiB frames, 16 per page

    // MEM_LARGE_PAGES | MEM_PHYSICAL | MEM_RESERVE, then map all 4 pages and write each.
    void* a = valloc(nullptr, 4 * kPage, 0x20402000, 0x21, PAGE_READWRITE);
    CHECK(a && reinterpret_cast<uintptr_t>(a) % (2u << 20) == 0);
    CHECK(map(a, 4, pages) == a);
    for (uint32_t i = 0; i < 4; ++i) *word(a, i) = 0x1000 + i;

    // A second region showing page 2 then page 0: same memory at another address.
    void* b = valloc(nullptr, 2 * kPage, 0x20402000, 3, 0x801);
    CHECK(b != nullptr);
    ULONG_PTR alias[2] = {pages[2], pages[0]};
    CHECK(map(b, 2, alias) == b);
    CHECK(*word(b, 0) == 0x1002 && *word(b, 1) == 0x1000);
    *word(b, 1) = 0xBEEF;
    CHECK(*word(a, 0) == 0xBEEF);

    // Unmap b's first page (NULL page array): reads now fault; remap it to page 3.
    CHECK(map(b, 1, nullptr) == b);
    MEMORY_BASIC_INFORMATION mbi{};
    auto vquery = reinterpret_cast<SIZE_T(WINAPI*)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T)>(
        rt::shims::find("api-ms-win-core-memory-l1-1-0.dll", "VirtualQuery"));
    auto vfree = reinterpret_cast<BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD)>(
        rt::shims::find("api-ms-win-core-memory-l1-1-0.dll", "VirtualFree"));
    auto vcommit = reinterpret_cast<LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD)>(
        rt::shims::find("api-ms-win-core-memory-l1-1-0.dll", "VirtualAlloc"));
    CHECK(vquery && vfree && vcommit);
    vquery(b, &mbi, sizeof(mbi));
    CHECK(mbi.State == MEM_RESERVE);
    CHECK(map(b, 1, &pages[3]) == b);
    CHECK(*word(b, 0) == 0x1003);

    // Remap over a mapped page without unmapping first.
    CHECK(map(b, 1, &pages[1]) == b);
    CHECK(*word(b, 0) == 0x1001);

    // The game's own pattern: reserve a placeholder (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS), split a
    // piece off with VirtualFree, then turn that piece into a physical region with MEM_REPLACE_PLACEHOLDER.
    uint8_t* big = static_cast<uint8_t*>(valloc(nullptr, 8 * kPage, 0x42000, 7, PAGE_NOACCESS));
    CHECK(big != nullptr);
    CHECK(vfree(big, 3 * kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));  // the title's split, through our shim
    void* sub = valloc(big, 3 * kPage, 0x20406000, 0x27, 0x1004);
    CHECK(sub == big);
    ULONG_PTR mid = pages[2];
    CHECK(map(big + kPage, 1, &mid) == big + kPage);   // middle page only
    CHECK(*word(big, 1) == 0x1002);

    // Scatter: page 3 at big+0, page 0 at big+2*64K; then unmap both.
    void* addrs[2] = {big, big + 2 * kPage};
    ULONG_PTR sp[2] = {pages[3], pages[0]};
    CHECK(scatter(addrs, 2, sp) == TRUE);
    CHECK(*word(big, 0) == 0x1003 && *word(big, 2) == 0xBEEF);
    CHECK(scatter(addrs, 2, nullptr) == TRUE);

    // Mapping outside unified memory fails cleanly.
    void* outside = VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(map(outside, 1, pages) == nullptr);
    VirtualFree(outside, 0, MEM_RELEASE);

    // Ordinary committed memory (MEM_LARGE_PAGES | MEM_RESERVE | MEM_COMMIT, console protect bits).
    auto* c = static_cast<uint8_t*>(valloc(nullptr, 3 * kPage, 0x20003000, 0x183, 0x22004));
    CHECK(c != nullptr);
    if (c) { c[0] = 1; c[3 * kPage - 1] = 2; }

    // Committed pages read as zero; the kernel32 calls work on unified memory (commit, query, decommit, release).
    CHECK(c && *word(c, 1) == 0);
    void* r = valloc(nullptr, 4 * kPage, MEM_RESERVE, 0, PAGE_READWRITE);
    CHECK(r && vcommit(static_cast<uint8_t*>(r) + kPage, kPage, MEM_COMMIT, PAGE_READWRITE) ==
                   static_cast<uint8_t*>(r) + kPage);
    *word(r, 1) = 7;
    CHECK(vquery(static_cast<uint8_t*>(r) + kPage, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
          mbi.RegionSize == kPage && mbi.AllocationBase == r);
    CHECK(vquery(r, &mbi, sizeof(mbi)) && mbi.State == MEM_RESERVE);
    auto before = rt::uma::stats().pool_pages_used;
    CHECK(vfree(static_cast<uint8_t*>(r) + kPage, kPage, MEM_DECOMMIT));
    CHECK(rt::uma::stats().pool_pages_used == before - 1);
    CHECK(vfree(r, 0, MEM_RELEASE));
    CHECK(vquery(r, &mbi, sizeof(mbi)) && mbi.State == MEM_FREE);

    // The GPU page table: a mapped page's entry, the same entry at an alias, nothing where unmapped.
    auto entry = [](const void* p) {
        return rt::uma::page_table()[(reinterpret_cast<uintptr_t>(p) - rt::uma::base()) / kPage];
    };
    CHECK(entry(word(a, 0)) != 0 && entry(word(a, 0)) == pages[0] / 16);  // pool index + 1 = frame / 16
    CHECK(entry(word(b, 0)) == entry(word(a, 1)));                         // page 1 at both
    CHECK(entry(big + 5 * kPage) == 0);

    // Freed physical pages stay valid while mapped, and go back to the pool with their last mapping.
    before = rt::uma::stats().pool_pages_used;
    CHECK(free_pages(4, pages) == TRUE);
    CHECK(*word(a, 3) == 0x1003);
    CHECK(rt::uma::stats().pool_pages_used == before);
    CHECK(map(a, 4, nullptr) == a);
    CHECK(map(b, 2, nullptr) == b);
    CHECK(map(big + kPage, 1, nullptr) == big + kPage);
    CHECK(rt::uma::stats().pool_pages_used == before - 4);

    // Queries.
    uint8_t stats[120] = {};
    CHECK(shim<StatsFn>("XMemGetWorkingSetStatistics")(0x20000001, stats) == TRUE);
    uint64_t budget, used;
    std::memcpy(&budget, stats, 8);
    std::memcpy(&used, stats + 8, 8);
    CHECK(budget > used && used > 0);
    CHECK(shim<BoolFn>("XMemIsToolsMemAvailable")() == TRUE);
    CHECK(shim<TransferFn>("XMemTransferMemory")(1, 16 << 20) == TRUE);

    if (g_failures) {
        std::fprintf(stderr, "xmem_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("xmem_test: ok\n");
    return 0;
}
