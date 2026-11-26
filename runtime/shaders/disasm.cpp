// disassemble(): every FSSM blob's code through llvm-mc, into the listings the translator reads (shaders/prep.h).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/fs.h"
#include "core/process.h"
#include "core/text.h"
#include "shaders/dxbc.h"
#include "shaders/prep.h"

namespace fp::shaders {

using dxbc::u32;

namespace {

const uint8_t kEndpgm[4] = {0x00, 0x00, 0x81, 0xbf};

// The last s_endpgm wholly inside [from, to), or npos.
size_t rfind_endpgm(const std::string& b, size_t from, size_t to) {
    if (to > b.size()) to = b.size();
    if (to < from + 4) return std::string::npos;
    for (size_t p = to - 4 + 1; p-- > from;)
        if (!std::memcmp(b.data() + p, kEndpgm, 4)) return p;
    return std::string::npos;
}

struct Decoded {
    std::string asm_text, err;
    int bad = 0;
    bool ran = false;
};

Decoded llvm_disasm(const std::wstring& llvm_mc, const std::wstring& mcpu, const uint8_t* code, size_t n) {
    std::string text;
    text.reserve(n * 5);
    char buf[8];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, i ? " 0x%02x" : "0x%02x", code[i]);
        text += buf;
    }
    Spawn s;
    s.exe = llvm_mc;
    s.args = {L"--disassemble", L"-triple=amdgcn--amdpal", L"-mcpu=" + mcpu, L"-show-encoding"};
    Decoded d;
    d.ran = run_capture(s, text, d.asm_text, d.err) >= 0;
    for (size_t at = 0; (at = d.err.find("invalid instruction encoding", at)) != std::string::npos; ++at) ++d.bad;
    return d;
}

std::string lf(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')) out += s[i];
    return out;
}

std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// One blob: "clean", "invalid words", "no s_endpgm", "scan failed" or "error".
std::string disassemble_one(const std::wstring& path, const std::wstring& out, const std::wstring& llvm_mc,
                            const std::wstring& mcpu) {
    std::string b;
    if (!read_file(path, b)) return "error";
    const auto* p = reinterpret_cast<const uint8_t*>(b.data());
    // The code section's offset is the u32 at 0xC0; instructions start 0x28 bytes into it and run to the last
    // 4-byte-aligned s_endpgm in the blob (some blobs hold several programs).
    size_t start = std::string::npos, end = std::string::npos;
    const char* how = "scan";
    if (b.size() >= 0xC4) {
        const uint32_t off = u32(p + 0xC0);
        const size_t s = off + 0x28ull;
        if (off > 0 && s < b.size()) {
            size_t last = rfind_endpgm(b, s, b.size());
            while (last != std::string::npos && (last - s) % 4) last = rfind_endpgm(b, s, last);
            if (last != std::string::npos) {
                start = s;
                end = last + 4;
                how = "header";
            }
        }
    }
    if (end == std::string::npos) {
        size_t first = b.find(std::string(reinterpret_cast<const char*>(kEndpgm), 4));
        if (first == std::string::npos) return "no s_endpgm";
        end = first + 4;
    }
    if (start == std::string::npos) {
        // Walk back in 4-byte steps from the end and keep the longest clean decode.
        size_t best = std::string::npos;
        const size_t floor_at = std::max<size_t>(0x100, end > 0x4000 ? end - 0x4000 : 0);
        for (size_t s = end - 4; s + 1 > floor_at && s < end; s -= 4) {
            if (llvm_disasm(llvm_mc, mcpu, p + s, end - s).bad == 0) best = s;
            if (s < 4) break;
        }
        if (best == std::string::npos) return "scan failed";
        start = best;
    }
    Decoded d = llvm_disasm(llvm_mc, mcpu, p + start, end - start);
    if (!d.ran) return "error";
    std::wstring name = file_name(path);
    name = name.substr(0, name.find_last_of(L'.'));
    char head[256];
    std::snprintf(head, sizeof head, "; %s  code 0x%zx..0x%zx (%s), %s, %d invalid words\n", narrow(name).c_str(), start,
                  end, how, narrow(mcpu).c_str(), d.bad);
    std::string text = head + lf(d.asm_text);
    std::string err = strip(lf(d.err));
    if (!err.empty()) {
        std::string quoted;
        for (char c : err) quoted += c == '\n' ? std::string("\n; ") : std::string(1, c);
        text += "\n; --- llvm-mc messages ---\n; " + quoted + "\n";
    }
    // written as Python's text mode did on Windows
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    if (!write_file(join(out, name + L".s"), crlf.data(), crlf.size())) return "error";
    return d.bad == 0 ? "clean" : "invalid words";
}

}  // namespace

bool disassemble(const std::wstring& fssm_dir, const std::wstring& out, const std::wstring& llvm_mc,
                 const std::wstring& mcpu, int jobs, const std::atomic<bool>& stop, const Progress& progress,
                 const Log& log) {
    if (!make_dirs(out)) return false;
    std::vector<std::wstring> files;
    for (auto& f : list_dir(fssm_dir))
        if (f.size() > 5 && f.compare(f.size() - 5, 5, L".fssm") == 0) files.push_back(join(fssm_dir, f));
    std::atomic<size_t> next{0}, done{0};
    std::mutex m;
    std::map<std::string, int> results;
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(1, jobs); ++t)
        pool.emplace_back([&] {
            for (size_t i; !stop && (i = next++) < files.size();) {
                std::string r = disassemble_one(files[i], out, llvm_mc, mcpu);
                std::lock_guard lock(m);
                ++results[r];
                ++done;
            }
        });
    while (done < files.size() && !stop) {
        progress(done, files.size());
        Sleep(250);
    }
    for (auto& t : pool) t.join();
    if (stop) return false;
    progress(files.size(), files.size());
    std::string summary = std::to_string(files.size()) + " shaders disassembled:";
    for (const auto& [r, count] : results) summary += " " + r + " " + std::to_string(count);
    log(summary);
    return results["error"] == 0;
}

}  // namespace fp::shaders
