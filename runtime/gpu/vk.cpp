#include "vk.h"

#include <windows.h>

namespace rt::gpu {

void Vk::load_global() {
    HMODULE lib = LoadLibraryExA("vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) throw std::runtime_error("vulkan-1.dll not found: install a current GPU driver");
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) throw std::runtime_error("vulkan-1.dll has no vkGetInstanceProcAddr");
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    vkEnumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    vkEnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"));
    vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!vkCreateInstance || !vkEnumerateInstanceLayerProperties || !vkEnumerateInstanceExtensionProperties)
        throw std::runtime_error("vulkan-1.dll is missing global entry points");
}

void Vk::load_instance(VkInstance inst) {
#define RT_VK_LOAD(name)                                                                     \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(inst, #name));                 \
    if (!name) throw std::runtime_error("Vulkan instance function missing: " #name);
    RT_VK_INSTANCE_FUNCS(RT_VK_LOAD)
#undef RT_VK_LOAD
    vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT"));
    vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT"));
}

void Vk::load_device(VkDevice dev) {
#define RT_VK_LOAD(name)                                                                     \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev, #name));                    \
    if (!name) throw std::runtime_error("Vulkan device function missing: " #name);
    RT_VK_DEVICE_FUNCS(RT_VK_LOAD)
#undef RT_VK_LOAD
}

}  // namespace rt::gpu
