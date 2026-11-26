// Unified memory: the title's GPU-visible memory (docs/gpu-memory.md).
//
// A pool of 64 KiB pages in 1 GiB pagefile-backed sections ("chunks", each later a D3D12 heap), and one reserved
// address range R, starting below 4 GiB, where every GPU-visible title allocation lives. R is a Windows placeholder
// split on demand: a committed or mapped page of R is a view of a pool page, so the CPU writes pool memory and the
// GPU, reading the same chunks, sees it. A page table (one entry per page of R: pool index + 1, 0 = unmapped)
// mirrors the mapping for translated shaders.
//
// Page numbers handed to the title are 4 KiB frames, as on the console: frame = (pool index + 1) * 16.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace rt::uma {

constexpr uint64_t kPage = 64 * 1024;
constexpr uint64_t kChunk = 1ull << 30;
constexpr uint64_t kPagesPerChunk = kChunk / kPage;

// Sets up R (once; true if R exists). Called at startup.
bool init();

uintptr_t base();
uint64_t size();
bool contains(uintptr_t va, uint64_t bytes = 1);

// ---- address reservations in R (bookkeeping: R stays placeholder until pages are mapped)
// A new reservation of `bytes` aligned to `align` (a power of two, at least kPage), or at exactly `at`; 0 if none fits.
uintptr_t reserve(uint64_t bytes, uint64_t align, uintptr_t at = 0);
// A new reservation wholly below 4 GiB (descriptor heaps: shaders may zero a table pointer's high dword). Ordinary
// reservations stay above 4 GiB while R has room there, so this space is kept for these.
uintptr_t reserve_low(uint64_t bytes, uint64_t align);
// The reservation holding `va`: its start and size; false if none.
bool reservation(uintptr_t va, uintptr_t* start, uint64_t* bytes);
// Frees a reservation (by its start): unmaps its pages; pages it committed go back to the pool.
bool release(uintptr_t start);

// ---- pages
// Commits [va, va + bytes) (whole pages, inside a reservation) with fresh pool pages; pages already mapped stay.
bool commit(uintptr_t va, uint64_t bytes, ULONG protect);
// Unmaps [va, va + bytes); committed pages go back to the pool, mapped physical pages stay the title's.
bool decommit(uintptr_t va, uint64_t bytes);
// A hook called for each page of R that stops showing what it showed (decommitted, released, unmapped or about to be
// mapped to another page), with the pool's lock held: it must only take note. What was placed there is gone.
void on_unmap(void (*hook)(uintptr_t va));
// Maps the title's physical page `frame` (0 unmaps) at `va`.
bool map_frame(uintptr_t va, uint64_t frame, ULONG protect);
// A second address below 4 GiB for the pages of [va, va + bytes) (which must all be mapped): the same pool pages,
// so writes through either show at both. For GPU addresses of memory the title placed higher (descriptor heaps).
// Returns the new address, or 0.
uintptr_t alias_low(uintptr_t va, uint64_t bytes);

// ---- the title's physical pages (XMemAllocatePhysicalPages)
bool alloc_frames(size_t count, uint64_t* frames);
void free_frames(size_t count, const uint64_t* frames);
bool valid_frame(uint64_t frame);
// The runtime's own pointer to the 64 KiB of physical page `frame` (a view of its whole chunk, made on first use);
// null for a frame that isn't one.
uint8_t* frame_pointer(uint64_t frame);

// ---- queries
// What VirtualQuery would say about `va` in R (state, type, protection, the run of pages alike).
bool query(uintptr_t va, MEMORY_BASIC_INFORMATION* info);

// ---- for the GPU side
size_t chunk_count();
// Bytes of pool in use (64 KiB pages taken by commits and frame allocations): the title's memory.
uint64_t pool_bytes_used();
// The whole chunk mapped at one address (the view frame_pointer uses), for OpenExistingHeapFromAddress. Not
// OpenExistingHeapFromFileMapping on the section: with the NVIDIA driver (32.0.16.1088) the section's commit stays
// held after the heap, the device and the process are gone, until a reboot.
void* chunk_view(size_t index);
// The page table (one uint32 per page of R) and the range of entries changed since clear_dirty().
const uint32_t* page_table();
size_t page_count();
void dirty(size_t* begin, size_t* end);
void clear_dirty();

// Statistics for logs and tests.
struct Stats {
    size_t chunks, pool_pages_used, pages_mapped, reservations;
};
Stats stats();

}  // namespace rt::uma
