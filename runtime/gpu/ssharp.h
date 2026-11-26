// GFX10 sampler descriptor (S#, 4 dwords) -> VkSamplerCreateInfo.
//
// Field layout from Mesa src/amd/common/sid.h (SQ_IMG_SAMP_WORD0..3). Unverified against the game: the literal
// S# quoted in design.md, (0x92, 0xC00000, 0x14500000, 0), decodes to clamp-to-edge on all axes, bilinear
// min/mag, point mips, LOD 0..12, which is plausible, but nothing has been checked on game captures yet.
// Anything Vulkan can't express exactly is listed in `approximations` so callers can log it once.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "vk.h"

namespace rt::gpu {

struct SamplerDecode {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    std::vector<std::string> approximations;   // custom (register) border colours, half-border modes, reductions
};

SamplerDecode decode_ssharp(const uint32_t s[4]);

}  // namespace rt::gpu
