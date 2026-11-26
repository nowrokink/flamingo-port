// The SPIR-V generator (rdna2spirv.Gen): its state, and the helpers every gen_*.cpp / ops_*.cpp file uses.
//
// Porting rules, so the output stays the Python's byte for byte:
//  - Lines are built with cat({...}) / emit({...}): a braced list is evaluated left to right, as an f-string is.
//    Never pass two calls that emit or take a tmp() as arguments of one function call (their order is unspecified).
//  - tmp() numbers (%tN) and the order of emitted lines are the output: call them in the Python's order.
//  - Constants (cu) are a sorted set: the order they are registered in doesn't matter, only that they are.
//  - Python's a[i] / a[-1] / a[1:4] are at(a, i) / at(a, -1) / slice(a, 1, 4): out of range raises
//    PyError("IndexError") as Python does; a missing dict key is PyError("KeyError"), a bad int() "ValueError".
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ir.h"
#include "translator.h"

namespace fp::xlat {

using S = std::string;
using Args = std::vector<Arg>;
using Mods = std::vector<std::string>;

// ---- text
S cat(std::initializer_list<std::string_view> parts);
S D(int64_t v);   // decimal
S X(uint64_t v);  // lowercase hex, no prefix
S join(const std::vector<S>& items, std::string_view sep = " ");
bool starts(std::string_view s, std::string_view prefix);
bool ends(std::string_view s, std::string_view suffix);
std::vector<S> split(std::string_view s, char sep);           // Python's str.split(sep)
std::vector<S> split_ws(std::string_view s);                  // Python's str.split()
S rstrip(std::string_view s, std::string_view chars);         // Python's str.rstrip(chars)
int64_t py_int(std::string_view s, int base = 10);            // Python's int(s, base); base 0 reads 0x / 0o / 0b
uint32_t f32_bits(double x);                                  // struct.pack("<f", x) as a uint
int popcount(uint64_t v);

// ---- Python-style access
[[noreturn]] void index_error();
[[noreturn]] void key_error();
template <class T>
const T& at(const std::vector<T>& v, int64_t i) {
    if (i < 0) i += static_cast<int64_t>(v.size());
    if (i < 0 || i >= static_cast<int64_t>(v.size())) index_error();
    return v[static_cast<size_t>(i)];
}
template <class T>
std::vector<T> slice(const std::vector<T>& v, int64_t lo, int64_t hi = INT64_MAX) {
    const int64_t n = static_cast<int64_t>(v.size());
    if (lo < 0) lo = std::max<int64_t>(0, lo + n);
    if (hi < 0) hi = std::max<int64_t>(0, hi + n);
    lo = std::min(lo, n);
    hi = std::min(hi, n);
    return lo < hi ? std::vector<T>(v.begin() + lo, v.begin() + hi) : std::vector<T>{};
}
template <class M, class K>
const typename M::mapped_type& key(const M& m, const K& k) {
    auto it = m.find(k);
    if (it == m.end()) key_error();
    return it->second;
}

// ---- modifiers (an instruction's mods: "glc", "offset:16", "dmask:0x3", "op_sel:[0,1,0]" ...)
bool has(const Mods& mods, std::string_view word);
// The first modifier starting with `prefix`, else `fallback` (next((m for m in mods if m.startswith(p)), d)).
std::optional<S> first_mod(const Mods& mods, std::string_view prefix);
S first_mod(const Mods& mods, std::string_view prefix, std::string_view fallback);
// The "[a,b,c]" list of a modifier such as "op_sel:[0,1,0]".
std::vector<int64_t> mod_bits(const S& mod);
// next((int(m.split(":")[1], 0) for m in mods if m.startswith("offset:")), 0)
int64_t inst_offset(const Mods& mods);

// The SDWA source/destination selects: name -> (bit offset, width).
const std::map<S, std::pair<int, int>>& sdwa_sel();

// An insertion-ordered map (a Python dict): set() keeps a key's first position.
template <class K, class V>
struct Ordered {
    std::vector<std::pair<K, V>> items;
    void set(const K& k, const V& v) {
        for (auto& [a, b] : items)
            if (a == k) {
                b = v;
                return;
            }
        items.emplace_back(k, v);
    }
    bool empty() const { return items.empty(); }
    auto begin() const { return items.begin(); }
    auto end() const { return items.end(); }
};

struct ComputeRegs {  // compute_regs(): the slot-67 desc's +8 block
    int size[3] = {1, 1, 1};
    int user_sgprs = 0;
    int tgid[3] = {0, 0, 0};
    int tg_size = 0;
    int tid_comps = 1;
    int scratch = 0;
    int lds_dwords = 0;
};

struct Mesh {         // Gen.mesh: {"group": invocations, "params": exported parameter indices}
    int group = 0;
    std::set<int> params;
};

// The user-data dword i's OpAccessChain indices after %user_data (s0..s31, heap base lo/hi, code base lo/hi).
S ud_word(int i, Target target);

constexpr int USER_SGPRS = 32;
constexpr int NULL_CHUNK = 64;        // d3d12: the chunk-array index of the null descriptor
constexpr int64_t LOOP_LIMIT = 1 << 16;
constexpr int MS_GROUP = 128;
constexpr int kFront = -1;            // frag_inputs: the register holding the front-facing sign

// The GFX10 format names by code (GFX10_FORMATS) and the buffer formats the decoder handles (Gen.FORMATS).
const std::map<int, S>& gfx10_formats();
struct BufFormat {
    int bytes;
    S kind;                   // "f32" "u32" "f16" "unorm" "snorm" "uint"
    std::vector<int> widths;  // component bit widths
};
// In Python's dict order (the decoders are emitted in it).
const std::vector<std::pair<int, BufFormat>>& buffer_formats();
const BufFormat* buffer_format(int code);

class Gen {
public:
    Gen(S stage, Target target) : stage(std::move(stage)), target(target) {}

    // ---- state (Gen.__init__)
    S stage;
    Target target;
    std::vector<S> body;
    std::set<uint32_t> consts;
    int n = 0;
    Ordered<S, S> outputs;   // variable -> export target
    Ordered<S, int> inputs;  // variable -> location
    bool uses_psb = false, uses_mem = false;
    std::unordered_map<S, S> psb_base;
    std::vector<S> nonuniform;
    bool uses_active = false, uses_tex = false, uses_subgroup = false, uses_tex_array = false,
         uses_cmp_sampler = false;
    S lookup_fn;
    bool uses_storage = false, uses_query = false, uses_tex_1d = false, uses_tex_3d = false, uses_v4uint = false,
         writes_depth = false, uses_smul = false, uses_minlod = false, uses_storage3d = false, uses_heap = false,
         uses_codebase = false, uses_long = false;
    std::vector<std::pair<int, int>> vertex_inputs;  // (D3D input register, component type 1=uint 2=sint 3=float)
    std::vector<std::pair<int, int>> frag_inputs;    // VGPR -> gl_FragCoord component, or kFront
    std::optional<ComputeRegs> compute;
    std::set<std::pair<S, S>> typed_stores;          // (dim, "f"/"u")
    bool uses_lds = false;
    std::optional<Mesh> mesh;
    int64_t lds_dwords = 0;
    std::vector<std::vector<S>> blocks;
    S block_label = "%entry";

    bool d3d() const { return target == Target::d3d12; }

    // ---- core (gen_regs.cpp)
    S tmp();
    S cu(int64_t v);
    void emit(std::string_view s);
    void emit(std::initializer_list<std::string_view> parts) { emit(cat(parts)); }
    std::vector<S> mul_extended(const S& res, const S& op, const S& a, const S& b);
    void mem_access(const S& result, const S& base, const S& index);
    S load_reg(std::string_view kind, int64_t r);
    void store_reg(std::string_view kind, int64_t r, const S& val);
    S src_u(const Arg& a);
    S modify(S v, const Arg& a);
    void write_lo16(const Arg& d, const S& lo16, bool preserve_high = true);
    S f16_rtz(const S& f);
    S modify_f(const S& f, const Arg& a);
    S host_addr(const S& xbox_va);
    S tof(const S& u);
    S tou(const S& f);
    S src_f(const Arg& a);
    S active_b();
    S exec_value();
    void set_exec(const S& mask_u);
    void dst(const Arg& a, S val);
    void mov64(const Arg& d, const Arg& s);
    S tmp_bool_nz(const S& v);
    S mask_src(const Arg& a);
    void mask_dst(const Arg& d, const S& v);
    S clamp_mods(S v, const Mods& mods);
    S fconst(double x);
    S as_int(const S& u);
    S tmp_and3(const S& v);
    S current_label_placeholder();
    void guarded(const S& cond, const std::function<void()>& body);
    void insn(const Ins& ins);         // SDWA operand marking, then insn_
    S sdwa_src(const S& v, const Arg& a);
    S sdwa_dst(const Arg& d, const S& val);
    static Arg vreg(int64_t r);        // {"k": "vgpr", "r": r, "n": 1}
    static Arg imm(int64_t v);         // {"k": "imm", "v": v}
    static Arg plain(Arg a);           // {**a, "neg": False, "abs": False}

    // ---- compares, exports, attributes (gen_export.cpp)
    void compare(const S& op, const Args& a, bool scalar, bool exec_only = false);
    void compare_x(const S& op, const Args& a);
    void cmp_class(const Args& a);
    S attribute(int n, int comp);
    void export_(const Ins& ins);

    // ---- memory (gen_memory.cpp)
    std::pair<S, S> lds_ptr(const S& byte_addr);
    S lds_addr(const S& base, int64_t off);
    void lds(const S& op, const Args& a, const Mods& mods);
    void swizzle(const Args& a, const Mods& mods, const Ins& ins);
    void buffer_load(const S& op, const Args& a, const Mods& mods);
    std::vector<S> decode_format(const std::vector<S>& words, int code);
    void buffer_atomic(const S& op, const Args& a, const Mods& mods);
    void buffer_load_format(const S& op, const Args& a, const Mods& mods, std::optional<int> fixed_fmt = {});
    void global_mem(const S& op, const Args& a, const Mods& mods);
    std::pair<S, S> buffer_address(const Args& a, const Mods& mods);
    S buffer_in_range(const Args& a, const Mods& mods, int64_t end);
    S active_and(const S& cond);
    void buffer_store(const S& op, const Args& a, const Mods& mods);
    void scalar_load(const S& op, const Args& a, const Mods& mods);

    // ---- images (gen_image.cpp)
    static constexpr int PROBES = 8;
    S desc_lookup_function();
    S desc_lookup(const std::vector<S>& words);
    std::vector<S> sgpr_words(const Arg& a, int count);
    std::vector<int> addr_regs(const Arg& a);
    void image_op(const S& op, const Args& a, const Mods& mods);
    void image_get_lod(const Args& a, const Mods& mods);
    void image_store(const Args& a, const Mods& mods);
    void image_misc(const S& op, const Args& a, const Mods& mods);
    static bool is_image_atomic(const S& op);  // op in IMG_ATOMICS

    // ---- the instruction dispatch (ops.cpp) and its families (ops_*.cpp)
    void insn_(const Ins& ins);
};

// rdna2spirv.base_op: the mnemonic without _e32 / _e64 / _sdwa / _dpp.
S base_op(const S& op);
// The branch mnemonics that end a basic block (BRANCHES).
bool is_branch(const S& op);
// branch_condition(g, op): whether the branch is taken for this lane (a bool id).
S branch_condition(Gen& g, const S& op);

// assemble.cpp
S assemble(Gen& gen);
S dedupe_vectors(const S& text, Target target);
S bindless_spaces(const S& text);

}  // namespace fp::xlat
