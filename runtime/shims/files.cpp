// Virtual drives: on the console the title's storage is drives of its own (G: the package, T: temporary space...),
// and titles build paths on them. Every path-taking import of the title and its DLLs is bound here: a path on a
// mounted drive ("X:\..", "X:/..", "X:", "\\?\X:\..", "\??\X:\..") is rewritten to the host folder behind it and
// handed to the host's wide function; any other path goes to the host unchanged. Narrow paths are converted from
// the ANSI code page first, so host folders with any characters work.
// The title calls _wfopen, _wopen and friends; their wrappers call the same functions, deprecated or not.
#define _CRT_SECURE_NO_WARNINGS
#include "files.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <werapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>

#include "shims.h"

namespace rt::files {
namespace {

std::mutex g_m;
std::wstring g_roots[26];  // by letter; empty when not mounted

int letter_index(wchar_t c) {
    c = static_cast<wchar_t>(std::towupper(c));
    return c >= L'A' && c <= L'Z' ? c - L'A' : -1;
}

// Logs, once per letter, a drive path that is neither mounted nor a host drive: a console drive we don't serve.
void unknown_drive(int i, const wchar_t* path) {
    static std::set<int> seen;
    if ((GetLogicalDrives() >> i) & 1) return;
    std::lock_guard lock(g_m);
    if (seen.insert(i).second) std::fprintf(stderr, "[rt] files: no drive %lc: (\"%ls\")\n", L'A' + i, path);
}

}  // namespace

void mount(wchar_t letter, const std::filesystem::path& root) {
    int i = letter_index(letter);
    if (i < 0) return;
    std::wstring r = std::filesystem::absolute(root).wstring();
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    std::lock_guard lock(g_m);
    g_roots[i] = r;
    std::fprintf(stderr, "[rt] files: %lc: -> %ls\n", static_cast<wchar_t>(L'A' + i), r.c_str());
}

wchar_t mount_free(const std::filesystem::path& root) {
    std::wstring r = std::filesystem::absolute(root).wstring();
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    DWORD host = GetLogicalDrives();
    int pick = -1;
    {
        std::lock_guard lock(g_m);
        for (int i = 0; i < 26; ++i)
            if (_wcsicmp(g_roots[i].c_str(), r.c_str()) == 0) return static_cast<wchar_t>(L'A' + i);
        for (int i = 25; i >= 3 && pick < 0; --i)  // never A:, B: or C:
            if (!((host >> i) & 1) && g_roots[i].empty()) pick = i;
    }
    if (pick < 0) return 0;
    mount(static_cast<wchar_t>(L'A' + pick), root);
    return static_cast<wchar_t>(L'A' + pick);
}

std::wstring to_host(const wchar_t* path) {
    if (!path) return {};
    const wchar_t* p = path;
    if ((p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/' || p[1] == L'?') && p[2] == L'?' &&
        (p[3] == L'\\' || p[3] == L'/'))
        p += 4;  // \\?\X: and \??\X:
    int i = (p[0] && p[1] == L':') ? letter_index(p[0]) : -1;
    if (i < 0) return path;
    std::wstring root;
    {
        std::lock_guard lock(g_m);
        root = g_roots[i];
    }
    if (root.empty()) {
        unknown_drive(i, path);
        return path;
    }
    const wchar_t* rest = p + 2;
    while (*rest == L'\\' || *rest == L'/') ++rest;
    std::wstring out = root;
    if (*rest) out += L'\\';
    for (; *rest; ++rest) out += *rest == L'/' ? L'\\' : *rest;
    return out;
}

std::wstring to_host(const char* path) {
    if (!path) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, path, -1, w.data(), n);
    return to_host(w.c_str());
}

}  // namespace rt::files

namespace {

using rt::files::to_host;

std::wstring wide(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

// The title's opens outside the package drive (its temporary and persistent storage: what it writes), the first of
// them logged with their outcome: a title that can't write its settings or saves waits on them.
std::atomic<int> g_open_logged{0};
HANDLE log_open(const wchar_t* title_path, const std::wstring& host, DWORD access, DWORD disposition, HANDLE result) {
    DWORD err = GetLastError();
    if (title_path && !((title_path[0] == L'G' || title_path[0] == L'g') && title_path[1] == L':') &&
        g_open_logged++ < 120)
        std::fprintf(stderr, "[rt] files: open \"%ls\" -> \"%ls\" access %#lx disposition %lu: %s (%lu)\n", title_path,
                     host.c_str(), access, disposition, result == INVALID_HANDLE_VALUE ? "FAILED" : "ok",
                     result == INVALID_HANDLE_VALUE ? err : 0);
    SetLastError(err);
    return result;
}

// Each wrapper keeps a null path null for the host, which decides what that means.
HANDLE WINAPI CreateFileW_(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                           DWORD flags, HANDLE templ) {
    std::wstring h = to_host(path);
    return log_open(path, h, access, disposition,
                    CreateFileW(path ? h.c_str() : nullptr, access, share, sa, disposition, flags, templ));
}
HANDLE WINAPI CreateFileA_(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                           DWORD flags, HANDLE templ) {
    std::wstring h = to_host(path);
    std::wstring w = wide(path);
    return log_open(path ? w.c_str() : nullptr, h, access, disposition,
                    CreateFileW(path ? h.c_str() : nullptr, access, share, sa, disposition, flags, templ));
}
HANDLE WINAPI CreateFile2_(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                           LPCREATEFILE2_EXTENDED_PARAMETERS params) {
    std::wstring h = to_host(path);
    return log_open(path, h, access, disposition, CreateFile2(path ? h.c_str() : nullptr, access, share, disposition, params));
}
BOOL WINAPI CreateDirectoryW_(LPCWSTR path, LPSECURITY_ATTRIBUTES sa) {
    std::wstring h = to_host(path);
    return CreateDirectoryW(path ? h.c_str() : nullptr, sa);
}
BOOL WINAPI DeleteFileW_(LPCWSTR path) {
    std::wstring h = to_host(path);
    return DeleteFileW(path ? h.c_str() : nullptr);
}
BOOL WINAPI RemoveDirectoryW_(LPCWSTR path) {
    std::wstring h = to_host(path);
    return RemoveDirectoryW(path ? h.c_str() : nullptr);
}
HANDLE WINAPI FindFirstFileW_(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    std::wstring h = to_host(path);
    HANDLE r = FindFirstFileW(path ? h.c_str() : nullptr, data);
    DWORD err = GetLastError();
    static std::atomic<int> logged{0};
    if (path && logged++ < 60)  // what the title lists (its packs among them), and whether anything is there
        std::fprintf(stderr, "[rt] files: list \"%ls\" -> \"%ls\": %s%ls\n", path, h.c_str(),
                     r == INVALID_HANDLE_VALUE ? "nothing" : "first ", r == INVALID_HANDLE_VALUE ? L"" : data->cFileName);
    SetLastError(err);
    return r;
}
DWORD WINAPI GetFileAttributesW_(LPCWSTR path) {
    std::wstring h = to_host(path);
    return GetFileAttributesW(path ? h.c_str() : nullptr);
}
DWORD WINAPI GetFileAttributesA_(LPCSTR path) {
    std::wstring h = to_host(path);
    return GetFileAttributesW(path ? h.c_str() : nullptr);
}
BOOL WINAPI GetFileAttributesExW_(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    std::wstring h = to_host(path);
    return GetFileAttributesExW(path ? h.c_str() : nullptr, level, info);
}
BOOL WINAPI SetFileAttributesW_(LPCWSTR path, DWORD attrs) {
    std::wstring h = to_host(path);
    return SetFileAttributesW(path ? h.c_str() : nullptr, attrs);
}
BOOL WINAPI GetDiskFreeSpaceExW_(LPCWSTR path, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free) {
    std::wstring h = to_host(path);
    return GetDiskFreeSpaceExW(path ? h.c_str() : nullptr, avail, total, free);
}
BOOL WINAPI MoveFileExW_(LPCWSTR from, LPCWSTR to, DWORD flags) {
    std::wstring f = to_host(from), t = to_host(to);
    return MoveFileExW(from ? f.c_str() : nullptr, to ? t.c_str() : nullptr, flags);
}
HRESULT WINAPI WerRegisterFile_(PCWSTR path, WER_REGISTER_FILE_TYPE type, DWORD flags) {
    std::wstring h = to_host(path);
    return WerRegisterFile(path ? h.c_str() : nullptr, type, flags);
}

// The C runtime: the host's own (ucrtbase), which the title uses as well, so FILE* and descriptors are shared.
// The C library's and the C++ library's opens outside the package drive, the first of them logged like log_open's.
FILE* log_fopen(const wchar_t* how, const wchar_t* title_path, const std::wstring& host, FILE* result) {
    if (title_path && !((title_path[0] == L'G' || title_path[0] == L'g') && title_path[1] == L':') &&
        g_open_logged++ < 120)
        std::fprintf(stderr, "[rt] files: %ls \"%ls\" -> \"%ls\": %s\n", how, title_path, host.c_str(),
                     result ? "ok" : "FAILED");
    return result;
}
FILE* __cdecl _wfopen_(const wchar_t* path, const wchar_t* mode) {
    std::wstring h = to_host(path);
    return log_fopen(L"_wfopen", path, h, _wfopen(path ? h.c_str() : nullptr, mode));
}
FILE* __cdecl fopen_(const char* path, const char* mode) {
    std::wstring h = to_host(path), m = wide(mode), w = wide(path);
    return log_fopen(L"fopen", path ? w.c_str() : nullptr, h,
                     _wfopen(path ? h.c_str() : nullptr, mode ? m.c_str() : nullptr));
}
// FILE* std::_Fiopen(path, ios_base::openmode, int protection), narrow and wide: what std::fstream opens a file
// through. The title's streams come from the host's msvcp140.dll, whose own open never passes the wrappers above,
// so a stream on a mounted drive ("Z:\settings\settings.xml", the title's settings) found no such drive and the
// title never wrote its settings (1.0.2.1 then waits on its loading screen). The path is translated here.
FILE* __cdecl fiopen_wide_(const wchar_t* path, int mode, int protection) {
    std::wstring h = to_host(path);
    return log_fopen(L"std::fstream", path, h,
                     std::_Fiopen(path ? h.c_str() : nullptr, static_cast<std::ios_base::openmode>(mode), protection));
}
FILE* __cdecl fiopen_narrow_(const char* path, int mode, int protection) {
    std::wstring w = wide(path);
    return fiopen_wide_(path ? w.c_str() : nullptr, mode, protection);
}
int __cdecl _wopen_(const wchar_t* path, int flags, int pmode) {
    std::wstring h = to_host(path);
    return _wopen(path ? h.c_str() : nullptr, flags, pmode);
}
int __cdecl _open_(const char* path, int flags, int pmode) {
    std::wstring h = to_host(path);
    return _wopen(path ? h.c_str() : nullptr, flags, pmode);
}
int __cdecl _wstat64_(const wchar_t* path, struct _stat64* st) {
    std::wstring h = to_host(path);
    return _wstat64(path ? h.c_str() : nullptr, st);
}
int __cdecl remove_(const char* path) {
    std::wstring h = to_host(path);
    return _wremove(path ? h.c_str() : nullptr);
}

// std::_Fiopen: how the title's std::ifstream/ofstream open files (MSVCP140), wide and narrow.
using FiopenW = FILE*(__cdecl*)(const wchar_t*, int, int);
constexpr const char* kFiopenW = "?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z";
constexpr const char* kFiopenA = "?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z";

FiopenW host_fiopen() {
    static FiopenW fn = [] {
        HMODULE m = LoadLibraryExW(L"msvcp140.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<FiopenW>(GetProcAddress(m, kFiopenW)) : nullptr;
    }();
    return fn;
}
FILE* __cdecl fiopen_w(const wchar_t* path, int mode, int prot) {
    std::wstring h = to_host(path);
    FiopenW fn = host_fiopen();
    return fn ? fn(path ? h.c_str() : nullptr, mode, prot) : nullptr;
}
FILE* __cdecl fiopen_a(const char* path, int mode, int prot) {
    std::wstring h = to_host(path);
    FiopenW fn = host_fiopen();
    return fn ? fn(path ? h.c_str() : nullptr, mode, prot) : nullptr;
}
[[maybe_unused]] const bool g_made_early = (host_fiopen(), true);  // the runtime has no thread-safe statics

}  // namespace

#define FN(dll, name) {dll, #name, reinterpret_cast<void*>(&name##_)}
SHIM_TABLE(files,
    FN("api-ms-win-core-file-l1-1-0.dll", CreateFileW),
    FN("api-ms-win-core-file-l1-1-0.dll", CreateDirectoryW),
    FN("api-ms-win-core-file-l1-1-0.dll", DeleteFileW),
    FN("api-ms-win-core-file-l1-1-0.dll", RemoveDirectoryW),
    FN("api-ms-win-core-file-l1-1-0.dll", FindFirstFileW),
    FN("api-ms-win-core-file-l1-1-0.dll", GetFileAttributesW),
    FN("api-ms-win-core-file-l1-1-0.dll", GetFileAttributesA),
    FN("api-ms-win-core-file-l1-1-0.dll", GetFileAttributesExW),
    FN("api-ms-win-core-file-l1-1-0.dll", SetFileAttributesW),
    FN("api-ms-win-core-file-l1-1-0.dll", GetDiskFreeSpaceExW),
    FN("api-ms-win-core-file-l2-1-0.dll", MoveFileExW),
    FN("api-ms-win-core-file-l1-2-0.dll", CreateFile2),
    FN("kernel32.dll", CreateFileW),
    FN("kernel32.dll", CreateFileA),
    FN("kernel32.dll", GetFileAttributesW),
    FN("api-ms-win-core-windowserrorreporting-l1-1-0.dll", WerRegisterFile),
    {"msvcp140.dll", "?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z", reinterpret_cast<void*>(&fiopen_wide_)},
    {"msvcp140.dll", "?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z", reinterpret_cast<void*>(&fiopen_narrow_)},
    FN("api-ms-win-crt-stdio-l1-1-0.dll", _wfopen),
    FN("api-ms-win-crt-stdio-l1-1-0.dll", fopen),
    FN("api-ms-win-crt-stdio-l1-1-0.dll", _wopen),
    FN("api-ms-win-crt-stdio-l1-1-0.dll", _open),
    FN("api-ms-win-crt-filesystem-l1-1-0.dll", _wstat64),
    FN("api-ms-win-crt-filesystem-l1-1-0.dll", remove),
    {"msvcp140.dll", kFiopenW, reinterpret_cast<void*>(&fiopen_w)},
    {"msvcp140.dll", kFiopenA, reinterpret_cast<void*>(&fiopen_a)},
);
