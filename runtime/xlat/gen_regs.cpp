// The generator's core (rdna2spirv.Gen): ids and constants, emitting lines (with the d3d12 target's rewrites),
// registers and operands, EXEC, destinations, modifiers and SDWA.
#include "gen.h"

namespace fp::xlat {
namespace {

bool word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The end of a `%\w+` id starting at `at`, or npos.
size_t id_end(std::string_view s, size_t at) {
    if (at >= s.size() || s[at] != '%') return std::string_view::npos;
    size_t e = at + 1;
    while (e < s.size() && word_char(s[e])) ++e;
    return e == at + 1 ? std::string_view::npos : e;
}

// Matches `s` against a pattern of ids and literal pieces: ids[i] receives the i-th `%\w+`. `pieces` alternates
// literal text and nullptr (an id); the match must cover the whole string (Python's re.match with `$`).
bool match(std::string_view s, std::initializer_list<const char*> pieces, std::vector<S>& ids) {
    ids.clear();
    size_t at = 0;
    for (const char* p : pieces) {
        if (!p) {
            size_t e = id_end(s, at);
            if (e == std::string_view::npos) return false;
            ids.emplace_back(s.substr(at, e - at));
            at = e;
        } else {
            std::string_view lit(p);
            if (s.substr(at, lit.size()) != lit) return false;
            at += lit.size();
        }
    }
    return at == s.size();
}

}  // namespace

S ud_word(int i, Target target) {
    if (target == Target::d3d12) return cat({"%c0 %c", X(static_cast<uint64_t>(i / 4)), " %c", X(static_cast<uint64_t>(i % 4))});
    if (i < USER_SGPRS) return cat({"%c0 %c", X(static_cast<uint64_t>(i))});
    return cat({"%c", X(static_cast<uint64_t>(i - USER_SGPRS + 1))});
}

Arg Gen::vreg(int64_t r) {
    Arg a;
    a.k = "vgpr";
    a.r = static_cast<int>(r);
    a.n = 1;
    return a;
}

Arg Gen::imm(int64_t v) {
    Arg a;
    a.k = "imm";
    a.v = v;
    return a;
}

Arg Gen::plain(Arg a) {
    a.neg = false;
    a.abs = false;
    return a;
}

S Gen::tmp() { return "%t" + std::to_string(++n); }

S Gen::cu(int64_t v) {
    uint32_t u = static_cast<uint32_t>(v & 0xFFFFFFFF);
    consts.insert(u);
    return "%c" + X(u);
}

std::vector<S> Gen::mul_extended(const S& res, const S& op, const S& a, const S& b) {
    // d3d12: OpUMulExtended / OpSMulExtended {lo, hi} without 64-bit integers. With a = ah:al and b = bh:bl (16-bit
    // halves), hi = ah*bh + (al*bh >> 16) + (ah*bl >> 16) + carries out of the middle column; the signed high word is
    // the unsigned one minus (a < 0 ? b : 0) and (b < 0 ? a : 0).
    const bool is_signed = starts(op, "OpS");
    std::vector<S> out;
    S x = a, y = b;
    if (is_signed) {
        x = tmp();
        y = tmp();
        out.push_back(cat({x, " = OpBitcast %uint ", a}));
        out.push_back(cat({y, " = OpBitcast %uint ", b}));
    }
    S c16 = cu(16), mask = cu(0xFFFF);
    S al = tmp(), ah = tmp(), bl = tmp(), bh = tmp(), ll = tmp(), lh = tmp(), hl = tmp(), hh = tmp();
    out.push_back(cat({al, " = OpBitwiseAnd %uint ", x, " ", mask}));
    out.push_back(cat({ah, " = OpShiftRightLogical %uint ", x, " ", c16}));
    out.push_back(cat({bl, " = OpBitwiseAnd %uint ", y, " ", mask}));
    out.push_back(cat({bh, " = OpShiftRightLogical %uint ", y, " ", c16}));
    out.push_back(cat({ll, " = OpIMul %uint ", al, " ", bl}));
    out.push_back(cat({lh, " = OpIMul %uint ", al, " ", bh}));
    out.push_back(cat({hl, " = OpIMul %uint ", ah, " ", bl}));
    out.push_back(cat({hh, " = OpIMul %uint ", ah, " ", bh}));
    S ll_h = tmp(), lh_l = tmp(), hl_l = tmp(), mid = tmp(), mid_h = tmp(), lh_h = tmp(), hl_h = tmp(), s1 = tmp(),
      s2 = tmp(), hi = tmp(), lo = tmp();
    out.push_back(cat({ll_h, " = OpShiftRightLogical %uint ", ll, " ", c16}));
    out.push_back(cat({lh_l, " = OpBitwiseAnd %uint ", lh, " ", mask}));
    out.push_back(cat({hl_l, " = OpBitwiseAnd %uint ", hl, " ", mask}));
    out.push_back(cat({s1, " = OpIAdd %uint ", ll_h, " ", lh_l}));
    out.push_back(cat({mid, " = OpIAdd %uint ", s1, " ", hl_l}));
    out.push_back(cat({mid_h, " = OpShiftRightLogical %uint ", mid, " ", c16}));
    out.push_back(cat({lh_h, " = OpShiftRightLogical %uint ", lh, " ", c16}));
    out.push_back(cat({hl_h, " = OpShiftRightLogical %uint ", hl, " ", c16}));
    out.push_back(cat({s2, " = OpIAdd %uint ", hh, " ", lh_h}));
    out.push_back(cat({hi, " = OpIAdd %uint ", s2, " ", hl_h}));
    out.push_back(cat({lo, " = OpIMul %uint ", x, " ", y}));
    S hi2 = tmp();
    out.push_back(cat({hi2, " = OpIAdd %uint ", hi, " ", mid_h}));
    if (!is_signed) {
        out.push_back(cat({res, " = OpCompositeConstruct %umul ", lo, " ", hi2}));
        return out;
    }
    S xn = tmp(), yn = tmp(), cx = tmp(), cy = tmp(), d1 = tmp(), d2 = tmp(), slo = tmp(), shi = tmp();
    S zero = cu(0);
    out.push_back(cat({xn, " = OpUGreaterThan %bool ", x, " ", cu(0x7FFFFFFF)}));
    out.push_back(cat({yn, " = OpUGreaterThan %bool ", y, " ", cu(0x7FFFFFFF)}));
    out.push_back(cat({cx, " = OpSelect %uint ", xn, " ", y, " ", zero}));
    out.push_back(cat({cy, " = OpSelect %uint ", yn, " ", x, " ", zero}));
    out.push_back(cat({d1, " = OpISub %uint ", hi2, " ", cx}));
    out.push_back(cat({d2, " = OpISub %uint ", d1, " ", cy}));
    out.push_back(cat({slo, " = OpBitcast %int ", lo}));
    out.push_back(cat({shi, " = OpBitcast %int ", d2}));
    out.push_back(cat({res, " = OpCompositeConstruct %smul ", slo, " ", shi}));
    return out;
}

void Gen::emit(std::string_view sv) {
    if (d3d()) {
        std::vector<S> m;
        // Logical addressing: a pointer made from an address only records the address; each dword access through it
        // becomes a page-table lookup into the pool's chunk buffers (mem_access).
        if (match(sv, {nullptr, " = OpConvertUToPtr %_ptr_PSB_arr ", nullptr}, m)) {
            psb_base[m[0]] = m[1];
            return;
        }
        if (match(sv, {nullptr, " = OpAccessChain %_ptr_PSB_uint ", nullptr, " ", nullptr}, m)) {
            S base = key(psb_base, m[1]);
            mem_access(m[0], base, m[2]);
            return;
        }
        S s(sv);
        for (size_t at; (at = s.find(" Aligned 4")) != S::npos;) s.erase(at, 10);
        // HLSL has no add-with-carry: SPIRV-Cross would write GLSL's uaddCarry. The sum and a compare instead.
        const bool add = match(s, {nullptr, " = OpIAddCarry %umul ", nullptr, " ", nullptr}, m);
        if (add || match(s, {nullptr, " = OpISubBorrow %umul ", nullptr, " ", nullptr}, m)) {
            const S x = m[0], a = m[1], b = m[2];
            S r = tmp(), lt = tmp(), c = tmp();
            std::vector<S> lines = {cat({r, " = ", add ? "OpIAdd" : "OpISub", " %uint ", a, " ", b}),
                                    cat({lt, " = OpULessThan %bool ", add ? r : a, " ", add ? a : b}),
                                    cat({c, " = OpSelect %uint ", lt, " ", cu(1), " ", cu(0)}),
                                    cat({x, " = OpCompositeConstruct %umul ", r, " ", c})};
            for (auto& l : lines) body.push_back("         " + l);
            return;
        }
        // Nor a widening multiply (GLSL's umulExtended): the high word from 16-bit halves, all in 32 bits.
        S op;
        if (match(s, {nullptr, " = OpUMulExtended %umul ", nullptr, " ", nullptr}, m))
            op = "OpUMulExtended %umul";
        else if (match(s, {nullptr, " = OpSMulExtended %smul ", nullptr, " ", nullptr}, m))
            op = "OpSMulExtended %smul";
        if (!op.empty()) {
            for (auto& l : mul_extended(m[0], op, m[1], m[2])) body.push_back("         " + l);
            return;
        }
        body.push_back("         " + s);
        return;
    }
    body.push_back(cat({"         ", sv}));
}

void Gen::mem_access(const S& result, const S& base, const S& index) {
    // `result` = a StorageBuffer pointer to the dword at console address `base` + 4 * `index`: the page table (one
    // uint per 64 KiB page of R: pool page + 1, 0 unmapped) gives the pool page, the pool is 1 GiB chunk buffers. An
    // unmapped page points into the null chunk (a null descriptor: reads 0, writes dropped).
    std::vector<S> t;
    for (int i = 0; i < 26; ++i) t.push_back(tmp());
    S c0 = cu(0), c1 = cu(1), c2 = cu(2), cnull = cu(NULL_CHUNK);
    std::vector<S> lines = {
        cat({t[0], " = OpUConvert %ulong ", index}),
        cat({t[1], " = OpShiftLeftLogical %ulong ", t[0], " %c64_2"}),
        cat({t[2], " = OpIAdd %ulong ", base, " ", t[1]}),
        cat({t[3], " = OpLoad %uint %ud_base_lo_p"}),
        cat({t[4], " = OpLoad %uint %ud_base_hi_p"}),
        cat({t[5], " = OpUConvert %ulong ", t[3]}),
        cat({t[6], " = OpUConvert %ulong ", t[4]}),
        cat({t[7], " = OpShiftLeftLogical %ulong ", t[6], " %c64_32"}),
        cat({t[8], " = OpBitwiseOr %ulong ", t[7], " ", t[5]}),
        cat({t[9], " = OpISub %ulong ", t[2], " ", t[8]}),
        cat({t[10], " = OpShiftRightLogical %ulong ", t[9], " %c64_16"}),
        cat({t[11], " = OpUConvert %uint ", t[10]}),
        cat({t[12], " = OpAccessChain %_ptr_SB_uint %pagetable ", c0, " ", t[11]}),
        cat({t[13], " = OpLoad %uint ", t[12]}),
        cat({t[14], " = OpISub %uint ", t[13], " ", c1}),
        cat({t[15], " = OpUConvert %ulong ", t[14]}),
        cat({t[16], " = OpShiftLeftLogical %ulong ", t[15], " %c64_16"}),
        cat({t[17], " = OpBitwiseAnd %ulong ", t[2], " %c64_ffff"}),
        cat({t[18], " = OpBitwiseOr %ulong ", t[16], " ", t[17]}),
        cat({t[19], " = OpShiftRightLogical %ulong ", t[18], " %c64_30"}),
        cat({t[20], " = OpUConvert %uint ", t[19]}),
        cat({t[21], " = OpUConvert %uint ", t[18]}),
        cat({t[22], " = OpBitwiseAnd %uint ", t[21], " %c_chunk_mask"}),
        cat({t[23], " = OpShiftRightLogical %uint ", t[22], " ", c2}),
        cat({t[24], " = OpINotEqual %bool ", t[13], " ", c0}),
        cat({t[25], " = OpSelect %uint ", t[24], " ", t[20], " ", cnull}),
        cat({result, " = OpAccessChain %_ptr_SB_uint %chunks ", t[25], " ", c0, " ", t[23]}),
    };
    for (auto& l : lines) body.push_back("         " + l);
    nonuniform.push_back(t[25]);
    nonuniform.push_back(result);
    uses_mem = uses_heap = true;
}

// ---- register access

S Gen::load_reg(std::string_view kind, int64_t r) {
    const char* arr = kind == "vgpr" ? "%vgpr" : "%sgpr";
    S p = tmp(), t = tmp();
    emit({p, " = OpAccessChain %_ptr_Function_uint ", arr, " ", cu(r)});
    emit({t, " = OpLoad %uint ", p});
    return t;
}

void Gen::store_reg(std::string_view kind, int64_t r, const S& val) {
    const char* arr = kind == "vgpr" ? "%vgpr" : "%sgpr";
    S p = tmp();
    emit({p, " = OpAccessChain %_ptr_Function_uint ", arr, " ", cu(r)});
    emit({"OpStore ", p, " ", val});
}

S Gen::src_u(const Arg& a) {
    const S& k = a.k;
    S v;
    if (k == "vgpr" || k == "sgpr") {
        if (a.n != 1) throw Unsupported("64-bit operand");
        v = load_reg(k, a.r);
    } else if (k == "special" && a.t == "null") {
        v = cu(0);  // RDNA 'null' source reads as zero (e.g. soffset)
    } else if (k == "special" && (a.t == "exec_lo" || a.t == "exec" || a.t == "exec_hi")) {
        v = exec_value();
    } else if (k == "special" && a.t == "m0") {
        v = tmp();
        emit({v, " = OpLoad %uint %m0"});
    } else if (k == "special" && a.t == "vcc_hi") {
        // wave32: vcc_hi is an ordinary SGPR (the lane mask lives in vcc_lo).
        v = tmp();
        emit({v, " = OpLoad %uint %vcc_hi"});
    } else if (k == "special" && a.t == "scc") {
        v = tmp();
        emit({v, " = OpLoad %uint %scc"});
    } else if (k == "imm") {
        v = cu(a.v);
    } else if (k == "fimm") {
        v = cu(f32_bits(a.f));
    } else if (k == "special" && (a.t == "vcc_lo" || a.t == "vcc")) {
        v = tmp();
        emit({v, " = OpLoad %uint %vcc"});
    } else {
        throw Unsupported("operand " + (a.t.empty() ? k : a.t));
    }
    if (!a.sdwa_sel.empty()) v = sdwa_src(v, a);
    return modify(v, a);
}

S Gen::modify(S v, const Arg& a) {
    if (a.abs || a.neg) {
        S f = tof(v);
        if (a.abs) {
            S g = tmp();
            emit({g, " = OpExtInst %float %glsl FAbs ", f});
            f = g;
        }
        if (a.neg) {
            S g = tmp();
            emit({g, " = OpFNegate %float ", f});
            f = g;
        }
        v = tou(f);
    }
    return v;
}

void Gen::write_lo16(const Arg& d, const S& lo16, bool preserve_high) {
    // Write a 16-bit result into the low half of VGPR d. On GFX10, f16 VOP1/VOP2 ops and opsel-capable VOP3 ops are
    // partial writes that keep the high half (Mesa ACO aco_ir.cpp: instr_is_16bit / can_use_opsel); non-opsel VOP2
    // 16-bit integer ops define the whole register (high half modelled as zero).
    if (!preserve_high || d.k != "vgpr") {
        dst(d, lo16);
        return;
    }
    S old = tmp(), keep = tmp(), r = tmp();
    emit({old, " = OpCopyObject %uint ", load_reg("vgpr", d.r)});
    emit({keep, " = OpBitwiseAnd %uint ", old, " ", cu(0xFFFF0000)});
    emit({r, " = OpBitwiseOr %uint ", keep, " ", lo16});
    dst(d, r);
}

S Gen::f16_rtz(const S& f) {
    // f32 -> f16 bits (low 16) with round-toward-zero, as v_cvt_pkrtz does: pack to nearest, and if that rounded away
    // from zero (|half| > |x|), step one f16 ulp back toward zero. Inf/NaN inputs keep the nearest result (inf/NaN).
    // Finite values above the f16 range become +-65504, never inf.
    S v2 = tmp(), p = tmp(), lo = tmp(), back2 = tmp(), back = tmp(), ab = tmp(), ax = tmp(), gt = tmp(), fin = tmp(),
      nz = tmp(), dec = tmp(), r = tmp();
    emit({v2, " = OpCompositeConstruct %v2float ", f, " %f0"});
    emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", v2});
    emit({lo, " = OpBitwiseAnd %uint ", p, " ", cu(0xFFFF)});
    emit({back2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", lo});
    emit({back, " = OpCompositeExtract %float ", back2, " 0"});
    emit({ab, " = OpExtInst %float %glsl FAbs ", back});
    emit({ax, " = OpExtInst %float %glsl FAbs ", f});
    emit({gt, " = OpFOrdGreaterThan %bool ", ab, " ", ax});  // false for NaN
    S isinf = tmp(), isnan = tmp();
    emit({isinf, " = OpIsInf %bool ", f});
    emit({isnan, " = OpIsNan %bool ", f});
    S special = tmp();
    emit({special, " = OpLogicalOr %bool ", isinf, " ", isnan});
    emit({fin, " = OpLogicalNot %bool ", special});
    emit({nz, " = OpLogicalAnd %bool ", gt, " ", fin});
    emit({dec, " = OpISub %uint ", lo, " ", cu(1)});  // magnitude bits - 1 = one ulp toward zero
    emit({r, " = OpSelect %uint ", nz, " ", dec, " ", lo});
    return r;
}

S Gen::modify_f(const S& f, const Arg& a) {
    S u = tou(f);
    S m = modify(u, a);
    return tof(m);
}

S Gen::host_addr(const S& xbox_va) {
    // Xbox GPU virtual address (ulong id) -> host buffer device address: heap base (push constant dwords 32..33) + VA.
    // The runtime maps the console's GPU address window into one host buffer. d3d12: the console address itself
    // (mem_access looks it up).
    if (d3d()) {
        uses_heap = true;
        return xbox_va;
    }
    S lo = tmp(), hi = tmp(), l64 = tmp(), h64 = tmp(), sh = tmp(), base = tmp(), r = tmp();
    emit({lo, " = OpLoad %uint %ud_base_lo_p"});
    emit({hi, " = OpLoad %uint %ud_base_hi_p"});
    emit({l64, " = OpUConvert %ulong ", lo});
    emit({h64, " = OpUConvert %ulong ", hi});
    emit({sh, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
    emit({base, " = OpBitwiseOr %ulong ", sh, " ", l64});
    emit({r, " = OpIAdd %ulong ", base, " ", xbox_va});
    uses_heap = true;
    cu(1);  // member indices of the push-constant block, declared before assembly
    cu(2);
    return r;
}

S Gen::tof(const S& u) {
    S f = tmp();
    emit({f, " = OpBitcast %float ", u});
    return f;
}

S Gen::tou(const S& f) {
    S u = tmp();
    emit({u, " = OpBitcast %uint ", f});
    return u;
}

S Gen::src_f(const Arg& a) {
    S u = src_u(a);
    return tof(u);
}

// ---- EXEC (this lane's active bit)

S Gen::active_b() {
    S b = tmp();
    emit({b, " = OpLoad %bool %active"});
    return b;
}

S Gen::exec_value() {
    // EXEC as a lane mask for this invocation: all-ones if active, else 0.
    S r = tmp();
    emit({r, " = OpSelect %uint ", active_b(), " ", cu(0xFFFFFFFF), " ", cu(0)});
    return r;
}

void Gen::set_exec(const S& mask_u) {
    emit({"OpStore %active ", tmp_bool_nz(mask_u)});
    uses_active = true;
}

void Gen::dst(const Arg& a, S val) {
    if (a.sdwa_dst && a.k == "vgpr") val = sdwa_dst(a, val);
    if (a.k == "special" && a.t == "null") return;  // result discarded
    if (a.k == "special" && (a.t == "exec_lo" || a.t == "exec")) {
        set_exec(val);
        return;
    }
    if (a.k == "vgpr" && a.n == 1) {
        // VALU writes only land in active lanes.
        S p = tmp(), old = tmp(), r = tmp();
        emit({p, " = OpAccessChain %_ptr_Function_uint %vgpr ", cu(a.r)});
        emit({old, " = OpLoad %uint ", p});
        emit({r, " = OpSelect %uint ", active_b(), " ", val, " ", old});
        emit({"OpStore ", p, " ", r});
    } else if (a.k == "sgpr" && a.n == 1) {
        store_reg(a.k, a.r, val);
    } else if (a.k == "special" && (a.t == "vcc_lo" || a.t == "vcc")) {
        emit({"OpStore %vcc ", val});
    } else if (a.k == "special" && a.t == "m0") {
        emit({"OpStore %m0 ", val});
    } else if (a.k == "special" && a.t == "vcc_hi") {
        emit({"OpStore %vcc_hi ", val});
    } else if (a.k == "special" && a.t == "scc") {
        emit({"OpStore %scc ", val});
    } else {
        throw Unsupported("destination " + (a.t.empty() ? a.k : a.t));
    }
}

void Gen::mov64(const Arg& d, const Arg& s) {
    // 64-bit scalar move, including EXEC save/restore (EXEC = this lane's active bit).
    if (d.k == "special" && d.t == "exec") {
        S m = mask_src(s);
        set_exec(m);
        return;
    }
    if (d.k == "special" && (d.t == "vcc" || d.t == "vcc_lo")) {
        emit({"OpStore %vcc ", mask_src(s)});
        return;
    }
    if (d.k != "sgpr" || d.n != 2) throw Unsupported("s_mov_b64 destination");
    S lo, hi;
    if (s.k == "special" && s.t == "exec") {
        lo = hi = exec_value();
    } else if (s.k == "sgpr" && s.n == 2) {
        lo = load_reg("sgpr", s.r);
        hi = load_reg("sgpr", s.r + 1);
    } else if (s.k == "imm") {
        uint64_t v = static_cast<uint64_t>(s.v);
        lo = cu(static_cast<int64_t>(v & 0xFFFFFFFF));
        hi = cu(static_cast<int64_t>(v >> 32));
    } else if (s.k == "special" && s.t == "vcc") {
        lo = tmp();
        emit({lo, " = OpLoad %uint %vcc"});
        hi = cu(0);
    } else {
        throw Unsupported("s_mov_b64 source");
    }
    store_reg("sgpr", d.r, lo);
    store_reg("sgpr", d.r + 1, hi);
}

S Gen::tmp_bool_nz(const S& v) {
    S b = tmp();
    emit({b, " = OpINotEqual %bool ", v, " ", cu(0)});
    return b;
}

S Gen::mask_src(const Arg& a) {
    // A lane mask operand (sgpr pair, vcc, exec, imm) as one uint for this lane.
    if (a.k == "sgpr") return load_reg("sgpr", a.r);
    if (a.k == "special" && (a.t == "vcc" || a.t == "vcc_lo")) {
        S v = tmp();
        emit({v, " = OpLoad %uint %vcc"});
        return v;
    }
    if (a.k == "special" && (a.t == "exec" || a.t == "exec_lo")) return exec_value();
    if (a.k == "imm") return cu((a.v & 1) ? 0xFFFFFFFF : 0);
    throw Unsupported("mask operand");
}

void Gen::mask_dst(const Arg& d, const S& v) {
    if (d.k == "special" && (d.t == "exec" || d.t == "exec_lo")) {
        set_exec(v);
    } else if (d.k == "sgpr") {
        store_reg("sgpr", d.r, v);
        if (d.n == 2) store_reg("sgpr", d.r + 1, v);
    } else if (d.k == "special" && (d.t == "vcc" || d.t == "vcc_lo")) {
        emit({"OpStore %vcc ", v});
    } else {
        throw Unsupported("mask destination");
    }
}

S Gen::clamp_mods(S v, const Mods& mods) {
    // Output modifier (mul:2 / mul:4 / div:2) is applied before clamp, as on hardware.
    for (const auto& m : mods) {
        if (m == "mul:2" || m == "mul:4" || m == "div:2") {
            double k = m == "mul:2" ? 2.0 : m == "mul:4" ? 4.0 : 0.5;
            S c = fconst(k);
            S g = tmp();
            emit({g, " = OpFMul %float ", v, " ", c});
            v = g;
        }
    }
    if (has(mods, "clamp")) {
        S g = tmp();
        emit({g, " = OpExtInst %float %glsl FClamp ", v, " %f0 %f1"});
        return g;
    }
    return v;
}

S Gen::fconst(double x) { return tof(cu(f32_bits(x))); }

S Gen::as_int(const S& u) {
    S i = tmp();
    emit({i, " = OpBitcast %int ", u});
    return i;
}

S Gen::tmp_and3(const S& v) {
    S r = tmp();
    emit({r, " = OpBitwiseAnd %uint ", v, " ", cu(3)});
    return r;
}

S Gen::current_label_placeholder() {
    // Label of the block currently being emitted (needed for OpPhi). Blocks are opened by OpLabel lines.
    for (auto it = body.rbegin(); it != body.rend(); ++it) {
        std::string_view s = *it;
        size_t b = s.find_first_not_of(" \t\r\n\v\f");
        size_t e = s.find_last_not_of(" \t\r\n\v\f");
        s = b == std::string_view::npos ? std::string_view{} : s.substr(b, e - b + 1);
        if (ends(s, "= OpLabel")) return S(s.substr(0, s.find(" = ")));
    }
    return block_label;
}

void Gen::guarded(const S& cond, const std::function<void()>& fn) {
    // Emit fn() only where cond holds (a structured selection).
    S t = tmp();
    S do_ = t + "_do", end = t + "_end";
    emit({"OpSelectionMerge ", end, " None"});
    emit({"OpBranchConditional ", cond, " ", do_, " ", end});
    emit({do_, " = OpLabel"});
    fn();
    emit({"OpBranch ", end});
    emit({end, " = OpLabel"});
}

// ---- instructions

void Gen::insn(const Ins& ins) {
    // SDWA (sub-dword addressing): each source is narrowed to src<i>_sel (zero-extended, or sign-extended under
    // sext()) before the op and its neg/abs, and a VGPR result is placed at dst_sel with dst_unused deciding the
    // other bits (PAD = zero, SEXT = sign-extend, PRESERVE = keep the old bits). VOPC has no dst_sel; v_cmpx (GFX10)
    // has no written destination operand.
    if (!ends(ins.op, "_sdwa")) {
        insn_(ins);
        return;
    }
    const S op = base_op(ins.op);
    std::map<S, S> kv;
    for (const auto& m : ins.mods) {
        size_t c = m.find(':');
        S k = m.substr(0, c);
        if (k == "src0_sel" || k == "src1_sel" || k == "dst_sel" || k == "dst_unused") {
            if (c == S::npos) throw PyError("ValueError");  // dict() of a one-element split
            kv[k] = m.substr(c + 1);
        }
    }
    auto get = [&](const char* k, const char* d) -> S {
        auto it = kv.find(k);
        return it == kv.end() ? S(d) : it->second;
    };
    Ins copy = ins;
    Args& args = copy.args;
    const size_t first = starts(op, "v_cmpx") ? 0 : 1;
    const char* keys[2] = {"src0_sel", "src1_sel"};
    for (size_t i = 0; i < 2; ++i) {
        size_t j = first + i;
        if (j < args.size() && (get(keys[i], "DWORD") != "DWORD" || args[j].sext)) args[j].sdwa_sel = get(keys[i], "DWORD");
    }
    if (!starts(op, "v_cmp") && !args.empty() && get("dst_sel", "DWORD") != "DWORD") {
        args[0].sdwa_dst = true;
        args[0].sdwa_dst_sel = kv["dst_sel"];
        args[0].sdwa_dst_unused = get("dst_unused", "UNUSED_PAD");
    }
    insn_(copy);
}

S Gen::sdwa_src(const S& v, const Arg& a) {
    auto [off, width] = key(sdwa_sel(), a.sdwa_sel);
    if (width == 32) return v;
    S r = tmp();
    const char* f = a.sext ? "OpBitFieldSExtract" : "OpBitFieldUExtract";
    emit({r, " = ", f, " %uint ", v, " ", cu(off), " ", cu(width)});
    return r;
}

S Gen::sdwa_dst(const Arg& d, const S& val) {
    const S& unused = d.sdwa_dst_unused;
    auto [off, width] = key(sdwa_sel(), d.sdwa_dst_sel);
    if (width == 32) return val;
    S r = tmp();
    if (unused == "UNUSED_PRESERVE") {
        emit({r, " = OpBitFieldInsert %uint ", load_reg("vgpr", d.r), " ", val, " ", cu(off), " ", cu(width)});
        return r;
    }
    S lo = tmp();
    const char* ext = unused == "UNUSED_SEXT" ? "OpBitFieldSExtract" : "OpBitFieldUExtract";
    emit({lo, " = ", ext, " %uint ", val, " ", cu(0), " ", cu(width)});
    // SEXT: the sign-extended field shifted up leaves sign copies above it; bits below stay zero.
    emit({r, " = OpShiftLeftLogical %uint ", lo, " ", cu(off)});
    return r;
}

}  // namespace fp::xlat
