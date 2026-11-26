// The Xbox GPU virtual-address window as one Vulkan buffer.
//
// Translated shaders compute Xbox GPU VAs and access `heap_base + VA` through buffer device addresses (design.md,
// "Xbox GPU addresses"). The runtime hands out the VAs itself (GetGPUVirtualAddress returns what we allocate), so
// one buffer covering [base_va, base_va + size) is enough: heap_base = BDA(buffer) - base_va.
//
// The console has unified memory and the game writes descriptors, constants and vertex data through CPU pointers,
// so the buffer is host-visible and persistently mapped: cpu(va) is the CPU pointer for a VA. Device-local
// host-visible memory (resizable BAR) is preferred when the GPU has it.
//
// Some shaders force the high dword of a descriptor-table pointer to 0, so the whole window stays below 4 GiB.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>

#include "vk.h"

namespace rt::gpu {

class Device;

// First-fit range allocator with coalescing. Pure bookkeeping, no GPU objects.
class VaAllocator {
public:
    VaAllocator(uint64_t base, uint64_t size);
    // Returns 0 when nothing fits (0 is never a valid address: base must be > 0). align must be a power of two.
    uint64_t alloc(uint64_t size, uint64_t align);
    // Frees a block returned by alloc. Returns false for an address that isn't the start of a live block.
    bool free(uint64_t addr);
    uint64_t used() const { return used_; }
    uint64_t base() const { return base_; }
    uint64_t size() const { return size_; }
    size_t free_blocks() const { return free_.size(); }

private:
    uint64_t base_, size_, used_ = 0;
    std::map<uint64_t, uint64_t> free_;   // start -> size
    std::map<uint64_t, uint64_t> live_;   // start -> size (as reserved, including alignment padding at the front)
    std::map<uint64_t, uint64_t> pad_;    // live start -> padding before it that belongs to the block
};

class GpuHeap {
public:
    static constexpr uint64_t kDefaultBase = 0x10000;              // keeps VA 0 (null) invalid
    static constexpr uint64_t kMaxWindowEnd = 0x100000000ull;      // everything below 4 GiB

    GpuHeap(Device& dev, uint64_t size, uint64_t base_va = kDefaultBase);
    ~GpuHeap();
    GpuHeap(const GpuHeap&) = delete;
    GpuHeap& operator=(const GpuHeap&) = delete;

    uint64_t alloc(uint64_t size, uint64_t align = 256);   // throws std::bad_alloc when the window is full
    void free(uint64_t va);

    // CPU pointer for a VA inside the window (nullptr outside it).
    void* cpu(uint64_t va) const;
    bool contains(uint64_t va, uint64_t size = 1) const;

    // Copies bytes into a fresh allocation and returns its VA (e.g. a shader's FSSM blob: the code base).
    uint64_t upload(const void* data, uint64_t size, uint64_t align = 256);

    // The 64-bit value translated shaders take as push-constant dwords 32-33.
    uint64_t heap_base() const { return bda_ - base_va_; }
    uint64_t base_va() const { return base_va_; }
    uint64_t size() const { return size_; }
    uint64_t used() const;
    VkBuffer buffer() const { return buffer_; }
    bool device_local() const { return device_local_; }

private:
    Device& dev_;
    uint64_t base_va_, size_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    uint8_t* mapped_ = nullptr;
    VkDeviceAddress bda_ = 0;
    bool device_local_ = false;
    mutable std::mutex mutex_;
    VaAllocator va_;
};

}  // namespace rt::gpu
