// Push-constant block every translated shader declares (rdna2spirv.assemble, %UserData).
#pragma once
#include <cstddef>
#include <cstdint>

namespace rt::gpu {

struct UserData {
    uint32_t sgpr[32];    // initial s0..s31: the user SGPRs the console driver loads from the root signature
    uint64_t heap_base;   // GpuHeap::heap_base(): host BDA of Xbox GPU VA 0
    uint64_t code_base;   // Xbox VA of this shader's FSSM blob (for s_getpc_b64-relative constant tables)

    void set_ptr(int sgpr_lo, uint64_t va) {
        sgpr[sgpr_lo] = uint32_t(va);
        sgpr[sgpr_lo + 1] = uint32_t(va >> 32);
    }
};

static_assert(sizeof(UserData) == 144, "push constant block must match the translator's 144-byte layout");
static_assert(offsetof(UserData, heap_base) == 128 && offsetof(UserData, code_base) == 136,
              "push constant offsets must match the translator (128 and 136)");

}  // namespace rt::gpu
