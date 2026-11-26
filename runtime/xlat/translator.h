// RDNA (gfx10) -> SPIR-V assembly: the C++ port of tools/rdna2spirv.py, output for output (the same text, byte for
// byte, so the two can be compared over the whole corpus while both exist).
//
// Files:  ir.*          the disassembly parser (shader_ir.py)
//         gen.h         the generator's state and its helpers, shared by the gen_*.cpp files
//         gen_regs.cpp  operands, registers, EXEC, destinations, modifiers, SDWA (Gen.src_u .. Gen.sdwa_dst)
//         gen_memory.cpp  buffers, global and scalar memory, LDS, the page-table access of the d3d12 target
//         gen_image.cpp   descriptor lookup, sampling, image loads/stores/atomics/queries
//         gen_export.cpp  exports, attributes, compares
//         ops.cpp       Gen::insn_: the instruction dispatch (Python's elif chain, in its order)
//         ops_*.cpp     the instruction families the dispatch calls
//         assemble.cpp  the module around the body: capabilities, types, variables, prologue, dispatch loop
//         program.cpp   translate(): stage, inputs, reachable code, basic blocks
//         formats.cpp   GFX10 buffer/image format names (Mesa's gfx10-rsrc.json)
#pragma once
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace fp::xlat {

// A shader the translator doesn't handle (yet): the reason, as rdna2spirv's Unsupported carries it.
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// What Python raised on bad input where the C++ code checks: "IndexError", "KeyError" or "ValueError" (translate_one
// reports these as "translator error <type>").
struct PyError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Target {
    vulkan,  // buffer device addresses, push constants as a uint array
    d3d12,   // logical addressing only, user data as uint4[9] (an HLSL cbuffer through SPIRV-Cross)
};

struct Options {
    Target target = Target::vulkan;
    // Compute shaders' register blocks the runtime saves per digest (<cache>/cs/<digest>.regs); empty: none.
    std::wstring cs_regs;
};

// What translations of one corpus share: the options and the stage index (index.json beside the containers), read
// once. Safe to use from several threads.
class Context {
public:
    explicit Context(Options o) : opt_(std::move(o)) {}
    const Options& options() const { return opt_; }
    // rdna2spirv.stage_of: the stage from the carve index, else from the name's prefix ("ps_..."), else "?".
    std::string stage_of(const std::string& name, const std::wstring& s_path);

private:
    Options opt_;
    std::mutex lock_;
    bool loaded_ = false;
    std::unordered_map<std::string, std::string> stages_;
};

// rdna2spirv.translate: the SPIR-V assembly text of one disassembly (<root>/disasm/<name>.s, with <root>/<name>.dxbc
// and <root>/fssm/<name>.fssm beside it). Throws Unsupported or PyError.
std::string translate(const std::wstring& s_path, Context& ctx);

}  // namespace fp::xlat
