// End-to-end GPU test of the runtime's Vulkan layer (runtime/gpu) with translated shaders.
//
//   gpu_layer_test <spv dir> [--validation]
//
// <spv dir> holds .spv files assembled from tests/translator/expected/*.spvasm (scripts/test_runtime.ps1 does it):
//   VS_FullscreenUV.spv            full-screen triangle, param0 = (u, v) * 2 at the vertices
//   PS_SampleHeapDescriptors.spv   reads a T# + S# from the heap through s4 and samples
//   PS_gpu_ps_sdwa.spv, PS_gpu_ps_ops.spv   constant outputs with known bit patterns
//
// What it checks:
//   1. sampling: T#/S# written into the GPU heap at allocated Xbox VAs, the descriptor hash table (started tiny and
//      forced to grow, so the GPU buffer is replaced), a decoded S# sampler, a decoy texture, magenta fallback at 0.
//      Every pixel must equal the nearest texel of an 8x8 gradient.
//   2. a T# that isn't registered must give the fallback (magenta) on every pixel.
//   3. ps_sdwa / ps_ops: every pixel's bits equal the values in the comments of runtime/test/shaders/*.asm.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../gpu/bindless.h"
#include "../gpu/device.h"
#include "../gpu/heap.h"
#include "../gpu/literal_samplers.h"
#include "../gpu/push.h"

#include <filesystem>

using namespace rt::gpu;

namespace {

constexpr uint32_t W = 64, H = 64, TW = 8;

std::vector<uint32_t> read_spv(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("can't open " + path);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> w(b.size() / 4);
    std::memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

class Fixture {
public:
    Device& dev;
    Vk& vk;
    explicit Fixture(Device& d) : dev(d), vk(d.vk) {}
    ~Fixture() {
        vk.vkDeviceWaitIdle(dev.device);
        for (auto p : pipelines) vk.vkDestroyPipeline(dev.device, p, nullptr);
        for (auto m : modules) vk.vkDestroyShaderModule(dev.device, m, nullptr);
        for (auto& i : images) {
            vk.vkDestroyImageView(dev.device, i.view, nullptr);
            vk.vkDestroyImage(dev.device, i.image, nullptr);
            vk.vkFreeMemory(dev.device, i.memory, nullptr);
        }
        for (auto& b : buffers) {
            vk.vkDestroyBuffer(dev.device, b.buffer, nullptr);
            vk.vkFreeMemory(dev.device, b.memory, nullptr);
        }
        for (auto s : samplers) vk.vkDestroySampler(dev.device, s, nullptr);
        if (fb) vk.vkDestroyFramebuffer(dev.device, fb, nullptr);
        if (rp) vk.vkDestroyRenderPass(dev.device, rp, nullptr);
    }

    std::vector<VkPipeline> pipelines;
    std::vector<VkShaderModule> modules;
    std::vector<Image> images;
    std::vector<Buffer> buffers;
    std::vector<VkSampler> samplers;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    Image target;

    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        RT_VK_CHECK(vk.vkCreateBuffer(dev.device, &bi, nullptr, &b.buffer));
        VkMemoryRequirements mr;
        vk.vkGetBufferMemoryRequirements(dev.device, b.buffer, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = dev.memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        RT_VK_CHECK(vk.vkAllocateMemory(dev.device, &ai, nullptr, &b.memory));
        RT_VK_CHECK(vk.vkBindBufferMemory(dev.device, b.buffer, b.memory, 0));
        RT_VK_CHECK(vk.vkMapMemory(dev.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
        buffers.push_back(b);
        return b;
    }

    Image image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
        Image i;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {w, h, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        RT_VK_CHECK(vk.vkCreateImage(dev.device, &ii, nullptr, &i.image));
        VkMemoryRequirements mr;
        vk.vkGetImageMemoryRequirements(dev.device, i.image, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = dev.memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        RT_VK_CHECK(vk.vkAllocateMemory(dev.device, &ai, nullptr, &i.memory));
        RT_VK_CHECK(vk.vkBindImageMemory(dev.device, i.image, i.memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = i.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        RT_VK_CHECK(vk.vkCreateImageView(dev.device, &vi, nullptr, &i.view));
        images.push_back(i);
        return i;
    }

    // RGBA8 texture uploaded and left in SHADER_READ_ONLY_OPTIMAL.
    Image texture(uint32_t w, uint32_t h, const std::vector<uint32_t>& rgba) {
        Image t = image(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer up = buffer(rgba.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(up.mapped, rgba.data(), rgba.size() * 4);
        dev.submit_and_wait([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = t.image;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {w, h, 1};
            vk.vkCmdCopyBufferToImage(cmd, up.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        });
        return t;
    }

    void make_target() {
        const VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
        target = image(W, H, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkAttachmentDescription ad{};
        ad.format = fmt;
        ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd{};
        sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sd.colorAttachmentCount = 1;
        sd.pColorAttachments = &ar;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1;
        rpi.pAttachments = &ad;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sd;
        RT_VK_CHECK(vk.vkCreateRenderPass(dev.device, &rpi, nullptr, &rp));
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass = rp;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &target.view;
        fbi.width = W;
        fbi.height = H;
        fbi.layers = 1;
        RT_VK_CHECK(vk.vkCreateFramebuffer(dev.device, &fbi, nullptr, &fb));
    }

    VkShaderModule module(const std::string& path) {
        auto code = read_spv(path);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = code.size() * 4;
        smi.pCode = code.data();
        VkShaderModule m;
        RT_VK_CHECK(vk.vkCreateShaderModule(dev.device, &smi, nullptr, &m));
        modules.push_back(m);
        return m;
    }

    VkPipeline pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout) {
        VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, float(W), float(H), 0, 1};
        VkRect2D sc{{0, 0}, {W, H}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.pViewports = &vp;
        vps.scissorCount = 1;
        vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.stageCount = 2;
        gpi.pStages = st;
        gpi.pVertexInputState = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vps;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pColorBlendState = &cb;
        gpi.layout = layout;
        gpi.renderPass = rp;
        VkPipeline p;
        RT_VK_CHECK(vk.vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &gpi, nullptr, &p));
        pipelines.push_back(p);
        return p;
    }

    // Draws one full-screen triangle and returns W*H*4 floats.
    std::vector<float> draw(VkPipeline p, Bindless& bl, const UserData& ud) {
        Buffer rb = buffer(VkDeviceSize(W) * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        dev.submit_and_wait([&](VkCommandBuffer cmd) {
            VkClearValue clear{};
            for (float& c : clear.color.float32) c = -1;
            VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rbi.renderPass = rp;
            rbi.framebuffer = fb;
            rbi.renderArea = {{0, 0}, {W, H}};
            rbi.clearValueCount = 1;
            rbi.pClearValues = &clear;
            vk.vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            VkDescriptorSet set = bl.set();
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bl.pipeline_layout(), 0, 1, &set, 0, nullptr);
            vk.vkCmdPushConstants(cmd, bl.pipeline_layout(), Bindless::kStages, 0, sizeof ud, &ud);
            vk.vkCmdDraw(cmd, 3, 1, 0, 0);
            vk.vkCmdEndRenderPass(cmd);
            VkBufferImageCopy rc{};
            rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rc.imageExtent = {W, H, 1};
            vk.vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &rc);
        });
        std::vector<float> out(size_t(W) * H * 4);
        std::memcpy(out.data(), rb.mapped, out.size() * 4);
        return out;
    }
};

int g_failed = 0;
void report(const char* name, int bad, const std::string& first) {
    std::printf("%-44s %s", name, bad ? "FAIL" : "ok");
    if (bad) std::printf("  (%d pixels wrong; first: %s)", bad, first.c_str());
    std::printf("\n");
    g_failed += bad != 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gpu_layer_test <spv dir> [--validation]\n");
        return 2;
    }
    std::string dir = argv[1];
    DeviceOptions opt;
    opt.validation = argc > 2 && std::string(argv[2]) == "--validation";
    try {
        Device dev(opt);
        std::printf("device: %s\n", dev.describe().c_str());
        GpuHeap heap(dev, 64ull << 20);
        std::printf("heap: %llu MiB at Xbox VA 0x%llx, %s memory\n", (unsigned long long)(heap.size() >> 20),
                    (unsigned long long)heap.base_va(), heap.device_local() ? "device-local host-visible" : "host");
        BindlessLimits lim;
        lim.table_slots = 8;   // tiny on purpose: the inserts below force it to grow
        Bindless bl(dev, lim);
        std::printf("bindless: %u images, %u samplers per binding\n", bl.image_capacity(), bl.sampler_capacity());

        Fixture fx(dev);
        fx.make_target();
        VkShaderModule vs = fx.module(dir + "/VS_FullscreenUV.spv");

        // ---- 1. sampling through heap descriptors
        std::vector<uint32_t> grad(TW * TW), decoy(TW * TW, 0xFF00FF00u);
        for (uint32_t y = 0; y < TW; y++)
            for (uint32_t x = 0; x < TW; x++) grad[y * TW + x] = (x * 32u) | ((y * 32u) << 8) | (128u << 16) | (255u << 24);
        Image tex = fx.texture(TW, TW, grad), dtex = fx.texture(TW, TW, decoy);
        uint32_t tex_idx = bl.add_image(ImageKind::Tex2D, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        uint32_t decoy_idx = bl.add_image(ImageKind::Tex2D, dtex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const uint32_t tsharp[8] = {0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8000};
        const uint32_t decoy_t[8] = {0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8001};
        const uint32_t ssharp[4] = {0x92, 0, 0, 0};   // clamp, point filtering, LOD 0
        uint32_t gen0 = bl.table().generation();
        for (uint32_t i = 0; i < 200; i++) {          // filler entries: grows the table several times
            uint32_t k[8] = {i, i * 7u, 0xABCD, i ^ 0x5555, 1, 2, 3, 4};
            bl.map_tsharp(k, decoy_idx);
        }
        bl.map_tsharp(decoy_t, decoy_idx);
        bl.map_tsharp(tsharp, tex_idx);
        uint32_t smp_idx = bl.bind_ssharp(ssharp);
        bl.flush();
        std::printf("hash table: %u entries in %u slots (grew %u times)\n", bl.table().size(), bl.table().slots(),
                    bl.table().generation() - gen0);
        // Descriptors in the heap: T# then S#, as the shader's s_load_dwordx8 / s_load_dwordx4 at +0 / +0x20 expect.
        uint64_t desc_va = heap.alloc(48, 256);
        std::memcpy(heap.cpu(desc_va), tsharp, 32);
        std::memcpy(static_cast<uint8_t*>(heap.cpu(desc_va)) + 32, ssharp, 16);
        VkPipeline p_sample = fx.pipeline(vs, fx.module(dir + "/PS_SampleHeapDescriptors.spv"), bl.pipeline_layout());
        UserData ud{};
        ud.set_ptr(4, desc_va);   // the shader forces the high dword to 0 itself
        ud.heap_base = heap.heap_base();
        auto px = fx.draw(p_sample, bl, ud);
        int bad = 0;
        std::string first;
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                float u = (x + 0.5f) / W, v = 1.0f - (y + 0.5f) / H;   // VS param0 interpolated
                uint32_t tx = uint32_t(u * TW), ty = uint32_t(v * TW);
                float want[4] = {tx * 32 / 255.0f, ty * 32 / 255.0f, 128 / 255.0f, 1.0f};
                const float* got = &px[(size_t(y) * W + x) * 4];
                bool ok = true;
                for (int c = 0; c < 4; c++) ok = ok && std::fabs(got[c] - want[c]) <= 1e-5f;
                if (!ok && !bad++) {
                    char b[160];
                    std::snprintf(b, sizeof b, "(%u,%u) got %.4f %.4f %.4f %.4f want %.4f %.4f %.4f %.4f", x, y, got[0],
                                  got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                    first = b;
                }
            }
        report("sample via heap T#/S# + hash table", bad, first);
        if (smp_idx == 0 || tex_idx == decoy_idx) report("index allocation", 1, "index 0 handed out");

        // ---- 2. unregistered T#: fallback (magenta)
        const uint32_t unknown[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        std::memcpy(heap.cpu(desc_va), unknown, 32);
        px = fx.draw(p_sample, bl, ud);
        bad = 0;
        first.clear();
        for (size_t i = 0; i < size_t(W) * H; i++) {
            const float* g = &px[i * 4];
            if (!(g[0] == 1 && g[1] == 0 && g[2] == 1 && g[3] == 1) && !bad++) first = "pixel " + std::to_string(i);
        }
        report("unregistered T# reads the fallback", bad, first);

        // ---- 3. constant-output shaders with known bits
        struct Case { const char* spv; uint32_t bits[4]; };
        const Case cases[] = {{"PS_gpu_ps_sdwa", {0x000000f1u, 0xfffefff2u, 0x3c003344u, 0xfff70000u}},
                              {"PS_gpu_ps_ops", {0x48804300u, 5u, 7u, 0xfffffffcu}}};
        for (auto& c : cases) {
            VkPipeline p = fx.pipeline(vs, fx.module(dir + "/" + c.spv + ".spv"), bl.pipeline_layout());
            px = fx.draw(p, bl, ud);
            bad = 0;
            first.clear();
            for (size_t i = 0; i < size_t(W) * H; i++) {
                uint32_t g[4];
                std::memcpy(g, &px[i * 4], 16);
                if (std::memcmp(g, c.bits, 16) && !bad++) {
                    char b[120];
                    std::snprintf(b, sizeof b, "%08x %08x %08x %08x", g[0], g[1], g[2], g[3]);
                    first = b;
                }
            }
            report((std::string(c.spv) + " output bits").c_str(), bad, first);
        }

        // ---- 4. literal samplers file: registered once, deduplicated, visible through the hash table
        {
            auto path = std::filesystem::temp_directory_path() / "gpu_layer_test_literal_samplers.txt";
            std::ofstream(path) << "# test\n0x00000092 0x00c00000 0x14500000 0x00000000\n0x92 0 0 0\n";
            size_t n = register_literal_samplers(bl, path);
            std::filesystem::remove(path);
            bl.flush();
            const uint32_t design[4] = {0x92, 0xC00000, 0x14500000, 0};
            uint32_t idx = bl.table().lookup(DescriptorHashTable::ssharp_key(design));
            uint32_t again = bl.bind_ssharp(design);
            uint32_t point = bl.table().lookup(DescriptorHashTable::ssharp_key(ssharp));
            int bad4 = (n != 2) + (idx == 0) + (again != idx) + (point != smp_idx);
            report("literal samplers registered and deduplicated", bad4, "see test 4");
        }

        heap.free(desc_va);
        if (dev.validation_errors()) {
            std::printf("validation: %u error(s)\n", dev.validation_errors());
            g_failed++;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf(g_failed ? "%d check(s) FAILED\n" : "all GPU checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
