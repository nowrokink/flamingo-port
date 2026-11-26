#include "stage_meta.h"

#include <cstring>

namespace rt::gpu {
namespace {

uint32_t u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

// A 16-byte header (2, size after its first 12 bytes, a per-shader value, records << 15 | flags), then 32-byte
// records: 0x41 bindings (kind, table offset in dwords, register, a resource-kind field, a packed word whose byte 1 is
// the user SGPR, byte 2 a root-constant count and, for static samplers, the high half the sampler's index), 0x42
// usage, 0x43 the HASH digest.
bool parse_stage(const uint8_t* b, size_t size, StageMeta& out) {
    // Two headers: `2, size - 12, a per-shader value, records << 15 | 0x440` (16 bytes), and a longer one,
    // `0x403, size - 20, a value, 0, 0, records << 15 | 0x440` (24 bytes), seen on the pixel stage of pipelines whose
    // first stage is a mesh shader (and on a stage at desc +152). The records are the same. The longer one wasn't
    // read: those stages had no bindings, so every table address and constant reached their shaders as zero.
    if (!b || size < 16) return false;
    size_t first;
    if (u32(b) == 2 && u32(b + 4) + 12 == size) first = 16;
    else if (u32(b) == 0x403 && size >= 24 && u32(b + 4) + 20 == size) first = 24;
    else return false;
    out = {};
    for (size_t at = first; at + 32 <= size; at += 32) {
        const uint8_t* r = b + at;
        uint32_t type = u32(r);
        if (type == 0x41) {
            StageBinding s;
            s.kind = u32(r + 4);
            s.table_dwords = u32(r + 8);
            s.reg = u32(r + 12);
            uint32_t packed = u32(r + 20);
            if (s.kind == StageBinding::kSampler) {
                s.static_sampler = packed >> 16;
                s.sgpr = packed >> 8 & 0xff;
            } else {
                s.sgpr = packed >> 8 & 0xff;
                s.count = packed >> 16 & 0xff;
            }
            out.bindings.push_back(s);
        } else if (type == 0x43) {
            std::memcpy(out.digest, r + 4, 16);
            out.has_digest = true;
        }
    }
    return true;
}

// From +0x4c: a count, then per element a name length (with its NUL), the name, and six dwords: semantic index,
// DXGI format, input slot, aligned byte offset, input slot class, instance step rate. Not aligned.
bool parse_input_layout(const uint8_t* b, size_t size, std::vector<InputElement>& out) {
    out.clear();
    if (!b || size < 0x50) return false;
    uint32_t n = u32(b + 0x4c);
    if (n > 32) return false;
    size_t at = 0x50;
    for (uint32_t i = 0; i < n; ++i) {
        if (at + 4 > size) return false;
        uint32_t len = u32(b + at);
        at += 4;
        if (len == 0 || len > 64 || at + len + 24 > size) return false;
        InputElement e;
        e.semantic.assign(reinterpret_cast<const char*>(b + at), strnlen(reinterpret_cast<const char*>(b + at), len));
        at += len;
        e.index = u32(b + at);
        e.format = u32(b + at + 4);
        e.slot = u32(b + at + 8);
        e.offset = u32(b + at + 12);
        e.per_instance = u32(b + at + 16);
        e.step = u32(b + at + 20);
        at += 24;
        out.push_back(e);
    }
    return true;
}

}  // namespace rt::gpu
