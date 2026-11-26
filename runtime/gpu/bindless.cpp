#include "bindless.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "device.h"
#include "ssharp.h"

namespace rt::gpu {

namespace {

constexpr uint32_t kImageBindings[] = {0, 3, 4, 5, 6, 7};
constexpr uint32_t kSamplerBinding = 1, kTableBinding = 2;

bool is_storage(uint32_t binding) { return binding == 4 || binding == 7; }

}  // namespace

Bindless::Bindless(Device& dev, const BindlessLimits& limits) : dev_(dev), table_(limits.table_slots) {
    auto& vk = dev.vk;
    const auto& l = dev.props.limits;
    const auto& p12 = dev.props12;
    if (l.maxPushConstantsSize < kPushConstantSize)
        throw std::runtime_error("device push constant limit is below the 144 bytes translated shaders use");

    // Four sampled-image bindings and two storage-image bindings share the image index space.
    image_cap_ = limits.images;
    image_cap_ = std::min(image_cap_, p12.maxDescriptorSetUpdateAfterBindSampledImages / 4);
    image_cap_ = std::min(image_cap_, p12.maxPerStageDescriptorUpdateAfterBindSampledImages / 4);
    image_cap_ = std::min(image_cap_, p12.maxDescriptorSetUpdateAfterBindStorageImages / 2);
    image_cap_ = std::min(image_cap_, p12.maxPerStageDescriptorUpdateAfterBindStorageImages / 2);
    sampler_cap_ = limits.samplers;
    sampler_cap_ = std::min(sampler_cap_, p12.maxDescriptorSetUpdateAfterBindSamplers);
    sampler_cap_ = std::min(sampler_cap_, p12.maxPerStageDescriptorUpdateAfterBindSamplers);
    uint64_t total = 6ull * image_cap_ + sampler_cap_ + 1;
    if (total > p12.maxPerStageUpdateAfterBindResources) {
        uint32_t room = p12.maxPerStageUpdateAfterBindResources > sampler_cap_ + 1
                            ? (p12.maxPerStageUpdateAfterBindResources - sampler_cap_ - 1) / 6 : 0;
        image_cap_ = std::min(image_cap_, room);
    }
    if (image_cap_ < 2 || sampler_cap_ < 2) throw std::runtime_error("device bindless descriptor limits are too small");
    image_kind_.assign(image_cap_, ImageKind::Tex2D);

    // ---- set layout: every binding partially bound and updatable after bind / while unused-but-pending
    VkDescriptorSetLayoutBinding b[8]{};
    VkDescriptorBindingFlags flags[8]{};
    for (uint32_t i = 0; i < 8; i++) {
        b[i].binding = i;
        b[i].stageFlags = kStages;
        flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    for (uint32_t i : kImageBindings) {
        b[i].descriptorType = is_storage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[i].descriptorCount = image_cap_;
    }
    b[kSamplerBinding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[kSamplerBinding].descriptorCount = sampler_cap_;
    b[kTableBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[kTableBinding].descriptorCount = 1;
    flags[kTableBinding] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;   // always written, replaced on growth
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 8;
    bfi.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &bfi;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 8;
    lci.pBindings = b;
    RT_VK_CHECK(vk.vkCreateDescriptorSetLayout(dev.device, &lci, nullptr, &set_layout_));

    VkPushConstantRange pcr{kStages, 0, kPushConstantSize};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    RT_VK_CHECK(vk.vkCreatePipelineLayout(dev.device, &pli, nullptr, &pipeline_layout_));

    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * image_cap_},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * image_cap_},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, sampler_cap_},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    RT_VK_CHECK(vk.vkCreateDescriptorPool(dev.device, &dpi, nullptr, &pool_));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    RT_VK_CHECK(vk.vkAllocateDescriptorSets(dev.device, &dai, &set_));

    create_fallbacks();
    for (uint32_t i : kImageBindings) write_image(i, 0, fallback_[i].view, fallback_[i].layout);
    write_sampler(0, fallback_sampler_);

    table_buf_ = make_table_buffer(table_.words().size());
    std::memcpy(table_buf_.mapped, table_.words().data(), table_.words().size() * 4);
    table_.clear_dirty();
    table_generation_ = table_.generation();
    write_table_binding();
}

Bindless::~Bindless() {
    auto& vk = dev_.vk;
    VkDevice d = dev_.device;
    vk.vkDeviceWaitIdle(d);
    for (auto s : owned_samplers_) vk.vkDestroySampler(d, s, nullptr);
    if (fallback_sampler_) vk.vkDestroySampler(d, fallback_sampler_, nullptr);
    for (auto& f : fallback_) {
        if (f.view) vk.vkDestroyImageView(d, f.view, nullptr);
        if (f.image) vk.vkDestroyImage(d, f.image, nullptr);
        if (f.memory) vk.vkFreeMemory(d, f.memory, nullptr);
    }
    destroy_table_buffer(table_buf_);
    for (auto& r : retired_) destroy_table_buffer(r);
    if (pool_) vk.vkDestroyDescriptorPool(d, pool_, nullptr);
    if (pipeline_layout_) vk.vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
    if (set_layout_) vk.vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
}

void Bindless::create_fallbacks() {
    auto& vk = dev_.vk;
    VkDevice d = dev_.device;
    for (uint32_t binding : kImageBindings) {
        auto& f = fallback_[binding];
        bool storage = is_storage(binding);
        bool is3d = binding == 6 || binding == 7, is1d = binding == 5, arr = binding == 3;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = is3d ? VK_IMAGE_TYPE_3D : is1d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
        ii.format = storage ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {1, 1, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT);
        RT_VK_CHECK(vk.vkCreateImage(d, &ii, nullptr, &f.image));
        VkMemoryRequirements mr;
        vk.vkGetImageMemoryRequirements(d, f.image, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = dev_.memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = dev_.memory_type(mr.memoryTypeBits, 0);
        RT_VK_CHECK(vk.vkAllocateMemory(d, &ai, nullptr, &f.memory));
        RT_VK_CHECK(vk.vkBindImageMemory(d, f.image, f.memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = f.image;
        vi.viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D : is1d ? VK_IMAGE_VIEW_TYPE_1D
                      : arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        RT_VK_CHECK(vk.vkCreateImageView(d, &vi, nullptr, &f.view));
        f.layout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    dev_.submit_and_wait([&](VkCommandBuffer cmd) {
        for (uint32_t binding : kImageBindings) {
            auto& f = fallback_[binding];
            bool storage = is_storage(binding);
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = f.image;
            ib.subresourceRange = range;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dev_.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                         nullptr, 0, nullptr, 1, &ib);
            VkClearColorValue c{};
            if (!storage) c.float32[0] = c.float32[2] = c.float32[3] = 1.0f;   // magenta: easy to spot
            dev_.vk.vkCmdClearColorImage(cmd, f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
            ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.newLayout = f.layout;
            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | (storage ? VK_ACCESS_SHADER_WRITE_BIT : 0);
            dev_.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                         nullptr, 0, nullptr, 1, &ib);
        }
    });
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    RT_VK_CHECK(vk.vkCreateSampler(d, &sci, nullptr, &fallback_sampler_));
}

void Bindless::write_image(uint32_t binding, uint32_t index, VkImageView view, VkImageLayout layout) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = binding;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = is_storage(binding) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    dev_.vk.vkUpdateDescriptorSets(dev_.device, 1, &w, 0, nullptr);
}

void Bindless::write_sampler(uint32_t index, VkSampler sampler) {
    VkDescriptorImageInfo ii{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = kSamplerBinding;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    dev_.vk.vkUpdateDescriptorSets(dev_.device, 1, &w, 0, nullptr);
}

void Bindless::write_table_binding() {
    VkDescriptorBufferInfo bi{table_buf_.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = kTableBinding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    dev_.vk.vkUpdateDescriptorSets(dev_.device, 1, &w, 0, nullptr);
}

uint32_t Bindless::add_image(ImageKind kind, VkImageView view, VkImageLayout layout) {
    uint32_t idx;
    if (!free_images_.empty()) {
        idx = free_images_.back();
        free_images_.pop_back();
    } else {
        if (next_image_ >= image_cap_) throw std::runtime_error("bindless image capacity exhausted");
        idx = next_image_++;
    }
    image_kind_[idx] = kind;
    for (uint32_t b : kImageBindings) {
        if (b == uint32_t(kind)) write_image(b, idx, view, layout);
        else write_image(b, idx, fallback_[b].view, fallback_[b].layout);
    }
    return idx;
}

uint32_t Bindless::add_sampler(VkSampler sampler) {
    uint32_t idx;
    if (!free_samplers_.empty()) {
        idx = free_samplers_.back();
        free_samplers_.pop_back();
    } else {
        if (next_sampler_ >= sampler_cap_) throw std::runtime_error("bindless sampler capacity exhausted");
        idx = next_sampler_++;
    }
    write_sampler(idx, sampler);
    return idx;
}

void Bindless::release_image(uint32_t index) {
    if (index == 0 || index >= next_image_) return;
    for (uint32_t b : kImageBindings) write_image(b, index, fallback_[b].view, fallback_[b].layout);
    free_images_.push_back(index);
}

void Bindless::release_sampler(uint32_t index) {
    if (index == 0 || index >= next_sampler_) return;
    write_sampler(index, fallback_sampler_);
    free_samplers_.push_back(index);
}

void Bindless::map_tsharp(const uint32_t t[8], uint32_t image_index) {
    table_.insert(DescriptorHashTable::tsharp_key(t), image_index);
}

void Bindless::map_ssharp(const uint32_t s[4], uint32_t sampler_index) {
    table_.insert(DescriptorHashTable::ssharp_key(s), sampler_index);
}

void Bindless::unmap_tsharp(const uint32_t t[8]) { table_.remove(DescriptorHashTable::tsharp_key(t)); }
void Bindless::unmap_ssharp(const uint32_t s[4]) { table_.remove(DescriptorHashTable::ssharp_key(s)); }

uint32_t Bindless::bind_ssharp(const uint32_t s[4]) {
    std::array<uint32_t, 4> key{s[0], s[1], s[2], s[3]};
    if (auto it = sampler_by_ssharp_.find(key); it != sampler_by_ssharp_.end()) return it->second;
    SamplerDecode dec = decode_ssharp(s);
    if (dec.info.anisotropyEnable)
        dec.info.maxAnisotropy = std::min(dec.info.maxAnisotropy, dev_.props.limits.maxSamplerAnisotropy);
    static std::set<std::string> logged;
    for (auto& a : dec.approximations)
        if (logged.insert(a).second) std::fprintf(stderr, "[gpu] sampler approximation: %s\n", a.c_str());
    VkSampler smp;
    RT_VK_CHECK(dev_.vk.vkCreateSampler(dev_.device, &dec.info, nullptr, &smp));
    owned_samplers_.push_back(smp);
    uint32_t idx = add_sampler(smp);
    map_ssharp(s, idx);
    sampler_by_ssharp_[key] = idx;
    return idx;
}

Bindless::TableBuffer Bindless::make_table_buffer(size_t words) {
    auto& vk = dev_.vk;
    TableBuffer t;
    t.words = words;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = words * 4;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    RT_VK_CHECK(vk.vkCreateBuffer(dev_.device, &bi, nullptr, &t.buffer));
    VkMemoryRequirements mr;
    vk.vkGetBufferMemoryRequirements(dev_.device, t.buffer, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = dev_.memory_type(mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) throw std::runtime_error("no host-visible memory for the descriptor table");
    RT_VK_CHECK(vk.vkAllocateMemory(dev_.device, &ai, nullptr, &t.memory));
    RT_VK_CHECK(vk.vkBindBufferMemory(dev_.device, t.buffer, t.memory, 0));
    void* p;
    RT_VK_CHECK(vk.vkMapMemory(dev_.device, t.memory, 0, VK_WHOLE_SIZE, 0, &p));
    t.mapped = static_cast<uint32_t*>(p);
    return t;
}

void Bindless::destroy_table_buffer(TableBuffer& b) {
    auto& vk = dev_.vk;
    if (b.mapped) vk.vkUnmapMemory(dev_.device, b.memory);
    if (b.buffer) vk.vkDestroyBuffer(dev_.device, b.buffer, nullptr);
    if (b.memory) vk.vkFreeMemory(dev_.device, b.memory, nullptr);
    b = {};
}

void Bindless::flush() {
    const auto& w = table_.words();
    if (table_.generation() != table_generation_) {
        retired_.push_back(table_buf_);
        table_buf_ = make_table_buffer(w.size());
        std::memcpy(table_buf_.mapped, w.data(), w.size() * 4);
        write_table_binding();
        table_generation_ = table_.generation();
    } else if (table_.dirty_begin() != table_.dirty_end()) {
        std::memcpy(table_buf_.mapped + table_.dirty_begin(), w.data() + table_.dirty_begin(),
                    size_t(table_.dirty_end() - table_.dirty_begin()) * 4);
    }
    table_.clear_dirty();
}

}  // namespace rt::gpu
