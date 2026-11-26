// Text and Python-semantics helpers of the generator (declared in gen.h).
#include <bit>
#include <cstring>

#include "gen.h"

namespace fp::xlat {

S cat(std::initializer_list<std::string_view> parts) {
    size_t n = 0;
    for (auto p : parts) n += p.size();
    S out;
    out.reserve(n);
    for (auto p : parts) out += p;
    return out;
}

S D(int64_t v) { return std::to_string(v); }

S X(uint64_t v) {
    char buf[24];
    char* e = buf + sizeof buf;
    char* p = e;
    do {
        *--p = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    return S(p, e);
}

S join(const std::vector<S>& items, std::string_view sep) {
    S out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

bool starts(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool ends(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::vector<S> split(std::string_view s, char sep) {
    std::vector<S> out;
    size_t at = 0;
    for (;;) {
        size_t k = s.find(sep, at);
        if (k == std::string_view::npos) {
            out.emplace_back(s.substr(at));
            return out;
        }
        out.emplace_back(s.substr(at, k - at));
        at = k + 1;
    }
}

namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
}  // namespace

std::vector<S> split_ws(std::string_view s) {
    std::vector<S> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && space(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !space(s[j])) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

S rstrip(std::string_view s, std::string_view chars) {
    size_t e = s.size();
    while (e && chars.find(s[e - 1]) != std::string_view::npos) --e;
    return S(s.substr(0, e));
}

[[noreturn]] void index_error() { throw PyError("IndexError"); }
[[noreturn]] void key_error() { throw PyError("KeyError"); }

namespace {
[[noreturn]] void value_error() { throw PyError("ValueError"); }

int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
}  // namespace

// Python's int(s, base) for the forms the tools meet: surrounding whitespace, a sign, base 0's 0x/0o/0b prefixes
// (and no leading zeros in a base-0 decimal), single underscores between digits. Wraps past 64 bits.
int64_t py_int(std::string_view s, int base) {
    size_t i = 0, e = s.size();
    while (i < e && space(s[i])) ++i;
    while (e > i && space(s[e - 1])) --e;
    bool neg = false;
    if (i < e && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    auto prefix = [&](char p) {
        return e - i >= 2 && s[i] == '0' && (s[i + 1] == p || s[i + 1] == p - 32);
    };
    if (base == 0) {
        if (prefix('x')) base = 16, i += 2;
        else if (prefix('o')) base = 8, i += 2;
        else if (prefix('b')) base = 2, i += 2;
        else {
            base = 10;
            // "010" isn't a base-0 literal; "0", "00" are
            if (e - i > 1 && s[i] == '0')
                for (size_t k = i; k < e; ++k)
                    if (s[k] != '0' && s[k] != '_') value_error();
        }
        if (base != 10 && i < e && s[i] == '_') ++i;  // 0x_1f
    } else if ((base == 16 && prefix('x')) || (base == 8 && prefix('o')) || (base == 2 && prefix('b'))) {
        i += 2;
    }
    if (i >= e) value_error();
    uint64_t v = 0;
    bool last_us = true;
    for (size_t k = i; k < e; ++k) {
        if (s[k] == '_') {
            if (last_us) value_error();
            last_us = true;
            continue;
        }
        int d = digit(s[k]);
        if (d >= base) value_error();
        v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
        last_us = false;
    }
    if (last_us) value_error();
    return neg ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v);
}

uint32_t f32_bits(double x) { return std::bit_cast<uint32_t>(static_cast<float>(x)); }

int popcount(uint64_t v) { return std::popcount(v); }

bool has(const Mods& mods, std::string_view word) {
    for (const auto& m : mods)
        if (m == word) return true;
    return false;
}

std::optional<S> first_mod(const Mods& mods, std::string_view prefix) {
    for (const auto& m : mods)
        if (starts(m, prefix)) return m;
    return std::nullopt;
}

S first_mod(const Mods& mods, std::string_view prefix, std::string_view fallback) {
    auto m = first_mod(mods, prefix);
    return m ? *m : S(fallback);
}

// [int(x) for x in mod.split("[")[1].rstrip("]").split(",")]
std::vector<int64_t> mod_bits(const S& mod) {
    auto parts = split(mod, '[');
    S inner = rstrip(at(parts, 1), "]");
    std::vector<int64_t> out;
    for (const auto& x : split(inner, ',')) out.push_back(py_int(x));
    return out;
}

int64_t inst_offset(const Mods& mods) {
    for (const auto& m : mods)
        if (starts(m, "offset:")) return py_int(at(split(m, ':'), 1), 0);
    return 0;
}

namespace {
// Namespace-scope tables, not function statics: FlamingoPort builds with /Zc:threadSafeInit- (the game takes the
// exe's TLS block), so a function-local static's first use isn't safe from several threads.
const std::map<S, std::pair<int, int>> kSdwaSel = {
    {"BYTE_0", {0, 8}}, {"BYTE_1", {8, 8}}, {"BYTE_2", {16, 8}}, {"BYTE_3", {24, 8}},
    {"WORD_0", {0, 16}}, {"WORD_1", {16, 16}}, {"DWORD", {0, 32}}};
}  // namespace

const std::map<S, std::pair<int, int>>& sdwa_sel() { return kSdwaSel; }

}  // namespace fp::xlat
