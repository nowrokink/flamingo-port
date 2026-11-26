// Half precision and 16-bit instructions: f16 arithmetic on a register's low half, packed f16x2 (VOP3P), the mixed
// f16/f32 FMAs, f16 conversions and interpolation, and the 16-bit integer ops (rdna2spirv.Gen._insn's branches).
#include <map>

#include "ops.h"

namespace fp::xlat::ops {
namespace {

const std::map<S, S> kF16Unary = {{"v_rsq_f16", "InverseSqrt"}, {"v_sqrt_f16", "Sqrt"},   {"v_exp_f16", "Exp2"},
                                  {"v_log_f16", "Log2"},        {"v_floor_f16", "Floor"}, {"v_fract_f16", "Fract"},
                                  {"v_ceil_f16", "Ceil"},       {"v_trunc_f16", "Trunc"}, {"v_rndne_f16", "RoundEven"}};
const std::map<S, S> kF16Binary = {{"v_add_f16", "OpFAdd"}, {"v_sub_f16", "OpFSub"}, {"v_mul_f16", "OpFMul"}};
const std::map<S, S> kInt16 = {{"v_add_u16", "OpIAdd"},     {"v_sub_u16", "OpISub"}, {"v_mul_lo_u16", "OpIMul"},
                               {"v_and_b16", "OpBitwiseAnd"}, {"v_or_b16", "OpBitwiseOr"},
                               {"v_xor_b16", "OpBitwiseXor"}};

// The f32 result r as f16 bits in the low half of a uint (the high half zero).
S pack_lo(Gen& g, const S& r) {
    S w2 = g.tmp(), p = g.tmp(), lo = g.tmp();
    g.emit({w2, " = OpCompositeConstruct %v2float ", r, " %f0"});
    g.emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", w2});
    g.emit({lo, " = OpBitwiseAnd %uint ", p, " ", g.cu(0xFFFF)});
    return lo;
}

void no_op_sel(const Op& o) {
    if (any_op_sel(o.mods)) throw Unsupported(o.op + " op_sel");
}

// The mixed FMAs' sources: each f32, or (op_sel_hi bit set) an f16 from the low/high half chosen by op_sel.
std::vector<S> mix_sources(const Op& o) {
    Gen& g = o.g;
    S sel = first_mod(o.mods, "op_sel:", "op_sel:[0,0,0]");
    S selhi = first_mod(o.mods, "op_sel_hi:", "op_sel_hi:[0,0,0]");
    auto sb = mod_bits(sel);
    auto hb = mod_bits(selhi);
    std::vector<S> vals;
    auto srcs = slice(o.a, 1, 4);
    for (size_t i = 0; i < srcs.size(); ++i) {
        const Arg& src = srcs[i];
        if (at(hb, static_cast<int64_t>(i))) {
            S u = g.src_u(Gen::plain(src));
            S v2 = g.tmp(), f = g.tmp();
            g.emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
            g.emit({f, " = OpCompositeExtract %float ", v2, " ", D(at(sb, static_cast<int64_t>(i)))});
            vals.push_back(g.modify_f(f, src));
        } else {
            vals.push_back(g.src_f(src));
        }
    }
    return vals;
}

}  // namespace

void cvt_f16_f32(const Op& o) {
    // Low 16 bits get the f16; on GFX10 the high half is kept (partial write: Mesa ACO instr_is_16bit lists
    // v_cvt_f16_f32 as 16-bit for gfx_level >= GFX10).
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S v = g.tmp(), p = g.tmp(), r = g.tmp();
    g.emit({v, " = OpCompositeConstruct %v2float ", x, " %f0"});
    g.emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", v});
    g.emit({r, " = OpBitwiseAnd %uint ", p, " ", g.cu(0xFFFF)});
    g.write_lo16(at(o.a, 0), r);
}

void pack_b32_f16(const Op& o) {
    // dst = f16(src0) | f16(src1) << 16, each taken from the low half (op_sel picks high halves).
    Gen& g = o.g;
    S sel = first_mod(o.mods, "op_sel:", "op_sel:[0,0,0]");
    auto sb = mod_bits(sel);
    std::vector<S> halves;
    auto srcs = slice(o.a, 1, 3);
    for (size_t i = 0; i < srcs.size(); ++i) {
        S u = g.src_u(Gen::plain(srcs[i]));
        if (at(sb, static_cast<int64_t>(i))) {
            S s = g.tmp();
            g.emit({s, " = OpShiftRightLogical %uint ", u, " ", g.cu(16)});
            u = s;
        }
        S m = g.tmp();
        g.emit({m, " = OpBitwiseAnd %uint ", u, " ", g.cu(0xFFFF)});
        halves.push_back(m);
    }
    for (const Arg& src : srcs)
        if (src.neg || src.abs) throw Unsupported("v_pack_b32_f16 modifiers");
    S hi = g.tmp(), r = g.tmp();
    g.emit({hi, " = OpShiftLeftLogical %uint ", at(halves, 1), " ", g.cu(16)});
    g.emit({r, " = OpBitwiseOr %uint ", at(halves, 0), " ", hi});
    g.dst(at(o.a, 0), r);
}

void fmac_f16(const Op& o) {
    Gen& g = o.g;
    no_op_sel(o);
    const Arg &s0 = at(o.a, 1), &s1 = at(o.a, 2), &s2 = at(o.a, 0);
    std::vector<S> vals;
    for (const Arg* s : {&s0, &s1, &s2}) vals.push_back(f16_src(g, *s));
    auto t = tmps(g, 6);  // r, v2, p, old, keep, newv
    const S &r = t[0], &v2 = t[1], &p = t[2];
    g.emit({r, " = OpExtInst %float %glsl Fma ", vals[0], " ", vals[1], " ", vals[2]});
    g.emit({v2, " = OpCompositeConstruct %v2float ", r, " %f0"});
    g.emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", v2});
    S lo = g.tmp();
    g.emit({lo, " = OpBitwiseAnd %uint ", p, " ", g.cu(0xFFFF)});
    g.write_lo16(at(o.a, 0), lo);  // GFX10: f16 ops keep the high half
}

void int16(const Op& o) {
    // 16-bit integer ops on the low halves (op_sel picks high halves in the VOP3 form). Per Mesa ACO (can_use_opsel):
    // the opsel-capable VOP3 (_e64) forms are partial writes that keep the high half, and the VOP2 (_e32) forms
    // define the whole register (high half modelled as zero).
    Gen& g = o.g;
    const S& op = o.op;
    const bool e64 = ends(o.ins.op, "_e64");
    S sel = first_mod(o.mods, "op_sel:", "op_sel:[0,0,0]");
    auto sb = mod_bits(sel);
    sb.insert(sb.end(), {0, 0, 0});
    if (!e64 && (sb[0] != 0 || sb[1] != 0)) throw Unsupported(op + " op_sel");
    std::vector<S> halves;
    auto srcs = slice(o.a, 1, 3);
    for (size_t k = 0; k < srcs.size(); ++k) {
        S u = g.src_u(srcs[k]);
        if (sb[k]) {
            S t = g.tmp();
            g.emit({t, " = OpShiftRightLogical %uint ", u, " ", g.cu(16)});
            u = t;
        }
        S h = g.tmp();
        g.emit({h, " = OpBitwiseAnd %uint ", u, " ", g.cu(0xFFFF)});
        halves.push_back(h);
    }
    if (halves.size() != 2) throw PyError("ValueError");
    const S &x = halves[0], &y = halves[1];
    S r = g.tmp();
    if (one_of(op, {"v_lshrrev_b16", "v_lshlrev_b16", "v_ashrrev_i16"})) {
        S sh = g.tmp();  // shift amount is src0 (reversed operand order), low 4 bits
        g.emit({sh, " = OpBitwiseAnd %uint ", x, " ", g.cu(15)});
        if (op == "v_lshrrev_b16") {
            g.emit({r, " = OpShiftRightLogical %uint ", y, " ", sh});
        } else if (op == "v_lshlrev_b16") {
            g.emit({r, " = OpShiftLeftLogical %uint ", y, " ", sh});
        } else {
            S yi = g.tmp(), ri = g.tmp();
            g.emit({yi, " = OpBitFieldSExtract %int ", g.as_int(y), " ", g.cu(0), " ", g.cu(16)});
            g.emit({ri, " = OpShiftRightArithmetic %int ", yi, " ", sh});
            g.emit({r, " = OpBitcast %uint ", ri});
        }
    } else if (one_of(op, {"v_max_i16", "v_min_i16"})) {
        S xi = g.tmp(), yi = g.tmp(), ri = g.tmp();
        g.emit({xi, " = OpBitFieldSExtract %int ", g.as_int(x), " ", g.cu(0), " ", g.cu(16)});
        g.emit({yi, " = OpBitFieldSExtract %int ", g.as_int(y), " ", g.cu(0), " ", g.cu(16)});
        g.emit({ri, " = OpExtInst %int %glsl ", contains(op, "max") ? "SMax" : "SMin", " ", xi, " ", yi});
        g.emit({r, " = OpBitcast %uint ", ri});
    } else {
        auto it = kInt16.find(op);
        if (op == "v_subrev_u16")
            g.emit({r, " = OpISub %uint ", y, " ", x});
        else if (it != kInt16.end())
            g.emit({r, " = ", it->second, " %uint ", x, " ", y});
        else
            g.emit({r, " = OpExtInst %uint %glsl ", contains(op, "max") ? "UMax" : "UMin", " ", x, " ", y});
    }
    S lo = g.tmp();
    g.emit({lo, " = OpBitwiseAnd %uint ", r, " ", g.cu(0xFFFF)});
    if (e64 && sb[2]) {
        // op_sel destination bit: the result goes to the high half, the low half is kept.
        auto t = tmps(g, 4);
        const S &old = t[0], &keep = t[1], &sh16 = t[2], &v = t[3];
        g.emit({old, " = OpCopyObject %uint ", g.load_reg("vgpr", reg(at(o.a, 0)))});
        g.emit({keep, " = OpBitwiseAnd %uint ", old, " ", g.cu(0xFFFF)});
        g.emit({sh16, " = OpShiftLeftLogical %uint ", lo, " ", g.cu(16)});
        g.emit({v, " = OpBitwiseOr %uint ", keep, " ", sh16});
        g.dst(at(o.a, 0), v);
    } else {
        g.write_lo16(at(o.a, 0), lo, e64);
    }
}

void fma_f16(const Op& o) {
    Gen& g = o.g;
    no_op_sel(o);
    std::vector<S> vals;
    for (const Arg& s : slice(o.a, 1, 4)) vals.push_back(f16_src(g, s));
    auto t = tmps(g, 4);
    S r = t[0];
    const S &w2 = t[1], &p = t[2], &lo = t[3];
    g.emit({r, " = OpExtInst %float %glsl Fma ", at(vals, 0), " ", at(vals, 1), " ", at(vals, 2)});
    r = g.clamp_mods(r, o.mods);
    g.emit({w2, " = OpCompositeConstruct %v2float ", r, " %f0"});
    g.emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", w2});
    g.emit({lo, " = OpBitwiseAnd %uint ", p, " ", g.cu(0xFFFF)});
    g.write_lo16(at(o.a, 0), lo);  // GFX10: f16 ops keep the high half
}

void pk_f16(const Op& o) {
    // Packed f16x2 (VOP3P). Result lo: source i takes its half from op_sel[i] (0 = lo, 1 = hi), negated by neg_lo[i].
    // Result hi: half from op_sel_hi[i], negated by neg_hi[i]. Defaults: op_sel [0,0], op_sel_hi [1,1], no negation.
    Gen& g = o.g;
    const S& op = o.op;
    const int ns = op == "v_pk_fma_f16" ? 3 : 2;
    auto bits = [&](const char* name, int64_t fallback) {
        auto m = first_mod(o.mods, S(name) + ":");
        return m ? mod_bits(*m) : std::vector<int64_t>(static_cast<size_t>(ns), fallback);
    };
    auto sel = bits("op_sel", 0), selhi = bits("op_sel_hi", 1);
    auto nlo = bits("neg_lo", 0), nhi = bits("neg_hi", 0);
    std::vector<S> srcs;
    for (int i = 0; i < ns; ++i) {
        S u = g.src_u(Gen::plain(at(o.a, 1 + i)));
        S v2 = g.tmp();
        g.emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
        const std::pair<int64_t, int64_t> halves[2] = {{at(sel, i), at(nlo, i)}, {at(selhi, i), at(nhi, i)}};
        std::vector<S> comps;
        for (const auto& [half_sel, neg] : halves) {
            S c = g.tmp();
            g.emit({c, " = OpCompositeExtract %float ", v2, " ", D(half_sel)});
            if (neg) {
                S n2 = g.tmp();
                g.emit({n2, " = OpFNegate %float ", c});
                c = n2;
            }
            comps.push_back(c);
        }
        S vv = g.tmp();
        g.emit({vv, " = OpCompositeConstruct %v2float ", comps[0], " ", comps[1]});
        srcs.push_back(vv);
    }
    const S &vx = srcs[0], &vy = srcs[1];
    S r = g.tmp(), p = g.tmp();
    if (op == "v_pk_fma_f16")
        g.emit({r, " = OpExtInst %v2float %glsl Fma ", vx, " ", vy, " ", srcs[2]});
    else if (op == "v_pk_add_f16" || op == "v_pk_mul_f16")
        g.emit({r, " = ", contains(op, "add") ? "OpFAdd" : "OpFMul", " %v2float ", vx, " ", vy});
    else
        g.emit({r, " = OpExtInst %v2float %glsl ", contains(op, "max") ? "FMax" : "FMin", " ", vx, " ", vy});
    if (has(o.mods, "clamp")) {
        S c = g.tmp();
        S z2 = g.tmp(), o2 = g.tmp();
        g.emit({z2, " = OpCompositeConstruct %v2float %f0 %f0"});
        g.emit({o2, " = OpCompositeConstruct %v2float %f1 %f1"});
        g.emit({c, " = OpExtInst %v2float %glsl FClamp ", r, " ", z2, " ", o2});
        r = c;
    }
    g.emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", r});
    g.dst(at(o.a, 0), p);
}

void med3_f16(const Op& o) {
    Gen& g = o.g;
    no_op_sel(o);
    std::vector<S> vals;
    for (const Arg& s : slice(o.a, 1, 4)) vals.push_back(f16_src(g, s));
    S lo_ = g.tmp(), hi_ = g.tmp(), r = g.tmp();
    g.emit({lo_, " = OpExtInst %float %glsl FMin ", at(vals, 0), " ", at(vals, 1)});
    g.emit({hi_, " = OpExtInst %float %glsl FMax ", at(vals, 0), " ", at(vals, 1)});
    g.emit({r, " = OpExtInst %float %glsl FClamp ", at(vals, 2), " ", lo_, " ", hi_});
    r = g.clamp_mods(r, o.mods);
    S lo = pack_lo(g, r);
    g.write_lo16(at(o.a, 0), lo);  // opsel-capable VOP3: keeps the high half (Mesa ACO can_use_opsel)
}

void f16_unary(const Op& o) {
    // f16 unary on the low half; result in the low half, high half cleared (non-op_sel form).
    Gen& g = o.g;
    no_op_sel(o);
    const Arg& s = at(o.a, 1);
    S u = g.src_u(Gen::plain(s));
    S v2 = g.tmp(), f = g.tmp();
    g.emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
    g.emit({f, " = OpCompositeExtract %float ", v2, " 0"});
    f = g.modify_f(f, s);
    S r = g.tmp();
    auto it = kF16Unary.find(o.op);
    if (it != kF16Unary.end())
        g.emit({r, " = OpExtInst %float %glsl ", it->second, " ", f});
    else
        g.emit({r, " = OpFDiv %float %f1 ", f});
    r = g.clamp_mods(r, o.mods);
    S lo = pack_lo(g, r);
    g.write_lo16(at(o.a, 0), lo);  // GFX10: f16 ops keep the high half
}

void f16_binary(const Op& o) {
    // f16 math on the low halves; the result's high half is cleared (non-op_sel form).
    Gen& g = o.g;
    no_op_sel(o);
    std::vector<S> vals;
    for (const Arg& s : slice(o.a, 1, 3)) vals.push_back(f16_src(g, s));
    S r = g.tmp();
    auto it = kF16Binary.find(o.op);
    if (it != kF16Binary.end())
        g.emit({r, " = ", it->second, " %float ", at(vals, 0), " ", at(vals, 1)});
    else
        g.emit({r, " = OpExtInst %float %glsl ", contains(o.op, "max") ? "FMax" : "FMin", " ", at(vals, 0), " ",
                at(vals, 1)});
    r = g.clamp_mods(r, o.mods);
    S lo = pack_lo(g, r);
    g.write_lo16(at(o.a, 0), lo);  // GFX10: f16 ops keep the high half
}

void fma_mix_f32(const Op& o) {
    // Each source is f32, or (op_sel_hi bit set) an f16 from the low/high half chosen by op_sel.
    Gen& g = o.g;
    auto vals = mix_sources(o);
    S r = g.tmp();
    g.emit({r, " = OpExtInst %float %glsl Fma ", at(vals, 0), " ", at(vals, 1), " ", at(vals, 2)});
    S c = g.clamp_mods(r, o.mods);
    g.dst(at(o.a, 0), g.tou(c));
}

void cvt_f32_f16(const Op& o) {
    Gen& g = o.g;
    const Arg& s = at(o.a, 1);
    S u = g.src_u(Gen::plain(s));
    S v2 = g.tmp(), f = g.tmp();
    g.emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
    g.emit({f, " = OpCompositeExtract %float ", v2, " 0"});
    const Arg& d = at(o.a, 0);
    S m = g.modify_f(f, s);
    g.dst(d, g.tou(m));
}

void interp_f16(const Op& o) {
    Gen& g = o.g;
    const Arg* attr = nullptr;
    for (const Arg& x : o.a)
        if (x.k == "attr") {
            attr = &x;
            break;
        }
    if (!attr || g.stage != "ps") throw Unsupported(o.op + " form");
    S f = g.attribute(attr->attr, attr->comp);
    if (o.op == "v_interp_p2_f16") {
        S v = g.tmp(), p = g.tmp(), r = g.tmp();
        g.emit({v, " = OpCompositeConstruct %v2float ", f, " %f0"});
        g.emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", v});
        g.emit({r, " = OpBitwiseAnd %uint ", p, " ", g.cu(0xFFFF)});
        g.dst(at(o.a, 0), r);
    } else {  // p1ll/p1lv produce the f32 intermediate consumed by p2_f16
        const Arg& d = at(o.a, 0);
        g.dst(d, g.tou(f));
    }
}

void fma_mixlo_hi(const Op& o) {
    Gen& g = o.g;
    auto vals = mix_sources(o);
    S r = g.tmp(), v = g.tmp(), p = g.tmp(), h16 = g.tmp();
    g.emit({r, " = OpExtInst %float %glsl Fma ", at(vals, 0), " ", at(vals, 1), " ", at(vals, 2)});
    r = g.clamp_mods(r, o.mods);
    g.emit({v, " = OpCompositeConstruct %v2float ", r, " %f0"});
    g.emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", v});
    g.emit({h16, " = OpBitwiseAnd %uint ", p, " ", g.cu(0xFFFF)});
    S old = g.load_reg("vgpr", reg(at(o.a, 0)));
    S keep = g.tmp(), newv = g.tmp();
    if (o.op == "v_fma_mixlo_f16") {
        g.emit({keep, " = OpBitwiseAnd %uint ", old, " ", g.cu(0xFFFF0000)});
        g.emit({newv, " = OpBitwiseOr %uint ", keep, " ", h16});
    } else {
        S sh = g.tmp();
        g.emit({keep, " = OpBitwiseAnd %uint ", old, " ", g.cu(0xFFFF)});
        g.emit({sh, " = OpShiftLeftLogical %uint ", h16, " ", g.cu(16)});
        g.emit({newv, " = OpBitwiseOr %uint ", keep, " ", sh});
    }
    g.dst(at(o.a, 0), newv);
}

void cvt_pkrtz(const Op& o) {
    // Packs two f32 as f16 into one dword with round-toward-zero (exact, see f16_rtz). The earlier round-to-nearest
    // version produced inf where hardware gives 65504 (found by the GPU smoke test).
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S y = g.src_f(at(o.a, 2));
    S lo = g.f16_rtz(x);
    S hi = g.f16_rtz(y);
    S sh = g.tmp(), r = g.tmp();
    g.emit({sh, " = OpShiftLeftLogical %uint ", hi, " ", g.cu(16)});
    g.emit({r, " = OpBitwiseOr %uint ", lo, " ", sh});
    g.dst(at(o.a, 0), r);
}

}  // namespace fp::xlat::ops
