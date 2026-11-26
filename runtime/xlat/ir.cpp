// tools/shader_ir.py in C++: LLVM AMDGPU disassembly lines -> instructions with typed operands. The Python's regular
// expressions are matched by hand (std::regex would take hours over the corpus), each one noted where it's done.
#include "ir.h"

#include <cstdlib>
#include <cstring>

#include "core/fs.h"
#include "core/text.h"
#include "gen.h"

namespace fp::xlat {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool hexdigit(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool word(char c) { return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

std::string strip(std::string_view s) {
    size_t i = 0, e = s.size();
    while (i < e && space(s[i])) ++i;
    while (e > i && space(s[e - 1])) --e;
    return std::string(s.substr(i, e - i));
}

// \d+ spanning all of s
bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!digit(c)) return false;
    return true;
}

int to_int(std::string_view s) { return static_cast<int>(py_int(s)); }

const char* const kSpecial[] = {"vcc", "vcc_lo", "vcc_hi", "exec", "exec_lo", "exec_hi", "m0", "scc", "null", "off",
                                "src_scc", "src_execz", "src_vccz", "src_lds_direct", "src_shared_base",
                                "src_shared_limit", "src_private_base", "src_private_limit",
                                "src_pops_exiting_wave_id", "lds", "flat_scratch", "flat_scratch_lo",
                                "flat_scratch_hi", "ttmp", "tba", "tma"};

bool is_special(const std::string& s) {
    for (const char* x : kSpecial)
        if (s == x) return true;
    return false;
}

// ([sv])(\d+) / ([sv])\[(\d+):(\d+)\]
bool reg_operand(const std::string& core, Arg& a) {
    if (core.size() < 2 || (core[0] != 's' && core[0] != 'v')) return false;
    std::string_view rest(core);
    rest.remove_prefix(1);
    if (all_digits(rest)) {
        a.k = core[0] == 's' ? "sgpr" : "vgpr";
        a.r = to_int(rest);
        a.n = 1;
        return true;
    }
    if (rest.size() >= 5 && rest.front() == '[' && rest.back() == ']') {
        std::string_view in = rest.substr(1, rest.size() - 2);
        size_t c = in.find(':');
        if (c != std::string_view::npos && all_digits(in.substr(0, c)) && all_digits(in.substr(c + 1))) {
            int lo = to_int(in.substr(0, c)), hi = to_int(in.substr(c + 1));
            a.k = core[0] == 's' ? "sgpr" : "vgpr";
            a.r = lo;
            a.n = hi - lo + 1;
            return true;
        }
    }
    return false;
}

// ttmp(\d+)|ttmp\[(\d+):(\d+)\]
bool ttmp_operand(const std::string& core) {
    if (!starts(core, "ttmp")) return false;
    std::string_view rest(core);
    rest.remove_prefix(4);
    if (all_digits(rest)) return true;
    if (rest.size() >= 5 && rest.front() == '[' && rest.back() == ']') {
        std::string_view in = rest.substr(1, rest.size() - 2);
        size_t c = in.find(':');
        return c != std::string_view::npos && all_digits(in.substr(0, c)) && all_digits(in.substr(c + 1));
    }
    return false;
}

// \[(v\d+(?:,\s*v\d+)+)\]
bool vlist_operand(const std::string& core, std::vector<int>& regs) {
    if (core.size() < 2 || core.front() != '[' || core.back() != ']') return false;
    std::string_view in(core);
    in = in.substr(1, in.size() - 2);
    regs.clear();
    size_t i = 0;
    for (;;) {
        if (i >= in.size() || in[i] != 'v') return false;
        size_t j = ++i;
        while (j < in.size() && digit(in[j])) ++j;
        if (j == i) return false;
        regs.push_back(to_int(in.substr(i, j - i)));
        i = j;
        if (i == in.size()) break;
        if (in[i] != ',') return false;
        ++i;
        while (i < in.size() && space(in[i])) ++i;
    }
    return regs.size() >= 2;
}

// -?(0x[0-9a-fA-F]+|\d+)
bool int_literal(const std::string& core) {
    std::string_view s(core);
    if (!s.empty() && s[0] == '-') s.remove_prefix(1);
    if (s.size() > 2 && s[0] == '0' && s[1] == 'x') {
        for (char c : s.substr(2))
            if (!hexdigit(c)) return false;
        return true;
    }
    return all_digits(s);
}

// -?\d+\.\d+(e[+-]?\d+)?
bool float_literal(const std::string& core) {
    std::string_view s(core);
    size_t i = 0;
    if (i < s.size() && s[i] == '-') ++i;
    size_t j = i;
    while (j < s.size() && digit(s[j])) ++j;
    if (j == i || j >= s.size() || s[j] != '.') return false;
    i = ++j;
    while (j < s.size() && digit(s[j])) ++j;
    if (j == i) return false;
    if (j == s.size()) return true;
    if (s[j] != 'e') return false;
    ++j;
    if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
    i = j;
    while (j < s.size() && digit(s[j])) ++j;
    return j > i && j == s.size();
}

// (pos|param|mrt|mrtz|null|prim)(\d*) fullmatch
bool target_operand(const std::string& core) {
    for (const char* p : {"pos", "param", "mrt", "mrtz", "null", "prim"}) {
        size_t n = std::strlen(p);
        if (core.compare(0, n, p) == 0) {
            std::string_view rest(core);
            rest.remove_prefix(n);
            bool ok = true;
            for (char c : rest) ok = ok && digit(c);
            if (ok) return true;
        }
    }
    return false;
}

}  // namespace

Arg parse_operand(const std::string& tok) {
    const std::string t = strip(tok);
    const bool neg = !t.empty() && t[0] == '-';
    std::string core = neg ? t.substr(1) : t;
    const bool absm = core.size() >= 1 && core.front() == '|' && core.back() == '|';
    if (absm) core = core.size() >= 2 ? core.substr(1, core.size() - 2) : std::string();
    Arg a;
    // sext\((.+)\): SDWA integer sign-extension of the selected source bits
    if (core.size() > 6 && starts(core, "sext(") && core.back() == ')') {
        Arg inner = parse_operand(core.substr(5, core.size() - 6));
        inner.sext = true;
        inner.neg = inner.neg || neg;
        return inner;
    }
    if (reg_operand(core, a)) {
        a.neg = neg;
        a.abs = absm;
        return a;
    }
    if (ttmp_operand(core)) {
        a.k = "ttmp";
        a.t = core;
        return a;
    }
    if (vlist_operand(core, a.list)) {  // NSA image address: non-contiguous VGPR list
        a.k = "vlist";
        return a;
    }
    // attr(\d+)\.([xyzw])
    if (starts(core, "attr") && core.size() >= 7 && core[core.size() - 2] == '.' &&
        std::strchr("xyzw", core.back()) && core.back() &&
        all_digits(std::string_view(core).substr(4, core.size() - 6))) {
        a.k = "attr";
        a.attr = to_int(std::string_view(core).substr(4, core.size() - 6));
        a.comp = static_cast<int>(std::strchr("xyzw", core.back()) - "xyzw");
        return a;
    }
    if (core == "p0" || core == "p10" || core == "p20") {
        a.k = "interp";
        a.t = core;
        return a;
    }
    if (target_operand(core) && core != "null") {
        a.k = "target";
        a.t = core;
        return a;
    }
    if (is_special(core)) {
        a.k = "special";
        a.t = core;
        a.neg = neg;
        a.abs = absm;
        return a;
    }
    if (int_literal(core)) {
        int64_t v = py_int(core, 0);
        a.k = "imm";
        a.v = neg ? static_cast<int64_t>(0 - static_cast<uint64_t>(v)) : v;
        return a;
    }
    if (float_literal(core)) {
        a.k = "fimm";
        a.f = std::strtod(core.c_str(), nullptr) * (neg ? -1 : 1);
        a.abs = absm;
        return a;
    }
    if (starts(core, "hwreg(") || starts(core, "sendmsg(") || starts(core, "vmcnt(") || starts(core, "expcnt(") ||
        starts(core, "lgkmcnt(") || starts(core, "vscnt(") || starts(core, "depctr") || starts(core, "instid") ||
        starts(core, "quad_perm") || starts(core, "row_") || starts(core, "dpp") || starts(core, "sel:") ||
        core.find('(') != std::string::npos) {
        a.k = "special";
        a.t = core;
        return a;
    }
    a.k = "raw";
    a.t = t;
    return a;
}

namespace {

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(' || ch == '[')
            ++depth;
        else if (ch == ')' || ch == ']')
            --depth;
        if (ch == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!strip(cur).empty()) out.push_back(cur);
    return out;
}

// One alternative of MODWORDS: a fixed word, or a prefix followed by \S+, \d+ or \[[^\]]*\].
enum class Tail { none, nonspace, digits, bracket };
struct ModWord {
    const char* text;
    Tail tail;
};
// In the Python's alternation order (the first that matches wins).
const ModWord kModWords[] = {
    {"offset:", Tail::nonspace}, {"offen", Tail::none}, {"idxen", Tail::none}, {"glc", Tail::none},
    {"slc", Tail::none}, {"dlc", Tail::none}, {"lds", Tail::none}, {"tfe", Tail::none}, {"unorm", Tail::none},
    {"da", Tail::none}, {"d16", Tail::none}, {"a16", Tail::none}, {"nv", Tail::none}, {"r128", Tail::none},
    {"lwe", Tail::none}, {"clamp", Tail::none}, {"mul:", Tail::digits}, {"div:", Tail::digits},
    {"op_sel:", Tail::bracket}, {"op_sel_hi:", Tail::bracket}, {"neg_lo:", Tail::bracket},
    {"neg_hi:", Tail::bracket}, {"dmask:", Tail::nonspace}, {"dim:", Tail::nonspace}, {"format:", Tail::nonspace},
    {"done", Tail::none}, {"vm", Tail::none}, {"compr", Tail::none}, {"row_mask:", Tail::nonspace},
    {"bank_mask:", Tail::nonspace}, {"bound_ctrl:", Tail::nonspace}, {"fi:", Tail::nonspace},
    {"offset0:", Tail::nonspace}, {"offset1:", Tail::nonspace}, {"gds", Tail::none},
    {"inst_offset:", Tail::nonspace}, {"dst_sel:", Tail::nonspace}, {"dst_unused:", Tail::nonspace},
    {"src0_sel:", Tail::nonspace}, {"src1_sel:", Tail::nonspace}, {"high", Tail::none},
    {"dpp8:", Tail::bracket}, {"row_mirror", Tail::none}, {"row_half_mirror", Tail::none},
    {"row_shl:", Tail::digits}, {"row_shr:", Tail::digits}, {"row_ror:", Tail::digits},
    {"row_share:", Tail::digits}, {"row_xmask:", Tail::digits}, {"quad_perm:", Tail::bracket},
};

// The lookahead (?=\s|,|$) at position i.
bool boundary_after(const std::string& s, size_t i) { return i == s.size() || space(s[i]) || s[i] == ','; }

// The length of the MODWORDS match at i (\b then an alternative then the lookahead), or 0.
size_t modword_at(const std::string& s, size_t i) {
    if (!word(s[i]) || (i > 0 && word(s[i - 1]))) return 0;  // \b before a word character
    for (const ModWord& m : kModWords) {
        size_t n = std::strlen(m.text);
        if (s.compare(i, n, m.text) != 0) continue;
        size_t j = i + n;
        switch (m.tail) {
            case Tail::none:
                break;
            case Tail::nonspace: {  // \S+ (greedy; the lookahead then holds at its end)
                size_t k = j;
                while (k < s.size() && !space(s[k])) ++k;
                if (k == j) continue;
                j = k;
                break;
            }
            case Tail::digits: {
                size_t k = j;
                while (k < s.size() && digit(s[k])) ++k;
                if (k == j) continue;
                j = k;
                break;
            }
            case Tail::bracket: {
                if (j >= s.size() || s[j] != '[') continue;
                size_t k = s.find(']', j + 1);
                if (k == std::string::npos) continue;
                j = k + 1;
                break;
            }
        }
        if (boundary_after(s, j)) return j - i;
    }
    return 0;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
        s.replace(at, from.size(), to);
        at += to.size();
    }
    return s;
}

}  // namespace

bool parse_line(const std::string& line, Ins& ins) {
    const size_t semi = line.find(';');
    std::string code = strip(semi == std::string::npos ? std::string_view(line) : std::string_view(line).substr(0, semi));
    // ;\s*encoding:\s*\[([^\]]*)\]  (searched in the whole line)
    ins.enc.clear();
    ins.has_enc = false;
    for (size_t at = line.find(';'); at != std::string::npos; at = line.find(';', at + 1)) {
        size_t i = at + 1;
        while (i < line.size() && space(line[i])) ++i;
        if (line.compare(i, 9, "encoding:") != 0) continue;
        i += 9;
        while (i < line.size() && space(line[i])) ++i;
        if (i >= line.size() || line[i] != '[') continue;
        size_t close = line.find(']', i + 1);
        if (close == std::string::npos) continue;
        for (const auto& x : split(std::string_view(line).substr(i + 1, close - i - 1), ','))
            if (!strip(x).empty()) ins.enc.push_back(static_cast<uint8_t>(py_int(x, 16)));
        ins.has_enc = !ins.enc.empty();
        break;
    }
    if (code.empty()) return false;
    const size_t sp = code.find(' ');
    ins.op = code.substr(0, sp);
    std::string rest = sp == std::string::npos ? std::string() : code.substr(sp + 1);
    // sendmsg(MSG_GS, GS_OP_NOP) contains a comma: keep it as one operand.  sendmsg\([^)]*\)
    for (size_t at = rest.find("sendmsg("); at != std::string::npos; at = rest.find("sendmsg(", at)) {
        size_t close = rest.find(')', at + 8);
        if (close == std::string::npos) break;
        std::string inner = replace_all(replace_all(rest.substr(at, close + 1 - at), ", ", "|"), ",", "|");
        rest.replace(at, close + 1 - at, inner);
        at += inner.size();
    }
    // MODWORDS.findall, then .sub("")
    ins.mods.clear();
    std::string kept;
    for (size_t i = 0; i < rest.size();) {
        size_t n = modword_at(rest, i);
        if (n) {
            ins.mods.push_back(rest.substr(i, n));
            i += n;
        } else {
            kept += rest[i++];
        }
    }
    std::vector<std::string> parts;
    for (auto& x : split_args(kept))
        if (!strip(x).empty()) parts.push_back(x);
    // 'exp pos0 v3, ...' / 'exp null off, ...': the target is space-separated from the first operand.
    if (!parts.empty()) {
        auto first = split_ws(parts[0]);
        if (first.size() == 2) {
            parts.erase(parts.begin());
            parts.insert(parts.begin(), first.begin(), first.end());
        }
    }
    ins.args.clear();
    for (const auto& p : parts) ins.args.push_back(parse_operand(p));
    return true;
}

Listing parse_listing(const std::string& text, const std::string& name) {
    // str.splitlines() on the file: \n, \r\n and \r end lines
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find_first_of("\r\n", at);
        if (e == std::string::npos) {
            lines.push_back(text.substr(at));
            break;
        }
        lines.push_back(text.substr(at, e - at));
        at = e + (text[e] == '\r' && e + 1 < text.size() && text[e + 1] == '\n' ? 2 : 1);
    }
    Listing info;
    info.name = name;
    // ^;\s*(\S+)\s+code 0x([0-9a-f]+)\.\.0x([0-9a-f]+)
    int64_t pc = 0;
    if (!lines.empty()) {
        const std::string& h = lines[0];
        size_t i = 0;
        if (i < h.size() && h[i] == ';') {
            ++i;
            while (i < h.size() && space(h[i])) ++i;
            size_t j = i;
            while (j < h.size() && !space(h[j])) ++j;
            size_t k = j;
            while (k < h.size() && space(h[k])) ++k;
            auto lowhex = [](char c) { return digit(c) || (c >= 'a' && c <= 'f'); };
            if (j > i && k > j && h.compare(k, 7, "code 0x") == 0) {
                size_t a = k + 7, b = a;
                while (b < h.size() && lowhex(h[b])) ++b;
                if (b > a && h.compare(b, 4, "..0x") == 0 && b + 4 < h.size() && lowhex(h[b + 4]))
                    pc = py_int(h.substr(a, b - a), 16);
            }
        }
    }
    info.code_start = pc;
    for (size_t li = 1; li < lines.size(); ++li) {
        const std::string& ln = lines[li];
        if (starts(ln, "; ---")) break;
        if (ln.empty() || ln[0] != '\t') continue;
        Ins ins;
        if (!parse_line(ln, ins)) continue;
        ins.pc = pc;
        if (ins.has_enc) pc += static_cast<int64_t>(ins.enc.size());
        info.insns.push_back(std::move(ins));
    }
    for (size_t k = text.find("invalid instruction encoding"); k != std::string::npos;
         k = text.find("invalid instruction encoding", k + 28))
        ++info.invalid;
    return info;
}

Listing parse_file(const std::wstring& path) {
    std::string text;
    read_file(path, text);
    std::wstring base = file_name(path);
    size_t dot = base.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) base = base.substr(0, dot);
    return parse_listing(text, narrow(base));
}

}  // namespace fp::xlat
