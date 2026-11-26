#include "ssharp.h"

namespace rt::gpu {

namespace {

uint32_t bits(uint32_t w, unsigned lo, unsigned n) { return (w >> lo) & ((1u << n) - 1); }

VkSamplerAddressMode address(uint32_t clamp, std::vector<std::string>& approx) {
    switch (clamp) {   // SQ_TEX_CLAMP
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;                    // WRAP
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;           // MIRROR
        case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;             // CLAMP_LAST_TEXEL
        case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;      // MIRROR_ONCE_LAST_TEXEL
        case 4: approx.push_back("CLAMP_HALF_BORDER as clamp-to-border"); return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 5: approx.push_back("MIRROR_ONCE_HALF_BORDER as mirror-clamp-to-edge");
                return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;           // CLAMP_BORDER
        default: approx.push_back("MIRROR_ONCE_BORDER as mirror-clamp-to-edge");
                 return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
}

}  // namespace

SamplerDecode decode_ssharp(const uint32_t s[4]) {
    SamplerDecode d;
    auto& ci = d.info;
    auto& approx = d.approximations;
    const uint32_t w0 = s[0], w1 = s[1], w2 = s[2], w3 = s[3];

    ci.addressModeU = address(bits(w0, 0, 3), approx);
    ci.addressModeV = address(bits(w0, 3, 3), approx);
    ci.addressModeW = address(bits(w0, 6, 3), approx);
    uint32_t aniso = bits(w0, 9, 3);                 // MAX_ANISO_RATIO: 0..4 = 1x, 2x, 4x, 8x, 16x
    uint32_t compare = bits(w0, 12, 3);              // DEPTH_COMPARE_FUNC, NEVER (0) when compare is off
    ci.unnormalizedCoordinates = bits(w0, 15, 1);    // FORCE_UNNORMALIZED
    if (bits(w0, 29, 2)) approx.push_back("min/max reduction filter ignored");   // FILTER_MODE

    ci.minLod = float(bits(w1, 0, 12)) / 256.0f;     // u4.8
    ci.maxLod = float(bits(w1, 12, 12)) / 256.0f;
    int32_t bias = int32_t(bits(w2, 0, 14) << 18) >> 18;   // s6.8
    ci.mipLodBias = float(bias) / 256.0f;

    uint32_t mag = bits(w2, 20, 2), min = bits(w2, 22, 2), mip = bits(w2, 26, 2);
    // SQ_TEX_XY_FILTER: POINT, BILINEAR, ANISO_POINT, ANISO_BILINEAR.
    ci.magFilter = (mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    // SQ_TEX_Z_FILTER for mips: NONE (base level only), POINT, LINEAR.
    ci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (mip == 0) ci.maxLod = ci.minLod;
    if (aniso && ((mag | min) & 2)) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = float(1u << (aniso > 4 ? 4 : aniso));
    }
    if (compare) {
        ci.compareEnable = VK_TRUE;
        // SQ_TEX_DEPTH_COMPARE: NEVER, LESS, EQUAL, LESSEQUAL, GREATER, NOTEQUAL, GREATEREQUAL, ALWAYS has the same
        // order as VkCompareOp.
        ci.compareOp = VkCompareOp(compare);
    }
    switch (bits(w3, 30, 2)) {   // BORDER_COLOR_TYPE
        case 0: ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
        case 1: ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
        case 2: ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
        default:
            ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            approx.push_back("register border colour as transparent black");
    }
    if (ci.unnormalizedCoordinates) {
        // Vulkan's rules for unnormalized coordinates: clamp addressing, no mips, no anisotropy, no compare.
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.minFilter = ci.magFilter;
        ci.minLod = ci.maxLod = 0;
        ci.anisotropyEnable = VK_FALSE;
        ci.compareEnable = VK_FALSE;
    }
    return d;
}

}  // namespace rt::gpu
