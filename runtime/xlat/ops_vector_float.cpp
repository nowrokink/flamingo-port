// Vector f32 instructions: arithmetic, min/max/median, transcendental, cube-map face math, conversions between f32
// and integers, and the f32 interpolation (rdna2spirv.Gen._insn's branches).
#include <map>

#include "ops.h"

namespace fp::xlat::ops {
namespace {

// v_<op>_f32 -> SPIR-V op, or "max"/"min" for the GLSL extended instructions (rdna2spirv.F2)
const std::map<S, S> kF2 = {{"v_add_f32", "OpFAdd"}, {"v_sub_f32", "OpFSub"}, {"v_mul_f32", "OpFMul"},
                            {"v_mul_legacy_f32", "OpFMul"}, {"v_max_f32", "max"}, {"v_min_f32", "min"}};
// one-source f32 ops -> GLSL.std.450 instruction, "rcp" for 1/x (rdna2spirv.F1)
const std::map<S, S> kF1 = {{"v_rcp_f32", "rcp"},     {"v_rsq_f32", "InverseSqrt"}, {"v_sqrt_f32", "Sqrt"},
                            {"v_exp_f32", "Exp2"},    {"v_log_f32", "Log2"},        {"v_floor_f32", "Floor"},
                            {"v_fract_f32", "Fract"}, {"v_ceil_f32", "Ceil"},       {"v_trunc_f32", "Trunc"}};

// dst = f32 result after the output modifiers
void put_f(const Op& o, const S& r) {
    S c = o.g.clamp_mods(r, o.mods);
    o.g.dst(at(o.a, 0), o.g.tou(c));
}

}  // namespace

bool is_f32_binary(const S& op) { return kF2.count(op) != 0; }
bool is_f32_unary(const S& op) { return kF1.count(op) != 0; }

void max3_min3_f32(const Op& o) {
    Gen& g = o.g;
    auto v = srcs_f(g, o.a, 1, 4, 3);
    const char* f = contains(o.op, "max") ? "FMax" : "FMin";
    S m = g.tmp(), r = g.tmp();
    g.emit({m, " = OpExtInst %float %glsl ", f, " ", v[0], " ", v[1]});
    g.emit({r, " = OpExtInst %float %glsl ", f, " ", m, " ", v[2]});
    put_f(o, r);
}

void sin_cos(const Op& o) {
    // RDNA takes the angle in turns: sin(x * 2*pi).
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S t = g.tmp(), r = g.tmp();
    g.emit({t, " = OpFMul %float ", x, " ", g.fconst(6.283185307179586)});
    g.emit({r, " = OpExtInst %float %glsl ", contains(o.op, "sin") ? "Sin" : "Cos", " ", t});
    put_f(o, r);
}

void rndne_f32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S r = g.tmp();
    g.emit({r, " = OpExtInst %float %glsl RoundEven ", x});
    put_f(o, r);
}

void madak_madmk(const Op& o) {
    Gen& g = o.g;
    S x, y, z;
    if (o.op == "v_madak_f32") {
        x = g.src_f(at(o.a, 1));
        y = g.src_f(at(o.a, 2));
        z = g.src_f(at(o.a, 3));
    } else {
        x = g.src_f(at(o.a, 1));
        y = g.src_f(at(o.a, 3));
        z = g.src_f(at(o.a, 2));
    }
    S m = g.tmp(), r = g.tmp();
    g.emit({m, " = OpFMul %float ", x, " ", y});
    g.emit({r, " = OpFAdd %float ", m, " ", z});
    g.dst(at(o.a, 0), g.tou(r));
}

void cube(const Op& o) {
    // Cube face selection (standard table): major axis Z if |z|>=|x| and |z|>=|y|, else Y if |y|>=|x|, else X.
    //   face: +X0 -X1 +Y2 -Y3 +Z4 -Z5
    //   +X: sc=-z tc=-y   -X: sc=+z tc=-y   +Y: sc=+x tc=+z   -Y: sc=+x tc=-z   +Z: sc=+x tc=-y   -Z: sc=-x tc=-y
    //   cubema = 2 * (signed major-axis coordinate)
    Gen& g = o.g;
    auto v = srcs_f(g, o.a, 1, 4, 3);
    const S &x = v[0], &y = v[1], &z = v[2];
    S ax = g.tmp(), ay = g.tmp(), az = g.tmp();
    g.emit({ax, " = OpExtInst %float %glsl FAbs ", x});
    g.emit({ay, " = OpExtInst %float %glsl FAbs ", y});
    g.emit({az, " = OpExtInst %float %glsl FAbs ", z});
    auto t5 = tmps(g, 5);
    const S &zx = t5[0], &zy = t5[1], &zmaj = t5[2], &yx = t5[3], &ymaj = t5[4];
    g.emit({zx, " = OpFOrdGreaterThanEqual %bool ", az, " ", ax});
    g.emit({zy, " = OpFOrdGreaterThanEqual %bool ", az, " ", ay});
    g.emit({zmaj, " = OpLogicalAnd %bool ", zx, " ", zy});
    g.emit({yx, " = OpFOrdGreaterThanEqual %bool ", ay, " ", ax});
    S nz = g.tmp();
    g.emit({nz, " = OpLogicalNot %bool ", zmaj});
    g.emit({ymaj, " = OpLogicalAnd %bool ", nz, " ", yx});

    auto neg = [&](const S& val) {
        S n = g.tmp();
        g.emit({n, " = OpFNegate %float ", val});
        return n;
    };
    auto lt0 = [&](const S& val) {
        S b = g.tmp();
        g.emit({b, " = OpFOrdLessThan %bool ", val, " %f0"});
        return b;
    };
    auto pick = [&](const S& zv, const S& yv, const S& xv) {
        S t1 = g.tmp(), t2 = g.tmp();
        g.emit({t1, " = OpSelect %float ", ymaj, " ", yv, " ", xv});
        g.emit({t2, " = OpSelect %float ", zmaj, " ", zv, " ", t1});
        return t2;
    };
    auto sel = [&](const S& c, const S& t, const S& f) {
        S r = g.tmp();
        g.emit({r, " = OpSelect %float ", c, " ", t, " ", f});
        return r;
    };
    S xn = lt0(x), yn = lt0(y), zn = lt0(z);
    S r;
    if (o.op == "v_cubema_f32") {
        S m = pick(z, y, x);
        r = g.tmp();
        g.emit({r, " = OpFMul %float ", m, " ", g.fconst(2.0)});
    } else if (o.op == "v_cubeid_f32") {
        S f5 = g.fconst(5.0), f4 = g.fconst(4.0);
        S s1 = sel(zn, f5, f4);
        S f3 = g.fconst(3.0), f2 = g.fconst(2.0);
        S s2 = sel(yn, f3, f2);
        S f1 = g.fconst(1.0), f0 = g.fconst(0.0);
        S s3 = sel(xn, f1, f0);
        r = pick(s1, s2, s3);
    } else if (o.op == "v_cubesc_f32") {
        S nx = neg(x);
        S s1 = sel(zn, nx, x);
        S nzv = neg(z);
        S s3 = sel(xn, z, nzv);
        r = pick(s1, x, s3);
    } else {
        S n1 = neg(y);
        S nzv = neg(z);
        S s2 = sel(yn, nzv, z);
        S n2 = neg(y);
        r = pick(n1, s2, n2);
    }
    put_f(o, r);
}

void mad_legacy_f32(const Op& o) {
    // Legacy multiply treats 0 * anything (inf/NaN included) as 0.
    Gen& g = o.g;
    auto v = srcs_f(g, o.a, 1, 4, 3);
    auto t = tmps(g, 6);
    const S &m = t[0], &zx = t[1], &zy = t[2], &either = t[3], &sel = t[4], &r = t[5];
    g.emit({m, " = OpFMul %float ", v[0], " ", v[1]});
    g.emit({zx, " = OpFOrdEqual %bool ", v[0], " %f0"});
    g.emit({zy, " = OpFOrdEqual %bool ", v[1], " %f0"});
    g.emit({either, " = OpLogicalOr %bool ", zx, " ", zy});
    g.emit({sel, " = OpSelect %float ", either, " %f0 ", m});
    g.emit({r, " = OpFAdd %float ", sel, " ", v[2]});
    put_f(o, r);
}

void mad_f32(const Op& o) {
    Gen& g = o.g;
    auto v = srcs_f(g, o.a, 1, 4, 3);
    S m = g.tmp(), r = g.tmp();
    g.emit({m, " = OpFMul %float ", v[0], " ", v[1]});
    g.emit({r, " = OpFAdd %float ", m, " ", v[2]});
    put_f(o, r);
}

void interp_f32(const Op& o) {
    // Single-lane model: the barycentric p1/p2 pair (and the flat mov) both yield the attribute value.
    Gen& g = o.g;
    const Arg* attr = nullptr;
    for (const Arg& x : o.a)
        if (x.k == "attr") {
            attr = &x;
            break;
        }
    if (!attr || g.stage != "ps") throw Unsupported(o.op + " form");
    const Arg& d = at(o.a, 0);
    S f = g.attribute(attr->attr, attr->comp);
    g.dst(d, g.tou(f));
}

void cvt_pk_u8_f32(const Op& o) {
    // Convert src0 to u8 (round to nearest even, clamp) and insert it at byte src1 of src2.
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S byte = g.src_u(at(o.a, 2));
    S base = g.src_u(at(o.a, 3));
    auto t = tmps(g, 9);
    const S &c = t[0], &rn = t[1], &u = t[2], &sh = t[3], &mask = t[4], &nmask = t[5], &keep = t[6], &ins_ = t[7],
            &r = t[8];
    g.emit({c, " = OpExtInst %float %glsl FClamp ", x, " %f0 ", g.fconst(255.0)});
    g.emit({rn, " = OpExtInst %float %glsl RoundEven ", c});
    g.emit({u, " = OpConvertFToU %uint ", rn});
    S b3 = g.tmp();
    g.emit({b3, " = OpBitwiseAnd %uint ", byte, " ", g.cu(3)});
    g.emit({sh, " = OpIMul %uint ", b3, " ", g.cu(8)});
    g.emit({mask, " = OpShiftLeftLogical %uint ", g.cu(0xFF), " ", sh});
    g.emit({nmask, " = OpNot %uint ", mask});
    g.emit({keep, " = OpBitwiseAnd %uint ", base, " ", nmask});
    g.emit({ins_, " = OpShiftLeftLogical %uint ", u, " ", sh});
    g.emit({r, " = OpBitwiseOr %uint ", keep, " ", ins_});
    g.dst(at(o.a, 0), r);
}

void rcp_iflag_f32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S r = g.tmp();
    g.emit({r, " = OpFDiv %float %f1 ", x});
    g.dst(at(o.a, 0), g.tou(r));
}

void f32_binary(const Op& o) {
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S y = g.src_f(at(o.a, 2));
    S r = g.tmp();
    const S& k = key(kF2, o.op);
    if (k == "max" || k == "min")
        g.emit({r, " = OpExtInst %float %glsl ", k == "max" ? "FMax" : "FMin", " ", x, " ", y});
    else
        g.emit({r, " = ", k, " %float ", x, " ", y});
    put_f(o, r);
}

void subrev_f32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S y = g.src_f(at(o.a, 2));
    S r = g.tmp();
    g.emit({r, " = OpFSub %float ", y, " ", x});
    put_f(o, r);
}

void fma_f32(const Op& o) {
    Gen& g = o.g;
    const S& op = o.op;
    S x, y, z;
    if (op == "v_fma_f32") {
        auto v = srcs_f(g, o.a, 1, 4, 3);
        x = v[0], y = v[1], z = v[2];
    } else if (op == "v_fmac_f32" || op == "v_mac_f32") {
        x = g.src_f(at(o.a, 1));
        y = g.src_f(at(o.a, 2));
        z = g.src_f(at(o.a, 0));
    } else if (op == "v_fmaak_f32") {
        x = g.src_f(at(o.a, 1));
        y = g.src_f(at(o.a, 2));
        z = g.src_f(at(o.a, 3));
    } else if (op == "v_fmamk_f32") {
        x = g.src_f(at(o.a, 1));
        y = g.src_f(at(o.a, 3));
        z = g.src_f(at(o.a, 2));
    } else {  // v_med3_f32
        auto v = srcs_f(g, o.a, 1, 4, 3);
        S lo = g.tmp(), hi = g.tmp(), r = g.tmp();
        g.emit({lo, " = OpExtInst %float %glsl FMin ", v[0], " ", v[1]});
        g.emit({hi, " = OpExtInst %float %glsl FMax ", v[0], " ", v[1]});
        g.emit({r, " = OpExtInst %float %glsl FClamp ", v[2], " ", lo, " ", hi});
        g.dst(at(o.a, 0), g.tou(r));
        return;
    }
    S r = g.tmp();
    g.emit({r, " = OpExtInst %float %glsl Fma ", x, " ", y, " ", z});
    put_f(o, r);
}

void f32_unary(const Op& o) {
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S r = g.tmp();
    const S& k = key(kF1, o.op);
    if (k == "rcp")
        g.emit({r, " = OpFDiv %float %f1 ", x});
    else
        g.emit({r, " = OpExtInst %float %glsl ", k, " ", x});
    put_f(o, r);
}

void cvt_f32_int(const Op& o) {
    Gen& g = o.g;
    S x = g.src_u(at(o.a, 1));
    S r = g.tmp();
    if (ends(o.op, "u32"))
        g.emit({r, " = OpConvertUToF %float ", x});
    else
        g.emit({r, " = OpConvertSToF %float ", g.as_int(x)});
    g.dst(at(o.a, 0), g.tou(r));
}

void cvt_f32_ubyte(const Op& o) {
    Gen& g = o.g;
    int64_t n = py_int(o.op.substr(o.op.size() - 1));
    S x = g.src_u(at(o.a, 1));
    S s = g.tmp(), m = g.tmp(), r = g.tmp();
    g.emit({s, " = OpShiftRightLogical %uint ", x, " ", g.cu(8 * n)});
    g.emit({m, " = OpBitwiseAnd %uint ", s, " ", g.cu(0xFF)});
    g.emit({r, " = OpConvertUToF %float ", m});
    g.dst(at(o.a, 0), g.tou(r));
}

void cvt_int_f32(const Op& o) {
    Gen& g = o.g;
    S x = g.src_f(at(o.a, 1));
    S r = g.tmp();
    if (o.op == "v_cvt_u32_f32") {
        g.emit({r, " = OpConvertFToU %uint ", x});
    } else {
        S i = g.tmp();
        g.emit({i, " = OpConvertFToS %int ", x});
        g.emit({r, " = OpBitcast %uint ", i});
    }
    g.dst(at(o.a, 0), r);
}

}  // namespace fp::xlat::ops
