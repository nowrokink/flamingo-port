#include "literal_samplers.h"

#include <cstdio>
#include <map>
#include <optional>

#include "core/fs.h"
#include "xlat/gen.h"

namespace fp::cache {
namespace {

using Known = std::map<int, uint32_t>;  // SGPR -> constant

// Scalar ops whose first operand is NOT a destination.
const char* const kNoDestPrefix[] = {"s_cmp", "s_bitcmp", "s_cbranch", "s_branch", "s_waitcnt", "s_store",
                                     "s_buffer_store", "s_scratch_store", "s_dcache", "s_icache", "s_setreg",
                                     "s_ttrace", "s_set_gpr_idx"};
const char* const kNoDest[] = {"s_nop", "s_endpgm", "s_sendmsg", "s_sendmsghalt", "s_setprio", "s_barrier",
                               "s_inst_prefetch", "s_clause", "s_sleep", "s_trap", "s_setpc_b64", "s_delay_alu",
                               "s_denorm_mode", "s_round_mode", "s_setkill", "s_sethalt", "s_incperflevel",
                               "s_decperflevel", "s_endpgm_saved", "s_code_end", "s_version", "s_wakeup",
                               "s_ttracedata", "s_ttracedata_imm", "s_icache_inv", "s_waitcnt_depctr"};
// Image instructions that take an S# as their 4th operand (vdata, vaddr, T#, S#).
const char* const kSampling[] = {"image_sample", "image_gather4", "image_get_lod"};

bool any_prefix(const std::string& op, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (xlat::starts(op, list[i])) return true;
    return false;
}

bool is_branch_op(const std::string& op) { return xlat::starts(op, "s_branch") || xlat::starts(op, "s_cbranch_"); }

// SGPR numbers an operand covers ([] when it isn't an SGPR operand).
std::vector<int> regs(const xlat::Arg& a) {
    std::vector<int> r;
    if (a.k != "sgpr") return r;
    for (int i = 0; i < a.n; ++i) r.push_back(a.r + i);
    return r;
}

// 32-bit value of an immediate operand, or nothing.
std::optional<uint32_t> imm_bits(const xlat::Arg& a) {
    if (a.k == "imm") return static_cast<uint32_t>(a.v);
    if (a.k == "fimm") return xlat::f32_bits(a.f);
    return std::nullopt;
}

// Byte offset a branch goes to, or nothing.
std::optional<int64_t> branch_target(const xlat::Ins& ins) {
    const xlat::Arg* imm = nullptr;
    for (const auto& x : ins.args)
        if (x.k == "imm") {
            imm = &x;
            break;
        }
    if (!imm || !ins.has_enc) return std::nullopt;
    const int64_t v = imm->v & 0xFFFF;
    return ins.next_pc() + 4 * (v & 0x8000 ? v - 0x10000 : v);
}

// Applies one instruction to the SGPR constant map; records literal S#s it samples with.
void step(const xlat::Ins& ins, Known& known, std::set<Sampler>& found) {
    const std::string op = xlat::base_op(ins.op);
    const auto& args = ins.args;
    if (any_prefix(op, kSampling, std::size(kSampling)) && args.size() >= 4) {
        const auto r = regs(args[3]);
        bool all = r.size() >= 4;
        for (size_t i = 0; all && i < 4; ++i) all = known.count(r[i]) != 0;
        if (all) found.insert({known[r[0]], known[r[1]], known[r[2]], known[r[3]]});
    }
    if ((op == "s_mov_b32" || op == "s_movk_i32") && !args.empty() && !regs(args[0]).empty() && args.size() > 1) {
        auto v = imm_bits(args[1]);
        if (op == "s_movk_i32" && v) {
            const uint32_t k = *v & 0xFFFF;
            *v = (*v & 0x8000) ? static_cast<uint32_t>(static_cast<int64_t>(k) - 0x10000) : k;
        }
        const int d = regs(args[0])[0];
        if (!v)
            known.erase(d);
        else
            known[d] = *v;
        return;
    }
    if (op == "s_mov_b64" && !args.empty() && regs(args[0]).size() == 2 && args.size() > 1) {
        const auto r = regs(args[0]);
        const auto v = imm_bits(args[1]);
        if (!v) {
            known.erase(r[0]);
            known.erase(r[1]);
        } else {  // a 64-bit inline/literal constant is the 32-bit value sign-extended
            known[r[0]] = *v;
            known[r[1]] = (*v & 0x80000000u) ? 0xFFFFFFFFu : 0;
        }
        return;
    }
    std::vector<int> written;
    if (xlat::starts(op, "s_")) {
        bool no_dest = false;
        for (const char* x : kNoDest) no_dest = no_dest || op == x;
        if (!args.empty() && !no_dest && !any_prefix(op, kNoDestPrefix, std::size(kNoDestPrefix)))
            written = regs(args[0]);
    } else if (xlat::starts(op, "v_")) {
        if (!args.empty()) written = regs(args[0]);  // v_cmp_*_e64 sdst, v_readlane/readfirstlane
        if (args.size() > 1 && (op.find("_co_") != std::string::npos || xlat::starts(op, "v_div_scale") ||
                                xlat::starts(op, "v_mad_u64") || xlat::starts(op, "v_mad_i64"))) {
            auto more = regs(args[1]);  // VOP3b carry-out / sdst in second position
            written.insert(written.end(), more.begin(), more.end());
        }
    }
    for (int x : written) known.erase(x);
}

}  // namespace

std::set<Sampler> literal_samplers(const std::vector<xlat::Ins>& insns) {
    std::set<Sampler> found;
    if (insns.empty()) return found;
    std::set<int64_t> leaders{insns[0].pc};
    for (size_t k = 0; k < insns.size(); ++k) {
        if (is_branch_op(xlat::base_op(insns[k].op))) {
            if (auto t = branch_target(insns[k])) leaders.insert(*t);
            if (k + 1 < insns.size()) leaders.insert(insns[k + 1].pc);
        }
    }
    std::vector<size_t> starts;
    for (size_t k = 0; k < insns.size(); ++k)
        if (leaders.count(insns[k].pc)) starts.push_back(k);
    std::map<int64_t, size_t> block_of;  // {pcs[s]: b}: a repeated pc keeps the last block
    for (size_t b = 0; b < starts.size(); ++b) block_of[insns[starts[b]].pc] = b;
    const size_t nb = starts.size();
    auto block_end = [&](size_t b) { return b + 1 < nb ? starts[b + 1] : insns.size(); };
    std::vector<std::vector<size_t>> succ(nb);
    bool unknown_target = false;
    for (size_t b = 0; b < nb; ++b) {
        const xlat::Ins& last = insns[block_end(b) - 1];
        const std::string op = xlat::base_op(last.op);
        std::vector<size_t> out;
        bool give_up = false;
        if (is_branch_op(op)) {
            auto t = branch_target(last);
            if (!t || !block_of.count(*t)) {
                give_up = true;  // unknown target: give up on constants after it
                unknown_target = true;
            } else {
                out.push_back(block_of[*t]);
            }
        }
        if (!give_up && op != "s_branch" && op != "s_endpgm" && op != "s_setpc_b64" && b + 1 < nb) out.push_back(b + 1);
        succ[b] = give_up ? std::vector<size_t>{} : out;
    }
    std::vector<std::optional<Known>> entry(nb);  // nothing = not reached yet (top)
    entry[0] = Known{};
    std::set<Sampler> scratch;
    std::vector<size_t> work{0};
    while (!work.empty()) {
        const size_t b = work.back();
        work.pop_back();
        Known known = *entry[b];
        for (size_t k = starts[b]; k < block_end(b); ++k) step(insns[k], known, scratch);
        for (size_t s : succ[b]) {
            Known next;
            if (!entry[s]) {
                next = known;
            } else {
                for (const auto& [r, v] : *entry[s]) {
                    auto it = known.find(r);
                    if (it != known.end() && it->second == v) next[r] = v;
                }
            }
            if (unknown_target) next.clear();
            if (!entry[s] || next != *entry[s]) {
                entry[s] = next;
                work.push_back(s);
            }
        }
    }
    // Record only with the converged block entries: an early visit may have seen constants another path breaks.
    for (size_t b = 0; b < nb; ++b) {
        if (!entry[b]) continue;
        Known known = *entry[b];
        for (size_t k = starts[b]; k < block_end(b); ++k) step(insns[k], known, found);
    }
    return found;
}

bool write_literal_samplers(const std::wstring& src, const std::wstring& out,
                            const std::function<void(const std::string&)>& log) {
    std::vector<std::wstring> files;
    if (src.size() > 2 && src.compare(src.size() - 2, 2, L".s") == 0) {
        files.push_back(src);
    } else {
        for (const auto& f : list_dir(src))
            if (f.size() > 2 && f.compare(f.size() - 2, 2, L".s") == 0) files.push_back(fp::join(src, f));
    }
    std::set<Sampler> all;
    size_t users = 0;
    for (const auto& p : files) {
        auto s = literal_samplers(xlat::parse_file(p).insns);
        users += !s.empty();
        all.insert(s.begin(), s.end());
    }
    std::string text = "# literal S#s from " + std::to_string(files.size()) + " shaders (" + std::to_string(users) +
                       " use at least one)\n";
    for (const auto& s : all) {
        char line[64];
        std::snprintf(line, sizeof line, "0x%08x 0x%08x 0x%08x 0x%08x\n", s[0], s[1], s[2], s[3]);
        text += line;
    }
    if (!write_file(out, text.data(), text.size())) return false;
    if (log)
        log(std::to_string(files.size()) + " shaders, " + std::to_string(users) + " with literal samplers, " +
            std::to_string(all.size()) + " distinct S#");
    return true;
}

}  // namespace fp::cache
