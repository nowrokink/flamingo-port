// One shader's translation as a whole (rdna2spirv.translate): its stage and stage records, the hardware inputs it
// reads, the code reachable from its entry, and its basic blocks run from one dispatch loop.
#include <bitset>
#include <cctype>
#include <cstring>

#include "core/fs.h"
#include "core/json.h"
#include "core/text.h"
#include "gen.h"
#include "shaders/dxbc.h"

namespace fp::xlat {
namespace {

const char* const kBranches[] = {"s_branch",       "s_cbranch_scc0",  "s_cbranch_scc1",  "s_cbranch_vccz",
                                 "s_cbranch_vccnz", "s_cbranch_execz", "s_cbranch_execnz"};

// dirname(dirname(realpath(path))): the folder holding index.json, <name>.dxbc and fssm/ (the .s is in disasm/).
std::wstring corpus_root(const std::wstring& s_path) { return parent(parent(full_path(s_path))); }

std::string read_or_raise(const std::wstring& path) {
    std::string b;
    if (!read_file(path, b)) throw PyError("FileNotFoundError");
    return b;
}

uint32_t rd32(const std::string& b, size_t at) { return dxbc::u32(reinterpret_cast<const uint8_t*>(b.data()) + at); }
uint16_t rd16(const std::string& b, size_t at) { return dxbc::u16(reinterpret_cast<const uint8_t*>(b.data()) + at); }

int64_t sext16(int64_t v) { return v & 0x8000 ? v - 0x10000 : v; }

// The 16-bit branch offset of a branch (its first immediate operand), 0 without one.
const Arg* first_imm(const Ins& ins) {
    for (const Arg& x : ins.args)
        if (x.k == "imm") return &x;
    return nullptr;
}

struct MeshInfo {
    int threads, vertices, primitives, user_sgprs, topology;
    bool wave32;
    uint32_t lds_bytes;
};

// A mesh shader's limits from its FSSM blob: the mesh record is at payload + 0x48 (payload = blob + 0x10); the stage
// data starts 0x54 after the record, and at +0x448 of it: u16 threads in a group, u16 maximum vertices, u16 maximum
// primitives, u8 user SGPRs, u8 topology (2 = triangles). The record's +0x14 bit 5 says wave32 (else wave64) and
// +0x4c the LDS bytes. None if the blob isn't there or has no mesh record.
std::optional<MeshInfo> mesh_info(const std::wstring& path, const std::string& name) {
    std::string b;
    if (!read_file(fp::join(fp::join(corpus_root(path), L"fssm"), widen(name + ".fssm")), b)) return std::nullopt;
    if (b.size() < 0x60 || b.compare(0, 4, "FSSM") != 0) return std::nullopt;
    const size_t pay = 0x10, pay_len = b.size() - pay;
    uint64_t rec;
    std::memcpy(&rec, b.data() + pay + 0x48, 8);
    if (rec == 0xDEADBEEFDEADBEEFull || rec > pay_len || rec + 0x54 + 0x450 > pay_len) return std::nullopt;
    const size_t s = pay + rec + 0x54 + 0x448;
    MeshInfo m{rd16(b, s), rd16(b, s + 2), rd16(b, s + 4), static_cast<uint8_t>(b[s + 6]),
               static_cast<uint8_t>(b[s + 7]), (rd32(b, pay + rec + 0x14) & 32) != 0, rd32(b, pay + rec + 0x4C)};
    return m;
}

// A compute shader's dispatch registers from the slot-67 desc's +8 block (0x1c bytes in 1.0.2.1), or None if the
// game hasn't made its pipeline yet. Layout, checked against every container's PSV0 NumThreads (1,949 shaders):
// dword 3 = NUM_THREAD_X | NUM_THREAD_Y << 16, dword 4 = NUM_THREAD_Z (low 16 bits), dword 5 = COMPUTE_PGM_RSRC1,
// dword 6 = COMPUTE_PGM_RSRC2 (SCRATCH_EN bit 0, USER_SGPR bits 1-5, TGID_X/Y/Z_EN bits 7-9, TG_SIZE_EN bit 10,
// TIDIG_COMP_CNT bits 11-12).
std::optional<ComputeRegs> compute_regs(const Options& opt, const std::wstring& path, const std::string& name) {
    if (opt.cs_regs.empty()) return std::nullopt;
    const std::string d = read_or_raise(fp::join(corpus_root(path), widen(name + ".dxbc")));
    auto digest = dxbc::hash_digest(d, true);
    if (!digest) return std::nullopt;
    std::string b;
    if (!read_file(fp::join(opt.cs_regs, widen(*digest + ".regs")), b)) return std::nullopt;
    if (b.size() < 28) return std::nullopt;
    uint32_t w[7];
    for (int i = 0; i < 7; ++i) w[i] = rd32(b, 4 * i);
    const uint32_t rsrc2 = w[6];
    ComputeRegs r;
    r.size[0] = static_cast<int>(w[3] & 0xFFFF);
    r.size[1] = static_cast<int>(w[3] >> 16);
    r.size[2] = static_cast<int>(w[4] & 0xFFFF);
    r.user_sgprs = static_cast<int>((rsrc2 >> 1) & 31);
    for (int i = 0; i < 3; ++i) r.tgid[i] = static_cast<int>((rsrc2 >> (7 + i)) & 1);
    r.tg_size = static_cast<int>((rsrc2 >> 10) & 1);
    r.tid_comps = static_cast<int>(((rsrc2 >> 11) & 3) + 1);
    r.scratch = static_cast<int>(rsrc2 & 1);
    // LDS_SIZE (bits 15-23) in 512-byte units (Mesa ac_shader_util: the GFX7+ encode granularity)
    r.lds_dwords = static_cast<int>(((rsrc2 >> 15) & 0x1FF) * 128);
    return r;
}

std::map<int64_t, size_t> pcs(const std::vector<Ins>& insns) {
    std::map<int64_t, size_t> by_pc;  // {pc: k}: a repeated pc keeps the last
    for (size_t k = 0; k < insns.size(); ++k) by_pc[insns[k].pc] = k;
    return by_pc;
}

// Instructions reachable from the first one, following fall-through and branch targets. Fall-through stops after
// s_endpgm, s_branch and s_setpc_b64. Raises Unsupported if a branch target isn't an instruction start.
std::vector<Ins> reachable_code(const std::vector<Ins>& insns) {
    const auto by_pc = pcs(insns);
    std::set<size_t> seen;
    std::vector<size_t> work = {0};
    while (!work.empty()) {
        std::optional<size_t> k = work.back();
        work.pop_back();
        while (k && *k < insns.size() && !seen.count(*k)) {
            seen.insert(*k);
            const Ins& ins = insns[*k];
            if (ins.data) throw Unsupported("undecodable words (code that isn't in the blob as decoded)");
            const S op = base_op(ins.op);
            const int64_t nxt = ins.next_pc();
            if (is_branch(op)) {
                const Arg* imm = first_imm(ins);
                const int64_t v = imm ? (imm->v & 0xFFFF) : 0;
                const int64_t tgt = nxt + 4 * sext16(v);
                auto it = by_pc.find(tgt);
                if (it == by_pc.end()) throw Unsupported("branch into data");
                work.push_back(it->second);
                if (op == "s_branch") break;
            }
            if (op == "s_endpgm" || op == "s_setpc_b64") break;
            auto it = by_pc.find(nxt);
            if (it == by_pc.end()) break;
            k = it->second;
        }
    }
    std::vector<Ins> out;
    for (size_t k : seen) out.push_back(insns[k]);
    return out;
}

bool reads_all_operands(const S& op) {
    for (const char* p : {"exp", "buffer_store", "image_store", "v_cmp", "v_cmpx", "ds_write", "global_store"})
        if (starts(op, p)) return true;
    return false;
}

bool accumulates(const S& b) { return b == "v_fmac_f32" || b == "v_mac_f32" || b == "v_fmac_f16"; }

// VGPRs a shader reads before writing them: for a fetch-calling VS these are the fetched attributes.
std::set<int> read_before_write_vgprs(const std::vector<Ins>& insns) {
    std::set<int> written;
    std::vector<int> first;
    for (const Ins& ins : insns) {
        const Args& a = ins.args;
        const S& op = ins.op;
        if (a.empty()) continue;
        const bool reads_all = reads_all_operands(op);
        std::vector<int> regs;
        for (size_t i = reads_all ? 0 : 1; i < a.size(); ++i) {
            const Arg& x = a[i];
            if (x.k == "vgpr")
                for (int r = x.r; r < x.r + x.n; ++r) regs.push_back(r);
            else if (x.k == "vlist")
                regs.insert(regs.end(), x.list.begin(), x.list.end());
        }
        if (accumulates(base_op(op)) && a[0].k == "vgpr") regs.push_back(a[0].r);
        for (int r : regs)
            if (!written.count(r) && std::find(first.begin(), first.end(), r) == first.end()) first.push_back(r);
        if (!reads_all && a[0].k == "vgpr")
            for (int r = a[0].r; r < a[0].r + a[0].n; ++r) written.insert(r);
    }
    return std::set<int>(first.begin(), first.end());
}

using Bits = std::bitset<512>;

void set_bit(Bits& b, int64_t r) {
    if (r >= 0 && r < 512) b.set(static_cast<size_t>(r));  // registers past 511 can't be in the result
}

// VGPRs read on some path from the entry before any write on that path: what the shader takes from outside.
// Unlike read_before_write_vgprs (a straight-line scan), this follows the branches: a loop counter read at the
// loop's head, which the entry jumps past and every back edge writes first, is not an input (315 pixel shaders of
// 1.0.2.1 keep one in v1, the second barycentric's register).
std::set<int> entry_live_vgprs(const std::vector<Ins>& insns) {
    for (const Ins& i : insns)
        if (!i.has_enc) return read_before_write_vgprs(insns);  // no encodings (hand-written listings): no addresses
    const auto by_pc = pcs(insns);
    const size_t n = insns.size();
    std::vector<std::vector<size_t>> succ(n);
    std::vector<Bits> uses(n), defs(n);
    for (size_t k = 0; k < n; ++k) {
        const Ins& ins = insns[k];
        const Args& a = ins.args;
        const S& op = ins.op;
        const S b = base_op(op);
        auto nxt = by_pc.find(ins.next_pc());
        if (is_branch(b)) {
            const Arg* imm = first_imm(ins);
            const int64_t v = imm ? (imm->v & 0xFFFF) : 0;
            auto tgt = by_pc.find(ins.next_pc() + 4 * sext16(v));
            if (tgt != by_pc.end()) succ[k].push_back(tgt->second);
        }
        if (b != "s_branch" && b != "s_endpgm" && b != "s_setpc_b64" && nxt != by_pc.end()) succ[k].push_back(nxt->second);
        if (a.empty()) continue;
        const bool reads_all = reads_all_operands(op);
        for (size_t i = reads_all ? 0 : 1; i < a.size(); ++i) {
            const Arg& x = a[i];
            if (x.k == "vgpr")
                for (int r = x.r; r < x.r + x.n; ++r) set_bit(uses[k], r);
            else if (x.k == "vlist")
                for (int r : x.list) set_bit(uses[k], r);
        }
        if (accumulates(b) && a[0].k == "vgpr") set_bit(uses[k], a[0].r);
        if (!reads_all && a[0].k == "vgpr")
            for (int r = a[0].r; r < a[0].r + a[0].n; ++r) set_bit(defs[k], r);
    }
    // forward "written on every path" sets: the entry starts with none, the rest with all until a path reaches them
    Bits top;
    top.set();
    std::vector<Bits> inn(n, top);
    if (n) inn[0].reset();
    std::vector<size_t> work;
    std::set<size_t> queued;
    if (n) {
        work.push_back(0);
        queued.insert(0);
    }
    while (!work.empty()) {
        const size_t k = work.back();
        work.pop_back();
        queued.erase(k);
        const Bits out = inn[k] | defs[k];
        for (size_t t : succ[k]) {
            const Bits nw = inn[t] & out;
            if (nw != inn[t] || (inn[t] == top && !queued.count(t) && t != 0)) {
                const bool changed = nw != inn[t];
                inn[t] = nw;
                if (changed && !queued.count(t)) {
                    work.push_back(t);
                    queued.insert(t);
                }
            }
        }
    }
    std::vector<bool> reached(n, false);
    std::vector<size_t> stack;
    if (n) stack.push_back(0);
    while (!stack.empty()) {
        const size_t k = stack.back();
        stack.pop_back();
        if (reached[k]) continue;
        reached[k] = true;
        stack.insert(stack.end(), succ[k].begin(), succ[k].end());
    }
    Bits live;
    for (size_t k = 0; k < n; ++k)
        if (reached[k]) live |= uses[k] & ~inn[k];
    std::set<int> out;
    for (int r = 0; r < 512; ++r)
        if (live[static_cast<size_t>(r)]) out.insert(r);
    return out;
}

// Whether blob bytes hold this instruction. LLVM prints its own encoding, not the bytes it read: a one- or two-source
// VOP3 (v_rcp_f32_e64, v_mul_hi_i32, ...) comes back with its unused SRC1/SRC2 fields (bits 9-17 and 18-26 of the
// second dword) as 0x80 where the console's compiler left 0. Those two fields match either way.
bool same_encoding(const std::string& blob, size_t pos, const std::vector<uint8_t>& enc) {
    const size_t avail = pos < blob.size() ? std::min(enc.size(), blob.size() - pos) : 0;  // blob[pos:pos+len]
    const auto* b = reinterpret_cast<const uint8_t*>(blob.data()) + (pos < blob.size() ? pos : 0);
    if (avail == enc.size() && (enc.empty() || std::memcmp(b, enc.data(), enc.size()) == 0)) return true;
    if (enc.size() < 8 || avail != enc.size() || std::memcmp(b + 8, enc.data() + 8, enc.size() - 8) != 0)
        return false;
    const uint32_t x = dxbc::u32(b + 4), y = dxbc::u32(enc.data() + 4);
    for (int sh : {9, 18}) {
        const uint32_t fa = x >> sh & 0x1FF, fb = y >> sh & 0x1FF;
        if (fa != fb && !(fa == 0 && fb == 0x80)) return false;
    }
    if (std::memcmp(b, enc.data(), 4) != 0) {
        // A packed op (VOP3P, 0xCC in the top byte) with no third source: LLVM also prints OP_SEL_HI[2] (bit 14 of
        // the first dword) as 1 where the console's compiler left 0 (v_pk_mul_f16, v_pk_add_f16).
        const uint32_t a0 = dxbc::u32(b), b0 = dxbc::u32(enc.data());
        if (a0 >> 24 != 0xCC || (a0 ^ b0) != 1u << 14 || (a0 & (1u << 14)) || (x >> 18 & 0x1FF) != 0) return false;
    }
    const uint32_t mask = ~((0x1FFu << 9) | (0x1FFu << 18));
    return ((x ^ y) & mask) == 0;
}

}  // namespace

S base_op(const S& op) {
    for (const char* suf : {"_e32", "_e64", "_sdwa", "_dpp"})
        if (ends(op, suf)) return op.substr(0, op.size() - std::strlen(suf));
    return op;
}

bool is_branch(const S& op) {
    for (const char* b : kBranches)
        if (op == b) return true;
    return false;
}

S branch_condition(Gen& g, const S& op) {
    // Boolean SPIR-V id: whether the branch is taken for this lane.
    if (op == "s_cbranch_scc0" || op == "s_cbranch_scc1") {
        const S s = g.tmp();
        g.emit({s, " = OpLoad %uint %scc"});
        const S b = g.tmp();
        g.emit({b, " = ", ends(op, "0") ? "OpIEqual" : "OpINotEqual", " %bool ", s, " ", g.cu(0)});
        return b;
    }
    if (op == "s_cbranch_vccz" || op == "s_cbranch_vccnz") {
        const S s = g.tmp();
        g.emit({s, " = OpLoad %uint %vcc"});
        const S b = g.tmp();
        g.emit({b, " = ", ends(op, "vccz") ? "OpIEqual" : "OpINotEqual", " %bool ", s, " ", g.cu(0)});
        return b;
    }
    const S a = g.active_b();
    if (op == "s_cbranch_execnz") return a;
    const S b = g.tmp();
    g.emit({b, " = OpLogicalNot %bool ", a});
    return b;
}

std::string Context::stage_of(const std::string& name, const std::wstring& s_path) {
    // Shader stage from the carve index (the DXIL program header in each container), not the name.
    {
        std::lock_guard<std::mutex> g(lock_);
        if (!loaded_) {
            loaded_ = true;
            const std::wstring idx = fp::join(corpus_root(s_path), L"index.json");
            json::Value v;
            if (is_file(idx) && json::load_file(idx, v))
                for (const auto& e : v.items) {
                    const std::string& f = e["file"].str();
                    stages_[f.size() > 5 ? f.substr(0, f.size() - 5) : std::string()] = e["stage"].str();
                }
        }
        auto it = stages_.find(name);
        if (it != stages_.end() && !it->second.empty()) return it->second;
    }
    if (name.size() > 2 && name[2] == '_') {
        std::string s = name.substr(0, 2);
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    return "?";
}

std::string translate(const std::wstring& s_path, Context& ctx) {
    const Options& opt = ctx.options();
    Listing info = parse_file(s_path);
    const S stage = ctx.stage_of(info.name, s_path);
    std::optional<MeshInfo> mesh;
    if (stage == "ms") {
        mesh = mesh_info(s_path, info.name);
        if (!mesh) throw Unsupported("mesh record unreadable");
        if (mesh->topology != 2) throw Unsupported("mesh topology " + D(mesh->topology));
        if (mesh->wave32 || mesh->threads % 64 || !(64 <= mesh->threads && mesh->threads <= 256))
            throw Unsupported("mesh group of " + D(mesh->threads) + " threads");
    }
    if (stage != "vs" && stage != "ps" && stage != "cs" && stage != "ms") throw Unsupported("stage " + stage);
    std::optional<ComputeRegs> regs;
    if (stage == "cs") {
        regs = compute_regs(opt, s_path, info.name);
        if (!regs) throw Unsupported("compute registers unknown");
        if (regs->scratch) throw Unsupported("compute scratch");
    }
    std::vector<Ins> insns = info.insns;
    if (info.invalid) {
        // Undecodable words are constant tables embedded in the code range (147/155 such VS/PS use s_getpc_b64 to
        // read them). LLVM skips them, so re-anchor every instruction to its true offset in the FSSM blob, then
        // translate only code reachable from entry; a branch into data is rejected below.
        const std::string blob = read_or_raise(fp::join(fp::join(corpus_root(s_path), L"fssm"), widen(info.name + ".fssm")));
        int64_t pos = info.code_start;
        for (Ins& ins : insns) {
            const int64_t start = pos, size = static_cast<int64_t>(blob.size());
            while (pos - start <= 4096 && pos < size && !same_encoding(blob, static_cast<size_t>(pos), ins.enc)) pos += 4;
            if (pos - start > 4096 || pos >= size) {
                // Not in the blob as LLVM prints it: table words that happen to decode (LLVM reads past bits it
                // ignores and prints its own encoding: 0xBE8D2242 comes back as s_rfe_b64, 0xBE802242). Kept as
                // data where the stream stands; reachable_code refuses a program that would run it.
                pos = start;
                ins.data = true;
            }
            ins.pc = pos;
            pos += static_cast<int64_t>(ins.enc.size());
        }
        insns = reachable_code(insns);
    }
    Gen g(stage, opt.target);
    g.cu(1);  // the dispatch loop's step and its limit (assemble())
    g.cu(LOOP_LIMIT);
    if (stage == "ms") {
        g.mesh = Mesh{mesh->threads, {}};
        g.lds_dwords = (static_cast<int64_t>(mesh->lds_bytes) + 3) / 4;
        g.cu(0x108);
        g.cu(2);
    }
    if (regs) {
        g.compute = *regs;
        g.lds_dwords = regs->lds_dwords;
        for (int i = 0; i < USER_SGPRS + 4; ++i) g.cu(i);  // the system-register indices the prologue stores to
        const int64_t x = regs->size[0], y = regs->size[1], z = regs->size[2];  // and the TG_SIZE prologue's constants
        for (int64_t v : {x, y, int64_t{5}, int64_t{6}, (x * y * z + 31) / 32}) g.cu(v);
    }
    if (stage == "ps") {
        // Pixel-shader system VGPRs (verified on 2,531 of 3,000 PS): v0-v1 = barycentrics (modelled through
        // v_interp -> attributes), then v2.. = the SV_Position components the shader reads, packed in order.
        // Anything else a PS reads before writing is an input layout we haven't confirmed: reject it.
        const auto sig = dxbc::input_signature(read_or_raise(fp::join(corpus_root(s_path), widen(info.name + ".dxbc"))));
        int used = 0;
        for (const auto& e : sig)
            if (e.system_value == 1) {
                used = e.rw_mask;
                break;
            }
        // Position w: AMD's POS_W_FLOAT input holds 1/w. Mesa (src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c)
        // lowers nir_intrinsic_load_frag_coord_w_rcp straight to frag_pos[3]. That's the same value as Vulkan's
        // FragCoord.w, so all four components map directly.
        // Hardware order (Mesa src/amd/vulkan/radv_shader_args.c): barycentric pairs, ..., POS_X/Y/Z/W, FRONT_FACE,
        // ANCILLARY, SAMPLE_COVERAGE, POS_FIXED_PT. Only enabled inputs are packed. Front face is a float whose
        // sign gives facing (Mesa: load_front_face = front_face > 0; load_front_face_fsign = the raw value).
        std::vector<std::pair<int, int>> layout;
        int nxt = 2;
        for (int c = 0; c < 4; ++c)
            if (used >> c & 1) layout.emplace_back(nxt++, c);
        for (const auto& e : sig)
            if (e.system_value == 9) {  // D3D_NAME_IS_FRONT_FACE
                layout.emplace_back(nxt++, kFront);
                break;
            }
        std::vector<Ins> no_bary;
        for (const Ins& i : insns) {
            Ins c = i;
            c.args.clear();
            for (size_t k = 0; k < i.args.size(); ++k)
                if (!(starts(i.op, "v_interp") && i.args[k].k == "vgpr" && k != 0)) c.args.push_back(i.args[k]);
            no_bary.push_back(std::move(c));
        }
        const std::set<int> reads = entry_live_vgprs(no_bary);
        // Reads inside the system-input range that aren't in the layout (raw barycentrics, unknown inputs): reject.
        // Reads past the last hardware input can't be hardware inputs: they're values undefined on some path, and
        // the zero-initialised registers stand in for them.
        for (int r : reads) {
            bool in_layout = false;
            for (const auto& [reg, c] : layout) in_layout |= reg == r;
            if (r < nxt && !in_layout) throw Unsupported("PS system inputs");
        }
        g.frag_inputs = layout;
        for (const auto& [r, c] : layout) g.cu(r);
    }
    bool swappc = false;
    for (const Ins& i : insns) swappc |= base_op(i.op) == "s_swappc_b64";
    if (stage == "vs" && swappc) {
        // The call goes to a driver-generated fetch program (not in the blob). Verified rule over 788 shaders:
        // input-signature register r, component c is fetched into v(4 + 4r + c). Replace the call with Vulkan
        // vertex inputs (location = register) written into those VGPRs at entry.
        std::vector<dxbc::InputElement> sig;
        for (auto& e : dxbc::input_signature(read_or_raise(fp::join(corpus_root(s_path), widen(info.name + ".dxbc")))))
            if (e.system_value == 0) sig.push_back(e);
        std::set<int64_t> predicted;
        for (const auto& e : sig)
            for (int c = 0; c < 4; ++c)
                if (e.mask >> c & 1) predicted.insert(4 + 4 * static_cast<int64_t>(e.reg) + c);
        for (int r : read_before_write_vgprs(insns))
            if (r >= 4 && !predicted.count(r)) throw Unsupported("fetch mapping");
        for (const auto& e : sig) g.vertex_inputs.emplace_back(static_cast<int>(e.reg), static_cast<int>(e.component_type));
        for (const auto& [reg, ctype] : g.vertex_inputs)  // register the VGPR index constants before assembly
            for (int c = 0; c < 4; ++c) g.cu(4 + 4 * static_cast<int64_t>(reg) + c);
        std::vector<Ins> kept;
        for (Ins& i : insns)
            if (base_op(i.op) != "s_swappc_b64") kept.push_back(std::move(i));
        insns = std::move(kept);
    }
    bool branchy = false;
    for (const Ins& i : insns) branchy |= is_branch(base_op(i.op));
    if (!branchy) {
        for (const Ins& ins : insns) {
            if (base_op(ins.op) == "s_endpgm") break;
            g.insn(ins);
        }
        return assemble(g);
    }

    // ---- basic blocks
    auto target = [](const Ins& ins) {
        const Arg* imm = first_imm(ins);
        if (!imm) throw Unsupported("branch without offset");
        return ins.next_pc() + 4 * sext16(imm->v & 0xFFFF);
    };
    std::set<int64_t> leaders = {at(insns, 0).pc};
    for (const Ins& ins : insns)
        if (is_branch(base_op(ins.op))) {
            leaders.insert(target(ins));
            leaders.insert(ins.next_pc());
        }
    std::set<int64_t> pcs_present;
    for (const Ins& i : insns) pcs_present.insert(i.pc);
    std::vector<int64_t> starts_;
    for (int64_t p : leaders)
        if (pcs_present.count(p)) starts_.push_back(p);
    std::map<int64_t, int64_t> index;
    for (size_t k = 0; k < starts_.size(); ++k) index[starts_[k]] = static_cast<int64_t>(k);
    for (const Ins& i : insns)
        if (is_branch(base_op(i.op)) && !index.count(target(i))) throw Unsupported("branch target outside code");
    std::vector<std::vector<const Ins*>> blocks(starts_.size());
    int64_t cur = -1;
    for (const Ins& ins : insns) {
        auto it = index.find(ins.pc);
        if (it != index.end()) cur = it->second;
        (cur < 0 ? blocks.back() : blocks[static_cast<size_t>(cur)]).push_back(&ins);
    }

    for (size_t n = 0; n < blocks.size(); ++n) {
        g.body.clear();
        g.block_label = "%blk" + D(static_cast<int64_t>(n));
        int term = 0;  // 0 none, 1 end, 2 branch
        S term_op;
        int64_t term_tgt = 0;
        for (const Ins* ins : blocks[n]) {
            const S op = base_op(ins->op);
            if (op == "s_endpgm") {
                term = 1;
                break;
            }
            if (is_branch(op)) {
                term = 2;
                term_op = op;
                term_tgt = key(index, target(*ins));
                break;
            }
            g.insn(*ins);
        }
        const bool has_fall = n + 1 < blocks.size();
        const int64_t fall = static_cast<int64_t>(n + 1);
        if (term == 0) {
            if (has_fall)
                g.emit({"OpStore %pc ", g.cu(fall)});
            else
                g.emit("OpStore %done %true");
        } else if (term == 1) {
            g.emit("OpStore %done %true");
        } else if (term_op == "s_branch") {
            g.emit({"OpStore %pc ", g.cu(term_tgt)});
        } else {
            const S cond = branch_condition(g, term_op);
            const S alt = has_fall ? g.cu(fall) : g.cu(static_cast<int64_t>(blocks.size()));  // past the end = done
            const S r = g.tmp();
            g.emit({r, " = OpSelect %uint ", cond, " ", g.cu(term_tgt), " ", alt});
            g.emit({"OpStore %pc ", r});
        }
        std::vector<S> stripped;
        for (const S& l : g.body) {
            size_t a = l.find_first_not_of(" \t\n\r\f\v"), b = l.find_last_not_of(" \t\n\r\f\v");
            stripped.push_back(a == S::npos ? S() : l.substr(a, b - a + 1));
        }
        g.blocks.push_back(std::move(stripped));
    }
    g.body.clear();
    return assemble(g);
}

}  // namespace fp::xlat
