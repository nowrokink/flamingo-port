// DXBC containers (the title's shader containers in shaders.rpf, and the ones the runtime saves at run time): their
// parts, the HASH digest a pipeline names a shader by, the input signature and the program's stage.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fp::dxbc {

uint32_t u32(const uint8_t* p);
uint16_t u16(const uint8_t* p);

struct Part {
    std::string name;      // the four-character code (bytes >= 0x80 as Latin-1 in UTF-8)
    size_t offset, size;   // of the part's data, after its 8-byte header
};

// Every part in table order (a name may repeat). false if the table doesn't fit in `n` bytes.
bool parts(const uint8_t* d, size_t n, std::vector<Part>& out);
// The first / last part named `name` (callers mirror which one the Python tools took).
const Part* first(const std::vector<Part>& ps, const char* name);
const Part* last(const std::vector<Part>& ps, const char* name);

// "ps" "vs" "gs" "hs" "ds" "cs" "lib" "ms" "as", else the number (a DXIL program header's kind).
std::string stage_name(uint32_t kind);
// The stage from the DXIL part's program header (version u32: kind in the high half), "?" without one.
std::string stage(const std::string& container);

// The HASH part's 16-byte digest (after its 4-byte flags) as 32 hex digits: the first HASH part, or the last
// (compute_regs in the translator read the last one). nullopt without one.
std::optional<std::string> hash_digest(const std::string& container, bool last_part = false);

// ISG1 elements (the first ISG1 part): 32 bytes each after the u32 count and u32 offset.
struct InputElement {
    uint32_t stream, name_offset, index, system_value, component_type, reg;
    uint8_t mask, rw_mask;
    std::string semantic;  // the name at name_offset (ASCII), read only when asked for
};
std::vector<InputElement> input_signature(const std::string& container, bool with_names = false);

}  // namespace fp::dxbc
