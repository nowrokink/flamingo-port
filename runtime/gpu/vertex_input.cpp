#include "vertex_input.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <stdexcept>

namespace rt::gpu {

namespace {

uint32_t rd32(const uint8_t* p, size_t size, size_t off) {
    if (off > size || size - off < 4) throw std::runtime_error("DXBC container truncated");
    uint32_t v;
    std::memcpy(&v, p + off, 4);
    return v;
}

bool same_semantic(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::vector<SignatureElement> parse_isg1(const uint8_t* d, size_t size) {
    if (size < 32 || std::memcmp(d, "DXBC", 4) != 0) throw std::runtime_error("not a DXBC container");
    uint32_t chunks = rd32(d, size, 28);
    for (uint32_t k = 0; k < chunks; k++) {
        uint32_t off = rd32(d, size, 32 + 4ull * k);
        if (off > size || size - off < 8) throw std::runtime_error("DXBC chunk offset out of range");
        if (std::memcmp(d + off, "ISG1", 4) != 0) continue;
        uint32_t len = rd32(d, size, off + 4);
        size_t base = off + 8ull;
        if (len > size - base) throw std::runtime_error("ISG1 chunk truncated");
        const uint8_t* p = d + base;
        uint32_t count = rd32(p, len, 0), eoff = rd32(p, len, 4);
        std::vector<SignatureElement> out;
        for (uint32_t i = 0; i < count; i++) {
            size_t e = eoff + 32ull * i;
            if (e > len || len - e < 32) throw std::runtime_error("ISG1 element out of range");
            SignatureElement s;
            uint32_t name = rd32(p, len, e + 4);
            if (name < len) {
                const char* n = reinterpret_cast<const char*>(p + name);
                s.semantic.assign(n, strnlen(n, len - name));
            }
            s.semantic_index = rd32(p, len, e + 8);
            s.system_value = rd32(p, len, e + 12);
            s.component_type = rd32(p, len, e + 16);
            s.reg = rd32(p, len, e + 20);
            s.mask = p[e + 24];
            s.rw_mask = p[e + 25];
            out.push_back(std::move(s));
        }
        return out;
    }
    return {};
}

VertexFormat vertex_format(uint32_t f) {
    constexpr uint32_t F = 3, U = 1, S = 2;
    switch (f) {   // DXGI_FORMAT values
        case 2: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16, F};
        case 3: return {VK_FORMAT_R32G32B32A32_UINT, 16, U};
        case 4: return {VK_FORMAT_R32G32B32A32_SINT, 16, S};
        case 6: return {VK_FORMAT_R32G32B32_SFLOAT, 12, F};
        case 7: return {VK_FORMAT_R32G32B32_UINT, 12, U};
        case 8: return {VK_FORMAT_R32G32B32_SINT, 12, S};
        case 10: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8, F};
        case 11: return {VK_FORMAT_R16G16B16A16_UNORM, 8, F};
        case 12: return {VK_FORMAT_R16G16B16A16_UINT, 8, U};
        case 13: return {VK_FORMAT_R16G16B16A16_SNORM, 8, F};
        case 14: return {VK_FORMAT_R16G16B16A16_SINT, 8, S};
        case 16: return {VK_FORMAT_R32G32_SFLOAT, 8, F};
        case 17: return {VK_FORMAT_R32G32_UINT, 8, U};
        case 18: return {VK_FORMAT_R32G32_SINT, 8, S};
        case 24: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, F};   // R10G10B10A2_UNORM
        case 25: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, U};    // R10G10B10A2_UINT
        case 26: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, F};    // R11G11B10_FLOAT
        case 28: return {VK_FORMAT_R8G8B8A8_UNORM, 4, F};
        case 30: return {VK_FORMAT_R8G8B8A8_UINT, 4, U};
        case 31: return {VK_FORMAT_R8G8B8A8_SNORM, 4, F};
        case 32: return {VK_FORMAT_R8G8B8A8_SINT, 4, S};
        case 34: return {VK_FORMAT_R16G16_SFLOAT, 4, F};
        case 35: return {VK_FORMAT_R16G16_UNORM, 4, F};
        case 36: return {VK_FORMAT_R16G16_UINT, 4, U};
        case 37: return {VK_FORMAT_R16G16_SNORM, 4, F};
        case 38: return {VK_FORMAT_R16G16_SINT, 4, S};
        case 41: return {VK_FORMAT_R32_SFLOAT, 4, F};
        case 42: return {VK_FORMAT_R32_UINT, 4, U};
        case 43: return {VK_FORMAT_R32_SINT, 4, S};
        case 49: return {VK_FORMAT_R8G8_UNORM, 2, F};
        case 50: return {VK_FORMAT_R8G8_UINT, 2, U};
        case 51: return {VK_FORMAT_R8G8_SNORM, 2, F};
        case 52: return {VK_FORMAT_R8G8_SINT, 2, S};
        case 54: return {VK_FORMAT_R16_SFLOAT, 2, F};
        case 56: return {VK_FORMAT_R16_UNORM, 2, F};
        case 57: return {VK_FORMAT_R16_UINT, 2, U};
        case 58: return {VK_FORMAT_R16_SNORM, 2, F};
        case 59: return {VK_FORMAT_R16_SINT, 2, S};
        case 61: return {VK_FORMAT_R8_UNORM, 1, F};
        case 62: return {VK_FORMAT_R8_UINT, 1, U};
        case 63: return {VK_FORMAT_R8_SNORM, 1, F};
        case 64: return {VK_FORMAT_R8_SINT, 1, S};
        case 87: return {VK_FORMAT_B8G8R8A8_UNORM, 4, F};
        default: return {};
    }
}

VertexInputLayout build_vertex_input(const std::vector<InputElement>& layout, const std::vector<SignatureElement>& sig) {
    VertexInputLayout out;
    // Resolve D3D12_APPEND_ALIGNED_ELEMENT: each element follows the previous one in the same slot.
    std::vector<uint32_t> offsets(layout.size());
    std::map<uint32_t, uint32_t> slot_end;
    for (size_t i = 0; i < layout.size(); i++) {
        const auto& e = layout[i];
        VertexFormat vf = vertex_format(e.dxgi_format);
        uint32_t& end = slot_end[e.slot];
        offsets[i] = e.offset == InputElement::kAppendAligned ? end : e.offset;
        end = offsets[i] + vf.size;
    }
    std::map<uint32_t, bool> slots_used;   // slot -> per instance
    for (const auto& s : sig) {
        if (s.system_value != 0) continue;   // SV_VertexID, SV_InstanceID...: not fetched from buffers
        auto it = std::find_if(layout.begin(), layout.end(), [&](const InputElement& e) {
            return e.semantic_index == s.semantic_index && same_semantic(e.semantic, s.semantic);
        });
        if (it == layout.end()) {
            out.problems.push_back("shader input " + s.semantic + std::to_string(s.semantic_index) +
                                   " has no element in the input layout");
            continue;
        }
        const auto& e = *it;
        VertexFormat vf = vertex_format(e.dxgi_format);
        if (vf.format == VK_FORMAT_UNDEFINED) {
            out.problems.push_back(e.semantic + std::to_string(e.semantic_index) + ": DXGI format " +
                                   std::to_string(e.dxgi_format) + " isn't mapped");
            continue;
        }
        if (s.component_type && vf.component_type != s.component_type)
            out.problems.push_back(e.semantic + std::to_string(e.semantic_index) +
                                   ": format type doesn't match the shader input type");
        if (e.per_instance && e.step_rate > 1)
            out.problems.push_back(e.semantic + std::to_string(e.semantic_index) + ": instance step rate " +
                                   std::to_string(e.step_rate) + " needs VK_EXT_vertex_attribute_divisor");
        out.attributes.push_back({s.reg, e.slot, vf.format, offsets[size_t(it - layout.begin())]});
        auto [pos, fresh] = slots_used.emplace(e.slot, e.per_instance);
        if (!fresh && pos->second != e.per_instance)
            out.problems.push_back("slot " + std::to_string(e.slot) + " mixes per-vertex and per-instance elements");
    }
    for (auto [slot, inst] : slots_used)
        out.bindings.push_back({slot, 0, inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    std::sort(out.attributes.begin(), out.attributes.end(),
              [](const auto& a, const auto& b) { return a.location < b.location; });
    return out;
}

}  // namespace rt::gpu
