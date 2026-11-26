// Compares (v_cmp*, s_cmp*, v_cmp_class), fragment attributes and exports (rdna2spirv.Gen).
#include "gen.h"

namespace fp::xlat {
namespace {

const std::map<S, S> kFloatCompares = {
    {"lt", "OpFOrdLessThan"},          {"le", "OpFOrdLessThanEqual"},    {"gt", "OpFOrdGreaterThan"},
    {"ge", "OpFOrdGreaterThanEqual"},  {"eq", "OpFOrdEqual"},            {"neq", "OpFUnordNotEqual"},
    {"lg", "OpFOrdNotEqual"},          {"nlt", "OpFUnordGreaterThanEqual"}, {"ngt", "OpFUnordLessThanEqual"},
    {"nle", "OpFUnordGreaterThan"},    {"nge", "OpFUnordLessThan"}};

const std::map<S, S> kSignedCompares = {
    {"lt", "OpSLessThan"}, {"le", "OpSLessThanEqual"}, {"gt", "OpSGreaterThan"}, {"ge", "OpSGreaterThanEqual"},
    {"eq", "OpIEqual"},    {"ne", "OpINotEqual"},      {"lg", "OpINotEqual"}};

const std::map<S, S> kUnsignedCompares = {
    {"lt", "OpULessThan"}, {"le", "OpULessThanEqual"}, {"gt", "OpUGreaterThan"}, {"ge", "OpUGreaterThanEqual"},
    {"eq", "OpIEqual"},    {"ne", "OpINotEqual"},      {"lg", "OpINotEqual"}};

}  // namespace

void Gen::compare(const S& op, const Args& a, bool scalar, bool exec_only) {
    // v_cmp_* / s_cmp_*: lane result into VCC/SGPR mask (vector), SCC (scalar), or EXEC (v_cmpx).
    const std::vector<S> parts = split(op, '_');  // v_cmp_lt_f32 / s_cmp_eq_u32
    const S cond = at(parts, 2), ty = at(parts, 3);
    const Args srcs = scalar ? slice(a, 0, 2) : slice(a, -2);
    std::optional<Arg> dest;
    if (!(scalar || exec_only)) dest = at(a, 0);
    S x, y;
    const std::map<S, S>* ops = nullptr;
    if (ty == "f32" || ty == "f16") {
        if (ty == "f32") {
            x = src_f(at(srcs, 0));
            y = src_f(at(srcs, 1));
        } else {  // f16 compare: the low half of each source, compared as f32 (exact for f16 values)
            std::vector<S> vals;
            for (const auto& s : srcs) {
                S u = src_u(plain(s));
                S v2 = tmp(), f = tmp();
                emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
                emit({f, " = OpCompositeExtract %float ", v2, " 0"});
                vals.push_back(modify_f(f, s));
            }
            if (vals.size() != 2) throw PyError("ValueError");
            x = vals[0];
            y = vals[1];
        }
        ops = &kFloatCompares;
    } else if (ty == "u32" || ty == "i32") {
        x = src_u(at(srcs, 0));
        y = src_u(at(srcs, 1));
        if (ty == "i32") {
            x = as_int(x);
            y = as_int(y);
        }
        ops = ty == "i32" ? &kSignedCompares : &kUnsignedCompares;
    } else {
        throw Unsupported(op);
    }
    auto it = ops->find(cond);
    if (it == ops->end()) throw Unsupported(op);
    S b = tmp(), r = tmp();
    emit({b, " = ", it->second, " %bool ", x, " ", y});
    // Lane masks are all-ones/zero per invocation (one lane), SCC is 1/0.
    emit({r, " = OpSelect %uint ", b, " ", cu(scalar ? 1 : 0xFFFFFFFF), " ", cu(0)});
    if (scalar) {
        emit({"OpStore %scc ", r});
    } else if (!dest) {  // v_cmpx: EXEC &= cond
        S a2 = tmp();
        emit({a2, " = OpLoad %bool %active"});
        S a3 = tmp();
        emit({a3, " = OpLogicalAnd %bool ", a2, " ", b});
        emit({"OpStore %active ", a3});
        uses_active = true;
    } else if (dest->k == "special" && (dest->t == "vcc" || dest->t == "vcc_lo")) {
        emit({"OpStore %vcc ", r});
    } else if (dest->k == "sgpr") {
        store_reg("sgpr", dest->r, r);
        if (dest->n == 2) store_reg("sgpr", dest->r + 1, r);
    } else {
        throw Unsupported("v_cmp destination");
    }
}

void Gen::compare_x(const S& op, const Args& a) {
    // v_cmpx_*: like v_cmp but the result ANDs into EXEC. e32 form has only the two sources; e64 also names a
    // destination (vcc/sgpr) which gets the compare result too.
    Args srcs = slice(a, -2);
    Args with_none{Arg{}};
    with_none.insert(with_none.end(), srcs.begin(), srcs.end());
    compare("v_cmp_" + op.substr(7), with_none, false, true);
}

void Gen::cmp_class(const Args& a) {
    // v_cmp_class_f32 dst, x, mask: true if x's class bit is set in mask.
    // Bits: 0 sNaN, 1 qNaN, 2 -inf, 3 -normal, 4 -denormal, 5 -0, 6 +0, 7 +denormal, 8 +normal, 9 +inf.
    S x = src_f(at(a, -2));
    S mask = src_u(at(a, -1));
    S u = tou(x);
    S sign = tmp(), absu = tmp();
    emit({sign, " = OpShiftRightLogical %uint ", u, " ", cu(31)});
    emit({absu, " = OpBitwiseAnd %uint ", u, " ", cu(0x7FFFFFFF)});
    S exp_ = tmp(), man = tmp();
    emit({exp_, " = OpShiftRightLogical %uint ", absu, " ", cu(23)});
    emit({man, " = OpBitwiseAnd %uint ", absu, " ", cu(0x7FFFFF)});
    S neg = tmp_bool_nz(sign);

    auto b = [&](const S& cond_line) {
        S t = tmp();
        emit({t, " = ", cond_line});
        return t;
    };
    S is_zero = b(cat({"OpIEqual %bool ", absu, " ", cu(0)}));
    S exp0 = b(cat({"OpIEqual %bool ", exp_, " ", cu(0)}));
    S expmax = b(cat({"OpIEqual %bool ", exp_, " ", cu(255)}));
    S man0 = b(cat({"OpIEqual %bool ", man, " ", cu(0)}));
    S man_nz = b(cat({"OpLogicalNot %bool ", man0}));
    S is_inf = b(cat({"OpLogicalAnd %bool ", expmax, " ", man0}));
    S is_nan = b(cat({"OpLogicalAnd %bool ", expmax, " ", man_nz}));
    S quiet = b(cat({"OpUGreaterThanEqual %bool ", man, " ", cu(0x400000)}));
    S not_quiet = b(cat({"OpLogicalNot %bool ", quiet}));
    S snan = b(cat({"OpLogicalAnd %bool ", is_nan, " ", not_quiet}));
    S qnan = b(cat({"OpLogicalAnd %bool ", is_nan, " ", quiet}));
    S denorm = b(cat({"OpLogicalAnd %bool ", exp0, " ", man_nz}));
    S not_exp0 = b(cat({"OpLogicalNot %bool ", exp0}));
    S not_expmax = b(cat({"OpLogicalNot %bool ", expmax}));
    S normal = b(cat({"OpLogicalAnd %bool ", not_exp0, " ", not_expmax}));
    S pos = b(cat({"OpLogicalNot %bool ", neg}));
    std::vector<S> classes{snan, qnan};
    classes.push_back(b(cat({"OpLogicalAnd %bool ", is_inf, " ", neg})));
    classes.push_back(b(cat({"OpLogicalAnd %bool ", normal, " ", neg})));
    classes.push_back(b(cat({"OpLogicalAnd %bool ", denorm, " ", neg})));
    classes.push_back(b(cat({"OpLogicalAnd %bool ", is_zero, " ", neg})));
    classes.push_back(b(cat({"OpLogicalAnd %bool ", is_zero, " ", pos})));
    classes.push_back(b(cat({"OpLogicalAnd %bool ", denorm, " ", pos})));
    classes.push_back(b(cat({"OpLogicalAnd %bool ", normal, " ", pos})));
    classes.push_back(b(cat({"OpLogicalAnd %bool ", is_inf, " ", pos})));
    S acc = "%false";
    for (size_t i = 0; i < classes.size(); ++i) {
        S bit = b(cat({"OpBitwiseAnd %uint ", mask, " ", cu(int64_t{1} << i)}));
        S on = b(cat({"OpINotEqual %bool ", bit, " ", cu(0)}));
        S hit = b(cat({"OpLogicalAnd %bool ", on, " ", classes[i]}));
        acc = b(cat({"OpLogicalOr %bool ", acc, " ", hit}));
    }
    S r = b(cat({"OpSelect %uint ", acc, " ", cu(0xFFFFFFFF), " ", cu(0)}));
    const Arg& d = at(a, 0);
    if (a.size() == 2 || (d.k == "special" && (d.t == "vcc" || d.t == "vcc_lo")))
        emit({"OpStore %vcc ", r});
    else
        mask_dst(d, r);
}

S Gen::attribute(int attr, int comp) {
    // Component `comp` of fragment input attribute n (declared as a vec4 Input at Location n).
    S name = "%in_attr" + D(attr);
    inputs.set(name, attr);
    S p = tmp(), v = tmp();
    emit({p, " = OpAccessChain %_ptr_Input_float ", name, " ", cu(comp)});
    emit({v, " = OpLoad %float ", p});
    return v;
}

void Gen::export_(const Ins& ins) {
    const Args& a = ins.args;
    std::optional<S> tgt;
    if (!a.empty() && (a[0].k == "target" || a[0].k == "special")) tgt = a[0].t;
    if (!tgt || *tgt == "null") return;
    const S& t = *tgt;
    if (mesh && t == "prim") {
        // NGG primitive export: three 10-bit vertex indices (bits 0, 10, 20), bit 31 = null primitive. Each lane
        // exports its own primitive: kept per invocation, written out by the epilogue.
        S v = src_u(at(a, 1));
        S old = tmp(), sel = tmp();
        emit({old, " = OpLoad %uint %ms_prim"});
        emit({sel, " = OpSelect %uint ", active_b(), " ", v, " ", old});
        emit({"OpStore %ms_prim ", sel});
        return;
    }
    std::vector<S> comps;
    if (has(ins.mods, "compr")) {
        // Compressed export: two registers, each holding two f16 values (x,y) and (z,w). LLVM lists each register
        // twice (one slot per half): 'exp mrt0 v4, v4, v5, v5 compr', so the pair is slots 1 and 3.
        // (Found by the GPU texture test: taking the first two operands exported (r,g,r,g).)
        Args srcs = slice(a, 1, 5);
        Args regs = srcs.size() >= 3 ? Args{srcs[0], srcs[2]} : slice(srcs, 0, 1);
        for (const auto& x : regs) {
            if (x.k == "special" && x.t == "off") {
                comps.push_back("%f0");
                comps.push_back("%f0");
                continue;
            }
            S u = src_u(x);
            S v2 = tmp();
            emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
            for (int c = 0; c < 2; ++c) {
                S f = tmp();
                emit({f, " = OpCompositeExtract %float ", v2, " ", D(c)});
                comps.push_back(f);
            }
        }
    } else {
        for (const auto& x : slice(a, 1, 5)) {
            if (x.k == "special" && x.t == "off")
                comps.push_back("%f0");
            else
                comps.push_back(src_f(x));
        }
    }
    while (comps.size() < 4) comps.push_back("%f0");
    comps.resize(4);
    S v = tmp();
    emit({v, " = OpCompositeConstruct %v4float ", join(comps)});
    std::optional<S> name;
    if (t == "pos0") name = "%out_pos";
    if (mesh) {
        // this lane's vertex: position and parameters kept per invocation (the epilogue writes them out)
        S var;
        if (t == "pos0") {
            var = "%ms_pos";
        } else if (starts(t, "param")) {
            int64_t k = py_int(std::string_view(t).substr(5));
            mesh->params.insert(static_cast<int>(k));
            var = "%ms_param" + D(k);
        } else {
            return;  // pos1+ (clip/cull distances): not modelled
        }
        S old = tmp(), sel = tmp();
        emit({old, " = OpLoad %v4float ", var});
        emit({sel, " = OpSelect %v4float ", active_b(), " ", v, " ", old});
        emit({"OpStore ", var, " ", sel});
        return;
    }
    if (t == "mrtz" && stage == "ps") {
        // Depth export: component x is the fragment depth (stencil/mask exports aren't modelled).
        writes_depth = true;
        S old = tmp(), sel = tmp();
        emit({old, " = OpLoad %float %out_depth"});
        emit({sel, " = OpSelect %float ", active_b(), " ", comps[0], " ", old});
        emit({"OpStore %out_depth ", sel});
        return;
    }
    if (!name) {
        if (starts(t, "param") && stage == "vs")
            name = "%out_param" + D(py_int(std::string_view(t).substr(5)));
        else if (starts(t, "mrt") && t != "mrtz" && stage == "ps")
            name = "%out_mrt" + D(py_int(std::string_view(t).substr(3)));
        else if (starts(t, "pos"))
            return;  // pos1+ (clip/cull/point size): ignored in v0
        else
            throw Unsupported("export " + t);
    }
    outputs.set(*name, t);
    // Exports from inactive lanes don't happen: keep the previous output value.
    S old = tmp(), sel = tmp();
    emit({old, " = OpLoad %v4float ", *name});
    emit({sel, " = OpSelect %v4float ", active_b(), " ", v, " ", old});
    emit({"OpStore ", *name, " ", sel});
}

}  // namespace fp::xlat
