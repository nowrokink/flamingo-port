// The translated-shader cache make_game.py writes (tools/dxil_cache.py): one DXIL container per shader of
// shaders.rpf, named by the 16-byte digest of its HASH chunk, which is also how a slot-57 pipeline's stage
// metadata names its shader (the block ends with the digest and 12 zero bytes).
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <utility>
#include <vector>

namespace rt::shader_cache {

// The digest a stage metadata block ends with; false if the block doesn't end that way.
bool digest_of(const uint8_t* block, size_t size, uint8_t digest[16]);

// The cached DXIL for a digest, and its program kind (0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute).
// False if the cache has no such shader. Thread-safe; reads the file each time (pipelines are made once).
bool find(const uint8_t digest[16], std::vector<uint8_t>& dxil, int& kind);

// The cache folder: RT_SHADER_CACHE, else "shaders" next to FlamingoPort.exe.
const wchar_t* folder();

// The HASH chunk's digest of a DXBC container (a slot-86 pipeline's stage: console code in the same container format
// as shaders.rpf's); false if it has none.
bool container_digest(const uint8_t* dxbc, size_t size, uint8_t digest[16]);

// Saves a container the cache doesn't have as <folder>/pending/<digest>.dxbc, for make_game.py to translate (once
// per digest; true if it's there now).
bool save_pending(const uint8_t digest[16], const uint8_t* dxbc, size_t size);

// A compute pipeline's register block (the slot-67 desc's +8 block, 0x1c bytes in 1.0.2.1) as
// <folder>/cs/<digest>.regs, once per digest: the translator reads the user-SGPR count, the workgroup-ID enables
// and the group size from it, which the shader container doesn't carry. True if it's there now.
bool save_compute_regs(const uint8_t digest[16], const uint8_t* regs, size_t size);

// A pipeline stage the cache doesn't have: its digest appended to <folder>/missing.txt, once per digest per run (the
// list of shaders the title asked for and the port lacks, for the metrics report and the next translation).
void note_missing(const uint8_t digest[16]);
// <cache>\owed.txt, written over each time: the digests draws were skipped for (no translated shader), most
// skipped first, "digest count" a line. What to translate first.
void write_owed(const std::vector<std::pair<std::array<uint8_t, 16>, uint64_t>>& owed);

// A vertex shader's inputs (<digest>.vsin, from its container's input signature): the translated shader takes the
// input of register `reg` as TEXCOORD<reg>. False if the cache has no such file.
struct VsInput {
    char semantic[32];
    uint32_t index, reg;
};
bool vs_inputs(const uint8_t digest[16], std::vector<VsInput>& out);

}  // namespace rt::shader_cache
