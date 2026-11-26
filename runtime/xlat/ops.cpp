// Gen::insn_: rdna2spirv.Gen._insn's elif chain, in its order (an op two branches would take goes to the first), each
// branch a handler of ops_*.cpp or a Gen method of the gen_*.cpp files.
#include "ops.h"

namespace fp::xlat {
namespace ops {

std::vector<S> tmps(Gen& g, int k) {
    std::vector<S> out;
    for (int i = 0; i < k; ++i) out.push_back(g.tmp());
    return out;
}

std::vector<S> srcs_u(Gen& g, const Args& a, int64_t lo, int64_t hi, size_t want) {
    std::vector<S> out;
    for (const Arg& x : slice(a, lo, hi)) out.push_back(g.src_u(x));
    if (out.size() != want) throw PyError("ValueError");
    return out;
}

std::vector<S> srcs_f(Gen& g, const Args& a, int64_t lo, int64_t hi, size_t want) {
    std::vector<S> out;
    for (const Arg& x : slice(a, lo, hi)) out.push_back(g.src_f(x));
    if (out.size() != want) throw PyError("ValueError");
    return out;
}

S f16_src(Gen& g, const Arg& s) {
    S u = g.src_u(Gen::plain(s));
    S v2 = g.tmp(), f = g.tmp();
    g.emit({v2, " = OpExtInst %v2float %glsl UnpackHalf2x16 ", u});
    g.emit({f, " = OpCompositeExtract %float ", v2, " 0"});
    return g.modify_f(f, s);
}

void scc_nz(Gen& g, const S& v) {
    S z = g.tmp();
    g.emit({z, " = OpSelect %uint ", g.tmp_bool_nz(v), " ", g.cu(1), " ", g.cu(0)});
    g.emit({"OpStore %scc ", z});
}

bool contains(std::string_view s, std::string_view sub) { return s.find(sub) != std::string_view::npos; }

bool one_of(const S& op, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (op == n) return true;
    return false;
}

int64_t reg(const Arg& a) {
    if (a.k != "vgpr" && a.k != "sgpr") key_error();
    return a.r;
}

int64_t imm_v(const Arg& a) {
    if (a.k != "imm") key_error();
    return a.v;
}

bool any_op_sel(const Mods& mods) {
    for (const S& m : mods)
        if (starts(m, "op_sel")) return true;
    return false;
}

Arg vcc_arg() {
    Arg a;
    a.k = "special";
    a.t = "vcc";
    return a;
}

namespace {

// Instructions with no effect here (rdna2spirv.NOOPS).
const char* const kNoops[] = {"v_nop", "s_nop", "s_inst_prefetch", "s_endpgm", "s_setprio", "s_clause",
                              "s_delay_alu", "s_waitcnt", "s_waitcnt_vscnt", "s_waitcnt_vmcnt", "s_waitcnt_expcnt",
                              "s_waitcnt_lgkmcnt", "s_waitcnt_depctr"};

bool is_noop(const S& op) {
    for (const char* n : kNoops)
        if (op == n) return true;
    return false;
}

// tbuffer_load_format_*: a typed load with the format in the instruction (format:[BUF_FMT_...]) instead of the V#.
void tbuffer_load(const Op& o) {
    auto fm = first_mod(o.mods, "format:");
    if (!fm) throw Unsupported(o.op + " without format");
    // "GFX10_FORMAT_" + fm.split("BUF_FMT_")[-1].rstrip("]")
    size_t k = fm->rfind("BUF_FMT_");
    S name = "GFX10_FORMAT_" + rstrip(k == S::npos ? *fm : fm->substr(k + 8), "]");
    std::optional<int> code;
    for (const auto& [c, n] : gfx10_formats())
        if (n == name) {
            code = c;
            break;
        }
    if (!code || !buffer_format(*code)) throw Unsupported(o.op + " " + name);
    o.g.buffer_load_format(o.op.substr(1), o.a, o.mods, *code);
}

}  // namespace
}  // namespace ops

void Gen::insn_(const Ins& ins) {
    using namespace ops;
    const S op = base_op(ins.op);
    const Args& a = ins.args;
    const Mods& mods = ins.mods;
    const Op o{*this, ins, op, a, mods};
    if (is_noop(op) || starts(op, "s_waitcnt")) return;
    if (one_of(op, {"s_setreg_imm32_b32", "s_setreg_b32", "s_denorm_mode", "s_round_mode"})) {
        // MODE register (rounding/denormals). SPIR-V float controls aren't modelled yet: Vulkan defaults are used,
        // which can differ in denormal handling. Recorded here so the gap is visible.
        return;
    }
    if (op == "v_mov_b32" || op == "s_mov_b32") {
        const Arg& d = at(a, 0);
        S v = src_u(at(a, 1));
        dst(d, v);
    } else if (op == "s_mov_b64") {
        mov64(at(a, 0), at(a, 1));
    } else if (one_of(op, {"v_max3_f32", "v_min3_f32"})) {
        max3_min3_f32(o);
    } else if (op == "v_and_or_b32") {
        and_or_b32(o);
    } else if (op == "s_brev_b32") {
        bit_reverse(o);
    } else if (op == "v_cvt_f16_f32") {
        cvt_f16_f32(o);
    } else if (op == "s_mulk_i32") {
        mulk(o);
    } else if (op == "v_readfirstlane_b32") {
        readfirstlane(o);
    } else if (one_of(op, {"image_sample", "image_sample_lz", "image_sample_l", "image_sample_b", "image_sample_c_lz",
                           "image_sample_d", "image_sample_d_cl", "image_load", "image_gather4_lz"})) {
        image_op(op, a, mods);
    } else if (one_of(op, {"v_sin_f32", "v_cos_f32"})) {
        sin_cos(o);
    } else if (op == "v_rndne_f32") {
        rndne_f32(o);
    } else if (one_of(op, {"v_madak_f32", "v_madmk_f32"})) {
        madak_madmk(o);
    } else if (one_of(op, {"v_bfi_b32", "v_lshl_or_b32", "v_lshl_add_u32", "v_add_lshl_u32", "v_or3_b32",
                           "v_xor3_b32", "v_add3_u32", "v_and_or_b32_"})) {
        three_int(o);
    } else if (op == "v_pack_b32_f16") {
        pack_b32_f16(o);
    } else if (op == "v_cmp_class_f32") {
        cmp_class(a);
    } else if (one_of(op, {"v_ffbl_b32", "v_ffbh_u32", "s_ff1_i32_b32", "s_flbit_i32_b32"})) {
        find_bit(o);
    } else if (one_of(op, {"image_get_resinfo", "image_load_mip", "image_store"}) || is_image_atomic(op)) {
        image_misc(op, a, mods);
    } else if (one_of(op, {"v_cubema_f32", "v_cubeid_f32", "v_cubesc_f32", "v_cubetc_f32"})) {
        cube(o);
    } else if (one_of(op, {"v_mbcnt_lo_u32_b32", "v_mbcnt_hi_u32_b32"})) {
        mbcnt(o);
    } else if (op == "v_bfrev_b32") {
        bit_reverse(o);
    } else if (op == "v_bfm_b32") {
        bfm(o);
    } else if (one_of(op, {"s_bcnt1_i32_b64", "s_bcnt1_i32_b32"})) {
        bcnt1(o);
    } else if (op == "v_subrev_nc_u32") {
        subrev_nc_u32(o);
    } else if (op == "s_ff1_i32_b64") {
        ff1_b64(o);
    } else if (op == "s_sendmsg" && mesh) {
        sendmsg(o);
    } else if (op == "s_barrier") {
        barrier(o);
    } else if (starts(op, "ds_") && op != "ds_swizzle_b32") {
        lds(op, a, mods);
    } else if (op == "ds_swizzle_b32") {
        swizzle(a, mods, ins);
    } else if (op == "v_readlane_b32") {
        readlane(o);
    } else if (op == "v_writelane_b32") {
        writelane(o);
    } else if (starts(op, "buffer_store_dword") ||
               one_of(op, {"buffer_store_format_x", "buffer_store_format_xy", "buffer_store_format_xyz",
                           "buffer_store_format_xyzw"})) {
        buffer_store(op, a, mods);
    } else if (one_of(op, {"v_permlanex16_b32", "v_permlane16_b32"})) {
        permlane(o);
    } else if (starts(op, "buffer_load_dword")) {
        buffer_load(op, a, mods);
    } else if (one_of(op, {"buffer_load_format_x", "buffer_load_format_xy", "buffer_load_format_xyz",
                           "buffer_load_format_xyzw"})) {
        buffer_load_format(op, a, mods);
    } else if (one_of(op, {"tbuffer_load_format_x", "tbuffer_load_format_xy", "tbuffer_load_format_xyz",
                           "tbuffer_load_format_xyzw"})) {
        tbuffer_load(o);
    } else if (op == "s_bitset1_b32") {
        bitset1(o);
    } else if (op == "v_alignbit_b32") {
        alignbit(o);
    } else if (op == "v_lshlrev_b64") {
        lshlrev_b64(o);
    } else if (op == "v_fmac_f16") {
        fmac_f16(o);
    } else if (one_of(op, {"v_lshrrev_b16", "v_lshlrev_b16", "v_ashrrev_i16", "v_add_u16", "v_sub_u16",
                           "v_subrev_u16", "v_mul_lo_u16", "v_max_u16", "v_min_u16", "v_max_i16", "v_min_i16",
                           "v_and_b16", "v_or_b16", "v_xor_b16"})) {
        int16(o);
    } else if (op == "v_bcnt_u32_b32") {
        bcnt_u32(o);
    } else if (op == "s_bitset0_b32") {
        bitset0(o);
    } else if (one_of(op, {"s_min_i32", "s_max_i32"})) {
        min_max_i32(o);
    } else if (op == "v_fma_f16") {
        fma_f16(o);
    } else if (one_of(op, {"v_pk_add_f16", "v_pk_mul_f16", "v_pk_max_f16", "v_pk_min_f16", "v_pk_fma_f16"})) {
        pk_f16(o);
    } else if (one_of(op, {"v_movrels_b32", "v_movreld_b32", "v_movrelsd_b32"})) {
        movrel(o);
    } else if (op == "v_med3_f16") {
        med3_f16(o);
    } else if (op == "v_xad_u32") {
        xad_u32(o);
    } else if (op == "s_not_b64") {
        not_b64(o);
    } else if (one_of(op, {"v_rsq_f16", "v_rcp_f16", "v_sqrt_f16", "v_exp_f16", "v_log_f16", "v_floor_f16",
                           "v_fract_f16", "v_ceil_f16", "v_trunc_f16", "v_rndne_f16"})) {
        f16_unary(o);
    } else if (op == "v_not_b32") {
        not_b32(o);
    } else if (op == "v_mul_hi_i32") {
        mul_hi_i32(o);
    } else if (one_of(op, {"v_add_f16", "v_sub_f16", "v_mul_f16", "v_max_f16", "v_min_f16"})) {
        f16_binary(o);
    } else if (starts(op, "v_cmp_") && (ends(op, "_u64") || ends(op, "_i64")) &&
               one_of(at(split(op, '_'), 2), {"eq", "ne", "lg"})) {
        cmp_64(o);
    } else if (op == "s_not_b32") {
        s_not_b32(o);
    } else if (one_of(op, {"s_cmp_eq_u64", "s_cmp_lg_u64"})) {
        cmp_u64(o);
    } else if (starts(op, "buffer_atomic_")) {
        buffer_atomic(op, a, mods);
    } else if (op == "image_get_lod") {
        image_get_lod(a, mods);
    } else if (op == "s_addk_i32") {
        addk(o);
    } else if (one_of(op, {"v_add_co_u32", "v_add_co_ci_u32", "v_sub_co_u32", "v_sub_co_ci_u32", "v_subrev_co_u32",
                           "v_subrev_co_ci_u32"})) {
        add_co(o);
    } else if (op == "v_mad_legacy_f32") {
        mad_legacy_f32(o);
    } else if (one_of(op, {"v_min3_i32", "v_min3_u32", "v_max3_i32", "v_max3_u32"})) {
        minmax3_int(o);
    } else if (one_of(op, {"v_med3_i32", "v_med3_u32"})) {
        med3_int(o);
    } else if (starts(op, "s_load_dword") || starts(op, "s_buffer_load_dword")) {
        scalar_load(op, a, ins.mods);
    } else if (one_of(op, {"s_lshl_b64", "s_lshr_b64"})) {
        shift_b64(o);
    } else if (op == "s_ashr_i32") {
        ashr_i32(o);
    } else if (starts(op, "global_load_dword") || starts(op, "global_store_dword")) {
        global_mem(op, a, mods);
    } else if (one_of(op, {"s_add_u32", "s_addc_u32", "s_sub_u32", "s_subb_u32"})) {
        add_carry(o);
    } else if (op == "s_getpc_b64") {
        getpc(o);
    } else if (one_of(op, {"s_add_i32", "s_sub_i32", "s_min_u32", "s_max_u32", "s_xor_b32", "s_andn2_b32",
                           "s_cselect_b32"})) {
        add_i32(o);
    } else if (one_of(op, {"s_wqm_b64", "s_wqm_b32"})) {
        wqm(o);
    } else if (op == "v_fma_mix_f32") {
        fma_mix_f32(o);
    } else if (op == "v_mad_f32") {
        mad_f32(o);
    } else if (one_of(op, {"v_mul_lo_u32", "v_min_u32", "v_max_u32", "v_min_i32", "v_max_i32"})) {
        mul_lo_minmax(o);
    } else if (op == "v_cvt_f32_f16") {
        cvt_f32_f16(o);
    } else if (one_of(op, {"v_interp_p1_f32", "v_interp_p2_f32", "v_interp_mov_f32"})) {
        interp_f32(o);
    } else if (op == "s_movk_i32") {
        movk(o);
    } else if (starts(op, "v_cmp_") && !starts(op, "v_cmp_class")) {
        compare(op, a, false);
    } else if (starts(op, "v_cmpx_") && !starts(op, "v_cmpx_class")) {
        compare_x(op, a);
    } else if (one_of(op, {"s_and_saveexec_b64", "s_or_saveexec_b64", "s_andn2_saveexec_b64", "s_and_saveexec_b32",
                           "s_or_saveexec_b32", "s_andn2_saveexec_b32"})) {
        saveexec(o);
    } else if (one_of(op, {"s_andn2_wrexec_b64", "s_andn2_wrexec_b32"})) {
        wrexec(o);
    } else if (one_of(op, {"s_bfe_u32", "s_bfe_i32"})) {
        bfe_s(o);
    } else if (op == "v_perm_b32") {
        perm(o);
    } else if (one_of(op, {"s_and_b64", "s_or_b64", "s_xor_b64", "s_andn2_b64", "s_orn2_b64", "s_and_b32_mask",
                           "s_or_b32_mask"})) {
        logic_b64(o);
    } else if (op == "s_cselect_b64") {
        cselect_b64(o);
    } else if (one_of(op, {"v_interp_p1ll_f16", "v_interp_p1lv_f16", "v_interp_p2_f16"})) {
        interp_f16(o);
    } else if (starts(op, "s_cmp_")) {
        compare(op, a, true);
    } else if (starts(op, "s_cmpk_")) {
        cmpk(o);
    } else if (op == "s_bfm_b32") {
        bfm(o);
    } else if (op == "s_mul_hi_u32") {
        mul_hi_u32(o);
    } else if (one_of(op, {"v_fma_mixlo_f16", "v_fma_mixhi_f16"})) {
        fma_mixlo_hi(o);
    } else if (op == "v_cvt_pk_u8_f32") {
        cvt_pk_u8_f32(o);
    } else if (op == "v_mul_hi_u32") {
        mul_hi_u32(o);
    } else if (op == "v_cvt_pkrtz_f16_f32") {
        cvt_pkrtz(o);
    } else if (op == "v_rcp_iflag_f32") {
        rcp_iflag_f32(o);
    } else if (is_f32_binary(op)) {
        f32_binary(o);
    } else if (op == "v_subrev_f32") {
        subrev_f32(o);
    } else if (one_of(op, {"v_fma_f32", "v_fmac_f32", "v_mac_f32", "v_fmaak_f32", "v_fmamk_f32", "v_med3_f32"})) {
        fma_f32(o);
    } else if (is_f32_unary(op)) {
        f32_unary(o);
    } else if (one_of(op, {"v_cvt_f32_u32", "v_cvt_f32_i32"})) {
        cvt_f32_int(o);
    } else if (starts(op, "v_cvt_f32_ubyte")) {
        cvt_f32_ubyte(o);
    } else if (one_of(op, {"v_cvt_u32_f32", "v_cvt_i32_f32"})) {
        cvt_int_f32(o);
    } else if (is_binary_u32(op)) {
        binary_u32(o);
    } else if (is_shift_rev(op)) {
        shift_rev(o);
    } else if (one_of(op, {"v_bfe_u32", "v_bfe_i32"})) {
        bfe_u32(o);
    } else if (op == "v_mad_u32_u24") {
        mad_u32_u24(o);
    } else if (op == "v_cndmask_b32") {
        cndmask(o);
    } else if (op == "exp") {
        export_(ins);
    } else {
        throw Unsupported(op);
    }
}

}  // namespace fp::xlat
