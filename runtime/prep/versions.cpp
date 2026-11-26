#include "prep/versions.h"

#include <windows.h>

#include <functional>
#include <set>

#include "core/fs.h"
#include "core/system.h"
#include "core/text.h"

namespace fp::prep {
namespace {

// Small files checked by size to recognise a version without reading anything large.
const char* const kProbes[] = {"MicrosoftGame.Config", "Catalog000.bin", "appdata0.rpf"};

std::wstring lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

}  // namespace

std::vector<json::Value> versions() {
    std::vector<json::Value> out;
    const std::wstring dir = join(repo_root(), L"game");
    for (const auto& f : list_dir(dir)) {
        if (f.size() < 6 || f.compare(f.size() - 5, 5, L".json")) continue;
        json::Value v;
        if (json::load_file(join(dir, f), v) && v["files"].is_object()) out.push_back(std::move(v));
    }
    return out;
}

json::Value match_version(const std::wstring& game) {
    if (game.empty()) return {};
    for (auto& v : versions()) {
        int probes = 0;
        bool ok = true;
        for (const char* p : kProbes) {
            const json::Value& f = v["files"][p];
            if (f.is_null()) continue;
            ++probes;
            if (file_size(join(game, widen(p))) != f["size"].integer(-2)) ok = false;
        }
        if (probes && ok) return v;
    }
    return {};
}

std::vector<std::pair<std::wstring, json::Value>> find_copies() {
    const std::wstring home = env(L"USERPROFILE");
    std::vector<std::wstring> roots;
    for (const wchar_t* d : {L"Downloads", L"Desktop", L"Documents", L"Games"}) roots.push_back(join(home, d));
    roots.push_back(home);
    DWORD drives = GetLogicalDrives();
    for (int k = 0; k < 26; ++k)
        if (drives & (1u << k)) {
            std::wstring r = std::wstring(1, static_cast<wchar_t>(L'A' + k)) + L":\\";
            UINT type = GetDriveTypeW(r.c_str());
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_REMOTE) roots.push_back(r);
        }
    static const std::set<std::wstring> skip = {L"windows", L"program files", L"program files (x86)", L"programdata",
                                                L"appdata", L"$recycle.bin", L"system volume information",
                                                L"node_modules", L".git"};
    std::set<std::wstring> seen;
    std::vector<std::pair<std::wstring, json::Value>> found;
    std::function<void(const std::wstring&, int)> visit = [&](const std::wstring& d, int depth) {
        if (!seen.insert(lower(full_path(d))).second) return;
        if (is_file(join(d, L"MicrosoftGame.Config"))) {
            json::Value v = match_version(d);
            if (!v.is_null()) found.emplace_back(full_path(d), std::move(v));
            return;
        }
        if (depth == 0) return;
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileExW(join(d, L"*").c_str(), FindExInfoBasic, &f, FindExSearchLimitToDirectories,
                                    nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> subs;
        do {
            if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            std::wstring name = f.cFileName;
            if (name == L"." || name == L".." || name[0] == L'.' || name[0] == L'$' || skip.count(lower(name)))
                continue;
            subs.push_back(join(d, name));
        } while (FindNextFileW(h, &f));
        FindClose(h);
        for (const auto& s : subs) visit(s, depth - 1);
    };
    for (const auto& r : roots)
        if (is_dir(r)) visit(r, 2);
    return found;
}

std::wstring default_out() { return join(documents(), L"Flamingo"); }
}  // namespace fp::prep
