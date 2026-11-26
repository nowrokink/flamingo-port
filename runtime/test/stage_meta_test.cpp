// stage_meta_test: the slot-57 metadata readers (gpu/stage_meta.cpp) on blocks built here in the layout found in
// 1.0.2.1 (docs/gpu-memory.md): a pixel stage with an SRV and a CBV in one table at s8, a root constant at s9 and a
// static sampler; an input layout of POSITION / COLOR / TEXCOORD.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../gpu/stage_meta.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

void put(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t x[4];
    std::memcpy(x, &v, 4);
    b.insert(b.end(), x, x + 4);
}

}  // namespace

int main() {
    using rt::gpu::StageBinding;
    // ---- a stage block: header, four 0x41 records, a 0x42 record, the 0x43 digest record
    std::vector<uint8_t> s;
    const uint32_t records[][8] = {
        {0x41, 0x300, 0, 2, 4, 0x808, 0xffffffff, 0},        // t2 at dword 0 of the table in s8
        {0x41, 0x302, 8, 0, 0, 0x804, 0xffffffff, 0},        // b0 at dword 8, same table
        {0x41, 0x2, 0, 500, 0, 0x10904, 0xffffffff, 0},      // b500: one root constant in s9
        {0x41, 0x401, 0, 3, 0, 0x50004, 0xffffffff, 0},      // s3: static sampler 5
        {0x42, 0, 0x20001, 2, 1, 0, 0xffffffff, 0},
    };
    const uint32_t n = 6;
    put(s, 2);
    put(s, 16 + 32 * n - 12);
    put(s, 0x56);
    put(s, n << 15 | 0x440);
    for (const auto& r : records)
        for (uint32_t v : r) put(s, v);
    put(s, 0x43);
    for (uint32_t i = 0; i < 4; ++i) put(s, 0x01020304u * (i + 1));
    for (int i = 0; i < 3; ++i) put(s, 0);

    rt::gpu::StageMeta m;
    CHECK(rt::gpu::parse_stage(s.data(), s.size(), m));
    CHECK(m.bindings.size() == 4);
    if (m.bindings.size() == 4) {
        CHECK(m.bindings[0].kind == StageBinding::kSrv && m.bindings[0].reg == 2 && m.bindings[0].sgpr == 8 &&
              m.bindings[0].table_dwords == 0);
        CHECK(m.bindings[1].kind == StageBinding::kCbv && m.bindings[1].sgpr == 8 && m.bindings[1].table_dwords == 8);
        CHECK(m.bindings[2].kind == StageBinding::kRootConstants && m.bindings[2].reg == 500 &&
              m.bindings[2].sgpr == 9 && m.bindings[2].count == 1);
        CHECK(m.bindings[3].kind == StageBinding::kSampler && m.bindings[3].static_sampler == 5);
    }
    CHECK(m.has_digest && m.digest[0] == 0x04 && m.digest[3] == 0x01);
    CHECK(!rt::gpu::parse_stage(s.data(), s.size() - 4, m));   // the size field must match

    // ---- the longer header (0x403, size - 20, a value, 0, 0, the record count): the same records from +24
    std::vector<uint8_t> w;
    for (uint32_t v : {0x403u, 24 + 32 * n - 20, 0x90u, 0u, 0u, n << 15 | 0x440}) put(w, v);
    w.insert(w.end(), s.begin() + 16, s.end());
    rt::gpu::StageMeta wide;
    CHECK(rt::gpu::parse_stage(w.data(), w.size(), wide));
    CHECK(wide.bindings.size() == 4 && wide.has_digest);
    if (wide.bindings.size() == 4) CHECK(wide.bindings[2].reg == 500 && wide.bindings[2].sgpr == 9);
    CHECK(!rt::gpu::parse_stage(w.data(), w.size() - 4, wide));

    // ---- an input layout block
    std::vector<uint8_t> l(0x4c, 0);
    put(l, 3);
    auto element = [&](const char* name, uint32_t format, uint32_t offset) {
        put(l, static_cast<uint32_t>(std::strlen(name) + 1));
        l.insert(l.end(), name, name + std::strlen(name) + 1);
        for (uint32_t v : {0u, format, 0u, offset, 0u, 0u}) put(l, v);
    };
    element("POSITION", 6, 0);
    element("COLOR", 28, 12);
    element("TEXCOORD", 16, 16);
    std::vector<rt::gpu::InputElement> e;
    CHECK(rt::gpu::parse_input_layout(l.data(), l.size(), e));
    CHECK(e.size() == 3);
    if (e.size() == 3) {
        CHECK(e[0].semantic == "POSITION" && e[0].format == 6 && e[0].offset == 0);
        CHECK(e[1].semantic == "COLOR" && e[1].format == 28 && e[1].offset == 12);
        CHECK(e[2].semantic == "TEXCOORD" && e[2].format == 16 && e[2].offset == 16);
    }
    std::printf(g_failures ? "stage_meta_test: FAILED\n" : "stage_meta_test: ok\n");
    return g_failures ? 1 : 0;
}
