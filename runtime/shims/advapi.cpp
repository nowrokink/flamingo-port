// Registry calls: forwarded to the host, and logged, since the console registry has keys a PC doesn't.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "shims.h"

namespace {

const wchar_t* root_name(HKEY k) {
    if (k == HKEY_LOCAL_MACHINE) return L"HKLM";
    if (k == HKEY_CURRENT_USER) return L"HKCU";
    if (k == HKEY_CLASSES_ROOT) return L"HKCR";
    if (k == HKEY_USERS) return L"HKU";
    return nullptr;
}

// The console's device type: code linked into the title reads ConsoleMode (a DWORD) from HKLM\Software\Microsoft\
// Windows NT\CurrentVersion\OEM and takes it missing for a PC. 0x64 Series X, 0x60 Series S (as the ztrx project
// serves it). The key is ours: a fixed non-host value stands for it.
HKEY const kOemKey = reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x0F1A4160));
constexpr const wchar_t* kOemPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";

LSTATUS WINAPI RegOpenKeyW_(HKEY key, LPCWSTR sub, PHKEY out) {
    if (key == HKEY_LOCAL_MACHINE && sub && out && _wcsicmp(sub, kOemPath) == 0) {
        *out = kOemKey;
        std::fprintf(stderr, "[rt] RegOpenKeyW(HKLM, OEM): the console's key (%s)\n",
                     rt::shims::series_s() ? "Xbox Series S" : "Xbox Series X");
        return ERROR_SUCCESS;
    }
    LSTATUS r = RegOpenKeyW(key, sub, out);
    wchar_t keyname[32];
    const wchar_t* root = root_name(key);
    if (!root) {
        std::swprintf(keyname, 32, L"key %p", static_cast<void*>(key));
        root = keyname;
    }
    std::fprintf(stderr, "[rt] RegOpenKeyW(%ls, \"%ls\") -> %ld\n", root, sub ? sub : L"", r);
    return r;
}

LSTATUS WINAPI RegQueryValueExW_(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
    if (key == kOemKey) {
        if (!name || _wcsicmp(name, L"ConsoleMode") != 0) return ERROR_FILE_NOT_FOUND;
        if (type) *type = REG_DWORD;
        if (!size) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
        const DWORD mode = rt::shims::series_s() ? 0x60 : 0x64;
        DWORD room = *size;
        *size = sizeof mode;
        if (!data) return ERROR_SUCCESS;
        if (room < sizeof mode) return ERROR_MORE_DATA;
        std::memcpy(data, &mode, sizeof mode);
        std::fprintf(stderr, "[rt] RegQueryValueExW(ConsoleMode) -> %#lx\n", mode);
        return ERROR_SUCCESS;
    }
    LSTATUS r = RegQueryValueExW(key, name, reserved, type, data, size);
    std::fprintf(stderr, "[rt] RegQueryValueExW(\"%ls\") -> %ld\n", name ? name : L"", r);
    return r;
}

}  // namespace

SHIM_TABLE(advapi,
    {"advapi32.dll", "RegOpenKeyW", reinterpret_cast<void*>(&RegOpenKeyW_)},
    {"advapi32.dll", "RegQueryValueExW", reinterpret_cast<void*>(&RegQueryValueExW_)},
);
