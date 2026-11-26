// CPU-only tests for the GPU layer's bookkeeping: VA allocator, descriptor hash table, S# decoding, ISG1 parsing and
// vertex input. No Vulkan device needed.
//
//   gpu_unit_test        exit code 0 when everything passes
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../gpu/desc_table.h"
#include "../gpu/heap.h"
#include "../gpu/literal_samplers.h"
#include "../gpu/push.h"
#include "../gpu/ssharp.h"
#include "../gpu/vertex_input.h"

using namespace rt::gpu;

static int g_failed = 0, g_checks = 0;
#define EXPECT(cond)                                                                \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failed++;                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)

static void test_va_allocator() {
    VaAllocator a(0x10000, 0x100000);
    uint64_t x = a.alloc(100, 256), y = a.alloc(1000, 4096), z = a.alloc(64, 64);
    EXPECT(x == 0x10000);
    EXPECT(y % 4096 == 0 && y >= x + 100);
    EXPECT(z && z % 64 == 0);
    EXPECT(a.alloc(0x200000, 256) == 0);       // too big
    EXPECT(a.alloc(16, 3) == 0);               // alignment not a power of two
    EXPECT(!a.free(x + 1));                    // not a block start
    EXPECT(a.free(y));
    EXPECT(!a.free(y));                        // double free
    EXPECT(a.free(x) && a.free(z));
    EXPECT(a.used() == 0);
    EXPECT(a.free_blocks() == 1);              // everything coalesced back into one block
    // Random churn: no overlaps, everything inside the window, full coalescing at the end.
    std::mt19937 rng(1234);
    std::vector<std::pair<uint64_t, uint64_t>> live;
    for (int i = 0; i < 4000; i++) {
        if (live.empty() || rng() % 3) {
            uint64_t size = 1 + rng() % 5000, align = 1ull << (rng() % 13);
            uint64_t p = a.alloc(size, align);
            if (!p) continue;
            EXPECT(p % align == 0 && p >= 0x10000 && p + size <= 0x10000 + 0x100000);
            for (auto& [q, s] : live) EXPECT(p + size <= q || q + s <= p);
            live.push_back({p, size});
        } else {
            size_t k = rng() % live.size();
            EXPECT(a.free(live[k].first));
            live.erase(live.begin() + k);
        }
    }
    for (auto& [p, s] : live) a.free(p);
    EXPECT(a.used() == 0 && a.free_blocks() == 1);
}

static void test_hash_table() {
    // Hash reference values: FNV-1a over 8 zero dwords, and the T# the texture test used.
    DescKey zero{};
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) h = (h ^ 0u) * 16777619u;
    EXPECT(DescriptorHashTable::hash(zero) == h);
    const uint32_t s[4] = {0x92, 0xC00000, 0x14500000, 0};
    DescKey sk = DescriptorHashTable::ssharp_key(s);
    EXPECT(sk[3] == 0 && sk[4] == 0xFFFFFFFFu && sk[7] == 0xFFFFFFFFu);

    DescriptorHashTable t(8);
    EXPECT(t.slots() == 8 && t.words().size() == 1 + 8 * 10 && t.words()[0] == 7);
    const uint32_t a[8] = {0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666, 0x77777777, 0x88888888};
    DescKey ak = DescriptorHashTable::tsharp_key(a);
    EXPECT(t.lookup(ak) == 0);
    t.insert(ak, 5);
    EXPECT(t.lookup(ak) == 5);
    t.insert(ak, 6);                           // update in place
    EXPECT(t.lookup(ak) == 6 && t.size() == 1);
    EXPECT(t.remove(ak) && t.lookup(ak) == 0 && t.size() == 0);
    EXPECT(!t.remove(ak));

    // Fill far past the initial size: the table must grow, keep every mapping, and stay a power of two.
    std::mt19937 rng(99);
    std::vector<std::pair<DescKey, uint32_t>> keys;
    uint32_t gen0 = t.generation();
    for (uint32_t i = 1; i <= 3000; i++) {
        DescKey k;
        for (auto& w : k) w = rng();
        t.insert(k, i);
        keys.push_back({k, i});
    }
    EXPECT(t.generation() > gen0);
    EXPECT((t.slots() & (t.slots() - 1)) == 0 && t.words()[0] == t.slots() - 1);
    EXPECT(t.size() == 3000);
    int wrong = 0;
    for (auto& [k, i] : keys) wrong += t.lookup(k) != i;
    EXPECT(wrong == 0);
    // Removing half keeps the other half intact (lookups scan past cleared slots, like the shader).
    for (size_t i = 0; i < keys.size(); i += 2) t.remove(keys[i].first);
    wrong = 0;
    for (size_t i = 0; i < keys.size(); i++) wrong += t.lookup(keys[i].first) != (i % 2 ? keys[i].second : 0u);
    EXPECT(wrong == 0);
    // Dirty range covers a later single change.
    t.clear_dirty();
    t.insert(keys[1].first, 77);
    EXPECT(t.dirty_end() - t.dirty_begin() == DescriptorHashTable::kEntryWords);
    EXPECT(t.words()[t.dirty_begin() + 8] == 77);
}

static void test_ssharp() {
    // The literal S# quoted in design.md.
    const uint32_t s[4] = {0x92, 0xC00000, 0x14500000, 0};
    auto d = decode_ssharp(s);
    EXPECT(d.info.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    EXPECT(d.info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    EXPECT(d.info.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    EXPECT(d.info.magFilter == VK_FILTER_LINEAR && d.info.minFilter == VK_FILTER_LINEAR);
    EXPECT(d.info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
    EXPECT(d.info.minLod == 0.0f && d.info.maxLod == 12.0f);
    EXPECT(!d.info.compareEnable && !d.info.anisotropyEnable && !d.info.unnormalizedCoordinates);
    EXPECT(d.info.borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    EXPECT(d.approximations.empty());
    // Wrap/mirror, compare LESSEQUAL, negative LOD bias, aniso 8x, trilinear, opaque-white border.
    const uint32_t s2[4] = {(0u) | (1u << 3) | (0u << 6) | (3u << 9) | (3u << 12),
                            (0x100u) | (0x400u << 12),
                            (0x3F80u) | (3u << 20) | (3u << 22) | (2u << 26),
                            2u << 30};
    auto e = decode_ssharp(s2);
    EXPECT(e.info.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    EXPECT(e.info.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
    EXPECT(e.info.compareEnable && e.info.compareOp == VK_COMPARE_OP_LESS_OR_EQUAL);
    EXPECT(e.info.mipLodBias == -0.5f);
    EXPECT(e.info.anisotropyEnable && e.info.maxAnisotropy == 8.0f);
    EXPECT(e.info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
    EXPECT(e.info.minLod == 1.0f && e.info.maxLod == 4.0f);
    EXPECT(e.info.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
    // Mip filter NONE pins sampling to the base level.
    const uint32_t s3[4] = {0, 0x800u << 12, 0, 0};
    EXPECT(decode_ssharp(s3).info.maxLod == 0.0f);
    // Register border colour is reported as an approximation.
    const uint32_t s4[4] = {6, 0, 0, 3u << 30};
    EXPECT(decode_ssharp(s4).approximations.size() == 1);
}

// DXBC with one ISG1 chunk: the elements' names follow the element table.
static std::vector<uint8_t> make_dxbc(const std::vector<SignatureElement>& els) {
    std::vector<uint8_t> payload(8 + 32 * els.size());
    auto put = [](std::vector<uint8_t>& v, size_t off, uint32_t x) { std::memcpy(v.data() + off, &x, 4); };
    put(payload, 0, uint32_t(els.size()));
    put(payload, 4, 8);
    for (size_t i = 0; i < els.size(); i++) {
        size_t e = 8 + 32 * i;
        put(payload, e + 4, uint32_t(payload.size()));
        payload.insert(payload.end(), els[i].semantic.begin(), els[i].semantic.end());
        payload.push_back(0);
        put(payload, e + 8, els[i].semantic_index);
        put(payload, e + 12, els[i].system_value);
        put(payload, e + 16, els[i].component_type);
        put(payload, e + 20, els[i].reg);
        payload[e + 24] = els[i].mask;
        payload[e + 25] = els[i].rw_mask;
    }
    std::vector<uint8_t> d(36);
    std::memcpy(d.data(), "DXBC", 4);
    put(d, 28, 1);
    put(d, 32, 36);
    d.insert(d.end(), {'I', 'S', 'G', '1'});
    d.resize(d.size() + 4);
    put(d, 40, uint32_t(payload.size()));
    d.insert(d.end(), payload.begin(), payload.end());
    return d;
}

static void test_vertex_input() {
    std::vector<SignatureElement> sig = {
        {"POSITION", 0, 0, 3, 0, 0x7, 0x7},
        {"TEXCOORD", 1, 0, 3, 2, 0x3, 0x3},
        {"BLENDINDICES", 0, 0, 1, 1, 0xF, 0xF},
        {"SV_VertexID", 0, 6, 1, 3, 0x1, 0x1},
    };
    auto dxbc = make_dxbc(sig);
    auto parsed = parse_isg1(dxbc.data(), dxbc.size());
    EXPECT(parsed.size() == 4);
    EXPECT(parsed.size() == 4 && parsed[1].semantic == "TEXCOORD" && parsed[1].semantic_index == 1 && parsed[1].reg == 2);
    EXPECT(parsed.size() == 4 && parsed[3].system_value == 6);
    bool threw = false;
    try { parse_isg1(dxbc.data(), 40); } catch (const std::exception&) { threw = true; }
    EXPECT(threw);

    const uint32_t A = InputElement::kAppendAligned;
    std::vector<InputElement> layout = {
        {"POSITION", 0, 6, 0, 0, false, 0},            // R32G32B32_FLOAT
        {"normal", 0, 28, 0, A, false, 0},             // not read by the shader
        {"TexCoord", 1, 34, 0, A, false, 0},           // R16G16_FLOAT, case-insensitive match
        {"BLENDINDICES", 0, 30, 1, 0, true, 1},        // R8G8B8A8_UINT, per instance in slot 1
    };
    auto v = build_vertex_input(layout, parsed);
    EXPECT(v.problems.empty());
    EXPECT(v.attributes.size() == 3);
    if (v.attributes.size() == 3) {
        EXPECT(v.attributes[0].location == 0 && v.attributes[0].offset == 0 && v.attributes[0].format == VK_FORMAT_R32G32B32_SFLOAT);
        EXPECT(v.attributes[1].location == 1 && v.attributes[1].binding == 1 && v.attributes[1].format == VK_FORMAT_R8G8B8A8_UINT);
        EXPECT(v.attributes[2].location == 2 && v.attributes[2].offset == 16 && v.attributes[2].format == VK_FORMAT_R16G16_SFLOAT);
    }
    EXPECT(v.bindings.size() == 2);
    if (v.bindings.size() == 2) EXPECT(v.bindings[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);

    // Missing element, unknown format, float format feeding a uint input.
    std::vector<InputElement> bad = {{"POSITION", 0, 999, 0, 0, false, 0}, {"BLENDINDICES", 0, 28, 0, A, false, 0}};
    auto b = build_vertex_input(bad, parsed);
    EXPECT(b.problems.size() == 3);
}

static void test_literal_samplers_file() {
    auto v = parse_literal_samplers("# from 3 shaders\n0x00000092 0x00c00000 0x14500000 0x00000000\n\n"
                                    "  1 2 3 4   # trailing comment\r\n");
    EXPECT(v.size() == 2);
    if (v.size() == 2) {
        EXPECT(v[0][0] == 0x92 && v[0][1] == 0xC00000 && v[0][2] == 0x14500000 && v[0][3] == 0);
        EXPECT(v[1][0] == 1 && v[1][3] == 4);
    }
    for (const char* bad : {"1 2 3\n", "1 2 3 4 5\n", "1 2 3 zz\n", "1 2 3 0x100000000\n", "1 2 3 -1\n"}) {
        bool threw = false;
        try { parse_literal_samplers(bad); } catch (const std::exception&) { threw = true; }
        EXPECT(threw);
    }
}

int main() {
    test_literal_samplers_file();
    test_va_allocator();
    test_hash_table();
    test_ssharp();
    test_vertex_input();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
