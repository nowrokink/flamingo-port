// The bindless descriptor set and pipeline layout every translated shader uses (set 0 + 144 bytes of push constants).
//
// Bindings, as rdna2spirv.assemble declares them:
//   0 sampled 2D images[]          4 storage 2D r32ui images[]
//   1 samplers[]                   5 sampled 1D images[]
//   2 descriptor hash table        6 sampled 3D images[]
//   3 sampled 2D-array images[]    7 storage 3D r32ui images[]
//      (cube textures go here as 6-layer 2D-array views: the shader already picked the face)
//
// A shader turns a T#/S# into an index through the hash table, then indexes the binding that matches how it
// samples. Image indices are shared by bindings 0 and 3-7: index i is written with the real view in its own binding
// and with that binding's fallback in all the others, so a shader that samples a texture with the wrong
// dimensionality reads a fallback, never an unwritten descriptor. Index 0 is the fallback in every binding
// (magenta for sampled images, zero for storage images, a nearest/clamp sampler), and is what a hash miss yields.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "desc_table.h"
#include "vk.h"

namespace rt::gpu {

class Device;

enum class ImageKind : uint32_t {
    Tex2D = 0,
    Tex2DArray = 3,
    Storage2D = 4,
    Tex1D = 5,
    Tex3D = 6,
    Storage3D = 7,
};

struct BindlessLimits {
    uint32_t images = 16384;     // per image binding, clamped to the device's update-after-bind limits
    uint32_t samplers = 2048;
    uint32_t table_slots = 4096; // initial descriptor hash table size (grows as needed)
};

class Bindless {
public:
    static constexpr uint32_t kPushConstantSize = 144;
    static constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

    Bindless(Device& dev, const BindlessLimits& limits = {});
    ~Bindless();
    Bindless(const Bindless&) = delete;
    Bindless& operator=(const Bindless&) = delete;

    // Writes a view into the binding for `kind` at a new index and returns it. The view must stay alive until
    // release_image, and be in `layout` whenever a shader may read it.
    uint32_t add_image(ImageKind kind, VkImageView view, VkImageLayout layout);
    uint32_t add_sampler(VkSampler sampler);
    void release_image(uint32_t index);      // puts fallbacks back and recycles the index
    void release_sampler(uint32_t index);

    // Hash table entries (what the game's descriptors resolve to).
    void map_tsharp(const uint32_t t[8], uint32_t image_index);
    void map_ssharp(const uint32_t s[4], uint32_t sampler_index);
    void unmap_tsharp(const uint32_t t[8]);
    void unmap_ssharp(const uint32_t s[4]);

    // Creates (once per distinct S#) a VkSampler from the descriptor bits, registers and maps it.
    uint32_t bind_ssharp(const uint32_t s[4]);

    // Copies hash table changes to the GPU. Call before submitting work that may read them. When the table had to
    // grow, the GPU buffer is replaced: any work still using the old one must have finished (the old buffer is
    // kept alive until destruction so in-flight reads stay valid either way).
    void flush();

    const DescriptorHashTable& table() const { return table_; }
    VkDescriptorSetLayout set_layout() const { return set_layout_; }
    VkPipelineLayout pipeline_layout() const { return pipeline_layout_; }
    VkDescriptorSet set() const { return set_; }
    uint32_t image_capacity() const { return image_cap_; }
    uint32_t sampler_capacity() const { return sampler_cap_; }

private:
    struct Fallback {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    struct TableBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t* mapped = nullptr;
        size_t words = 0;
    };

    Device& dev_;
    uint32_t image_cap_ = 0, sampler_cap_ = 0;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    Fallback fallback_[8];            // by binding number (1 and 2 unused)
    VkSampler fallback_sampler_ = VK_NULL_HANDLE;
    std::vector<uint32_t> free_images_, free_samplers_;
    uint32_t next_image_ = 1, next_sampler_ = 1;
    std::vector<ImageKind> image_kind_;          // by index
    DescriptorHashTable table_;
    TableBuffer table_buf_;
    uint32_t table_generation_ = 0;
    std::vector<TableBuffer> retired_;
    std::map<std::array<uint32_t, 4>, uint32_t> sampler_by_ssharp_;   // S# -> sampler index (bind_ssharp)
    std::vector<VkSampler> owned_samplers_;

    void create_fallbacks();
    void write_image(uint32_t binding, uint32_t index, VkImageView view, VkImageLayout layout);
    void write_sampler(uint32_t index, VkSampler sampler);
    void write_table_binding();
    TableBuffer make_table_buffer(size_t words);
    void destroy_table_buffer(TableBuffer& b);
};

}  // namespace rt::gpu
