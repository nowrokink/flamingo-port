// Debug output: the game's OutputDebugString text goes to stderr too (it reports failed checks this way), and to
// the real function for an attached debugger.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

void WINAPI OutputDebugStringA_(LPCSTR text) {
    if (text) {
        size_t n = std::strlen(text);
        std::fprintf(stderr, "[game] %.*s%s", static_cast<int>(n), text, n && text[n - 1] == '\n' ? "" : "\n");
    }
    OutputDebugStringA(text);
}

void WINAPI OutputDebugStringW_(LPCWSTR text) {
    if (text) {
        size_t n = std::wcslen(text);
        std::fprintf(stderr, "[game] %.*ls%s", static_cast<int>(n), text, n && text[n - 1] == L'\n' ? "" : "\n");
    }
    OutputDebugStringW(text);
}

}  // namespace

SHIM_TABLE(debug,
    {"api-ms-win-core-debug-l1-1-0.dll", "OutputDebugStringA", reinterpret_cast<void*>(&OutputDebugStringA_)},
    {"api-ms-win-core-debug-l1-1-0.dll", "OutputDebugStringW", reinterpret_cast<void*>(&OutputDebugStringW_)},
    {"kernel32.dll", "OutputDebugStringA", reinterpret_cast<void*>(&OutputDebugStringA_)},
    {"kernel32.dll", "OutputDebugStringW", reinterpret_cast<void*>(&OutputDebugStringW_)},
);
