// Library loading by name. Console-only DLLs the runtime implements (xgameruntime.dll) load as a pseudo handle
// whose GetProcAddress finds our functions: the host may have a desktop xgameruntime.dll (Gaming Services) with
// different interfaces, which the title must not get. Everything else goes to the host.
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>

#include "shims.h"

namespace rt::shims {

// Pseudo handles: odd values no real module has (modules are 64 KiB aligned).
HMODULE pseudo_module(const char* dll_lower);
const char* pseudo_module_name(HMODULE h);

}  // namespace rt::shims

namespace {

const char* const kPseudoDlls[] = {"xgameruntime.dll"};

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The file name part of a narrow or wide DLL name, lower case, with ".dll" added if there's no extension.
std::string dll_key(const std::string& name) {
    size_t slash = name.find_last_of("\\/");
    std::string base = lower_ascii(slash == std::string::npos ? name : name.substr(slash + 1));
    if (base.find('.') == std::string::npos) base += ".dll";
    return base;
}

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) s += *w < 0x80 ? static_cast<char>(*w) : '?';
    return s;
}

HMODULE pseudo_for(const std::string& name) {
    std::string key = dll_key(name);
    for (const char* d : kPseudoDlls)
        if (key == d) return rt::shims::pseudo_module(d);
    return nullptr;
}

HMODULE WINAPI LoadLibraryExW_(LPCWSTR name, HANDLE file, DWORD flags) {
    std::string n = narrow(name);
    if (HMODULE p = pseudo_for(n)) {
        std::fprintf(stderr, "[rt] LoadLibraryExW(\"%s\") -> runtime's own\n", n.c_str());
        return p;
    }
    HMODULE h = LoadLibraryExW(name, file, flags);
    std::fprintf(stderr, "[rt] LoadLibraryExW(\"%s\", %lx) -> %p\n", n.c_str(), flags, static_cast<void*>(h));
    return h;
}

HMODULE WINAPI LoadLibraryExA_(LPCSTR name, HANDLE file, DWORD flags) {
    std::string n = name ? name : "";
    if (HMODULE p = pseudo_for(n)) {
        std::fprintf(stderr, "[rt] LoadLibraryExA(\"%s\") -> runtime's own\n", n.c_str());
        return p;
    }
    HMODULE h = LoadLibraryExA(name, file, flags);
    std::fprintf(stderr, "[rt] LoadLibraryExA(\"%s\", %lx) -> %p\n", n.c_str(), flags, static_cast<void*>(h));
    return h;
}

HMODULE WINAPI LoadLibraryW_(LPCWSTR name) { return LoadLibraryExW_(name, nullptr, 0); }

HMODULE WINAPI GetModuleHandleW_(LPCWSTR name) {
    if (name)
        if (HMODULE p = pseudo_for(narrow(name))) return p;
    return GetModuleHandleW(name);
}

HMODULE WINAPI GetModuleHandleA_(LPCSTR name) {
    if (name)
        if (HMODULE p = pseudo_for(name)) return p;
    return GetModuleHandleA(name);
}

FARPROC WINAPI GetProcAddress_(HMODULE module, LPCSTR name) {
    bool by_ordinal = reinterpret_cast<uintptr_t>(name) <= 0xFFFF;
    std::string sym = by_ordinal ? "#" + std::to_string(reinterpret_cast<uintptr_t>(name)) : name;
    if (const char* dll = rt::shims::pseudo_module_name(module)) {
        void* fn = rt::shims::find(dll, sym);
        std::fprintf(stderr, "[rt] GetProcAddress(%s, %s) -> %s\n", dll, sym.c_str(), fn ? "ours" : "MISSING");
        if (!fn) SetLastError(ERROR_PROC_NOT_FOUND);
        return reinterpret_cast<FARPROC>(fn);
    }
    // A host DLL: our override where import binding would use one (so the network block, file paths etc. can't be
    // sidestepped by looking functions up by name), else the host's own.
    wchar_t file[MAX_PATH] = {};
    if (module && GetModuleFileNameW(module, file, MAX_PATH))
        if (void* ours = rt::shims::find(dll_key(narrow(file)), sym)) {
            std::fprintf(stderr, "[rt] GetProcAddress(%s, %s) -> ours\n", dll_key(narrow(file)).c_str(), sym.c_str());
            return reinterpret_cast<FARPROC>(ours);
        }
    FARPROC p = GetProcAddress(module, name);
    std::fprintf(stderr, "[rt] GetProcAddress(%p, %s) -> %p\n", static_cast<void*>(module), sym.c_str(),
                 reinterpret_cast<void*>(p));
    return p;
}

std::wstring& title_path() {
    static std::wstring p;
    return p;
}

// For the process's own module the title gets its own path, as on the console, not FlamingoPort.exe's.
DWORD WINAPI GetModuleFileNameW_(HMODULE module, LPWSTR out, DWORD size) {
    if (module || title_path().empty()) return GetModuleFileNameW(module, out, size);
    const std::wstring& p = title_path();
    if (!out || !size) return 0;
    DWORD n = static_cast<DWORD>(p.size()) < size ? static_cast<DWORD>(p.size()) : size - 1;
    std::wmemcpy(out, p.c_str(), n);
    out[n] = 0;
    if (n < p.size()) SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return n < p.size() ? size : n;
}

DWORD WINAPI GetModuleFileNameA_(HMODULE module, LPSTR out, DWORD size) {
    if (module || title_path().empty()) return GetModuleFileNameA(module, out, size);
    std::string p = narrow(title_path().c_str());
    if (!out || !size) return 0;
    DWORD n = static_cast<DWORD>(p.size()) < size ? static_cast<DWORD>(p.size()) : size - 1;
    std::memcpy(out, p.c_str(), n);
    out[n] = 0;
    if (n < p.size()) SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return n < p.size() ? size : n;
}

BOOL WINAPI FreeLibrary_(HMODULE module) {
    if (rt::shims::pseudo_module_name(module)) return TRUE;
    return FreeLibrary(module);
}

}  // namespace

namespace rt::shims {

void set_title_path(const std::wstring& path) { title_path() = path; }

HMODULE pseudo_module(const char* dll_lower) {
    for (size_t i = 0; i < std::size(kPseudoDlls); ++i)
        if (std::strcmp(dll_lower, kPseudoDlls[i]) == 0) return reinterpret_cast<HMODULE>(0x5EED0001 + 2 * i);
    return nullptr;
}

const char* pseudo_module_name(HMODULE h) {
    uintptr_t v = reinterpret_cast<uintptr_t>(h);
    if (v < 0x5EED0001 || (v - 0x5EED0001) % 2) return nullptr;
    size_t i = (v - 0x5EED0001) / 2;
    return i < std::size(kPseudoDlls) ? kPseudoDlls[i] : nullptr;
}

}  // namespace rt::shims

#define LL(dll) "api-ms-win-core-libraryloader-" dll ".dll"
SHIM_TABLE(loader,
    {LL("l1-2-0"), "LoadLibraryExW", reinterpret_cast<void*>(&LoadLibraryExW_)},
    {LL("l1-2-0"), "LoadLibraryExA", reinterpret_cast<void*>(&LoadLibraryExA_)},
    {LL("l1-2-1"), "LoadLibraryW", reinterpret_cast<void*>(&LoadLibraryW_)},
    {LL("l1-2-0"), "GetModuleHandleW", reinterpret_cast<void*>(&GetModuleHandleW_)},
    {LL("l1-2-0"), "GetModuleHandleA", reinterpret_cast<void*>(&GetModuleHandleA_)},
    {LL("l1-2-0"), "GetProcAddress", reinterpret_cast<void*>(&GetProcAddress_)},
    {LL("l1-2-0"), "FreeLibrary", reinterpret_cast<void*>(&FreeLibrary_)},
    {LL("l1-2-0"), "GetModuleFileNameW", reinterpret_cast<void*>(&GetModuleFileNameW_)},
    {LL("l1-2-0"), "GetModuleFileNameA", reinterpret_cast<void*>(&GetModuleFileNameA_)},
);
