// The instruction handlers Gen::insn_ (ops.cpp) dispatches to: rdna2spirv.Gen._insn's branches, one function each,
// grouped by family in ops_*.cpp. Private to the xlat ops files.
#pragma once
#include <initializer_list>
#include <string_view>
#include <vector>

#include "gen.h"

namespace fp::xlat::ops {

// One instruction as _insn sees it: op = base_op(mnemonic), a = its operands, mods = its modifiers.
struct Op {
    Gen& g;
    const Ins& ins;
    const S& op;
    const Args& a;
    const Mods& mods;
};
using Handler = void (*)(const Op&);

// ---- shared helpers (ops.cpp)
std::vector<S> tmps(Gen& g, int k);  // (self.tmp() for _ in range(k))
// `x, y, z = (self.src_u(t) for t in a[lo:hi])`: the sources in order, then ValueError unless there are `want`.
std::vector<S> srcs_u(Gen& g, const Args& a, int64_t lo, int64_t hi, size_t want);
std::vector<S> srcs_f(Gen& g, const Args& a, int64_t lo, int64_t hi, size_t want);
// An f16 source: the low half of the operand without neg/abs, unpacked, then its neg/abs applied as f32.
S f16_src(Gen& g, const Arg& s);
// z = (v != 0) ? 1 : 0; OpStore %scc z
void scc_nz(Gen& g, const S& v);
bool contains(std::string_view s, std::string_view sub);
bool one_of(const S& op, std::initializer_list<const char*> names);
int64_t reg(const Arg& a);    // a["r"] (KeyError for an operand without registers)
int64_t imm_v(const Arg& a);  // a["v"]
bool any_op_sel(const Mods& mods);
Arg vcc_arg();                // {"k": "special", "t": "vcc"}

// ---- vector float (ops_vector_float.cpp)
void max3_min3_f32(const Op&);
void sin_cos(const Op&);
void rndne_f32(const Op&);
void madak_madmk(const Op&);
void cube(const Op&);
void mad_legacy_f32(const Op&);
void mad_f32(const Op&);
void interp_f32(const Op&);
void cvt_pk_u8_f32(const Op&);
void rcp_iflag_f32(const Op&);
void f32_binary(const Op&);
void subrev_f32(const Op&);
void fma_f32(const Op&);
void f32_unary(const Op&);
void cvt_f32_int(const Op&);
void cvt_f32_ubyte(const Op&);
void cvt_int_f32(const Op&);
bool is_f32_binary(const S& op);  // op in F2
bool is_f32_unary(const S& op);   // op in F1

// ---- half precision and 16-bit (ops_half.cpp)
void cvt_f16_f32(const Op&);
void pack_b32_f16(const Op&);
void fmac_f16(const Op&);
void int16(const Op&);
void fma_f16(const Op&);
void pk_f16(const Op&);
void med3_f16(const Op&);
void f16_unary(const Op&);
void f16_binary(const Op&);
void fma_mix_f32(const Op&);
void cvt_f32_f16(const Op&);
void interp_f16(const Op&);
void fma_mixlo_hi(const Op&);
void cvt_pkrtz(const Op&);

// ---- vector integer and bits (ops_vector_int.cpp)
void and_or_b32(const Op&);
void three_int(const Op&);
void find_bit(const Op&);
void bit_reverse(const Op&);
void bfm(const Op&);
void subrev_nc_u32(const Op&);
void alignbit(const Op&);
void lshlrev_b64(const Op&);
void bcnt_u32(const Op&);
void xad_u32(const Op&);
void not_b32(const Op&);
void mul_hi_i32(const Op&);
void cmp_64(const Op&);
void add_co(const Op&);
void minmax3_int(const Op&);
void med3_int(const Op&);
void mul_lo_minmax(const Op&);
void perm(const Op&);
void mul_hi_u32(const Op&);
void binary_u32(const Op&);
void shift_rev(const Op&);
void bfe_u32(const Op&);
void mad_u32_u24(const Op&);
void cndmask(const Op&);
void movrel(const Op&);
bool is_binary_u32(const S& op);  // op in U2 or op in S2
bool is_shift_rev(const S& op);   // op in SHIFT_REV

// ---- scalar (ops_scalar.cpp)
void mulk(const Op&);
void addk(const Op&);
void bcnt1(const Op&);
void ff1_b64(const Op&);
void bitset1(const Op&);
void bitset0(const Op&);
void min_max_i32(const Op&);
void not_b64(const Op&);
void s_not_b32(const Op&);
void cmp_u64(const Op&);
void shift_b64(const Op&);
void ashr_i32(const Op&);
void add_carry(const Op&);
void getpc(const Op&);
void add_i32(const Op&);
void wqm(const Op&);
void movk(const Op&);
void saveexec(const Op&);
void wrexec(const Op&);
void bfe_s(const Op&);
void logic_b64(const Op&);
void cselect_b64(const Op&);
void cmpk(const Op&);

// ---- lanes, subgroups and the workgroup (ops_lanes.cpp)
void readfirstlane(const Op&);
void mbcnt(const Op&);
void sendmsg(const Op&);
void barrier(const Op&);
void readlane(const Op&);
void writelane(const Op&);
void permlane(const Op&);

}  // namespace fp::xlat::ops
