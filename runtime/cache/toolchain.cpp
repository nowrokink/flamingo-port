#include "toolchain.h"

#include <windows.h>

#include <atomic>

#include "core/fs.h"
#include "core/system.h"

namespace fp::cache {

std::string Tools::missing() const {
    std::string m;
    auto add = [&](const std::wstring& p, const char* name) {
        if (p.empty()) m += (m.empty() ? "" : ", ") + std::string(name);
    };
    add(spirv_as, "spirv-as");
    add(spirv_val, "spirv-val");
    add(spirv_cross, "spirv-cross");
    add(dxc, "dxc");
    return m;
}

Tools find_tools() {
    const std::wstring path = tool_path();
    return {which(L"spirv-as", path), which(L"spirv-val", path), which(L"spirv-cross", path), which(L"dxc", path)};
}

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\v\f"), b = s.find_last_not_of(" \t\r\n\v\f");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find_first_of("\r\n", at);
        if (e == std::string::npos) {
            out.push_back(text.substr(at));
            break;
        }
        out.push_back(text.substr(at, e - at));
        at = e + (text[e] == '\r' && e + 1 < text.size() && text[e + 1] == '\n' ? 2 : 1);
    }
    return out;
}

void remove_tree(const std::wstring& dir) {
    for (const auto& f : list_dir(dir, true, false)) DeleteFileW(join(dir, f).c_str());
    for (const auto& d : list_dir(dir, false, true)) remove_tree(join(dir, d));
    RemoveDirectoryW(dir.c_str());
}

std::atomic<unsigned> g_scratch{0};

}  // namespace

std::string first_line(const std::string& text) {
    const auto lines = lines_of(text);
    for (const auto& l : lines) {
        size_t k = l.find("error:");
        if (k != std::string::npos) return trim(l.substr(k + 6)).substr(0, 160);
    }
    for (const auto& l : lines)
        if (!trim(l).empty()) return trim(l).substr(0, 160);
    return "?";
}

ScratchDir::ScratchDir(const wchar_t* prefix) {
    wchar_t tmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (!n) return;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::wstring p = join(std::wstring(tmp, n), prefix + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                                        std::to_wstring(++g_scratch));
        if (CreateDirectoryW(p.c_str(), nullptr)) {
            path_ = p;
            return;
        }
    }
}

ScratchDir::~ScratchDir() {
    if (!path_.empty()) remove_tree(path_);
}

}  // namespace fp::cache
