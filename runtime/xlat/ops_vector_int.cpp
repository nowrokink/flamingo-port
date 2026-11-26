// Vector integer and bit instructions: three-source integer ALU, bit fields and counts, carries, 64-bit shifts and
// compares, multiplies, byte permutes, selects and relative register indexing (rdna2spirv.Gen._insn's branches).
// Some scalar forms that share a body (s_brev_b32, s_ff1_i32_b32, s_flbit_i32_b32, s_bfm_b32, s_mul_hi_u32 and the
// S2 table) are here too.
#include <map>

#include "ops.h"

namespace fp::xlat::ops {
namespace {

// two-source integer ops -> SPIR-V op, "mul24" for the 24-bit multiply (rdna2spirv.U2, then S2)
const std::map<S, S> kU2 = {{"v_add_nc_u32", "OpIAdd"},     {"v_sub_nc_u32", "OpISub"},
                            {"v_and_b32", "OpBitwiseAnd"},  {"v_or_b32", "OpBitwiseOr"},
                            {"v_xor_b32", "OpBitwiseXor"},  {"v_mul_u32_u24", "mul24"}};
const std::map<S, S> kS2 = {{"s_and_b32", "OpBitwiseAnd"},       {"s_or_b32", "OpBitwiseOr"},
                            {"s_lshl_b32", "OpShiftLeftLogical"}, {"s_lshr_b32", "OpShiftRightLogical"},
                            {"s_mul_i32", "OpIMul"}};
const std::map<S, S> kShiftRev = {{"v_lshlrev_b32", "OpShiftLeftLogical"},
                                  {"v_lshrrev_b32", "OpShiftRightLogical"},
                                  {"v_ashrrev_i32", "OpShiftRightArithmetic"}};
const std::map<S, S> kThree = {{"v_or3_b32", "OpBitwiseOr"}, {"v_xor3_b32", "OpBitwiseXor"}, {"v_add3_u32", "OpIAdd"}};
const std::map<S, S> kMinMax = {{"v_min_u32", "UMin"}, {"v_max_u32", "UMax"}, {"v_min_i32", "SMin"},
                                {"v_max_i32", "SMax"}};

}  // namespace

bool is_binary_u32(const S& op) { return kU2.count(op) || kS2.count(op); }
bool is_shift_rev(const S& op) { return kShiftRev.count(op) != 0; }

void and_or_b32(const Op& o) {
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    S m = g.tmp(), r = g.tmp();
    g.emit({m, " = OpBitwiseAnd %uint ", v[0], " ", v[1]});
    g.emit({r, " = OpBitwiseOr %uint ", m, " ", v[2]});
    g.dst(at(o.a, 0), r);
}

void three_int(const Op& o) {
    Gen& g = o.g;
    const S& op = o.op;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    const S &x = v[0], &y = v[1], &z = v[2];
    S r = g.tmp();
    if (op == "v_bfi_b32") {
        S p = g.tmp(), n = g.tmp(), q = g.tmp();
        g.emit({p, " = OpBitwiseAnd %uint ", x, " ", y});
        g.emit({n, " = OpNot %uint ", x});
        g.emit({q, " = OpBitwiseAnd %uint ", n, " ", z});
        g.emit({r, " = OpBitwiseOr %uint ", p, " ", q});
    } else if (op == "v_lshl_or_b32" || op == "v_lshl_add_u32") {
        S s = g.tmp(), m = g.tmp();
        g.emit({m, " = OpBitwiseAnd %uint ", y, " ", g.cu(31)});
        g.emit({s, " = OpShiftLeftLogical %uint ", x, " ", m});
        g.emit({r, " = ", op == "v_lshl_or_b32" ? "OpBitwiseOr" : "OpIAdd", " %uint ", s, " ", z});
    } else if (op == "v_add_lshl_u32") {
        S s = g.tmp(), m = g.tmp();
        g.emit({s, " = OpIAdd %uint ", x, " ", y});
        g.emit({m, " = OpBitwiseAnd %uint ", z, " ", g.cu(31)});
        g.emit({r, " = OpShiftLeftLogical %uint ", s, " ", m});
    } else {
        const S& k = key(kThree, op);
        S s = g.tmp();
        g.emit({s, " = ", k, " %uint ", x, " ", y});
        g.emit({r, " = ", k, " %uint ", s, " ", z});
    }
    g.dst(at(o.a, 0), r);
}

void find_bit(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S r = g.tmp();
    if (o.op == "v_ffbl_b32" || o.op == "s_ff1_i32_b32") {
        // Index of lowest set bit, -1 (0xFFFFFFFF) if none: FindILsb has the same convention.
        S ri = g.tmp();
        g.emit({ri, " = OpExtInst %int %glsl FindILsb ", g.as_int(x)});
        g.emit({r, " = OpBitcast %uint ", ri});
    } else {
        // Leading-zero count from the MSB side, -1 if zero: 31 - FindUMsb(x), with FindUMsb(0) = -1.
        S m = g.tmp(), z = g.tmp(), d = g.tmp();
        g.emit({m, " = OpExtInst %int %glsl FindUMsb ", x});
        S mu = g.tmp();
        g.emit({mu, " = OpBitcast %uint ", m});
        g.emit({d, " = OpISub %uint ", g.cu(31), " ", mu});
        g.emit({z, " = OpIEqual %bool ", x, " ", g.cu(0)});
        g.emit({r, " = OpSelect %uint ", z, " ", g.cu(0xFFFFFFFF), " ", d});
    }
    g.dst(at(o.a, 0), r);
}

void bit_reverse(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S r = g.tmp();
    g.emit({r, " = OpBitReverse %uint ", x});
    g.dst(at(o.a, 0), r);
}

void bfm(const Op& o) {
    // Bitfield mask: ((1 << width) - 1) << offset, width/offset from the low 5 bits of the sources.
    Gen& g = o.g;
    S w = g.src_u(at(o.a, 1));
    S off = g.src_u(at(o.a, 2));
    auto t = tmps(g, 5);
    const S &wm = t[0], &om = t[1], &one = t[2], &m1 = t[3], &r = t[4];
    g.emit({wm, " = OpBitwiseAnd %uint ", w, " ", g.cu(31)});
    g.emit({om, " = OpBitwiseAnd %uint ", off, " ", g.cu(31)});
    g.emit({one, " = OpShiftLeftLogical %uint ", g.cu(1), " ", wm});
    g.emit({m1, " = OpISub %uint ", one, " ", g.cu(1)});
    g.emit({r, " = OpShiftLeftLogical %uint ", m1, " ", om});
    g.dst(at(o.a, 0), r);
}

void subrev_nc_u32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    S r = g.tmp();
    g.emit({r, " = OpISub %uint ", y, " ", x});
    g.dst(at(o.a, 0), r);
}

void alignbit(const Op& o) {
    // (({src0, src1} as 64 bits) >> (src2 & 31)) low 32 bits
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    const S &hi_ = v[0], &lo_ = v[1], &sh = v[2];
    auto t = tmps(g, 7);
    const S &m = t[0], &inv = t[1], &a1 = t[2], &b1 = t[3], &nz = t[4], &r0 = t[5], &r = t[6];
    g.emit({m, " = OpBitwiseAnd %uint ", sh, " ", g.cu(31)});
    g.emit({inv, " = OpISub %uint ", g.cu(32), " ", m});
    g.emit({a1, " = OpShiftRightLogical %uint ", lo_, " ", m});
    g.emit({b1, " = OpShiftLeftLogical %uint ", hi_, " ", inv});
    g.emit({r0, " = OpBitwiseOr %uint ", a1, " ", b1});
    g.emit({nz, " = OpINotEqual %bool ", m, " ", g.cu(0)});
    g.emit({r, " = OpSelect %uint ", nz, " ", r0, " ", lo_});
    g.dst(at(o.a, 0), r);
}

void lshlrev_b64(const Op& o) {
    // v[d:d+1] = v[s:s+1] << (src0 & 63)
    Gen& g = o.g;
    const Arg& sh = at(o.a, 1);
    const Arg& src = at(o.a, 2);
    if (at(o.a, 0).n != 2) throw Unsupported(o.op + " operands");
    S lo, hi;
    if ((src.k == "vgpr" || src.k == "sgpr") && src.n == 2) {
        lo = g.load_reg(src.k, src.r);
        hi = g.load_reg(src.k, src.r + 1);
    } else if (src.k == "imm") {
        // 64-bit inline/literal constant: sign-extended from 32 bits.
        uint64_t v = static_cast<uint64_t>(src.v);
        lo = g.cu(static_cast<int64_t>(v & 0xFFFFFFFF));
        hi = g.cu(static_cast<int64_t>((v >> 32) & 0xFFFFFFFF));
    } else {
        throw Unsupported(o.op + " operands");
    }
    S s = g.src_u(sh);
    auto t = tmps(g, 9);
    const S &l64 = t[0], &h64 = t[1], &x = t[2], &sh64 = t[3], &m = t[4], &res = t[5], &rlo = t[6], &rhi = t[7],
            &tt = t[8];
    g.emit({l64, " = OpUConvert %ulong ", lo});
    g.emit({h64, " = OpUConvert %ulong ", hi});
    g.emit({tt, " = OpShiftLeftLogical %ulong ", h64, " %c64_32"});
    g.emit({x, " = OpBitwiseOr %ulong ", tt, " ", l64});
    g.emit({m, " = OpBitwiseAnd %uint ", s, " ", g.cu(63)});
    g.emit({sh64, " = OpUConvert %ulong ", m});
    g.emit({res, " = OpShiftLeftLogical %ulong ", x, " ", sh64});
    g.emit({rlo, " = OpUConvert %uint ", res});
    S hs = g.tmp();
    g.emit({hs, " = OpShiftRightLogical %ulong ", res, " %c64_32"});
    g.emit({rhi, " = OpUConvert %uint ", hs});
    g.uses_psb = true;  // needs Int64 (declared with the PSB types)
    const int64_t d = reg(at(o.a, 0));
    g.dst(Gen::vreg(d), rlo);
    g.dst(Gen::vreg(d + 1), rhi);
}

void bcnt_u32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S acc = g.src_u(at(o.a, 2));
    S c = g.tmp(), r = g.tmp();
    g.emit({c, " = OpBitCount %uint ", x});
    g.emit({r, " = OpIAdd %uint ", c, " ", acc});
    g.dst(at(o.a, 0), r);
}

void xad_u32(const Op& o) {
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    S t = g.tmp(), r = g.tmp();
    g.emit({t, " = OpBitwiseXor %uint ", v[0], " ", v[1]});
    g.emit({r, " = OpIAdd %uint ", t, " ", v[2]});
    g.dst(at(o.a, 0), r);
}

void not_b32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S r = g.tmp();
    g.emit({r, " = OpNot %uint ", x});
    g.dst(at(o.a, 0), r);
}

void mul_hi_i32(const Op& o) {
    Gen& g = o.g;
    S x = g.as_int(g.src_u(at(o.a, 1)));
    S y = g.as_int(g.src_u(at(o.a, 2)));
    S s = g.tmp(), hi = g.tmp(), r = g.tmp();
    g.emit({s, " = OpSMulExtended %smul ", x, " ", y});
    g.emit({hi, " = OpCompositeExtract %int ", s, " 1"});
    g.emit({r, " = OpBitcast %uint ", hi});
    g.uses_smul = true;
    g.dst(at(o.a, 0), r);
}

void cmp_64(const Op& o) {
    // 64-bit equality compare: both halves of the register pairs.
    Gen& g = o.g;
    const Arg& d = at(o.a, 0);
    const Arg& x = at(o.a, 1);
    const Arg& y = at(o.a, 2);
    auto pair = [&](const Arg& s) -> std::pair<S, S> {
        if ((s.k == "vgpr" || s.k == "sgpr") && s.n == 2) {
            S l = g.load_reg(s.k, s.r);
            S h = g.load_reg(s.k, s.r + 1);
            return {l, h};
        }
        if (s.k == "imm") {
            uint64_t v = static_cast<uint64_t>(s.v);
            S l = g.cu(static_cast<int64_t>(v & 0xFFFFFFFF));
            S h = g.cu(static_cast<int64_t>(v >> 32));
            return {l, h};
        }
        throw Unsupported(o.op + " operand");
    };
    auto [x0, x1] = pair(x);
    auto [y0, y1] = pair(y);
    auto t = tmps(g, 4);
    const S &e0 = t[0], &e1 = t[1], &r = t[3];
    S eq = t[2];
    g.emit({e0, " = OpIEqual %bool ", x0, " ", y0});
    g.emit({e1, " = OpIEqual %bool ", x1, " ", y1});
    g.emit({eq, " = OpLogicalAnd %bool ", e0, " ", e1});
    if (at(split(o.op, '_'), 2) != "eq") {
        S ne = g.tmp();
        g.emit({ne, " = OpLogicalNot %bool ", eq});
        eq = ne;
    }
    g.emit({r, " = OpSelect %uint ", eq, " ", g.cu(0xFFFFFFFF), " ", g.cu(0)});
    if (d.special("vcc") || d.special("vcc_lo"))
        g.emit({"OpStore %vcc ", r});
    else
        g.mask_dst(d, r);
}

void add_co(const Op& o) {
    // Lane masks are all-ones/zero: carry in = (vcc or the named mask) != 0, carry out stored the same way.
    Gen& g = o.g;
    const S& op = o.op;
    const Arg& vd = at(o.a, 0);
    const Arg& cd = at(o.a, 1);
    auto srcs = slice(o.a, 2);
    S x = g.src_u(at(srcs, 0));
    S y = g.src_u(at(srcs, 1));
    if (starts(op, "v_subrev")) std::swap(x, y);  // reversed: src1 - src0 (- borrow)
    const bool sub = contains(op, "sub");
    S cin;
    if (ends(op, "ci_u32")) {
        S cin_m = srcs.size() > 2 ? g.mask_src(srcs[2]) : g.mask_src(vcc_arg());
        cin = g.tmp();
        g.emit({cin, " = OpSelect %uint ", g.tmp_bool_nz(cin_m), " ", g.cu(1), " ", g.cu(0)});
    } else {
        cin = g.cu(0);
    }
    auto t = tmps(g, 6);
    const S &s1 = t[0], &c1 = t[1], &s2 = t[2], &c2 = t[3], &r = t[4], &carry = t[5];
    g.emit({s1, sub ? " = OpISubBorrow %umul " : " = OpIAddCarry %umul ", x, " ", y});
    S r1 = g.tmp();
    g.emit({r1, " = OpCompositeExtract %uint ", s1, " 0"});
    g.emit({c1, " = OpCompositeExtract %uint ", s1, " 1"});
    g.emit({s2, " = ", sub ? "OpISubBorrow" : "OpIAddCarry", " %umul ", r1, " ", cin});
    g.emit({r, " = OpCompositeExtract %uint ", s2, " 0"});
    g.emit({c2, " = OpCompositeExtract %uint ", s2, " 1"});
    g.emit({carry, " = OpBitwiseOr %uint ", c1, " ", c2});
    g.dst(vd, r);
    S m = g.tmp();
    g.emit({m, " = OpSelect %uint ", g.tmp_bool_nz(carry), " ", g.cu(0xFFFFFFFF), " ", g.cu(0)});
    if (!cd.special("null")) g.mask_dst(cd, m);
}

void minmax3_int(const Op& o) {
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    S f = S(ends(o.op, "i32") ? "S" : "U") + (contains(o.op, "min") ? "Min" : "Max");
    S m = g.tmp(), r = g.tmp();
    g.emit({m, " = OpExtInst %uint %glsl ", f, " ", v[0], " ", v[1]});
    g.emit({r, " = OpExtInst %uint %glsl ", f, " ", m, " ", v[2]});
    g.dst(at(o.a, 0), r);
}

void med3_int(const Op& o) {
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    const char* s = ends(o.op, "i32") ? "S" : "U";
    S lo = g.tmp(), hi = g.tmp(), r = g.tmp();
    g.emit({lo, " = OpExtInst %uint %glsl ", s, "Min ", v[0], " ", v[1]});
    g.emit({hi, " = OpExtInst %uint %glsl ", s, "Max ", v[0], " ", v[1]});
    g.emit({r, " = OpExtInst %uint %glsl ", s, "Clamp ", v[2], " ", lo, " ", hi});
    g.dst(at(o.a, 0), r);
}

void mul_lo_minmax(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    S r = g.tmp();
    if (o.op == "v_mul_lo_u32")
        g.emit({r, " = OpIMul %uint ", x, " ", y});
    else
        g.emit({r, " = OpExtInst %uint %glsl ", key(kMinMax, o.op), " ", x, " ", y});
    g.dst(at(o.a, 0), r);
}

void perm(const Op& o) {
    // Byte permute: result byte i = selector byte i picks from the 8 bytes {src0, src1} (src1 is low). Selector
    // values 0-7 pick a byte; 12 -> 0x00, >=13 -> 0xFF (sign/other cases not used here are 0).
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    const S &s0 = v[0], &s1 = v[1], &sel = v[2];
    S out = g.cu(0);
    for (int i = 0; i < 4; ++i) {
        auto t = tmps(g, 12);
        const S &sb = t[0], &lo_pick = t[1], &hi_pick = t[2], &k = t[3], &b = t[4], &lo_sh = t[5], &hi_sh = t[6],
                &chosen = t[7], &inrange = t[8], &val = t[9], &shifted = t[10], &acc = t[11];
        g.emit({sb, " = OpBitFieldUExtract %uint ", sel, " ", g.cu(8 * i), " ", g.cu(8)});
        g.emit({k, " = OpBitwiseAnd %uint ", sb, " ", g.cu(7)});
        g.emit({b, " = OpULessThan %bool ", k, " ", g.cu(4)});
        g.emit({lo_sh, " = OpIMul %uint ", k, " ", g.cu(8)});
        g.emit({lo_pick, " = OpShiftRightLogical %uint ", s1, " ", lo_sh});
        g.emit({hi_sh, " = OpISub %uint ", lo_sh, " ", g.cu(32)});
        g.emit({hi_pick, " = OpShiftRightLogical %uint ", s0, " ", hi_sh});
        g.emit({chosen, " = OpSelect %uint ", b, " ", lo_pick, " ", hi_pick});
        g.emit({inrange, " = OpULessThan %bool ", sb, " ", g.cu(8)});
        S ff = g.tmp(), big = g.tmp();
        g.emit({big, " = OpUGreaterThanEqual %bool ", sb, " ", g.cu(13)});
        g.emit({ff, " = OpSelect %uint ", big, " ", g.cu(0xFF), " ", g.cu(0)});
        g.emit({val, " = OpSelect %uint ", inrange, " ", chosen, " ", ff});
        S m8 = g.tmp();
        g.emit({m8, " = OpBitwiseAnd %uint ", val, " ", g.cu(0xFF)});
        g.emit({shifted, " = OpShiftLeftLogical %uint ", m8, " ", g.cu(8 * i)});
        g.emit({acc, " = OpBitwiseOr %uint ", out, " ", shifted});
        out = acc;
    }
    g.dst(at(o.a, 0), out);
}

void mul_hi_u32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    S s = g.tmp(), r = g.tmp();
    g.emit({s, " = OpUMulExtended %umul ", x, " ", y});
    g.emit({r, " = OpCompositeExtract %uint ", s, " 1"});
    g.dst(at(o.a, 0), r);
}

void binary_u32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    S r = g.tmp();
    auto it = kU2.find(o.op);
    const S& kind = it != kU2.end() ? it->second : key(kS2, o.op);
    if (kind == "mul24") {
        S xm = g.tmp(), ym = g.tmp();
        g.emit({xm, " = OpBitwiseAnd %uint ", x, " ", g.cu(0xFFFFFF)});
        g.emit({ym, " = OpBitwiseAnd %uint ", y, " ", g.cu(0xFFFFFF)});
        g.emit({r, " = OpIMul %uint ", xm, " ", ym});
    } else {
        g.emit({r, " = ", kind, " %uint ", x, " ", y});
    }
    g.dst(at(o.a, 0), r);
}

void shift_rev(const Op& o) {
    Gen& g = o.g;
    S sh = g.src_u(at(o.a, 1));
    S x = g.src_u(at(o.a, 2));
    S m = g.tmp(), r = g.tmp();
    g.emit({m, " = OpBitwiseAnd %uint ", sh, " ", g.cu(31)});
    g.emit({r, " = ", key(kShiftRev, o.op), " %uint ", x, " ", m});
    g.dst(at(o.a, 0), r);
}

void bfe_u32(const Op& o) {
    // offset and width are taken mod 32 (a width of 0 gives 0)
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    S om = g.tmp(), wm = g.tmp(), r = g.tmp();
    g.emit({om, " = OpBitwiseAnd %uint ", v[1], " ", g.cu(31)});
    g.emit({wm, " = OpBitwiseAnd %uint ", v[2], " ", g.cu(31)});
    g.emit({r, " = ", ends(o.op, "i32") ? "OpBitFieldSExtract" : "OpBitFieldUExtract", " %uint ", v[0], " ", om, " ",
            wm});
    g.dst(at(o.a, 0), r);
}

void mad_u32_u24(const Op& o) {
    Gen& g = o.g;
    auto v = srcs_u(g, o.a, 1, 4, 3);
    S xm = g.tmp(), ym = g.tmp(), p = g.tmp(), r = g.tmp();
    g.emit({xm, " = OpBitwiseAnd %uint ", v[0], " ", g.cu(0xFFFFFF)});
    g.emit({ym, " = OpBitwiseAnd %uint ", v[1], " ", g.cu(0xFFFFFF)});
    g.emit({p, " = OpIMul %uint ", xm, " ", ym});
    g.emit({r, " = OpIAdd %uint ", p, " ", v[2]});
    g.dst(at(o.a, 0), r);
}

void cndmask(const Op& o) {
    // e32: dst, src0, src1 (vcc) ; e64: dst, src0, src1, sN[:N+1]/vcc
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S y = g.src_u(at(o.a, 2));
    S c = o.a.size() > 3 ? g.mask_src(o.a[3]) : g.mask_src(vcc_arg());
    S b = g.tmp(), r = g.tmp();
    g.emit({b, " = OpINotEqual %bool ", c, " ", g.cu(0)});
    g.emit({r, " = OpSelect %uint ", b, " ", y, " ", x});
    g.dst(at(o.a, 0), r);
}

void movrel(const Op& o) {
    // Relative VGPR indexing through M0: movrels reads VGPR[src + M0], movreld writes VGPR[dst + M0], movrelsd does
    // both. The register file is an array, so these become dynamic array accesses.
    Gen& g = o.g;
    const S& op = o.op;
    const Arg& d = at(o.a, 0);
    const Arg& s = at(o.a, 1);
    if (d.k != "vgpr" || s.k != "vgpr") throw Unsupported(op + " operands");
    S m0 = g.tmp();
    g.emit({m0, " = OpLoad %uint %m0"});
    S v;
    if (op == "v_movrels_b32" || op == "v_movrelsd_b32") {
        S si = g.tmp(), sp = g.tmp();
        v = g.tmp();
        g.emit({si, " = OpIAdd %uint ", g.cu(s.r), " ", m0});
        g.emit({sp, " = OpAccessChain %_ptr_Function_uint %vgpr ", si});
        g.emit({v, " = OpLoad %uint ", sp});
    } else {
        v = g.load_reg("vgpr", s.r);
    }
    if (op == "v_movreld_b32" || op == "v_movrelsd_b32") {
        auto t = tmps(g, 4);
        const S &di = t[0], &dp = t[1], &old = t[2], &sel = t[3];
        g.emit({di, " = OpIAdd %uint ", g.cu(d.r), " ", m0});
        g.emit({dp, " = OpAccessChain %_ptr_Function_uint %vgpr ", di});
        g.emit({old, " = OpLoad %uint ", dp});
        g.emit({sel, " = OpSelect %uint ", g.active_b(), " ", v, " ", old});
        g.emit({"OpStore ", dp, " ", sel});
    } else {
        g.dst(d, v);
    }
}

}  // namespace fp::xlat::ops
