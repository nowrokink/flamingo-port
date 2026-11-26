#include "heap.h"

#include <cstring>
#include <new>

#include "device.h"

namespace rt::gpu {

// ---------------------------------------------------------------- VaAllocator

VaAllocator::VaAllocator(uint64_t base, uint64_t size) : base_(base), size_(size) {
    if (base == 0) throw std::invalid_argument("VaAllocator base must be non-zero");
    if (size) free_[base] = size;
}

uint64_t VaAllocator::alloc(uint64_t size, uint64_t align) {
    if (size == 0 || align == 0 || (align & (align - 1))) return 0;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        uint64_t start = it->first, len = it->second;
        uint64_t aligned = (start + align - 1) & ~(align - 1);
        uint64_t pad = aligned - start;
        if (aligned < start || pad > len || len - pad < size) continue;
        free_.erase(it);
        uint64_t tail = len - pad - size;
        if (tail) free_[aligned + size] = tail;
        live_[aligned] = size;
        if (pad) pad_[aligned] = pad;     // the padding is owned by the block and returned with it
        used_ += pad + size;
        return aligned;
    }
    return 0;
}

bool VaAllocator::free(uint64_t addr) {
    auto it = live_.find(addr);
    if (it == live_.end()) return false;
    uint64_t pad = 0;
    if (auto p = pad_.find(addr); p != pad_.end()) {
        pad = p->second;
        pad_.erase(p);
    }
    uint64_t start = addr - pad, len = pad + it->second;
    live_.erase(it);
    used_ -= len;
    // Merge with the following and preceding free blocks.
    if (auto next = free_.find(start + len); next != free_.end()) {
        len += next->second;
        free_.erase(next);
    }
    auto prev = free_.lower_bound(start);
    if (prev != free_.begin()) {
        --prev;
        if (prev->first + prev->second == start) {
            start = prev->first;
            len += prev->second;
            free_.erase(prev);
        }
    }
    free_[start] = len;
    return true;
}

// ---------------------------------------------------------------- GpuHeap

GpuHeap::GpuHeap(Device& dev, uint64_t size, uint64_t base_va)
    : dev_(dev), base_va_(base_va), size_(size), va_(base_va, size) {
    if (base_va == 0 || size == 0 || base_va + size > kMaxWindowEnd || base_va + size < base_va)
        throw std::invalid_argument("GPU heap window must be non-empty, start above 0 and end at or below 4 GiB");
    auto& vk = dev.vk;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    RT_VK_CHECK(vk.vkCreateBuffer(dev.device, &bi, nullptr, &buffer_));
    VkMemoryRequirements mr;
    vk.vkGetBufferMemoryRequirements(dev.device, buffer_, &mr);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = dev.memory_type(mr.memoryTypeBits, host, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) throw std::runtime_error("no host-visible coherent memory for the GPU heap");
    device_local_ = dev.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // A device-local host-visible type can live in a small (256 MiB) heap when resizable BAR is off.
    if (device_local_ && dev.memory.memoryHeaps[dev.memory.memoryTypes[type].heapIndex].size < mr.size) {
        device_local_ = false;
        type = UINT32_MAX;
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount; i++) {
            auto f = dev.memory.memoryTypes[i].propertyFlags;
            if ((mr.memoryTypeBits & (1u << i)) && (f & host) == host && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX) throw std::runtime_error("no host-visible memory heap large enough for the GPU heap");
    }
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fi;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    RT_VK_CHECK(vk.vkAllocateMemory(dev.device, &ai, nullptr, &memory_));
    RT_VK_CHECK(vk.vkBindBufferMemory(dev.device, buffer_, memory_, 0));
    void* p = nullptr;
    RT_VK_CHECK(vk.vkMapMemory(dev.device, memory_, 0, VK_WHOLE_SIZE, 0, &p));
    mapped_ = static_cast<uint8_t*>(p);
    std::memset(mapped_, 0, size);
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = buffer_;
    bda_ = vk.vkGetBufferDeviceAddress(dev.device, &dai);
}

GpuHeap::~GpuHeap() {
    auto& vk = dev_.vk;
    if (mapped_) vk.vkUnmapMemory(dev_.device, memory_);
    if (buffer_) vk.vkDestroyBuffer(dev_.device, buffer_, nullptr);
    if (memory_) vk.vkFreeMemory(dev_.device, memory_, nullptr);
}

uint64_t GpuHeap::alloc(uint64_t size, uint64_t align) {
    std::lock_guard lock(mutex_);
    uint64_t va = va_.alloc(size, align);
    if (!va) throw std::bad_alloc();
    return va;
}

void GpuHeap::free(uint64_t va) {
    std::lock_guard lock(mutex_);
    va_.free(va);
}

uint64_t GpuHeap::used() const {
    std::lock_guard lock(mutex_);
    return va_.used();
}

bool GpuHeap::contains(uint64_t va, uint64_t size) const {
    return va >= base_va_ && size <= size_ && va - base_va_ <= size_ - size;
}

void* GpuHeap::cpu(uint64_t va) const {
    return contains(va) ? mapped_ + (va - base_va_) : nullptr;
}

uint64_t GpuHeap::upload(const void* data, uint64_t size, uint64_t align) {
    uint64_t va = alloc(size, align);
    std::memcpy(cpu(va), data, size);
    return va;
}

}  // namespace rt::gpu
