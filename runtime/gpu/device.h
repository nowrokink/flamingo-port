// Vulkan instance + device with every feature the translated SPIR-V relies on (see docs/design.md, runtime
// contract). Creation fails with a list of everything missing rather than the first thing.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "vk.h"

namespace rt::gpu {

struct DeviceOptions {
    bool validation = false;    // enable VK_LAYER_KHRONOS_validation when installed (Vulkan SDK)
    int device_index = -1;      // -1: first discrete GPU, else the first device
};

class Device {
public:
    explicit Device(const DeviceOptions& opt = {});
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Vk vk;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceVulkan12Properties props12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t subgroup_size = 0;    // translated shaders need subgroup size == the shader's wave size (32 or 64)

    // Memory type with all of `want`, preferring one that also has `prefer`; UINT32_MAX when none.
    uint32_t memory_type(uint32_t type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0) const;

    // Validation messages reported as errors since creation (0 when validation is off).
    uint32_t validation_errors() const { return errors_.load(); }
    std::string describe() const;

    // Records, submits and waits for a one-off command buffer.
    template <class F>
    void submit_and_wait(F&& record);

private:
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::atomic<uint32_t> errors_{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                      void* self);
    VkCommandBuffer begin_once();
    void end_once(VkCommandBuffer cmd);
};

template <class F>
void Device::submit_and_wait(F&& record) {
    VkCommandBuffer cmd = begin_once();
    record(cmd);
    end_once(cmd);
}

}  // namespace rt::gpu
