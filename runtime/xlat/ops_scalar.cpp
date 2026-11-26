// Scalar (SALU) instructions: 16-bit immediates, bit operations, lane-mask logic and EXEC save/restore, 64-bit
// compares and shifts, carries through SCC, and the program counter (rdna2spirv.Gen._insn's branches).
#include <map>

#include "ops.h"

namespace fp::xlat::ops {
namespace {

const std::map<S, S> kAddI32 = {{"s_add_i32", "OpIAdd"}, {"s_sub_i32", "OpISub"}, {"s_sub_u32", "OpISub"},
                                {"s_xor_b32", "OpBitwiseXor"}};
const std::map<S, S> kLogic = {{"and", "OpBitwiseAnd"}, {"or", "OpBitwiseOr"}, {"xor", "OpBitwiseXor"},
                               {"andn2", "OpBitwiseAnd"}, {"orn2", "OpBitwiseOr"}};

// A 16-bit immediate, sign-extended (SOPK).
int64_t simm16(const Arg& a) {
    int64_t v = imm_v(a) & 0xFFFF;
    return (v & 0x8000) ? v - 0x10000 : v;
}

// The two halves of a 64-bit scalar operand: an SGPR pair, a 64-bit immediate, or (else) a lane mask for both.
std::pair<S, S> halves64(Gen& g, const Arg& s) {
    if (s.k == "sgpr" && s.n == 2) {
        S l = g.load_reg("sgpr", s.r);
        S h = g.load_reg("sgpr", s.r + 1);
        return {l, h};
    }
    if (s.k == "imm") {
        uint64_t v = static_cast<uint64_t>(s.v);
        S l = g.cu(static_cast<int64_t>(v & 0xFFFFFFFF));
        S h = g.cu(static_cast<int64_t>(v >> 32));
        return {l, h};
    }
    S m = g.mask_src(s);
    return {m, m};
}

}  // namespace

void mulk(const Op& o) {
    Gen& g = o.g;
    int64_t k = simm16(at(o.a, 1));
    S x = g.load_reg("sgpr", reg(at(o.a, 0)));
    S r = g.tmp();
    g.emit({r, " = OpIMul %uint ", x, " ", g.cu(k)});
    g.dst(at(o.a, 0), r);
}

void addk(const Op& o) {
    Gen& g = o.g;
    int64_t k = simm16(at(o.a, 1));
    S x = g.load_reg("sgpr", reg(at(o.a, 0)));
    S r = g.tmp();
    g.emit({r, " = OpIAdd %uint ", x, " ", g.cu(k)});
    g.dst(at(o.a, 0), r);
}

void bcnt1(const Op& o) {
    Gen& g = o.g;
    const Arg& s = at(o.a, 1);
    S lo, hi;
    if (ends(o.op, "b64") && s.k == "sgpr" && s.n == 2) {
        lo = g.load_reg("sgpr", s.r);
        hi = g.load_reg("sgpr", s.r + 1);
    } else if (ends(o.op, "b64")) {
        lo = g.mask_src(s);
        hi = g.cu(0);
    } else {
        lo = g.src_u(s);
        hi = g.cu(0);
    }
    S c1 = g.tmp(), c2 = g.tmp(), r = g.tmp();
    g.emit({c1, " = OpBitCount %uint ", lo});
    g.emit({c2, " = OpBitCount %uint ", hi});
    g.emit({r, " = OpIAdd %uint ", c1, " ", c2});
    g.dst(at(o.a, 0), r);
    scc_nz(g, r);
}

void ff1_b64(const Op& o) {
    // Lowest set bit of a 64-bit mask, -1 if none.
    Gen& g = o.g;
    const Arg& s = at(o.a, 1);
    S lo, hi;
    if (s.k != "sgpr" || s.n != 2) {
        lo = g.mask_src(s);
        hi = g.cu(0);
    } else {
        lo = g.load_reg("sgpr", s.r);
        hi = g.load_reg("sgpr", s.r + 1);
    }
    auto t = tmps(g, 5);
    const S &l = t[0], &h = t[1], &hp = t[2], &lz = t[3], &r = t[4];
    g.emit({l, " = OpExtInst %int %glsl FindILsb ", g.as_int(lo)});
    g.emit({h, " = OpExtInst %int %glsl FindILsb ", g.as_int(hi)});
    S hu = g.tmp();
    g.emit({hu, " = OpBitcast %uint ", h});
    S hz = g.tmp();
    g.emit({hz, " = OpIEqual %bool ", hi, " ", g.cu(0)});
    g.emit({hp, " = OpIAdd %uint ", hu, " ", g.cu(32)});
    S hsel = g.tmp();
    g.emit({hsel, " = OpSelect %uint ", hz, " ", g.cu(0xFFFFFFFF), " ", hp});
    g.emit({lz, " = OpIEqual %bool ", lo, " ", g.cu(0)});
    S lu = g.tmp();
    g.emit({lu, " = OpBitcast %uint ", l});
    g.emit({r, " = OpSelect %uint ", lz, " ", hsel, " ", lu});
    g.dst(at(o.a, 0), r);
}

void bitset1(const Op& o) {
    // sD |= 1 << (src & 31)   (sD is both source and destination)
    Gen& g = o.g;
    S b = g.src_u(at(o.a, 1));
    auto t = tmps(g, 4);
    const S &m = t[0], &sh = t[1], &old = t[2], &r = t[3];
    g.emit({m, " = OpBitwiseAnd %uint ", b, " ", g.cu(31)});
    g.emit({sh, " = OpShiftLeftLogical %uint ", g.cu(1), " ", m});
    g.emit({old, " = OpCopyObject %uint ", g.load_reg("sgpr", reg(at(o.a, 0)))});
    g.emit({r, " = OpBitwiseOr %uint ", old, " ", sh});
    g.dst(at(o.a, 0), r);
}

void bitset0(const Op& o) {
    Gen& g = o.g;
    S b = g.src_u(at(o.a, 1));
    auto t = tmps(g, 4);
    const S &m = t[0], &sh = t[1], &n = t[2], &r = t[3];
    g.emit({m, " = OpBitwiseAnd %uint ", b, " ", g.cu(31)});
    g.emit({sh, " = OpShiftLeftLogical %uint ", g.cu(1), " ", m});
    g.emit({n, " = OpNot %uint ", sh});
    g.emit({r, " = OpBitwiseAnd %uint ", g.load_reg("sgpr", reg(at(o.a, 0))), " ", n});
    g.dst(at(o.a, 0), r);
}

void min_max_i32(const Op& o) {
    Gen& g = o.g;
    const bool min = contains(o.op, "min");
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    S r = g.tmp();
    g.emit({r, " = OpExtInst %uint %glsl ", min ? "SMin" : "SMax", " ", x, " ", y});
    g.dst(at(o.a, 0), r);
    // SCC = (src0 selected) per ISA; select-src0 iff src0 <= src1 (min) / >= (max)
    S c = g.tmp(), z = g.tmp();
    S xi = g.as_int(x);
    S yi = g.as_int(y);
    g.emit({c, " = ", min ? "OpSLessThanEqual" : "OpSGreaterThanEqual", " %bool ", xi, " ", yi});
    g.emit({z, " = OpSelect %uint ", c, " ", g.cu(1), " ", g.cu(0)});
    g.emit({"OpStore %scc ", z});
}

void not_b64(const Op& o) {
    Gen& g = o.g;
    S v = g.mask_src(at(o.a, 1));
    S r = g.tmp();
    g.emit({r, " = OpNot %uint ", v});
    g.mask_dst(at(o.a, 0), r);
    scc_nz(g, r);
}

void s_not_b32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S r = g.tmp();
    g.emit({r, " = OpNot %uint ", x});
    g.dst(at(o.a, 0), r);
    scc_nz(g, r);
}

void cmp_u64(const Op& o) {
    // 64-bit compare of masks/pointers: equal iff both halves are equal.
    Gen& g = o.g;
    auto [x0, x1] = halves64(g, at(o.a, 0));
    auto [y0, y1] = halves64(g, at(o.a, 1));
    auto t = tmps(g, 4);
    const S &e0 = t[0], &e1 = t[1], &r = t[3];
    S eq = t[2];
    g.emit({e0, " = OpIEqual %bool ", x0, " ", y0});
    g.emit({e1, " = OpIEqual %bool ", x1, " ", y1});
    g.emit({eq, " = OpLogicalAnd %bool ", e0, " ", e1});
    if (o.op == "s_cmp_lg_u64") {
        S ne = g.tmp();
        g.emit({ne, " = OpLogicalNot %bool ", eq});
        eq = ne;
    }
    g.emit({r, " = OpSelect %uint ", eq, " ", g.cu(1), " ", g.cu(0)});
    g.emit({"OpStore %scc ", r});
}

void shift_b64(const Op& o) {
    // 64-bit shift of an SGPR pair by (src1 & 63); SCC = result != 0.
    Gen& g = o.g;
    const Arg& d = at(o.a, 0);
    const Arg& x = at(o.a, 1);
    if (d.k != "sgpr" || d.n != 2) throw Unsupported(o.op + " destination");
    S lo, hi;
    if (x.k == "sgpr" && x.n == 2) {
        lo = g.load_reg("sgpr", x.r);
        hi = g.load_reg("sgpr", x.r + 1);
    } else if (x.k == "imm") {
        uint64_t v = static_cast<uint64_t>(x.v);
        lo = g.cu(static_cast<int64_t>(v & 0xFFFFFFFF));
        hi = g.cu(static_cast<int64_t>(v >> 32));
    } else {
        throw Unsupported(o.op + " source");
    }
    S sh = g.src_u(at(o.a, 2));
    auto t = tmps(g, 10);
    const S &l64 = t[0], &h64 = t[1], &tt = t[2], &v64 = t[3], &m = t[4], &s64 = t[5], &r = t[6], &rlo = t[7],
            &rh = t[8], &rhi = t[9];
    g.emit({l64, " = OpUConvert %ulong ", lo});
    g.emit({h64, " = OpUConvert %ulong ", hi});
    g.emit({tt, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
    g.emit({v64, " = OpBitwiseOr %ulong ", tt, " ", l64});
    g.emit({m, " = OpBitwiseAnd %uint ", sh, " ", g.cu(63)});
    g.emit({s64, " = OpUConvert %ulong ", m});
    g.emit({r, " = ", o.op == "s_lshl_b64" ? "OpShiftLeftLogical" : "OpShiftRightLogical", " %ulong ", v64, " ", s64});
    g.emit({rlo, " = OpUConvert %uint ", r});
    g.emit({rh, " = OpShiftRightLogical %ulong ", r, " %c64_32"});
    g.emit({rhi, " = OpUConvert %uint ", rh});
    g.store_reg("sgpr", d.r, rlo);
    g.store_reg("sgpr", d.r + 1, rhi);
    S nz = g.tmp(), z = g.tmp();  // nz: unused, kept for the numbering
    (void)nz;
    S orr = g.tmp();
    g.emit({orr, " = OpBitwiseOr %uint ", rlo, " ", rhi});
    g.emit({z, " = OpSelect %uint ", g.tmp_bool_nz(orr), " ", g.cu(1), " ", g.cu(0)});
    g.emit({"OpStore %scc ", z});
    g.uses_psb = true;  // Int64 capability and %ulong come with the PSB declarations
}

void ashr_i32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S sh = g.src_u(at(o.a, 2));
    auto t = tmps(g, 4);
    const S &m = t[0], &xi = t[1], &ri = t[2], &r = t[3];
    g.emit({m, " = OpBitwiseAnd %uint ", sh, " ", g.cu(31)});
    g.emit({xi, " = OpBitcast %int ", x});
    g.emit({ri, " = OpShiftRightArithmetic %int ", xi, " ", m});
    g.emit({r, " = OpBitcast %uint ", ri});
    g.dst(at(o.a, 0), r);
    scc_nz(g, r);
}

void add_carry(const Op& o) {
    // 32-bit add/sub with carry/borrow through SCC (used for 64-bit address arithmetic, e.g. s_getpc + offset).
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    const bool sub = starts(o.op, "s_sub");
    const char* kind = sub ? "OpISubBorrow" : "OpIAddCarry";
    S s1 = g.tmp(), r1 = g.tmp(), c1 = g.tmp();
    g.emit({s1, " = ", kind, " %umul ", x, " ", y});
    g.emit({r1, " = OpCompositeExtract %uint ", s1, " 0"});
    g.emit({c1, " = OpCompositeExtract %uint ", s1, " 1"});
    S r = r1, carry = c1;
    if (o.op == "s_addc_u32" || o.op == "s_subb_u32") {
        auto t = tmps(g, 5);
        const S &cin = t[0], &s2 = t[1], &r2 = t[2], &c2 = t[3], &cc = t[4];
        g.emit({cin, " = OpLoad %uint %scc"});
        g.emit({s2, " = ", kind, " %umul ", r1, " ", cin});
        g.emit({r2, " = OpCompositeExtract %uint ", s2, " 0"});
        g.emit({c2, " = OpCompositeExtract %uint ", s2, " 1"});
        g.emit({cc, " = OpBitwiseOr %uint ", c1, " ", c2});
        r = r2;
        carry = cc;
    }
    g.dst(at(o.a, 0), r);
    g.emit({"OpStore %scc ", carry});
}

void getpc(const Op& o) {
    // Address of the next instruction: code base (push constants 34-35: where the runtime put this shader's FSSM
    // blob in the Xbox VA window) + offset of the next instruction within the blob.
    Gen& g = o.g;
    const Arg& d = at(o.a, 0);
    if (d.k != "sgpr" || d.n != 2) throw Unsupported("s_getpc_b64 destination");
    const int64_t nxt = o.ins.next_pc();
    auto t = tmps(g, 6);
    const S &lo = t[0], &hi = t[1], &s = t[2], &rlo = t[3], &c = t[4], &rhi = t[5];
    g.emit({lo, " = OpLoad %uint %ud_code_lo_p"});
    g.emit({hi, " = OpLoad %uint %ud_code_hi_p"});
    g.emit({s, " = OpIAddCarry %umul ", lo, " ", g.cu(nxt)});
    g.emit({rlo, " = OpCompositeExtract %uint ", s, " 0"});
    g.emit({c, " = OpCompositeExtract %uint ", s, " 1"});
    g.emit({rhi, " = OpIAdd %uint ", hi, " ", c});
    g.store_reg("sgpr", d.r, rlo);
    g.store_reg("sgpr", d.r + 1, rhi);
    g.uses_codebase = true;
    g.cu(3);
    g.cu(4);
}

void add_i32(const Op& o) {
    Gen& g = o.g;
    const S& op = o.op;
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    S r = g.tmp();
    if (op == "s_cselect_b32") {
        S c = g.tmp(), b = g.tmp();
        g.emit({c, " = OpLoad %uint %scc"});
        g.emit({b, " = OpINotEqual %bool ", c, " ", g.cu(0)});
        g.emit({r, " = OpSelect %uint ", b, " ", x, " ", y});
    } else if (op == "s_andn2_b32") {
        S n = g.tmp();
        g.emit({n, " = OpNot %uint ", y});
        g.emit({r, " = OpBitwiseAnd %uint ", x, " ", n});
    } else if (op == "s_min_u32" || op == "s_max_u32") {
        g.emit({r, " = OpExtInst %uint %glsl ", contains(op, "min") ? "UMin" : "UMax", " ", x, " ", y});
    } else {
        g.emit({r, " = ", key(kAddI32, op), " %uint ", x, " ", y});
    }
    g.dst(at(o.a, 0), r);
}

void wqm(const Op& o) {
    // Whole-quad mode: a lane becomes active if any lane of its 2x2 quad is. One lane per invocation can't see its
    // neighbours, so this is the identity (APPROXIMATION: helper lanes stay as they were).
    Gen& g = o.g;
    S v = g.mask_src(at(o.a, 1));
    g.mask_dst(at(o.a, 0), v);
    scc_nz(g, v);
}

void movk(const Op& o) {
    Gen& g = o.g;
    const Arg& d = at(o.a, 0);
    int64_t v = simm16(at(o.a, 1));
    g.dst(d, g.cu(v));
}

void saveexec(const Op& o) {
    Gen& g = o.g;
    S saved = g.exec_value();
    S src = g.mask_src(at(o.a, 1));
    S r = g.tmp();
    S kind = o.op.substr(2, o.op.find("_saveexec") == S::npos ? S::npos : o.op.find("_saveexec") - 2);
    if (kind == "andn2") {
        S n = g.tmp();
        g.emit({n, " = OpNot %uint ", saved});
        g.emit({r, " = OpBitwiseAnd %uint ", src, " ", n});
    } else {
        g.emit({r, " = ", kind == "and" ? "OpBitwiseAnd" : "OpBitwiseOr", " %uint ", src, " ", saved});
    }
    g.mask_dst(at(o.a, 0), saved);
    g.set_exec(r);
    scc_nz(g, r);
}

void wrexec(const Op& o) {
    // RDNA ISA: EXEC = S0 & ~EXEC; D = EXEC; SCC = (EXEC != 0). Two operands: sdst, ssrc0.
    Gen& g = o.g;
    S x = g.mask_src(at(o.a, 1));
    S y = g.exec_value();
    S n = g.tmp(), r = g.tmp();
    g.emit({n, " = OpNot %uint ", y});
    g.emit({r, " = OpBitwiseAnd %uint ", x, " ", n});
    g.set_exec(r);
    g.mask_dst(at(o.a, 0), r);
    scc_nz(g, r);
}

void bfe_s(const Op& o) {
    // src1 packs offset in bits [4:0] and width in bits [22:16].
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S ctl = g.src_u(at(o.a, 2));
    S off = g.tmp(), wid = g.tmp(), r = g.tmp();
    g.emit({off, " = OpBitwiseAnd %uint ", ctl, " ", g.cu(31)});
    g.emit({wid, " = OpBitFieldUExtract %uint ", ctl, " ", g.cu(16), " ", g.cu(7)});
    if (o.op == "s_bfe_u32") {
        g.emit({r, " = OpBitFieldUExtract %uint ", x, " ", off, " ", wid});
    } else {
        S xi = g.as_int(x);
        S ri = g.tmp();
        g.emit({ri, " = OpBitFieldSExtract %int ", xi, " ", off, " ", wid});
        g.emit({r, " = OpBitcast %uint ", ri});
    }
    g.dst(at(o.a, 0), r);
    scc_nz(g, r);
}

void logic_b64(const Op& o) {
    Gen& g = o.g;
    S x = g.mask_src(at(o.a, 1));
    S y = g.mask_src(at(o.a, 2));
    S r = g.tmp();
    if (ends(o.op, "n2_b64")) {
        S n = g.tmp();
        g.emit({n, " = OpNot %uint ", y});
        y = n;
    }
    // op[2:].rsplit("_", 1)[0]: "s_andn2_b64" -> "andn2"
    S rest = o.op.substr(2);
    size_t us = rest.rfind('_');
    const S& k = key(kLogic, us == S::npos ? rest : rest.substr(0, us));
    g.emit({r, " = ", k, " %uint ", x, " ", y});
    g.mask_dst(at(o.a, 0), r);
    scc_nz(g, r);
}

void cselect_b64(const Op& o) {
    Gen& g = o.g;
    S x = g.mask_src(at(o.a, 1));
    S y = g.mask_src(at(o.a, 2));
    S c = g.tmp(), b = g.tmp(), r = g.tmp();
    g.emit({c, " = OpLoad %uint %scc"});
    g.emit({b, " = OpINotEqual %bool ", c, " ", g.cu(0)});
    g.emit({r, " = OpSelect %uint ", b, " ", x, " ", y});
    g.mask_dst(at(o.a, 0), r);
}

void cmpk(const Op& o) {
    // s_cmpk_<cond>_<ty> sX, imm16: zero-extended for u32, sign-extended for i32.
    const S& op = o.op;
    S ty = op.substr(op.rfind('_') + 1);
    int64_t v = imm_v(at(o.a, 1)) & 0xFFFF;
    if (ty == "i32" && (v & 0x8000)) v -= 0x10000;
    o.g.compare("s_cmp_" + op.substr(7), {at(o.a, 0), Gen::imm(v)}, true);
}

}  // namespace fp::xlat::ops
