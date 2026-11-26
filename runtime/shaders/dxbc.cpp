#include "dxbc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fp::dxbc {

uint32_t u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint16_t u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

namespace {

std::string latin1(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 63));
        }
    }
    return s;
}

const uint8_t* bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

}  // namespace

bool parts(const uint8_t* d, size_t n, std::vector<Part>& out) {
    out.clear();
    if (n < 32) return false;
    uint32_t count = u32(d + 28);
    if (count > 1024 || 32 + 4ull * count > n) return false;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t off = u32(d + 32 + 4 * k);
        if (off + 8ull > n) return false;
        out.push_back({latin1(d + off, 4), off + 8u, u32(d + off + 4)});
    }
    return true;
}

const Part* first(const std::vector<Part>& ps, const char* name) {
    for (const auto& p : ps)
        if (p.name == name) return &p;
    return nullptr;
}

const Part* last(const std::vector<Part>& ps, const char* name) {
    const Part* r = nullptr;
    for (const auto& p : ps)
        if (p.name == name) r = &p;
    return r;
}

std::string stage_name(uint32_t kind) {
    switch (kind) {
        case 0: return "ps";
        case 1: return "vs";
        case 2: return "gs";
        case 3: return "hs";
        case 4: return "ds";
        case 5: return "cs";
        case 6: return "lib";
        case 13: return "ms";
        case 14: return "as";
        default: return std::to_string(kind);
    }
}

std::string stage(const std::string& c) {
    std::vector<Part> ps;
    if (!parts(bytes(c), c.size(), ps)) return "?";
    const Part* p = first(ps, "DXIL");
    if (!p || p->offset + 4 > c.size()) return "?";
    std::string s = stage_name(u32(bytes(c) + p->offset) >> 16);
    // pending_shaders.py's table: kinds it doesn't name are "?"
    return (s.empty() || (s[0] >= '0' && s[0] <= '9')) ? "?" : s;
}

std::optional<std::string> hash_digest(const std::string& c, bool last_part) {
    std::vector<Part> ps;
    parts(bytes(c), c.size(), ps);
    const Part* p = last_part ? last(ps, "HASH") : first(ps, "HASH");
    if (!p || p->offset + 20 > c.size()) return std::nullopt;
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < 16; ++i) {
        uint8_t b = bytes(c)[p->offset + 4 + i];
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    return hex;
}

std::vector<InputElement> input_signature(const std::string& c, bool with_names) {
    std::vector<InputElement> out;
    std::vector<Part> ps;
    parts(bytes(c), c.size(), ps);
    const Part* p = first(ps, "ISG1");
    if (!p || p->offset + 8 > c.size()) return out;
    const uint8_t* d = bytes(c) + p->offset;
    size_t size = std::min<size_t>(p->size, c.size() - p->offset);
    uint32_t count = u32(d), at = u32(d + 4);
    for (uint32_t i = 0; i < count && at + 32ull * i + 32 <= size; ++i) {
        const uint8_t* e = d + at + 32 * i;
        InputElement x{u32(e), u32(e + 4), u32(e + 8), u32(e + 12), u32(e + 16), u32(e + 20), e[24], e[25], {}};
        if (with_names)
            for (size_t k = x.name_offset; k < size && d[k]; ++k) x.semantic += static_cast<char>(d[k]);
        out.push_back(std::move(x));
    }
    return out;
}

}  // namespace fp::dxbc
