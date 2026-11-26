// Images: T#/S# -> bindless index through a runtime-filled hash table, sampling, loads, stores, atomics and queries.
// A port of rdna2spirv.Gen's desc_lookup_function .. image_misc (and image_get_lod), line for line.
//
// Table layout (storage buffer, set 0 binding 2): uint mask; then entries of 10 uints: key[0..7] (descriptor
// dwords; S# uses key[0..3] and key[4..7] = 0xFFFFFFFF), index, pad. Probes 8 slots from hash(key) & mask; a miss
// yields index 0 (the runtime puts a fallback there).
#include <map>
#include <tuple>

#include "gen.h"

namespace fp::xlat {
namespace {

// Tables at namespace scope: the program is built with /Zc:threadSafeInit- and translates on many threads, so no
// function-local statics.

// image atomic -> (SPIR-V op, value that leaves memory unchanged, used for inactive lanes)
const std::map<S, std::pair<const char*, uint32_t>> kImgAtomics = {
    {"image_atomic_umin", {"OpAtomicUMin", 0xFFFFFFFFu}}, {"image_atomic_umax", {"OpAtomicUMax", 0u}},
    {"image_atomic_smin", {"OpAtomicSMin", 0x7FFFFFFFu}}, {"image_atomic_smax", {"OpAtomicSMax", 0x80000000u}},
    {"image_atomic_or", {"OpAtomicOr", 0u}},              {"image_atomic_xor", {"OpAtomicXor", 0u}},
    {"image_atomic_and", {"OpAtomicAnd", 0xFFFFFFFFu}},   {"image_atomic_add", {"OpAtomicIAdd", 0u}},
    {"image_atomic_sub", {"OpAtomicISub", 0u}}};

// image_store dims -> coordinate count; the typed storage arrays (bindings 11-18; bindless_spaces gives each a
// register space) by (dim, component type).
const std::map<S, int> kStoreDims = {{"1D", 1}, {"2D", 2}, {"2D_ARRAY", 3}, {"3D", 3}};
const std::map<std::pair<S, S>, int> kStoreBinding = {
    {{"2D", "f"}, 11}, {{"3D", "f"}, 12}, {{"2D_ARRAY", "f"}, 13}, {{"1D", "f"}, 14},
    {{"2D", "u"}, 15}, {{"3D", "u"}, 16}, {{"2D_ARRAY", "u"}, 17}, {{"1D", "u"}, 18}};

// GFX10 image formats whose texels are integers (T# word 1 bits 20-28): stored as uint4, the rest as float4. Computed
// on each call (gfx10_formats() is another file's table: no cross-file initialisation order to rely on).
std::vector<int> int_formats() {
    std::vector<int> out;
    for (const auto& [code, name] : gfx10_formats()) {
        S n = rstrip(name, " \t\n\r\f\v");
        if (ends(n, "_UINT") || ends(n, "_SINT")) out.push_back(code);
    }
    return out;  // a std::map: already sorted by code
}

// dim.split("SQ_RSRC_IMG_")[-1]
S after_img(const S& dim) {
    size_t k = dim.rfind("SQ_RSRC_IMG_");
    return k == S::npos ? dim : dim.substr(k + 12);
}

// int(dm.split(":")[1], 0)
int64_t mod_int(const S& mod) { return py_int(at(split(mod, ':'), 1), 0); }

int bit_length(int64_t v) {
    int n = 0;
    for (uint64_t u = static_cast<uint64_t>(v < 0 ? -v : v); u; u >>= 1) ++n;
    return n;
}

// The register of an operand that names one (a["r"]; a KeyError for kinds without registers).
int64_t reg_r(const Arg& a) {
    if (a.k != "vgpr" && a.k != "sgpr" && a.k != "vlist") key_error();
    return a.r;
}

struct ImgKind {
    const char* itype;
    const char* iarr;
    const char* stype;
    int ncoord;
    const char* iptr;
};
const std::map<S, ImgKind> kImgKinds = {
    {"dim:SQ_RSRC_IMG_2D", {"img2d", "%images", "simg2d", 2, "_ptr_UC_img"}},
    {"dim:SQ_RSRC_IMG_2D_ARRAY", {"img2da", "%images_arr", "simg2da", 3, "_ptr_UC_imga"}},
    // Cube: the shader already did face selection (v_cube*), so (s, t, face) samples a 6-layer 2D array view of the
    // cube. The runtime binds cube textures as 2D-array views.
    {"dim:SQ_RSRC_IMG_CUBE", {"img2da", "%images_arr", "simg2da", 3, "_ptr_UC_imga"}},
    {"dim:SQ_RSRC_IMG_1D", {"img1d", "%images_1d", "simg1d", 1, "_ptr_UC_img1"}},
    {"dim:SQ_RSRC_IMG_3D", {"img3d", "%images_3d", "simg3d", -3, "_ptr_UC_img3"}}};

}  // namespace

// Gen.STORE_BINDING[(dim, kind)], for assemble().
int store_binding(const S& dim, const S& kind) { return key(kStoreBinding, std::make_pair(dim, kind)); }

bool Gen::is_image_atomic(const S& op) { return kImgAtomics.count(op) != 0; }

S Gen::desc_lookup_function() {
    // d3d12: %desc_lookup(8 descriptor words) -> bindless index, the same search as desc_lookup's inline form: FNV-1a
    // of the words, then up to PROBES linear probes. The table's word 0 is the slot mask; slot n is words
    // 10n..10n+9, the 8 descriptor words at +1..+8 and the index at +9. 0 when not found.
    std::vector<S> w;
    for (int i = 0; i < 8; ++i) w.push_back("%dl_w" + D(i));
    std::vector<S> L = {"%desc_lookup = OpFunction %uint None %fn_lookup"};
    for (const auto& x : w) L.push_back(x + " = OpFunctionParameter %uint");
    L.push_back("%dl_entry = OpLabel");
    L.push_back(cat({"%dl_k = OpVariable %_ptr_Function_uint Function ", cu(0)}));
    L.push_back(cat({"%dl_r = OpVariable %_ptr_Function_uint Function ", cu(0)}));
    S h = cu(2166136261);
    for (int i = 0; i < 8; ++i) {
        L.push_back(cat({"%dl_x", D(i), " = OpBitwiseXor %uint ", h, " ", w[i]}));
        L.push_back(cat({"%dl_h", D(i), " = OpIMul %uint %dl_x", D(i), " ", cu(16777619)}));
        h = "%dl_h" + D(i);
    }
    L.push_back(cat({"%dl_mp = OpAccessChain %_ptr_SB_uint %desc_map ", cu(0), " ", cu(0)}));
    for (const char* l : {"%dl_mask = OpLoad %uint %dl_mp", "OpBranch %dl_head", "%dl_head = OpLabel",
                          "OpLoopMerge %dl_exit %dl_cont None", "OpBranch %dl_body", "%dl_body = OpLabel",
                          "%dl_kv = OpLoad %uint %dl_k"})
        L.push_back(l);
    L.push_back(cat({"%dl_s = OpIAdd %uint ", h, " %dl_kv"}));
    L.push_back("%dl_slot = OpBitwiseAnd %uint %dl_s %dl_mask");
    L.push_back(cat({"%dl_base = OpIMul %uint %dl_slot ", cu(10)}));
    S match = "%true";
    for (int j = 0; j < 8; ++j) {
        const S J = D(j);
        L.push_back(cat({"%dl_o", J, " = OpIAdd %uint %dl_base ", cu(1 + j)}));
        L.push_back(cat({"%dl_p", J, " = OpAccessChain %_ptr_SB_uint %desc_map ", cu(0), " %dl_o", J}));
        L.push_back(cat({"%dl_v", J, " = OpLoad %uint %dl_p", J}));
        L.push_back(cat({"%dl_e", J, " = OpIEqual %bool %dl_v", J, " ", w[j]}));
        L.push_back(cat({"%dl_m", J, " = OpLogicalAnd %bool ", match, " %dl_e", J}));
        match = "%dl_m" + J;
    }
    L.push_back(cat({"%dl_o9 = OpIAdd %uint %dl_base ", cu(9)}));
    L.push_back(cat({"%dl_p9 = OpAccessChain %_ptr_SB_uint %desc_map ", cu(0), " %dl_o9"}));
    L.push_back("%dl_idx = OpLoad %uint %dl_p9");
    L.push_back("%dl_old = OpLoad %uint %dl_r");
    L.push_back(cat({"%dl_new = OpSelect %uint ", match, " %dl_idx %dl_old"}));
    for (const char* l : {"OpStore %dl_r %dl_new", "OpBranch %dl_cont", "%dl_cont = OpLabel"}) L.push_back(l);
    L.push_back(cat({"%dl_k1 = OpIAdd %uint %dl_kv ", cu(1)}));
    L.push_back("OpStore %dl_k %dl_k1");
    L.push_back(cat({"%dl_more = OpULessThan %bool %dl_k1 ", cu(PROBES)}));
    L.push_back(cat({"%dl_miss = OpLogicalNot %bool ", match}));
    for (const char* l : {"%dl_go = OpLogicalAnd %bool %dl_more %dl_miss",
                          "OpBranchConditional %dl_go %dl_head %dl_exit", "%dl_exit = OpLabel",
                          "%dl_res = OpLoad %uint %dl_r", "OpReturnValue %dl_res", "OpFunctionEnd"})
        L.push_back(l);
    std::vector<S> out;
    for (const auto& l : L) {
        const bool label = starts(l, "%") && l.find(" = Op") != S::npos && l.find("OpLabel") != S::npos;
        out.push_back(label ? l : "         " + l);
    }
    return join(out, "\n");
}

S Gen::desc_lookup(const std::vector<S>& words) {
    // words: 8 uint ids. Returns a uint id with the bindless index.
    uses_tex = true;
    if (d3d()) {
        // One function with a loop, called per sample: unrolled inline (8 probes of 9 loads each, per sample),
        // SPIRV-Cross writes it as one nested expression and DXC spends minutes on a shader.
        if (lookup_fn.empty()) lookup_fn = desc_lookup_function();
        S r = tmp();
        emit({r, " = OpFunctionCall %uint %desc_lookup ", join(words)});
        return r;
    }
    S h = cu(2166136261);
    for (const auto& w : words) {  // FNV-1a over the 8 dwords
        S x = tmp(), m = tmp();
        emit({x, " = OpBitwiseXor %uint ", h, " ", w});
        emit({m, " = OpIMul %uint ", x, " ", cu(16777619)});
        h = m;
    }
    S mp = tmp(), mask = tmp();
    emit({mp, " = OpAccessChain %_ptr_SB_uint %desc_map %c0 %c0"});
    emit({mask, " = OpLoad %uint ", mp});
    S found = "%false", idx = cu(0);
    for (int k = 0; k < PROBES; ++k) {
        S s = tmp(), slot = tmp(), base = tmp();
        emit({s, " = OpIAdd %uint ", h, " ", cu(k)});
        emit({slot, " = OpBitwiseAnd %uint ", s, " ", mask});
        emit({base, " = OpIMul %uint ", slot, " ", cu(10)});
        S match = "%true";
        for (size_t j = 0; j < words.size(); ++j) {
            S o = tmp(), p = tmp(), v = tmp(), e = tmp(), m2 = tmp();
            emit({o, " = OpIAdd %uint ", base, " ", cu(1 + static_cast<int64_t>(j))});
            emit({p, " = OpAccessChain %_ptr_SB_uint %desc_map %c0 ", o});
            emit({v, " = OpLoad %uint ", p});
            emit({e, " = OpIEqual %bool ", v, " ", words[j]});
            emit({m2, " = OpLogicalAnd %bool ", match, " ", e});
            match = m2;
        }
        S o = tmp(), p = tmp(), v = tmp();
        emit({o, " = OpIAdd %uint ", base, " ", cu(9)});
        emit({p, " = OpAccessChain %_ptr_SB_uint %desc_map %c0 ", o});
        emit({v, " = OpLoad %uint ", p});
        S nf = tmp(), take = tmp(), nidx = tmp(), nfound = tmp();
        emit({nf, " = OpLogicalNot %bool ", found});
        emit({take, " = OpLogicalAnd %bool ", nf, " ", match});
        emit({nidx, " = OpSelect %uint ", take, " ", v, " ", idx});
        emit({nfound, " = OpLogicalOr %bool ", found, " ", match});
        idx = nidx;
        found = nfound;
    }
    return idx;
}

std::vector<S> Gen::sgpr_words(const Arg& a, int count) {
    if (a.k != "sgpr" || a.n < count) throw Unsupported("descriptor operand");
    std::vector<S> out;
    for (int i = 0; i < count; ++i) out.push_back(load_reg("sgpr", a.r + i));
    return out;
}

std::vector<int> Gen::addr_regs(const Arg& a) {
    // Image address operand: contiguous v[x:y] or an NSA list.
    if (a.k == "vgpr") {
        std::vector<int> out;
        for (int i = 0; i < a.n; ++i) out.push_back(a.r + i);
        return out;
    }
    if (a.k == "vlist") return a.list;
    throw Unsupported("image address operand");
}

void Gen::image_op(const S& op, const Args& a, const Mods& mods) {
    const bool a16 = has(mods, "a16");
    const bool d16 = has(mods, "d16");
    const S dim = first_mod(mods, "dim:", "dim:SQ_RSRC_IMG_2D");
    auto kit = kImgKinds.find(dim);
    if (kit == kImgKinds.end()) throw Unsupported(op + " " + after_img(dim));
    const ImgKind& kd = kit->second;
    int ncoord = kd.ncoord;
    const bool is3d = ncoord == -3;  // 3D: three real coordinates (and 3-component gradients)
    if (is3d) {
        ncoord = 3;
        uses_tex_3d = true;
    } else if (ncoord == 3) {
        uses_tex_array = true;
    }
    if (ncoord == 1) uses_tex_1d = true;
    if (is3d && ends(op, "_d")) throw Unsupported(op + " 3D gradients");
    const S dm = first_mod(mods, "dmask:", "dmask:0x1");
    const int64_t dmask = mod_int(dm);
    const Arg& vdata = at(a, 0);
    const std::vector<int> addr = addr_regs(at(a, 1));
    std::vector<S> t = sgpr_words(at(a, 2), 8);
    S img_idx = desc_lookup(t);
    S ip = tmp(), img = tmp();
    emit({ip, " = OpAccessChain %", kd.iptr, " ", kd.iarr, " ", img_idx});
    emit({img, " = OpLoad %", kd.itype, " ", ip});
    std::vector<S> coords;
    for (int r : addr) coords.push_back(load_reg("vgpr", r));
    if (a16) {
        // 16-bit addresses: each register packs two f16 coordinates (low half first).
        std::vector<S> unpacked;
        for (const auto& c : coords) {
            S v2 = tmp();
            emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", c});
            for (int k : {0, 1}) {
                S f = tmp();
                emit({f, " = OpCompositeExtract %float ", v2, " ", D(k)});
                unpacked.push_back(tou(f));
            }
        }
        coords = unpacked;
        if (op == "image_load") {
            // 16-bit integer coordinates: each half is an unsigned 16-bit int, not an f16.
            std::vector<S> regs, ints;
            for (int r : addr) regs.push_back(load_reg("vgpr", r));
            for (const auto& c : regs) {
                S lo = tmp(), hi = tmp();
                emit({lo, " = OpBitwiseAnd %uint ", c, " ", cu(0xFFFF)});
                emit({hi, " = OpShiftRightLogical %uint ", c, " ", cu(16)});
                ints.push_back(lo);
                ints.push_back(hi);
            }
            coords = ints;
        }
    }
    S res;
    int ncomp = 0;
    if (op == "image_load") {
        if (static_cast<int>(coords.size()) < ncoord) throw Unsupported("image_load coords");
        std::vector<S> ints;
        for (const auto& c : slice(coords, 0, ncoord)) ints.push_back(as_int(c));
        S iv;
        if (ncoord == 1) {
            iv = at(ints, 0);
        } else {
            iv = tmp();
            emit({iv, " = OpCompositeConstruct ", ncoord == 2 ? "%v2int" : "%v3int", " ", join(ints)});
        }
        res = tmp();
        emit({res, " = OpImageFetch %v4float ", img, " ", iv, " Lod ", cu(0)});
        ncomp = popcount(static_cast<uint64_t>(dmask));
    } else {
        std::vector<S> s = sgpr_words(at(a, 3), 4);
        S all_ones = cu(0xFFFFFFFF);
        for (int i = 0; i < 4; ++i) s.push_back(all_ones);
        S smp_idx = desc_lookup(s);
        S sp = tmp(), smp = tmp(), si = tmp();
        emit({sp, " = OpAccessChain %_ptr_UC_smp %samplers ", smp_idx});
        emit({smp, " = OpLoad %smp ", sp});
        emit({si, " = OpSampledImage %", kd.stype, " ", img, " ", smp});
        std::vector<S> f;
        for (const auto& c : coords) f.push_back(tof(c));
        if (op == "image_gather4_lz") {
            // Gather the 2x2 footprint of one channel (dmask picks it) at mip 0 -> 4 results.
            if (popcount(static_cast<uint64_t>(dmask)) != 1 || ncoord != 2) throw Unsupported("image_gather4_lz form");
            S uv = tmp();
            res = tmp();
            emit({uv, " = OpCompositeConstruct %v2float ", at(f, 0), " ", at(f, 1)});
            emit({res, " = OpImageGather %v4float ", si, " ", uv, " ", cu(bit_length(dmask) - 1)});
            std::vector<S> comps;
            for (int c = 0; c < 4; ++c) {
                S comp = tmp();
                emit({comp, " = OpCompositeExtract %float ", res, " ", D(c)});
                comps.push_back(comp);
            }
            if (d16) {
                for (int i = 0; i < 4; i += 2) {
                    S v = tmp(), p = tmp();
                    emit({v, " = OpCompositeConstruct %v2float ", comps[i], " ", comps[i + 1]});
                    emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", v});
                    dst(vreg(reg_r(vdata) + i / 2), p);
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    S u = tou(comps[i]);
                    dst(vreg(reg_r(vdata) + i), u);
                }
            }
            return;
        }
        S kind = op.size() > 12 ? op.substr(12) : S();  // '', '_lz', '_l', '_b', '_c_lz', '_d', '_d_cl'
        std::optional<S> minlod;
        if (kind == "_d_cl") {
            // Same as _d plus a trailing min-LOD clamp coordinate.
            minlod = at(f, -1);
            f = slice(f, 0, -1);
            kind = "_d";
            uses_minlod = true;
        }
        S ref;
        if (starts(kind, "_c")) {
            ref = at(f, 0);
            f = slice(f, 1);
        }
        S bias;
        if (kind == "_b" || kind == "_c_b") {
            bias = at(f, 0);
            f = slice(f, 1);
        }
        std::pair<S, S> grads;
        if (kind == "_d") {
            // Address: gradients first, then coords. 2D and 2D-array have 2-component gradients (the array layer has
            // none): ds/dx, dt/dx, ds/dy, dt/dy. 1D has scalar ds/dx, ds/dy.
            if (ncoord == 1) {
                if (f.size() < 3) throw Unsupported(op + " coords");
                grads = {f[0], f[1]};
                f = slice(f, 2);
            } else {
                if (static_cast<int>(f.size()) < 4 + ncoord) throw Unsupported(op + " coords");
                S gx = tmp(), gy = tmp();
                emit({gx, " = OpCompositeConstruct %v2float ", f[0], " ", f[1]});
                emit({gy, " = OpCompositeConstruct %v2float ", f[2], " ", f[3]});
                grads = {gx, gy};
                f = slice(f, 4);
            }
        }
        if (static_cast<int>(f.size()) < ncoord) throw Unsupported(op + " coords");
        S uv;
        if (ncoord == 1) {
            uv = f[0];
        } else if (ncoord == 3) {
            // The array layer arrives as a float coordinate; Vulkan rounds it the same way.
            uv = tmp();
            emit({uv, " = OpCompositeConstruct %v3float ", f[0], " ", f[1], " ", f[2]});
        } else {
            uv = tmp();
            emit({uv, " = OpCompositeConstruct %v2float ", f[0], " ", f[1]});
        }
        res = tmp();
        if (kind == "_d") {
            if (minlod)
                emit({res, " = OpImageSampleExplicitLod %v4float ", si, " ", uv, " Grad|MinLod ", grads.first, " ",
                      grads.second, " ", *minlod});
            else
                emit({res, " = OpImageSampleExplicitLod %v4float ", si, " ", uv, " Grad ", grads.first, " ",
                      grads.second});
        } else if (kind.empty()) {
            if (stage == "ps")
                emit({res, " = OpImageSampleImplicitLod %v4float ", si, " ", uv});
            else
                emit({res, " = OpImageSampleExplicitLod %v4float ", si, " ", uv, " Lod %f0"});
        } else if (kind == "_lz") {
            emit({res, " = OpImageSampleExplicitLod %v4float ", si, " ", uv, " Lod %f0"});
        } else if (kind == "_l") {
            emit({res, " = OpImageSampleExplicitLod %v4float ", si, " ", uv, " Lod ", f.size() > 2 ? f[2] : "%f0"});
        } else if (kind == "_b" && stage == "ps") {
            emit({res, " = OpImageSampleImplicitLod %v4float ", si, " ", uv, " Bias ", bias});
        } else if (kind == "_c_lz") {
            S d = tmp();
            if (d3d()) {
                // HLSL types a sampler as comparison or not: depth compares read a second view of the same heap.
                uses_cmp_sampler = true;
                sp = tmp();
                smp = tmp();
                si = tmp();
                emit({sp, " = OpAccessChain %_ptr_UC_smp %samplers_cmp ", smp_idx});
                emit({smp, " = OpLoad %smp ", sp});
                emit({si, " = OpSampledImage %", kd.stype, " ", img, " ", smp});
            }
            emit({d, " = OpImageSampleDrefExplicitLod %float ", si, " ", uv, " ", ref, " Lod %f0"});
            S u = tou(d);
            dst(vreg(reg_r(vdata)), u);
            return;
        } else {
            throw Unsupported(op);
        }
        ncomp = popcount(static_cast<uint64_t>(dmask));
    }
    std::vector<S> comps;
    for (int c = 0; c < 4; ++c)
        if (dmask >> c & 1) {
            S comp = tmp();
            emit({comp, " = OpCompositeExtract %float ", res, " ", D(c)});
            comps.push_back(comp);
        }
    if (static_cast<int>(comps.size()) != ncomp) throw Unsupported("dmask");
    if (d16) {
        // d16: components are packed two per register as f16 (x|y<<16, z|w<<16).
        for (size_t i = 0; i < comps.size(); i += 2) {
            S second = i + 1 < comps.size() ? comps[i + 1] : S("%f0");
            S v = tmp(), p = tmp();
            emit({v, " = OpCompositeConstruct %v2float ", comps[i], " ", second});
            emit({p, " = OpExtInst %uint %glsl PackHalf2x16 ", v});
            dst(vreg(reg_r(vdata) + static_cast<int64_t>(i / 2)), p);
        }
    } else {
        for (size_t i = 0; i < comps.size(); ++i) {
            S u = tou(comps[i]);
            dst(vreg(reg_r(vdata) + static_cast<int64_t>(i)), u);
        }
    }
}

void Gen::image_get_lod(const Args& a, const Mods& mods) {
    // image_get_lod vD, vaddr, T#, S#: (clamped lod, unclamped lod) like OpImageQueryLod (2D).
    const S dim = first_mod(mods, "dim:", "dim:SQ_RSRC_IMG_2D");
    if (dim != "dim:SQ_RSRC_IMG_2D" || stage != "ps" || has(mods, "a16")) throw Unsupported("image_get_lod form");
    const S dm = first_mod(mods, "dmask:", "dmask:0x3");
    const int64_t dmask = mod_int(dm);
    const Arg& vdata = at(a, 0);
    const std::vector<int> addr = addr_regs(at(a, 1));
    S img_idx = desc_lookup(sgpr_words(at(a, 2), 8));
    std::vector<S> sw = sgpr_words(at(a, 3), 4);
    S all_ones = cu(0xFFFFFFFF);
    for (int i = 0; i < 4; ++i) sw.push_back(all_ones);
    S smp_idx = desc_lookup(sw);
    S ip = tmp(), img = tmp(), sp = tmp(), smp = tmp(), si = tmp();
    emit({ip, " = OpAccessChain %_ptr_UC_img %images ", img_idx});
    emit({img, " = OpLoad %img2d ", ip});
    emit({sp, " = OpAccessChain %_ptr_UC_smp %samplers ", smp_idx});
    emit({smp, " = OpLoad %smp ", sp});
    emit({si, " = OpSampledImage %simg2d ", img, " ", smp});
    std::vector<S> f;
    for (int r : slice(addr, 0, 2)) {
        S u = load_reg("vgpr", r);
        f.push_back(tof(u));
    }
    S uv = tmp(), lod = tmp();
    emit({uv, " = OpCompositeConstruct %v2float ", at(f, 0), " ", at(f, 1)});
    emit({lod, " = OpImageQueryLod %v2float ", si, " ", uv});
    uses_query = true;
    int out = 0;
    for (int c = 0; c < 2; ++c)
        if (dmask >> c & 1) {
            S comp = tmp();
            emit({comp, " = OpCompositeExtract %float ", lod, " ", D(c)});
            S u = tou(comp);
            dst(vreg(reg_r(vdata) + out), u);
            ++out;
        }
}

void Gen::image_store(const Args& a, const Mods& mods) {
    // image_store vdata, vaddr, T# dmask dim: a formatted store, as the PC does it: a typed UAV of the texture in its
    // own format (float4 for float/unorm/snorm formats, uint4 for integer ones, chosen at run time from the T#'s
    // format). dmask names the components vdata holds, in order (d16: two per register); the others are 0.
    const S dim = after_img(first_mod(mods, "dim:", "dim:SQ_RSRC_IMG_2D"));
    if (!kStoreDims.count(dim) || has(mods, "a16")) throw Unsupported("image_store " + dim);
    const bool d16 = has(mods, "d16");
    const Arg& vdata = at(a, 0);
    const std::vector<int> addr = addr_regs(at(a, 1));
    std::vector<S> t = sgpr_words(at(a, 2), 8);
    S idx = desc_lookup(t);
    const int count = kStoreDims.at(dim);
    if (static_cast<int>(addr.size()) < count) throw Unsupported("image_store coords");
    std::vector<S> coords;
    for (int r : slice(addr, 0, count)) {
        S u = load_reg("vgpr", r);
        coords.push_back(as_int(u));
    }
    S iv;
    if (count == 1) {
        iv = coords[0];
    } else {
        iv = tmp();
        emit({iv, " = OpCompositeConstruct ", count == 2 ? "%v2int" : "%v3int", " ", join(coords)});
    }
    const S dm = first_mod(mods, "dmask:", "dmask:0xf");
    const int64_t dmask = mod_int(dm);
    std::vector<S> fcomp(4, "%f0"), ucomp(4, cu(0));
    int k = 0;
    for (int c = 0; c < 4; ++c) {
        if (!(dmask >> c & 1)) continue;
        if (d16) {
            S reg = load_reg("vgpr", reg_r(vdata) + k / 2);
            S u = tmp();
            if (k % 2)
                emit({u, " = OpShiftRightLogical %uint ", reg, " ", cu(16)});
            else
                emit({u, " = OpBitwiseAnd %uint ", reg, " ", cu(0xFFFF)});
            S h = tmp(), f = tmp();
            emit({h, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
            emit({f, " = OpCompositeExtract %float ", h, " 0"});
            fcomp[c] = f;
            ucomp[c] = u;
        } else {
            S u = load_reg("vgpr", reg_r(vdata) + k);
            fcomp[c] = tof(u);
            ucomp[c] = u;
        }
        ++k;
    }
    S fv = tmp(), uv = tmp();
    emit({fv, " = OpCompositeConstruct %v4float ", join(fcomp)});
    emit({uv, " = OpCompositeConstruct %v4uint_st ", join(ucomp)});
    // an integer format? one bit per format code, 32 codes a word
    S fmt = tmp(), word = tmp(), bit = tmp();
    emit({fmt, " = OpBitFieldUExtract %uint ", at(t, 1), " ", cu(20), " ", cu(9)});
    emit({word, " = OpShiftRightLogical %uint ", fmt, " ", cu(5)});
    emit({bit, " = OpBitwiseAnd %uint ", fmt, " ", cu(31)});
    uint32_t masks[10] = {};
    for (int code : int_formats())
        if (code < 320) masks[code >> 5] |= 1u << (code & 31);
    S m = cu(0);
    for (int w = 0; w < 10; ++w)
        if (masks[w]) {
            S e = tmp(), sel = tmp();
            emit({e, " = OpIEqual %bool ", word, " ", cu(w)});
            emit({sel, " = OpSelect %uint ", e, " ", cu(masks[w]), " ", m});
            m = sel;
        }
    S sh = tmp(), one = tmp(), is_int = tmp();
    emit({sh, " = OpShiftRightLogical %uint ", m, " ", bit});
    emit({one, " = OpBitwiseAnd %uint ", sh, " ", cu(1)});
    emit({is_int, " = OpINotEqual %bool ", one, " ", cu(0)});
    S act = active_b();  // computed before OpSelectionMerge (the merge directly precedes its branch)
    S base = tmp();
    const S do_ = base + "_do", end = base + "_end", fl = base + "_f", ul = base + "_u", fsel = base + "_sel";
    typed_stores.insert({dim, "f"});
    typed_stores.insert({dim, "u"});
    S tag = dim;
    for (auto& ch : tag)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    emit({"OpSelectionMerge ", end, " None"});
    emit({"OpBranchConditional ", act, " ", do_, " ", end});
    emit({do_, " = OpLabel"});
    emit({"OpSelectionMerge ", fsel, " None"});
    emit({"OpBranchConditional ", is_int, " ", ul, " ", fl});
    const std::tuple<S, S, S> branches[] = {{fl, "f", fv}, {ul, "u", uv}};
    for (const auto& [lbl, kind, val] : branches) {
        S ip = tmp(), img = tmp();
        emit({lbl, " = OpLabel"});
        emit({ip, " = OpAccessChain %_ptr_UC_st_", kind, tag, " %st_", kind, tag, " ", idx});
        emit({img, " = OpLoad %st_img_", kind, tag, " ", ip});
        emit({"OpImageWrite ", img, " ", iv, " ", val});
        emit({"OpBranch ", fsel});
    }
    emit({fsel, " = OpLabel"});
    emit({"OpBranch ", end});
    emit({end, " = OpLabel"});
}

void Gen::image_misc(const S& op, const Args& a, const Mods& mods) {
    // image_get_resinfo / image_load_mip on 2D sampled images, image_atomic_* on 2D (and 3D) r32ui storage images.
    const S dim = first_mod(mods, "dim:", "dim:SQ_RSRC_IMG_2D");
    const bool is3d_atomic = dim == "dim:SQ_RSRC_IMG_3D" && is_image_atomic(op);
    if (op == "image_store") {
        image_store(a, mods);
        return;
    }
    if ((dim != "dim:SQ_RSRC_IMG_2D" && !is3d_atomic) || has(mods, "d16") || has(mods, "a16"))
        throw Unsupported(op + " " + after_img(dim));
    const Arg& vdata = at(a, 0);
    const std::vector<int> addr = addr_regs(at(a, 1));
    std::vector<S> t = sgpr_words(at(a, 2), 8);
    S idx = desc_lookup(t);
    if (is3d_atomic) {
        // 3D r32ui storage image atomics (binding 7).
        uses_storage3d = true;
        std::vector<S> coords;
        for (int r : slice(addr, 0, 3)) {
            S u = load_reg("vgpr", r);
            coords.push_back(as_int(u));
        }
        if (coords.size() < 3) throw Unsupported(op + " coords");
        S iv = tmp(), ip = tmp(), tp = tmp(), old = tmp(), safe = tmp();
        emit({iv, " = OpCompositeConstruct %v3int ", coords[0], " ", coords[1], " ", coords[2]});
        emit({ip, " = OpAccessChain %_ptr_UC_simg3 %simages3d ", idx});
        S val = load_reg("vgpr", reg_r(vdata));
        const auto& [spv, neutral] = key(kImgAtomics, op);
        emit({tp, " = OpImageTexelPointer %_ptr_Image_uint ", ip, " ", iv, " ", cu(0)});
        emit({safe, " = OpSelect %uint ", active_b(), " ", val, " ", cu(neutral)});
        emit({old, " = ", spv, " %uint ", tp, " ", cu(1), " ", cu(0), " ", safe});
        if (has(mods, "glc")) dst(vreg(reg_r(vdata)), old);
        return;
    }
    if (is_image_atomic(op)) {
        // Storage image (2D r32ui). Atomics: vdata holds the operand; with 'glc' the pre-op value comes back.
        uses_storage = true;
        std::vector<S> coords;
        for (int r : slice(addr, 0, 2)) {
            S u = load_reg("vgpr", r);
            coords.push_back(as_int(u));
        }
        S iv = tmp();
        emit({iv, " = OpCompositeConstruct %v2int ", at(coords, 0), " ", at(coords, 1)});
        S ip = tmp();
        emit({ip, " = OpAccessChain %_ptr_UC_simg %simages ", idx});
        S val = load_reg("vgpr", reg_r(vdata));
        const auto& [spv, neutral] = key(kImgAtomics, op);
        S tp = tmp(), old = tmp(), safe = tmp();
        emit({tp, " = OpImageTexelPointer %_ptr_Image_uint ", ip, " ", iv, " ", cu(0)});
        emit({safe, " = OpSelect %uint ", active_b(), " ", val, " ", cu(neutral)});
        emit({old, " = ", spv, " %uint ", tp, " ", cu(1), " ", cu(0), " ", safe});
        if (has(mods, "glc")) dst(vreg(reg_r(vdata)), old);
        return;
    }
    S ip = tmp(), img = tmp();
    emit({ip, " = OpAccessChain %_ptr_UC_img %images ", idx});
    emit({img, " = OpLoad %img2d ", ip});
    const S dm = first_mod(mods, "dmask:", "dmask:0x1");
    const int64_t dmask = mod_int(dm);
    uses_query = true;
    if (op == "image_get_resinfo") {
        S lod = as_int(load_reg("vgpr", at(addr, 0)));
        S sz = tmp(), lv = tmp();
        emit({sz, " = OpImageQuerySizeLod %v2int ", img, " ", lod});
        emit({lv, " = OpImageQueryLevels %int ", img});
        S w = tmp(), h = tmp();
        emit({w, " = OpCompositeExtract %int ", sz, " 0"});
        emit({h, " = OpCompositeExtract %int ", sz, " 1"});
        const std::vector<S> vals = {w, h, as_int(cu(1)), lv};  // x=width, y=height, z=depth(1), w=mip levels
        int out = 0;
        for (int c = 0; c < 4; ++c)
            if (dmask >> c & 1) {
                S u = tmp();
                emit({u, " = OpBitcast %uint ", vals[c]});
                dst(vreg(reg_r(vdata) + out), u);
                ++out;
            }
        return;
    }
    // image_load_mip: x, y, mip
    if (addr.size() < 3) throw Unsupported("image_load_mip coords");
    std::vector<S> xs;
    for (int r : slice(addr, 0, 3)) {
        S u = load_reg("vgpr", r);
        xs.push_back(as_int(u));
    }
    S iv = tmp(), res = tmp();
    emit({iv, " = OpCompositeConstruct %v2int ", xs[0], " ", xs[1]});
    emit({res, " = OpImageFetch %v4float ", img, " ", iv, " Lod ", xs[2]});
    int out = 0;
    for (int c = 0; c < 4; ++c)
        if (dmask >> c & 1) {
            S comp = tmp();
            emit({comp, " = OpCompositeExtract %float ", res, " ", D(c)});
            S u = tou(comp);
            dst(vreg(reg_r(vdata) + out), u);
            ++out;
        }
}

}  // namespace fp::xlat
