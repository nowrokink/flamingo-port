// What a slot-57 pipeline's blocks say (docs/gpu-memory.md, "Stage metadata"): per stage, which user SGPR gets each
// descriptor table address and each root constant; and the pipeline's input layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rt::gpu {

// One 0x41 record of a stage block.
struct StageBinding {
    enum Kind : uint32_t { kRootConstants = 0x2, kSrv = 0x300, kCbv = 0x302, kSampler = 0x401 };
    uint32_t kind = 0;
    uint32_t table_dwords = 0;  // the descriptor's offset in its table, in dwords
    uint32_t reg = 0;           // shader register (t#, b#, s#)
    uint32_t sgpr = 0;          // the user SGPR that gets the table's address, or the first root constant
    uint32_t count = 0;         // root constants: how many SGPRs
    uint32_t static_sampler = 0;  // samplers: the index in the root signature's static samplers
};

struct StageMeta {
    std::vector<StageBinding> bindings;
    uint8_t digest[16] = {};    // the 0x43 record: the shader's HASH digest
    bool has_digest = false;
};

// A stage block (the (pointer, size) pairs at +56, +88... of the desc). False if it isn't one.
bool parse_stage(const uint8_t* block, size_t size, StageMeta& out);

// One element of the input layout block (+24): a D3D12_INPUT_ELEMENT_DESC with its semantic name.
struct InputElement {
    std::string semantic;
    uint32_t index = 0, format = 0, slot = 0, offset = 0, per_instance = 0, step = 0;
};

bool parse_input_layout(const uint8_t* block, size_t size, std::vector<InputElement>& out);

}  // namespace rt::gpu
