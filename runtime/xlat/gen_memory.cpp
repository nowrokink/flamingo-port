// Memory instructions: LDS (ds_*), lane swizzles, buffers (MUBUF/MTBUF), global memory and scalar loads.
// A port of rdna2spirv.Gen's lds .. buffer_store and scalar_load, line for line.
#include <cctype>
#include <map>

#include <optional>

#include "gen.h"

namespace fp::xlat {
namespace {

// An operand's "r" (Python: a["r"], a KeyError for kinds without registers).
int64_t reg_r(const Arg& a) {
    if (a.k != "vgpr" && a.k != "sgpr" && a.k != "vlist") key_error();
    return a.r;
}

// int(op.rsplit("x", 1)[1]): the dword count of buffer_load_dwordx4 and the like.
int64_t count_after_x(const S& op) {
    size_t k = op.rfind('x');
    if (k == S::npos) index_error();
    return py_int(op.substr(k + 1));
}

// [] if vaddr["k"] == "special" else [vaddr["r"] + i for i in range(vaddr.get("n", 1))]
std::vector<int64_t> vregs_of(const Arg& v) {
    std::vector<int64_t> out;
    if (v.k == "special") return out;
    int64_t r = reg_r(v);
    for (int i = 0; i < v.n; ++i) out.push_back(r + i);
    return out;
}

// len(op.rsplit("_", 1)[1]): the components of buffer_load_format_xyzw and the like.
size_t suffix_len(const S& op) {
    size_t k = op.rfind('_');
    if (k == S::npos) index_error();
    return op.size() - k - 1;
}

bool has_any(const Mods& mods, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (has(mods, w)) return true;
    return false;
}

// LDS atomics: the SPIR-V op (ds_<name>[_rtn]_<type>). Tables at namespace scope: the program is built with
// /Zc:threadSafeInit- and translates on many threads, so no function-local statics.
const std::map<S, S> kLdsAtomics = {
        {"add_u32", "OpAtomicIAdd"}, {"sub_u32", "OpAtomicISub"}, {"and_b32", "OpAtomicAnd"},
        {"or_b32", "OpAtomicOr"},    {"xor_b32", "OpAtomicXor"},  {"min_u32", "OpAtomicUMin"},
        {"max_u32", "OpAtomicUMax"}, {"min_i32", "OpAtomicSMin"}, {"max_i32", "OpAtomicSMax"},
        {"wrxchg_b32", "OpAtomicExchange"}};

// Buffer atomics: (SPIR-V op, the value that leaves memory unchanged, used for inactive lanes; none for swaps)
struct BufAtomic {
    const char* spv;
    std::optional<uint32_t> neutral;
};
const std::map<S, BufAtomic> kBufAtomics = {
        {"add", {"OpAtomicIAdd", 0u}},          {"sub", {"OpAtomicISub", 0u}},
        {"umin", {"OpAtomicUMin", 0xFFFFFFFFu}}, {"umax", {"OpAtomicUMax", 0u}},
        {"smin", {"OpAtomicSMin", 0x7FFFFFFFu}}, {"smax", {"OpAtomicSMax", 0x80000000u}},
        {"and", {"OpAtomicAnd", 0xFFFFFFFFu}},   {"or", {"OpAtomicOr", 0u}},
        {"xor", {"OpAtomicXor", 0u}},            {"swap", {"OpAtomicExchange", std::nullopt}},
        {"cmpswap", {"OpAtomicCompareExchange", std::nullopt}}};

bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// re.search(r"swizzle\((\w+)((?:,\s*\d+)*)\)", line): (kind, the numbers), or false.
bool find_swizzle(const S& line, S& kind, std::vector<int64_t>& args) {
    for (size_t at = line.find("swizzle("); at != S::npos; at = line.find("swizzle(", at + 1)) {
        size_t p = at + 8, w = p;
        while (p < line.size() && is_word(line[p])) ++p;
        if (p == w) continue;
        S k = line.substr(w, p - w);
        std::vector<int64_t> nums;
        // (?:,\s*\d+)* greedy: each repetition needs a comma, spaces, then digits
        for (;;) {
            size_t q = p;
            if (q >= line.size() || line[q] != ',') break;
            ++q;
            while (q < line.size() && is_space(line[q])) ++q;
            size_t d = q;
            while (q < line.size() && is_digit(line[q])) ++q;
            if (q == d) break;
            nums.push_back(py_int(line.substr(d, q - d)));
            p = q;
        }
        if (p < line.size() && line[p] == ')') {
            kind = k;
            args = nums;
            return true;
        }
    }
    return false;
}

}  // namespace

namespace {
// swizzle\(BITMASK_PERM,\s*"?([01pi]{5})"?\) searched in `text`: the five characters, or nothing.
std::optional<S> find_bitmask_perm(const S& text) {
    const S head = "swizzle(BITMASK_PERM,";
    for (size_t at = text.find(head); at != S::npos; at = text.find(head, at + 1)) {
        size_t i = at + head.size();
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        for (int quote = 1; quote >= 0; --quote) {  // "? greedy first, then without the quote
            size_t j = i;
            if (quote) {
                if (j >= text.size() || text[j] != '"') continue;
                ++j;
            }
            bool ok = j + 5 <= text.size();
            for (size_t k = 0; ok && k < 5; ++k) ok = S("01pi").find(text[j + k]) != S::npos;
            if (!ok) continue;
            size_t e = j + 5;
            if (e < text.size() && text[e] == '"' && e + 1 < text.size() && text[e + 1] == ')')
                return text.substr(j, 5);
            if (e < text.size() && text[e] == ')') return text.substr(j, 5);
        }
    }
    return std::nullopt;
}
}  // namespace

void Gen::swizzle(const Args& a, const Mods& mods, const Ins&) {
    // ds_swizzle_b32 vD, vS offset:swizzle(...): a subgroup shuffle (one invocation per lane).
    std::vector<S> words;
    for (const auto& x : a)
        if (x.k == "special" || x.k == "raw") words.push_back(x.t);
    S line = join(mods, " ") + " " + join(words, " ");
    S kind;
    std::vector<int64_t> args;
    const S text = line + " ";
    const bool m = find_swizzle(text, kind, args);
    const std::optional<S> perm = find_bitmask_perm(text);
    if (!m && !perm) throw Unsupported("ds_swizzle pattern");
    if (perm) kind = "BITMASK_PERM";
    uses_subgroup = true;
    S lane = tmp();
    emit({lane, " = OpLoad %uint %sg_lane"});
    S src;
    if (kind == "BITMASK_PERM") {
        // One character a bit of the lane's index within its 32 lanes, the highest first: 0 and 1 set the bit, p
        // keeps it, i inverts it (LLVM's AMDGPU assembler: the offset's and, or and xor masks).
        uint32_t keep = 0, force = 0, flip = 0;
        for (int i = 0; i < 5; ++i) {
            const uint32_t bit = 1u << (4 - i);
            const char ch = (*perm)[static_cast<size_t>(i)];
            if (ch == '1') {
                force |= bit;
            } else if (ch == 'p') {
                keep |= bit;
            } else if (ch == 'i') {
                keep |= bit;
                flip |= bit;
            }
        }
        S kept = tmp(), forced = tmp();
        src = tmp();
        emit({kept, " = OpBitwiseAnd %uint ", lane, " ", cu(0xFFFFFFE0u | keep)});
        emit({forced, " = OpBitwiseOr %uint ", kept, " ", cu(force)});
        emit({src, " = OpBitwiseXor %uint ", forced, " ", cu(flip)});
    } else if (kind == "QUAD_PERM") {
        S sel = tmp(), base = tmp();
        src = cu(0);
        emit({sel, " = OpBitwiseAnd %uint ", lane, " ", cu(3)});
        emit({base, " = OpBitwiseAnd %uint ", lane, " ", cu(0xFFFFFFFC)});
        S pick = cu(at(args, 0));
        for (int i : {1, 2, 3}) {
            S e = tmp(), nxt = tmp();
            emit({e, " = OpIEqual %bool ", sel, " ", cu(i)});
            emit({nxt, " = OpSelect %uint ", e, " ", cu(at(args, i)), " ", pick});
            pick = nxt;
        }
        src = tmp();
        emit({src, " = OpBitwiseOr %uint ", base, " ", pick});
    } else if (kind == "SWAP") {
        src = tmp();
        emit({src, " = OpBitwiseXor %uint ", lane, " ", cu(at(args, 0))});
    } else if (kind == "REVERSE") {
        src = tmp();
        emit({src, " = OpBitwiseXor %uint ", lane, " ", cu(at(args, 0) - 1)});
    } else if (kind == "BROADCAST") {
        src = tmp();
        S keep = tmp();
        emit({keep, " = OpBitwiseAnd %uint ", lane, " ", cu(~(at(args, 0) - 1) & 0xFFFFFFFF)});
        emit({src, " = OpBitwiseOr %uint ", keep, " ", cu(at(args, 1))});
    } else {
        throw Unsupported("ds_swizzle " + kind);
    }
    S v = src_u(at(a, 1));
    S r = tmp();
    emit({r, " = OpGroupNonUniformShuffle %uint %c3 ", v, " ", src});
    dst(at(a, 0), r);
}

std::pair<S, S> Gen::lds_ptr(const S& byte_addr) {
    // (Workgroup pointer, in-bounds bool) for the LDS dword at a byte address. Out-of-range LDS reads give 0 and
    // writes are dropped, as on the hardware (its LDS range check); the PC's groupshared has no such check.
    S idx = tmp(), inb = tmp(), safe = tmp(), p = tmp();
    emit({idx, " = OpShiftRightLogical %uint ", byte_addr, " ", cu(2)});
    emit({inb, " = OpULessThan %bool ", idx, " ", cu(lds_dwords)});
    emit({safe, " = OpSelect %uint ", inb, " ", idx, " ", cu(0)});
    emit({p, " = OpAccessChain %_ptr_WG_uint %lds ", safe});
    return {p, inb};
}

S Gen::lds_addr(const S& base, int64_t off) {
    if (!off) return base;
    S r = tmp();
    emit({r, " = OpIAdd %uint ", base, " ", cu(off)});
    return r;
}

void Gen::lds(const S& op, const Args& a, const Mods& mods) {
    // ds_* on the workgroup's LDS (byte addresses; vaddr + offset, write2/read2: offset0/offset1 in dwords, x64 for
    // st64). Writes and atomics only in active lanes and inside the LDS range.
    if (!lds_dwords && !compute && !mesh) throw Unsupported("LDS outside compute");
    if (has(mods, "gds")) throw Unsupported("gds");
    if (!lds_dwords) throw Unsupported("LDS size 0");
    if (lds_dwords > 8192) throw Unsupported("LDS over 32 KiB");
    uses_lds = true;
    std::map<S, int64_t> off;
    for (const auto& m : mods)
        if (starts(m, "offset")) {
            size_t c = m.find(':');
            if (c == S::npos) throw PyError("ValueError");  // dict() of a one-element split
            off[m.substr(0, c)] = py_int(m.substr(c + 1), 0);
        }
    auto get = [&](const char* k) {
        auto it = off.find(k);
        return it == off.end() ? int64_t{0} : it->second;
    };
    const S name = op.size() > 3 ? op.substr(3) : S();
    const S dev = "%c2";
    cu(2);
    const S sem = cu(0);

    auto write_word = [&](const S& addr, const S& val) {
        auto [p, inb] = lds_ptr(addr);
        S ok = tmp();
        emit({ok, " = OpLogicalAnd %bool ", active_b(), " ", inb});
        S pp = p;
        guarded(ok, [&] { emit({"OpStore ", pp, " ", val}); });
    };
    auto read_word = [&](const S& addr) {
        auto [p, inb] = lds_ptr(addr);
        S v = tmp(), r = tmp();
        emit({v, " = OpLoad %uint ", p});
        emit({r, " = OpSelect %uint ", inb, " ", v, " ", cu(0)});
        return r;
    };

    // the verb: "write" / "read" at the start
    S verb, rest;
    if (starts(name, "write")) {
        verb = "write";
        rest = name.substr(5);
    } else if (starts(name, "read")) {
        verb = "read";
        rest = name.substr(4);
    }

    // (write|read)_b(32|64|96|128)$
    if (!verb.empty() && (rest == "_b32" || rest == "_b64" || rest == "_b96" || rest == "_b128")) {
        int64_t cnt = py_int(rest.substr(2)) / 32;
        if (verb == "write") {
            S base = lds_addr(src_u(at(a, 0)), get("offset"));
            for (int64_t i = 0; i < cnt; ++i) {
                S addr = lds_addr(base, 4 * i);
                S val = load_reg("vgpr", reg_r(at(a, 1)) + i);
                write_word(addr, val);
            }
        } else {
            S base = lds_addr(src_u(at(a, 1)), get("offset"));
            std::vector<S> vals;
            for (int64_t i = 0; i < cnt; ++i) vals.push_back(read_word(lds_addr(base, 4 * i)));
            for (size_t i = 0; i < vals.size(); ++i) dst(vreg(reg_r(at(a, 0)) + static_cast<int64_t>(i)), vals[i]);
        }
        return;
    }
    // (write|read)2(st64)?_b32$
    if (!verb.empty() && (rest == "2_b32" || rest == "2st64_b32")) {
        int64_t scale = rest == "2st64_b32" ? 256 : 4;
        if (verb == "write") {
            S base = src_u(at(a, 0));
            S d0 = src_u(at(a, 1));
            S d1 = src_u(at(a, 2));
            write_word(lds_addr(base, get("offset0") * scale), d0);
            write_word(lds_addr(base, get("offset1") * scale), d1);
        } else {
            S base = src_u(at(a, 1));
            S v0 = read_word(lds_addr(base, get("offset0") * scale));
            S v1 = read_word(lds_addr(base, get("offset1") * scale));
            dst(vreg(reg_r(at(a, 0)) + 0), v0);
            dst(vreg(reg_r(at(a, 0)) + 1), v1);
        }
        return;
    }
    // (write|read)_(b|u|i)(8|16)(_d16(_hi)?)?$
    if (!verb.empty() && rest.size() >= 3 && rest[0] == '_' && (rest[1] == 'b' || rest[1] == 'u' || rest[1] == 'i')) {
        S tail = rest.substr(2);
        int bits = 0;
        if (starts(tail, "8")) {
            bits = 8;
            tail = tail.substr(1);
        } else if (starts(tail, "16")) {
            bits = 16;
            tail = tail.substr(2);
        }
        if (bits && (tail.empty() || tail == "_d16" || tail == "_d16_hi")) {
            const bool hi = tail == "_d16_hi";
            const bool d16 = !tail.empty();
            const char ty = rest[1];
            if (verb == "write") {
                S addr = lds_addr(src_u(at(a, 0)), get("offset"));
                S data = src_u(at(a, 1));
                if (hi) {
                    S t = tmp();
                    emit({t, " = OpShiftRightLogical %uint ", data, " ", cu(16)});
                    data = t;
                }
                auto [p, inb] = lds_ptr(addr);
                S sh = tmp(), lo = tmp(), part = tmp(), mask = tmp(), keep = tmp(), ok = tmp();
                emit({sh, " = OpBitFieldUExtract %uint ", addr, " ", cu(0), " ", cu(2)});
                emit({sh, "_8 = OpShiftLeftLogical %uint ", sh, " ", cu(3)});
                emit({lo, " = OpBitFieldUExtract %uint ", data, " ", cu(0), " ", cu(bits)});
                emit({part, " = OpShiftLeftLogical %uint ", lo, " ", sh, "_8"});
                emit({mask, " = OpShiftLeftLogical %uint ", cu((int64_t{1} << bits) - 1), " ", sh, "_8"});
                emit({keep, " = OpNot %uint ", mask});
                emit({ok, " = OpLogicalAnd %bool ", active_b(), " ", inb});
                S pp = p;
                guarded(ok, [&] {
                    // two atomics, so lanes writing the other part of the same dword don't race
                    S x = tmp(), y = tmp();
                    emit({x, " = OpAtomicAnd %uint ", pp, " ", dev, " ", sem, " ", keep});
                    emit({y, " = OpAtomicOr %uint ", pp, " ", dev, " ", sem, " ", part});
                });
            } else {
                S addr = lds_addr(src_u(at(a, 1)), get("offset"));
                S w = read_word(addr);
                S sh = tmp(), v = tmp();
                emit({sh, " = OpShiftLeftLogical %uint ", lds_addr(tmp_and3(addr), 0), " ", cu(3)});
                const char* ext = ty == 'i' ? "OpBitFieldSExtract" : "OpBitFieldUExtract";
                emit({v, " = ", ext, " %uint ", w, " ", sh, " ", cu(bits)});
                if (d16) {  // d16: into one half of the register, the other half kept
                    S old = load_reg("vgpr", reg_r(at(a, 0)));
                    S r = tmp();
                    emit({r, " = OpBitFieldInsert %uint ", old, " ", v, " ", cu(hi ? 16 : 0), " ", cu(16)});
                    v = r;
                }
                dst(vreg(reg_r(at(a, 0))), v);
            }
            return;
        }
    }
    const bool rtn = name.find("_rtn_") != S::npos;
    S key_name = name;
    for (size_t k; (k = key_name.find("_rtn")) != S::npos;) key_name.erase(k, 4);
    auto atom = kLdsAtomics.find(key_name);
    if (atom != kLdsAtomics.end()) {
        std::optional<Arg> vd;
        const Arg* addr_a;
        const Arg* data_a;
        if (rtn) {
            vd = at(a, 0);
            addr_a = &at(a, 1);
            data_a = &at(a, 2);
        } else {
            addr_a = &at(a, 0);
            data_a = &at(a, 1);
        }
        S addr = lds_addr(src_u(*addr_a), get("offset"));
        S data = src_u(*data_a);
        auto [p, inb] = lds_ptr(addr);
        S ok = tmp();
        emit({ok, " = OpLogicalAnd %bool ", active_b(), " ", inb});
        const S spv = atom->second;
        S pp = p;
        guarded(ok, [&] {
            S old = tmp();
            emit({old, " = ", spv, " %uint ", pp, " ", dev, " ", sem, " ", data});
            if (vd) dst(vreg(reg_r(*vd)), old);
        });
        return;
    }
    if (name == "cmpst_rtn_b32") {
        // ds_cmpst_rtn_b32 vD, vaddr, vcmp, vsrc: if LDS == cmp then LDS = src; vD = the old value
        S addr = lds_addr(src_u(at(a, 1)), get("offset"));
        S cmp = src_u(at(a, 2));
        S src = src_u(at(a, 3));
        auto [p, inb] = lds_ptr(addr);
        S ok = tmp();
        emit({ok, " = OpLogicalAnd %bool ", active_b(), " ", inb});
        S pp = p;
        guarded(ok, [&] {
            S old = tmp();
            emit({old, " = OpAtomicCompareExchange %uint ", pp, " ", dev, " ", sem, " ", sem, " ", src, " ", cmp});
            dst(vreg(reg_r(at(a, 0))), old);
        });
        return;
    }
    if (name == "bpermute_b32") {
        // vD = vdata of lane (vaddr + offset) / 4: a subgroup shuffle (no LDS access)
        S addr = lds_addr(src_u(at(a, 1)), get("offset"));
        S lane = tmp(), r = tmp();
        emit({lane, " = OpShiftRightLogical %uint ", addr, " ", cu(2)});
        uses_subgroup = true;
        emit({r, " = OpGroupNonUniformShuffle %uint %c3 ", src_u(at(a, 2)), " ", lane});
        dst(vreg(reg_r(at(a, 0))), r);
        return;
    }
    throw Unsupported(op);
}

void Gen::buffer_load(const S& op, const Args& a, const Mods& mods) {
    // buffer_load_dword[xN] vD, vaddr, s[V#], soffset [offen|idxen] offset:N  -> buffer-device-address loads.
    // addr = V#.base + soffset + inst_offset + (offen ? vaddr : 0) + (idxen ? vindex * V#.stride : 0).
    if (has_any(mods, {"lds", "tfe"}) || op.find("format") != S::npos) throw Unsupported(op + " form");
    int64_t count = ends(op, "dword") ? 1 : count_after_x(op);
    const Arg& vd = at(a, 0);
    const Arg& vaddr = at(a, 1);
    const Arg& rsrc = at(a, 2);
    const Arg soff = a.size() > 3 ? a[3] : imm(0);
    if (rsrc.k != "sgpr" || rsrc.n != 4) throw Unsupported("buffer_load V#");
    S d0 = load_reg("sgpr", rsrc.r);
    S d1 = load_reg("sgpr", rsrc.r + 1);
    S hi = tmp(), stride = tmp();
    emit({hi, " = OpBitwiseAnd %uint ", d1, " ", cu(0xFFFF)});
    emit({stride, " = OpBitFieldUExtract %uint ", d1, " ", cu(16), " ", cu(14)});
    S off = src_u(soff);
    int64_t inst = inst_offset(mods);
    S total = tmp();
    emit({total, " = OpIAdd %uint ", off, " ", cu(inst)});
    const bool offen = has(mods, "offen"), idxen = has(mods, "idxen");
    auto vregs = vregs_of(vaddr);
    if (idxen) {
        S idx = load_reg("vgpr", at(vregs, 0));
        S m = tmp(), t = tmp();
        emit({m, " = OpIMul %uint ", idx, " ", stride});
        emit({t, " = OpIAdd %uint ", total, " ", m});
        total = t;
    }
    if (offen) {
        S o = load_reg("vgpr", idxen ? at(vregs, 1) : at(vregs, 0));
        S t = tmp();
        emit({t, " = OpIAdd %uint ", total, " ", o});
        total = t;
    }
    S l64 = tmp(), h64 = tmp(), sh = tmp(), addr = tmp(), o64 = tmp(), full = tmp(), ptr = tmp();
    emit({l64, " = OpUConvert %ulong ", d0});
    emit({h64, " = OpUConvert %ulong ", hi});
    emit({sh, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
    emit({addr, " = OpBitwiseOr %ulong ", sh, " ", l64});
    emit({o64, " = OpUConvert %ulong ", total});
    emit({full, " = OpIAdd %ulong ", addr, " ", o64});
    emit({ptr, " = OpConvertUToPtr %_ptr_PSB_arr ", host_addr(full)});
    uses_psb = true;
    for (int64_t i = 0; i < count; ++i) {
        S p = tmp(), v = tmp();
        emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(i)});
        emit({v, " = OpLoad %uint ", p, " Aligned 4"});
        dst(vreg(reg_r(vd) + i), v);
    }
}

std::vector<S> Gen::decode_format(const std::vector<S>& words, int code) {
    // Decode one element of format `code` from raw dwords -> 4 uint ids holding f32 bits (or uint for *_UINT/uint).
    const BufFormat* fmt = buffer_format(code);
    if (!fmt) key_error();
    const S& kind = fmt->kind;
    if (kind == "f32" || kind == "u32") {
        // integers are passed through as their bits
        std::vector<S> vals = slice(words, 0, static_cast<int64_t>(fmt->widths.size()));
        S zero = cu(0);
        size_t pad = vals.size() < 4 ? 4 - vals.size() : 0;
        for (size_t i = 0; i < pad; ++i) vals.push_back(zero);
        return vals;
    }
    std::vector<S> out;
    int bit = 0;
    for (int w : fmt->widths) {
        const S& src = at(words, bit / 32);
        S f = tmp();
        emit({f, " = OpBitFieldUExtract %uint ", src, " ", cu(bit % 32), " ", cu(w)});
        if (kind == "f16") {
            S v2 = tmp(), x = tmp();
            emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", f});
            emit({x, " = OpCompositeExtract %float ", v2, " 0"});
            out.push_back(tou(x));
        } else if (kind == "unorm") {
            S c = tmp(), x = tmp();
            emit({c, " = OpConvertUToF %float ", f});
            emit({x, " = OpFMul %float ", c, " ", fconst(1.0 / static_cast<double>((int64_t{1} << w) - 1))});
            out.push_back(tou(x));
        } else if (kind == "snorm") {
            S si = tmp(), c = tmp(), x = tmp(), cl = tmp();
            emit({si, " = OpBitFieldSExtract %int ", as_int(src), " ", cu(bit % 32), " ", cu(w)});
            emit({c, " = OpConvertSToF %float ", si});
            emit({x, " = OpFMul %float ", c, " ", fconst(1.0 / static_cast<double>((int64_t{1} << (w - 1)) - 1))});
            emit({cl, " = OpExtInst %float %glsl FMax ", x, " ", fconst(-1.0)});
            out.push_back(tou(cl));
        } else {  // uint: raw integer
            out.push_back(f);
        }
        bit += w;
    }
    S one = (kind == "f16" || kind == "unorm" || kind == "snorm") ? cu(0x3F800000) : cu(1);
    while (out.size() < 4) out.push_back(out.size() < 3 ? cu(0) : one);
    return out;
}

void Gen::buffer_atomic(const S& op, const Args& a, const Mods& mods) {
    // buffer_atomic_<op> vdata, vaddr, s[V#], soffset ...: 32-bit atomic at the element address (buffer device
    // address). With 'glc' the pre-op value is returned in vdata. Inactive lanes don't touch memory.
    const S kind = op.size() > 14 ? op.substr(14) : S();
    auto it = kBufAtomics.find(kind);
    if (it == kBufAtomics.end() || kind.find("x2") != S::npos) throw Unsupported(op);
    const S spv = it->second.spv;
    const auto neutral = it->second.neutral;
    S addr = buffer_address(a, mods).first;
    const Arg& vd = at(a, 0);
    S ptr = tmp(), p = tmp();
    emit({ptr, " = OpConvertUToPtr %_ptr_PSB_arr ", host_addr(addr)});
    emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(0)});
    S val = load_reg("vgpr", reg_r(vd));
    S act = active_and(buffer_in_range(a, mods, 4));  // outside the V#'s records: no change
    S old;
    if (neutral) {
        S safe = tmp();
        old = tmp();
        emit({safe, " = OpSelect %uint ", act, " ", val, " ", cu(*neutral)});
        emit({old, " = ", spv, " %uint ", p, " ", cu(1), " ", cu(0), " ", safe});
    } else {
        // swap/cmpswap have no neutral operand: guard with a structured if and merge the result.
        S lbl_do = tmp() + "_do";
        S lbl_end = tmp() + "_end";
        S res_do = tmp();
        old = tmp();
        S cur = tmp();
        emit({cur, " = OpLoad %uint ", p, " Aligned 4"});
        emit({"OpSelectionMerge ", lbl_end, " None"});
        emit({"OpBranchConditional ", act, " ", lbl_do, " ", lbl_end});
        S pre_label = current_label_placeholder();
        emit({lbl_do, " = OpLabel"});
        if (kind == "cmpswap") {
            S cmp = load_reg("vgpr", reg_r(vd) + 1);
            emit({res_do, " = OpAtomicCompareExchange %uint ", p, " ", cu(1), " ", cu(0), " ", cu(0), " ", val, " ",
                  cmp});
        } else {
            emit({res_do, " = OpAtomicExchange %uint ", p, " ", cu(1), " ", cu(0), " ", val});
        }
        emit({"OpBranch ", lbl_end});
        emit({lbl_end, " = OpLabel"});
        emit({old, " = OpPhi %uint ", res_do, " ", lbl_do, " ", cur, " ", pre_label});
    }
    if (has(mods, "glc")) dst(vreg(reg_r(vd)), old);
}

void Gen::buffer_load_format(const S& op, const Args& a, const Mods& mods, std::optional<int> fixed_fmt) {
    // buffer_load_format_[x|xy|xyz|xyzw]: element at V#.base + offsets, decoded per the V# FORMAT field (word3 bits
    // 12-18), then swizzled by DST_SEL_X/Y/Z/W (word3 bits 0-11: 0=0, 1=1, 4..7 = X..W). Formats outside FORMATS
    // decode to 0. num_records bounds checking isn't modelled.
    if (has_any(mods, {"lds", "tfe", "d16"})) throw Unsupported(op + " form");
    const size_t ncomp = suffix_len(op);
    const Arg& vd = at(a, 0);
    const Arg& vaddr = at(a, 1);
    const Arg& rsrc = at(a, 2);
    const Arg soff = a.size() > 3 ? a[3] : imm(0);
    if (rsrc.k != "sgpr" || rsrc.n != 4) throw Unsupported("buffer_load_format V#");
    S d0 = load_reg("sgpr", rsrc.r + 0);
    S d1 = load_reg("sgpr", rsrc.r + 1);
    S d3 = load_reg("sgpr", rsrc.r + 3);
    S hi = tmp(), stride = tmp();
    emit({hi, " = OpBitwiseAnd %uint ", d1, " ", cu(0xFFFF)});
    emit({stride, " = OpBitFieldUExtract %uint ", d1, " ", cu(16), " ", cu(14)});
    S fmt;
    if (fixed_fmt) {
        fmt = cu(*fixed_fmt);  // tbuffer: format from the instruction
    } else {
        fmt = tmp();
        emit({fmt, " = OpBitFieldUExtract %uint ", d3, " ", cu(12), " ", cu(7)});
    }
    int64_t inst = inst_offset(mods);
    S total = tmp();
    emit({total, " = OpIAdd %uint ", src_u(soff), " ", cu(inst)});
    auto vregs = vregs_of(vaddr);
    if (has(mods, "idxen")) {
        S idx = load_reg("vgpr", at(vregs, 0));
        S m = tmp(), t = tmp();
        emit({m, " = OpIMul %uint ", idx, " ", stride});
        emit({t, " = OpIAdd %uint ", total, " ", m});
        total = t;
    }
    if (has(mods, "offen")) {
        S o = load_reg("vgpr", has(mods, "idxen") ? at(vregs, 1) : at(vregs, 0));
        S t = tmp();
        emit({t, " = OpIAdd %uint ", total, " ", o});
        total = t;
    }
    uses_psb = true;
    S l64 = tmp(), h64 = tmp(), sh = tmp(), base = tmp();
    emit({l64, " = OpUConvert %ulong ", d0});
    emit({h64, " = OpUConvert %ulong ", hi});
    emit({sh, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
    emit({base, " = OpBitwiseOr %ulong ", sh, " ", l64});
    // Byte address of the element, split into a dword-aligned address and a byte shift (for 1/2-byte formats).
    S o64 = tmp(), addr = tmp(), alo = tmp();
    emit({o64, " = OpUConvert %ulong ", total});
    emit({addr, " = OpIAdd %ulong ", base, " ", o64});
    emit({alo, " = OpUConvert %uint ", addr});
    S bsh = tmp(), al64 = tmp(), aligned = tmp();
    S b3 = tmp();
    emit({b3, " = OpBitwiseAnd %uint ", alo, " ", cu(3)});
    emit({bsh, " = OpIMul %uint ", b3, " ", cu(8)});
    emit({al64, " = OpUConvert %ulong ", b3});
    emit({aligned, " = OpISub %ulong ", addr, " ", al64});
    // How many dwords this element needs, by format (others: 1). Extra loads re-read dword 0, so a load never goes
    // past the element (the last element of a buffer stays in bounds).
    S need = cu(1);
    for (const auto& [code, f] : buffer_formats()) {
        S e = tmp(), n2 = tmp();
        emit({e, " = OpIEqual %bool ", fmt, " ", cu(code)});
        emit({n2, " = OpSelect %uint ", e, " ", cu(std::max(1, (f.bytes + 3) / 4)), " ", need});
        need = n2;
    }
    S ptr = tmp();
    emit({ptr, " = OpConvertUToPtr %_ptr_PSB_arr ", host_addr(aligned)});
    std::vector<S> words;
    for (int i = 0; i < 4; ++i) {
        S ok = tmp(), idx = tmp(), p = tmp(), v = tmp();
        emit({ok, " = OpULessThan %bool ", cu(i), " ", need});
        emit({idx, " = OpSelect %uint ", ok, " ", cu(i), " ", cu(0)});
        emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", idx});
        emit({v, " = OpLoad %uint ", p, " Aligned 4"});
        words.push_back(v);
    }
    // Sub-dword formats start at the byte shift inside dword 0 (and may spill into dword 1).
    S w0s = tmp(), w1s = tmp(), nz = tmp(), w1sh = tmp(), merged = tmp(), bw0 = tmp();
    emit({w0s, " = OpShiftRightLogical %uint ", words[0], " ", bsh});
    emit({w1sh, " = OpISub %uint ", cu(32), " ", bsh});
    emit({w1s, " = OpShiftLeftLogical %uint ", words[1], " ", w1sh});
    emit({nz, " = OpINotEqual %bool ", bsh, " ", cu(0)});
    emit({merged, " = OpBitwiseOr %uint ", w0s, " ", w1s});
    emit({bw0, " = OpSelect %uint ", nz, " ", merged, " ", words[0]});
    words[0] = bw0;
    // Decode every supported format, then pick by FORMAT.
    std::vector<S> comps(4, cu(0));
    for (const auto& [code, f] : buffer_formats()) {
        std::vector<S> dec = decode_format(words, code);
        S e = tmp();
        emit({e, " = OpIEqual %bool ", fmt, " ", cu(code)});
        std::vector<S> nxt;
        for (int c = 0; c < 4; ++c) {
            S s = tmp();
            emit({s, " = OpSelect %uint ", e, " ", dec[c], " ", comps[c]});
            nxt.push_back(s);
        }
        comps = nxt;
    }
    // DST_SEL swizzle: 0 -> 0, 1 -> 1.0, 4..7 -> X..W.
    for (size_t c = 0; c < ncomp; ++c) {
        S sel = tmp();
        emit({sel, " = OpBitFieldUExtract %uint ", d3, " ", cu(3 * static_cast<int64_t>(c)), " ", cu(3)});
        S r = cu(0);
        const std::pair<int, S> choices[] = {
            {1, cu(0x3F800000)}, {4, comps[0]}, {5, comps[1]}, {6, comps[2]}, {7, comps[3]}};
        for (const auto& [code_v, src] : choices) {
            S e = tmp(), s = tmp();
            emit({e, " = OpIEqual %bool ", sel, " ", cu(code_v)});
            emit({s, " = OpSelect %uint ", e, " ", src, " ", r});
            r = s;
        }
        dst(vreg(reg_r(vd) + static_cast<int64_t>(c)), r);
    }
}

void Gen::global_mem(const S& op, const Args& a, const Mods& mods) {
    // global_load_dword[xN] vD, vaddr, saddr|off offset:N / global_store_dword[xN] vaddr, vD, saddr|off.
    // Address = (saddr: 64-bit SGPR pair + zero-extended 32-bit vaddr | off: 64-bit VGPR pair) + offset, an Xbox GPU
    // VA translated through the heap base. Stores and loads only take effect in active lanes.
    const bool load = starts(op, "global_load");
    if (has_any(mods, {"lds", "tfe", "d16"})) throw Unsupported(op + " form");
    int64_t count = ends(op, "dword") ? 1 : count_after_x(op);
    const Arg& data = load ? at(a, 0) : at(a, 1);
    const Arg& vaddr = load ? at(a, 1) : at(a, 0);
    const Arg& saddr = at(a, 2);
    int64_t inst = inst_offset(mods);
    if (inst & 0x800) inst -= 0x1000;  // 12-bit signed offset
    S va;
    if (saddr.k == "sgpr" && saddr.n == 2) {
        S lo = load_reg("sgpr", saddr.r);
        S hi = load_reg("sgpr", saddr.r + 1);
        S voff = load_reg("vgpr", reg_r(vaddr));
        S l64 = tmp(), h64 = tmp(), sh = tmp(), base = tmp(), v64 = tmp();
        va = tmp();
        emit({l64, " = OpUConvert %ulong ", lo});
        emit({h64, " = OpUConvert %ulong ", hi});
        emit({sh, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
        emit({base, " = OpBitwiseOr %ulong ", sh, " ", l64});
        emit({v64, " = OpUConvert %ulong ", voff});
        emit({va, " = OpIAdd %ulong ", base, " ", v64});
    } else if (saddr.special("off") && vaddr.k == "vgpr" && vaddr.n == 2) {
        S lo = load_reg("vgpr", vaddr.r);
        S hi = load_reg("vgpr", vaddr.r + 1);
        S l64 = tmp(), h64 = tmp(), sh = tmp();
        va = tmp();
        emit({l64, " = OpUConvert %ulong ", lo});
        emit({h64, " = OpUConvert %ulong ", hi});
        emit({sh, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
        emit({va, " = OpBitwiseOr %ulong ", sh, " ", l64});
    } else {
        throw Unsupported(op + " addressing");
    }
    uses_psb = true;
    S off64 = tmp(), full = tmp();
    S ioff = tmp();
    emit({ioff, " = OpBitcast %uint ", as_int(cu(inst & 0xFFFFFFFF))});
    emit({off64, " = OpSConvert %long ", as_int(ioff)});
    S o2 = tmp();
    emit({o2, " = OpBitcast %ulong ", off64});
    emit({full, " = OpIAdd %ulong ", va, " ", o2});
    uses_long = true;
    S act = active_b();
    S lbl_do = tmp() + "_do";
    S lbl_end = tmp() + "_end";
    const int64_t dr = reg_r(data);
    if (load) {
        // Inactive lanes don't load (and keep their registers): guard the access, merge with OpPhi per dword.
        std::vector<S> olds;
        for (int64_t i = 0; i < count; ++i) olds.push_back(load_reg("vgpr", dr + i));
        S pre = current_label_placeholder();
        emit({"OpSelectionMerge ", lbl_end, " None"});
        emit({"OpBranchConditional ", act, " ", lbl_do, " ", lbl_end});
        emit({lbl_do, " = OpLabel"});
        S ptr = tmp();
        emit({ptr, " = OpConvertUToPtr %_ptr_PSB_arr ", host_addr(full)});
        std::vector<S> vals;
        for (int64_t i = 0; i < count; ++i) {
            S p = tmp(), v = tmp();
            emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(i)});
            emit({v, " = OpLoad %uint ", p, " Aligned 4"});
            vals.push_back(v);
        }
        emit({"OpBranch ", lbl_end});
        emit({lbl_end, " = OpLabel"});
        std::vector<S> phis;
        for (int64_t i = 0; i < count; ++i) {  // all OpPhi first: they must open the merge block
            S ph = tmp();
            emit({ph, " = OpPhi %uint ", vals[i], " ", lbl_do, " ", olds[i], " ", pre});
            phis.push_back(ph);
        }
        for (int64_t i = 0; i < count; ++i) store_reg("vgpr", dr + i, phis[i]);
    } else {
        std::vector<S> vals;
        for (int64_t i = 0; i < count; ++i) vals.push_back(load_reg("vgpr", dr + i));
        emit({"OpSelectionMerge ", lbl_end, " None"});
        emit({"OpBranchConditional ", act, " ", lbl_do, " ", lbl_end});
        emit({lbl_do, " = OpLabel"});
        S ptr = tmp();
        emit({ptr, " = OpConvertUToPtr %_ptr_PSB_arr ", host_addr(full)});
        for (int64_t i = 0; i < count; ++i) {
            S p = tmp();
            emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(i)});
            emit({"OpStore ", p, " ", vals[i], " Aligned 4"});
        }
        emit({"OpBranch ", lbl_end});
        emit({lbl_end, " = OpLabel"});
    }
}

std::pair<S, S> Gen::buffer_address(const Args& a, const Mods& mods) {
    // (64-bit element address id, V# word3 id) for a MUBUF instruction: vdata, vaddr, srsrc, soffset.
    const Arg& vaddr = at(a, 1);
    const Arg& rsrc = at(a, 2);
    const Arg soff = a.size() > 3 ? a[3] : imm(0);
    if (rsrc.k != "sgpr" || rsrc.n != 4) throw Unsupported("buffer V#");
    S d0 = load_reg("sgpr", rsrc.r + 0);
    S d1 = load_reg("sgpr", rsrc.r + 1);
    S d3 = load_reg("sgpr", rsrc.r + 3);
    S hi = tmp(), stride = tmp();
    emit({hi, " = OpBitwiseAnd %uint ", d1, " ", cu(0xFFFF)});
    emit({stride, " = OpBitFieldUExtract %uint ", d1, " ", cu(16), " ", cu(14)});
    int64_t inst = inst_offset(mods);
    S total = tmp();
    emit({total, " = OpIAdd %uint ", src_u(soff), " ", cu(inst)});
    auto vregs = vregs_of(vaddr);
    if (has(mods, "idxen")) {
        S idx = load_reg("vgpr", at(vregs, 0));
        S m = tmp(), t = tmp();
        emit({m, " = OpIMul %uint ", idx, " ", stride});
        emit({t, " = OpIAdd %uint ", total, " ", m});
        total = t;
    }
    if (has(mods, "offen")) {
        S o = load_reg("vgpr", has(mods, "idxen") ? at(vregs, 1) : at(vregs, 0));
        S t = tmp();
        emit({t, " = OpIAdd %uint ", total, " ", o});
        total = t;
    }
    uses_psb = true;
    S l64 = tmp(), h64 = tmp(), sh = tmp(), base = tmp(), o64 = tmp(), addr = tmp();
    emit({l64, " = OpUConvert %ulong ", d0});
    emit({h64, " = OpUConvert %ulong ", hi});
    emit({sh, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
    emit({base, " = OpBitwiseOr %ulong ", sh, " ", l64});
    emit({o64, " = OpUConvert %ulong ", total});
    emit({addr, " = OpIAdd %ulong ", base, " ", o64});
    return {addr, d3};
}

S Gen::buffer_in_range(const Args& a, const Mods& mods, int64_t end) {
    // Whether a MUBUF access ending `end` bytes past its element offset is inside the V#'s records: the console
    // drops writes outside them. A raw buffer (stride 0): the offset (VGPR plus the instruction's, not soffset) plus
    // the bytes against num_records (word 2) in bytes; a structured one: the index against num_records.
    const Arg& vaddr = at(a, 1);
    const Arg& rsrc = at(a, 2);
    S d1 = load_reg("sgpr", rsrc.r + 1);
    S d2 = load_reg("sgpr", rsrc.r + 2);
    S stride = tmp();
    emit({stride, " = OpBitFieldUExtract %uint ", d1, " ", cu(16), " ", cu(14)});
    auto vregs = vregs_of(vaddr);
    S idx = has(mods, "idxen") ? load_reg("vgpr", at(vregs, 0)) : cu(0);
    S off = cu(inst_offset(mods) + end);
    if (has(mods, "offen")) {
        S o = load_reg("vgpr", has(mods, "idxen") ? at(vregs, 1) : at(vregs, 0));
        S t = tmp();
        emit({t, " = OpIAdd %uint ", o, " ", off});
        off = t;
    }
    S raw = tmp(), by_off = tmp(), by_idx = tmp(), ok = tmp();
    emit({raw, " = OpIEqual %bool ", stride, " ", cu(0)});
    emit({by_off, " = OpULessThanEqual %bool ", off, " ", d2});
    emit({by_idx, " = OpULessThan %bool ", idx, " ", d2});
    emit({ok, " = OpSelect %bool ", raw, " ", by_off, " ", by_idx});
    return ok;
}

S Gen::active_and(const S& cond) {
    S b = tmp();
    emit({b, " = OpLogicalAnd %bool ", active_b(), " ", cond});
    return b;
}

void Gen::buffer_store(const S& op, const Args& a, const Mods& mods) {
    // buffer_store_dword[xN] (raw) and buffer_store_format_* (f32 formats stored raw, f16x2/x4, unorm8x4). Only
    // active lanes store, inside a structured if.
    if (has_any(mods, {"lds", "tfe", "d16"})) throw Unsupported(op + " form");
    const Arg& vd = at(a, 0);
    auto [addr, d3] = buffer_address(a, mods);
    const bool raw = starts(op, "buffer_store_dword");
    std::vector<S> words, comps, fl;
    S fmt;
    // Encodings for the formats that appear in store-heavy shaders; others store nothing.
    const std::pair<int, int> f32[] = {{22, 1}, {64, 2}, {74, 3}, {77, 4}};
    if (raw) {
        int64_t count = ends(op, "dword") ? 1 : count_after_x(op);
        for (int64_t i = 0; i < count; ++i) words.push_back(load_reg("vgpr", reg_r(vd) + i));
    } else {
        size_t count = suffix_len(op);
        for (size_t i = 0; i < count; ++i) comps.push_back(load_reg("vgpr", reg_r(vd) + static_cast<int64_t>(i)));
        fmt = tmp();
        emit({fmt, " = OpBitFieldUExtract %uint ", d3, " ", cu(12), " ", cu(7)});
        // Float views of the components, computed before any branch so every branch can use them.
        for (const auto& c : comps) fl.push_back(tof(c));
        for (size_t i = comps.size(); i < 4; ++i) fl.push_back("%f0");
    }
    // the store's bytes (a formatted one: its first) inside the V#'s records, as the console drops the rest
    S act = active_and(buffer_in_range(a, mods, raw ? 4 * static_cast<int64_t>(words.size()) : 1));
    S lbl_do = tmp() + "_do";
    S lbl_end = tmp() + "_end";
    emit({"OpSelectionMerge ", lbl_end, " None"});
    emit({"OpBranchConditional ", act, " ", lbl_do, " ", lbl_end});
    emit({lbl_do, " = OpLabel"});
    S ptr = tmp();
    emit({ptr, " = OpConvertUToPtr %_ptr_PSB_arr ", host_addr(addr)});
    if (raw) {
        for (size_t i = 0; i < words.size(); ++i) {
            S p = tmp();
            emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(static_cast<int64_t>(i))});
            emit({"OpStore ", p, " ", words[i], " Aligned 4"});
        }
    } else {
        // f32 formats: store min(n, format components) raw dwords; f16x4 (71) packs pairs; unorm8x4 (56) packs.
        for (const auto& [code, cnt] : f32) {
            S e = tmp();
            emit({e, " = OpIEqual %bool ", fmt, " ", cu(code)});
            S ld = tmp() + "_f";
            S le = tmp() + "_fe";
            emit({"OpSelectionMerge ", le, " None"});
            emit({"OpBranchConditional ", e, " ", ld, " ", le});
            emit({ld, " = OpLabel"});
            for (size_t i = 0; i < std::min<size_t>(static_cast<size_t>(cnt), comps.size()); ++i) {
                S p = tmp();
                emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(static_cast<int64_t>(i))});
                emit({"OpStore ", p, " ", comps[i], " Aligned 4"});
            }
            emit({"OpBranch ", le});
            emit({le, " = OpLabel"});
        }
        {
            S e = tmp();
            emit({e, " = OpIEqual %bool ", fmt, " ", cu(71)});
            S ld = tmp() + "_h";
            S le = tmp() + "_he";
            emit({"OpSelectionMerge ", le, " None"});
            emit({"OpBranchConditional ", e, " ", ld, " ", le});
            emit({ld, " = OpLabel"});
            for (int i = 0; i < 2; ++i) {
                S v = tmp(), pk = tmp(), p = tmp();
                emit({v, " = OpCompositeConstruct %v2float ", at(fl, 2 * i), " ", at(fl, 2 * i + 1)});
                emit({pk, " = OpExtInst %uint %glsl PackHalf2x16 ", v});
                emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(i)});
                emit({"OpStore ", p, " ", pk, " Aligned 4"});
            }
            emit({"OpBranch ", le});
            emit({le, " = OpLabel"});
        }
        {
            S e = tmp();
            emit({e, " = OpIEqual %bool ", fmt, " ", cu(56)});
            S ld = tmp() + "_u";
            S le = tmp() + "_ue";
            emit({"OpSelectionMerge ", le, " None"});
            emit({"OpBranchConditional ", e, " ", ld, " ", le});
            emit({ld, " = OpLabel"});
            S v4 = tmp(), pk = tmp(), p = tmp();
            emit({v4, " = OpCompositeConstruct %v4float ", join(slice(fl, 0, 4))});
            emit({pk, " = OpExtInst %uint %glsl PackUnorm4x8 ", v4});
            emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(0)});
            emit({"OpStore ", p, " ", pk, " Aligned 4"});
            emit({"OpBranch ", le});
            emit({le, " = OpLabel"});
        }
    }
    emit({"OpBranch ", lbl_end});
    emit({lbl_end, " = OpLabel"});
}

void Gen::scalar_load(const S& op, const Args& a, const Mods&) {
    // s_load_dwordxN sD, s[p:p+1], off   -> N dwords at (64-bit pointer + off)
    // s_buffer_load_dwordxN sD, s[v:v+3], off -> N dwords at (V# base address + off)
    // Both become PhysicalStorageBuffer loads (Vulkan buffer device address). V# base = dword0 | (dword1 & 0xFFFF)
    // << 32 (AMD RDNA ISA, buffer resource descriptor).
    int64_t count = ends(op, "dword") ? 1 : count_after_x(op);
    const Arg& d = at(a, 0);
    const Arg& base = at(a, 1);
    if (d.k != "sgpr" || base.k != "sgpr") throw Unsupported(op + " operands");
    S lo = load_reg("sgpr", base.r);
    S hi = load_reg("sgpr", base.r + 1);
    if (starts(op, "s_buffer")) {
        if (base.n != 4) throw Unsupported("s_buffer_load without V#");
        S m = tmp();
        emit({m, " = OpBitwiseAnd %uint ", hi, " ", cu(0xFFFF)});
        hi = m;
    } else {
        if (base.n != 2) throw Unsupported("s_load without 64-bit pointer");
    }
    const Arg off = a.size() > 2 ? a[2] : imm(0);
    S offv = src_u(off);
    S l64 = tmp(), h64 = tmp(), sh = tmp(), addr = tmp(), o64 = tmp(), full = tmp();
    emit({l64, " = OpUConvert %ulong ", lo});
    emit({h64, " = OpUConvert %ulong ", hi});
    emit({sh, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
    emit({addr, " = OpBitwiseOr %ulong ", sh, " ", l64});
    emit({o64, " = OpUConvert %ulong ", offv});
    emit({full, " = OpIAdd %ulong ", addr, " ", o64});
    S ptr = tmp();
    emit({ptr, " = OpConvertUToPtr %_ptr_PSB_arr ", host_addr(full)});
    uses_psb = true;
    for (int64_t i = 0; i < count; ++i) {
        S p = tmp(), v = tmp();
        emit({p, " = OpAccessChain %_ptr_PSB_uint ", ptr, " ", cu(i)});
        emit({v, " = OpLoad %uint ", p, " Aligned 4"});
        store_reg("sgpr", d.r + i, v);
    }
}

}  // namespace fp::xlat
