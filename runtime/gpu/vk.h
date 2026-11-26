// Vulkan entry points, loaded at runtime from vulkan-1.dll (no import library or SDK loader needed to run).
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace rt::gpu {

#define RT_VK_INSTANCE_FUNCS(X)                                                                                \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                        \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice)            \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)

#define RT_VK_DEVICE_FUNCS(X)                                                                                  \
    X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkQueueWaitIdle)             \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)                \
    X(vkGetBufferDeviceAddress) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory)            \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                    \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)                          \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)                   \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                           \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule)     \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateRenderPass) X(vkDestroyRenderPass)             \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateCommandPool) X(vkDestroyCommandPool)              \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)          \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)          \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw)                        \
    X(vkCmdBindVertexBuffers) X(vkCmdClearColorImage)

struct Vk {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;    // optional (validation only)
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;
#define RT_VK_DECL(name) PFN_##name name = nullptr;
    RT_VK_INSTANCE_FUNCS(RT_VK_DECL)
    RT_VK_DEVICE_FUNCS(RT_VK_DECL)
#undef RT_VK_DECL

    void load_global();                 // vulkan-1.dll and the pre-instance functions
    void load_instance(VkInstance inst);
    void load_device(VkDevice dev);
};

struct VkError : std::runtime_error {
    VkResult result;
    VkError(const char* what, VkResult r) : std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(r)), result(r) {}
};

inline void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw VkError(what, r);
}

}  // namespace rt::gpu

#define RT_VK_CHECK(call) ::rt::gpu::check((call), #call)
