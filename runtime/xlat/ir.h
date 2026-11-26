// The RDNA (gfx10) disassembly as the translator reads it: LLVM's listing (shaders/disasm.cpp writes it) parsed into
// instructions with typed operands. A port of tools/shader_ir.py: every field mirrors a key of its dicts.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fp::xlat {

// One operand. `k` is the kind:
//   "vgpr" / "sgpr"   r = first register, n = count (v3 -> 1, v[0:3] -> 4); neg / abs modifiers
//   "vlist"           list = an NSA image address's registers ([v1, v4, v2])
//   "special"         t = vcc, exec_lo, m0, scc, null, off, ... or a parenthesised word (hwreg(...), sendmsg(...))
//   "imm"             v = the integer (negation folded in)
//   "fimm"            f = the float (negation folded in); abs
//   "attr"            attr = n, comp = 0..3 (attr12.y)
//   "interp" "target" "ttmp" "raw"   t = the text
// SDWA (set by Gen::insn): sdwa_sel on a source; sdwa_dst with its select and unused mode on the destination.
struct Arg {
    std::string k;
    int r = 0;
    int n = 1;
    std::vector<int> list;
    bool neg = false, abs = false, sext = false;
    std::string t;
    int64_t v = 0;
    double f = 0;
    int attr = 0, comp = 0;
    std::string sdwa_sel;
    bool sdwa_dst = false;
    std::string sdwa_dst_sel, sdwa_dst_unused;

    bool is(const char* kind) const { return k == kind; }
    bool special(const char* name) const { return k == "special" && t == name; }
};

struct Ins {
    std::string op;
    std::vector<Arg> args;
    std::vector<std::string> mods;
    std::vector<uint8_t> enc;  // the encoding bytes LLVM printed (empty: none)
    bool has_enc = false;
    int64_t pc = 0;            // byte offset in the FSSM blob
    bool data = false;         // words that aren't in the blob as LLVM printed them (translate's re-anchoring)

    int64_t next_pc() const { return pc + static_cast<int64_t>(enc.size()); }
};

struct Listing {
    std::string name;        // the file name without .s
    int64_t code_start = 0;  // from the header line "; <name> code 0x<start>..0x<end>"
    std::vector<Ins> insns;
    int invalid = 0;         // "invalid instruction encoding" lines (data words inside the code range)
};

// shader_ir.operand / parse_line / parse_file. parse_line returns false for a line with no instruction. A malformed
// number throws PyError("ValueError"), as Python's int() would.
Arg parse_operand(const std::string& token);
bool parse_line(const std::string& line, Ins& out);
Listing parse_listing(const std::string& text, const std::string& name);
Listing parse_file(const std::wstring& path);

}  // namespace fp::xlat
