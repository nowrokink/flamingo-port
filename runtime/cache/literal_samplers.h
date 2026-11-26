// The sampler descriptors (S#) that shaders build from immediates instead of loading them (design.md, "Literal
// samplers"). The game never binds those, so the runtime registers them in the descriptor hash table itself: it reads
// the file this writes and creates one sampler per line (rt::gpu::register_literal_samplers).
//
// The scan is conservative: within straight-line code it tracks SGPRs last written by s_mov_b32 / s_movk_i32 /
// s_mov_b64 with an immediate, forgets them at every branch target (another path may reach it with other values) and
// at any other write. An S# is listed only when all four dwords are known constants at the image instruction. When in
// doubt a literal is missed (the shader then reads the fallback sampler), never invented.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "xlat/ir.h"

namespace fp::cache {

using Sampler = std::array<uint32_t, 4>;

// Literal S#s used by image instructions in one shader (forward dataflow over basic blocks: a block starts with the
// SGPR constants every predecessor agrees on).
std::set<Sampler> literal_samplers(const std::vector<xlat::Ins>& insns);

// Over a .s file or every .s in a folder: one S# per line, four hex dwords, sorted, no duplicates, after a '#' line.
bool write_literal_samplers(const std::wstring& src, const std::wstring& out,
                            const std::function<void(const std::string&)>& log);

}  // namespace fp::cache
