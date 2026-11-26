#include "device.h"

#include <cstdio>
#include <cstring>

namespace rt::gpu {

namespace {

bool has_layer(const Vk& vk, const char* name) {
    uint32_t n = 0;
    vk.vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> v(n);
    vk.vkEnumerateInstanceLayerProperties(&n, v.data());
    for (auto& l : v)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool has_instance_ext(const Vk& vk, const char* name) {
    uint32_t n = 0;
    vk.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> v(n);
    vk.vkEnumerateInstanceExtensionProperties(nullptr, &n, v.data());
    for (auto& e : v)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

}  // namespace

VKAPI_ATTR VkBool32 VKAPI_CALL Device::on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT type,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void* self) {
    // Only validation messages count: the loader also reports "general" errors about other software's layers
    // (overlays with stale manifests and so on) that say nothing about this program.
    if (!(type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)))
        return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        static_cast<Device*>(self)->errors_++;
        std::fprintf(stderr, "[vk] error: %s\n", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[vk] warning: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

Device::Device(const DeviceOptions& opt) {
    vk.load_global();
    uint32_t api = VK_API_VERSION_1_0;
    if (vk.vkEnumerateInstanceVersion) vk.vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_2) throw std::runtime_error("the Vulkan loader is older than 1.2: update the GPU driver");

    std::vector<const char*> layers, exts;
    bool validation = opt.validation && has_layer(vk, "VK_LAYER_KHRONOS_validation");
    if (opt.validation && !validation)
        std::fprintf(stderr, "[vk] validation requested but VK_LAYER_KHRONOS_validation isn't installed\n");
    bool debug_utils = validation && has_instance_ext(vk, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation) layers.push_back("VK_LAYER_KHRONOS_validation");
    if (debug_utils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "FlamingoPort";
    app.apiVersion = VK_API_VERSION_1_2;
    VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = &Device::on_message;
    mci.pUserData = this;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = debug_utils ? &mci : nullptr;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    RT_VK_CHECK(vk.vkCreateInstance(&ici, nullptr, &instance));
    vk.load_instance(instance);
    if (debug_utils && vk.vkCreateDebugUtilsMessengerEXT)
        RT_VK_CHECK(vk.vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &messenger_));

    uint32_t n = 0;
    vk.vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vk.vkEnumeratePhysicalDevices(instance, &n, pds.data());
    if (pds.empty()) throw std::runtime_error("no Vulkan device");
    if (opt.device_index >= 0) {
        if (opt.device_index >= int(pds.size())) throw std::runtime_error("device index out of range");
        physical = pds[opt.device_index];
    } else {
        physical = pds[0];
        for (auto d : pds) {
            VkPhysicalDeviceProperties p;
            vk.vkGetPhysicalDeviceProperties(d, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { physical = d; break; }
        }
    }

    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    p11.pNext = &props12;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &p11;
    vk.vkGetPhysicalDeviceProperties2(physical, &p2);
    props = p2.properties;
    props12.pNext = nullptr;
    subgroup_size = p11.subgroupSize;
    vk.vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have12;
    vk.vkGetPhysicalDeviceFeatures2(physical, &have);

    // What the generated SPIR-V declares (rdna2spirv.assemble) and how the bindless set is used.
    std::vector<std::string> missing;
    if (props.apiVersion < VK_API_VERSION_1_2) missing.push_back("Vulkan 1.2 device");
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f12;
#define RT_NEED(have_field, want_field, label)          \
    if (have_field) want_field = VK_TRUE;               \
    else missing.push_back(label);
    RT_NEED(have.features.shaderInt64, f.features.shaderInt64, "shaderInt64 (Int64)")
    RT_NEED(have.features.shaderResourceMinLod, f.features.shaderResourceMinLod, "shaderResourceMinLod (MinLod)")
    RT_NEED(have.features.fragmentStoresAndAtomics, f.features.fragmentStoresAndAtomics, "fragmentStoresAndAtomics")
    RT_NEED(have.features.vertexPipelineStoresAndAtomics, f.features.vertexPipelineStoresAndAtomics,
            "vertexPipelineStoresAndAtomics")
    RT_NEED(have.features.shaderImageGatherExtended, f.features.shaderImageGatherExtended, "shaderImageGatherExtended")
    RT_NEED(have.features.samplerAnisotropy, f.features.samplerAnisotropy, "samplerAnisotropy")
    RT_NEED(have12.samplerMirrorClampToEdge, f12.samplerMirrorClampToEdge, "samplerMirrorClampToEdge")
    RT_NEED(have12.bufferDeviceAddress, f12.bufferDeviceAddress, "bufferDeviceAddress")
    RT_NEED(have12.descriptorIndexing, f12.descriptorIndexing, "descriptorIndexing")
    RT_NEED(have12.runtimeDescriptorArray, f12.runtimeDescriptorArray, "runtimeDescriptorArray")
    RT_NEED(have12.descriptorBindingPartiallyBound, f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound")
    RT_NEED(have12.descriptorBindingSampledImageUpdateAfterBind, f12.descriptorBindingSampledImageUpdateAfterBind,
            "descriptorBindingSampledImageUpdateAfterBind")
    RT_NEED(have12.descriptorBindingStorageImageUpdateAfterBind, f12.descriptorBindingStorageImageUpdateAfterBind,
            "descriptorBindingStorageImageUpdateAfterBind")
    RT_NEED(have12.descriptorBindingStorageBufferUpdateAfterBind, f12.descriptorBindingStorageBufferUpdateAfterBind,
            "descriptorBindingStorageBufferUpdateAfterBind")
    RT_NEED(have12.descriptorBindingUpdateUnusedWhilePending, f12.descriptorBindingUpdateUnusedWhilePending,
            "descriptorBindingUpdateUnusedWhilePending")
    RT_NEED(have12.shaderSampledImageArrayNonUniformIndexing, f12.shaderSampledImageArrayNonUniformIndexing,
            "shaderSampledImageArrayNonUniformIndexing")
    RT_NEED(have12.shaderStorageImageArrayNonUniformIndexing, f12.shaderStorageImageArrayNonUniformIndexing,
            "shaderStorageImageArrayNonUniformIndexing")
#undef RT_NEED
    // GroupNonUniform + GroupNonUniformShuffle in vertex and fragment shaders (cross-lane ops).
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if ((p11.subgroupSupportedOperations & ops) != ops) missing.push_back("subgroup basic + shuffle operations");
    if ((p11.subgroupSupportedStages & stages) != stages) missing.push_back("subgroup operations in VS and PS");
    if (!missing.empty()) {
        std::string msg = std::string(props.deviceName) + " lacks what translated shaders need:";
        for (auto& m : missing) msg += "\n  - " + m;
        throw std::runtime_error(msg);
    }

    uint32_t qn = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qf.data());
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queue_family = i;
            break;
        }
    if (queue_family == UINT32_MAX) throw std::runtime_error("no graphics + compute queue");
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    RT_VK_CHECK(vk.vkCreateDevice(physical, &dci, nullptr, &device));
    vk.load_device(device);
    vk.vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queue_family;
    RT_VK_CHECK(vk.vkCreateCommandPool(device, &cpi, nullptr, &pool_));
}

Device::~Device() {
    if (device) {
        vk.vkDeviceWaitIdle(device);
        if (pool_) vk.vkDestroyCommandPool(device, pool_, nullptr);
        vk.vkDestroyDevice(device, nullptr);
    }
    if (messenger_ && vk.vkDestroyDebugUtilsMessengerEXT) vk.vkDestroyDebugUtilsMessengerEXT(instance, messenger_, nullptr);
    if (instance) vk.vkDestroyInstance(instance, nullptr);
}

uint32_t Device::memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) const {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = memory.memoryTypes[i].propertyFlags;
        if ((f & want) != want) continue;
        if ((f & prefer) == prefer) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

std::string Device::describe() const {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s, Vulkan %u.%u.%u, subgroup size %u", props.deviceName,
                  VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                  VK_API_VERSION_PATCH(props.apiVersion), subgroup_size);
    return buf;
}

VkCommandBuffer Device::begin_once() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    RT_VK_CHECK(vk.vkAllocateCommandBuffers(device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    RT_VK_CHECK(vk.vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

void Device::end_once(VkCommandBuffer cmd) {
    RT_VK_CHECK(vk.vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    RT_VK_CHECK(vk.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    RT_VK_CHECK(vk.vkQueueWaitIdle(queue));
    vk.vkFreeCommandBuffers(device, pool_, 1, &cmd);
}

}  // namespace rt::gpu
