#include <windows.h>

#include "system.h"

#include <shlobj.h>

#include "fs.h"

namespace fp {

std::wstring env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return v;
}

std::wstring self_path() {
    std::wstring p(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
        if (n < p.size()) {
            p.resize(n);
            return p;
        }
        p.resize(p.size() * 2);
    }
}

std::wstring local_appdata() {
    std::wstring base = env(L"LOCALAPPDATA");
    if (base.empty()) base = env(L"USERPROFILE");
    return join(base, L"Flamingo");
}

std::wstring documents() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    if (out.empty()) out = join(env(L"USERPROFILE"), L"Documents");
    return out;
}

std::wstring repo_root() {
    std::wstring dir = parent(self_path());
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        if (is_dir(join(dir, L"game")) && is_dir(join(dir, L"runtime"))) return dir;
        std::wstring p = parent(dir);
        if (p == dir) break;
        dir = p;
    }
    return {};
}

std::wstring tool_path() {
    std::wstring path = env(L"PATH");
    std::wstring sdk = env(L"VULKAN_SDK");
    if (sdk.empty()) {
        // the machine-wide value, when this session's environment predates the SDK's installer
        wchar_t buf[MAX_PATH] = {};
        DWORD size = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                         L"VULKAN_SDK", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            sdk = buf;
    }
    if (!sdk.empty() && is_dir(join(sdk, L"Bin"))) path += L";" + join(sdk, L"Bin");
    return path;
}

std::wstring which(const std::wstring& exe, const std::wstring& path_in) {
    if (exe.find_first_of(L"\\/") != std::wstring::npos) return is_file(exe) ? exe : L"";
    std::wstring path = path_in.empty() ? env(L"PATH") : path_in;
    std::wstring name = exe;
    if (name.find(L'.') == std::wstring::npos) name += L".exe";
    size_t at = 0;
    while (at <= path.size()) {
        size_t semi = path.find(L';', at);
        std::wstring dir = path.substr(at, semi == std::wstring::npos ? std::wstring::npos : semi - at);
        at = semi == std::wstring::npos ? path.size() + 1 : semi + 1;
        if (dir.size() > 1 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) continue;
        std::wstring p = join(dir, name);
        if (is_file(p)) return p;
    }
    return {};
}

}  // namespace fp
