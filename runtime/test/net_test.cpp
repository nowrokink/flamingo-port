// The title must stay offline: every network entry point it imports is bound to a shim that fails.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <cstdio>

#include "../shims/shims.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

template <class F>
F shim(const char* dll, const char* name) {
    void* p = rt::shims::find(dll, name);
    if (!p) {
        std::fprintf(stderr, "FAIL: %s!%s is not blocked\n", dll, name);
        ++g_failures;
    }
    return reinterpret_cast<F>(p);
}

}  // namespace

int main() {
    auto open = shim<HINTERNET(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD)>("winhttp.dll", "WinHttpOpen");
    auto connect = shim<HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD)>("winhttp.dll", "WinHttpConnect");
    auto send = shim<BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR)>("winhttp.dll",
                                                                                              "WinHttpSendRequest");
    if (open) CHECK(open(L"test", 0, nullptr, nullptr, 0) == nullptr);
    if (connect) CHECK(connect(reinterpret_cast<HINTERNET>(1), L"example.com", 443, 0) == nullptr);
    if (send) CHECK(!send(reinterpret_cast<HINTERNET>(1), nullptr, 0, nullptr, 0, 0, 0));
    shim<void*>("winhttp.dll", "WinHttpOpenRequest");
    shim<void*>("winhttp.dll", "WinHttpWriteData");

    // Winsock, by name and by the ordinals the exe imports.
    for (const char* name : {"socket", "#23"}) {
        auto sock = shim<SOCKET(WSAAPI*)(int, int, int)>("ws2_32.dll", name);
        if (sock) CHECK(sock(AF_INET, SOCK_STREAM, 0) == INVALID_SOCKET);
    }
    for (const char* name : {"connect", "#4"}) {
        auto conn = shim<int(WSAAPI*)(SOCKET, const sockaddr*, int)>("ws2_32.dll", name);
        if (conn) CHECK(conn(0, nullptr, 0) == SOCKET_ERROR);
    }
    for (const char* name : {"sendto", "#20"}) shim<void*>("ws2_32.dll", name);
    for (const char* name : {"gethostbyname", "#52"}) {
        auto byname = shim<hostent*(WSAAPI*)(const char*)>("ws2_32.dll", name);
        if (byname) CHECK(byname("example.com") == nullptr);
    }
    auto gai = shim<INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*)>("ws2_32.dll", "getaddrinfo");
    PADDRINFOA result = reinterpret_cast<PADDRINFOA>(1);
    if (gai) CHECK(gai("example.com", "443", nullptr, &result) != 0 && result == nullptr);

    if (g_failures) {
        std::fprintf(stderr, "net_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("net_test: ok\n");
    return 0;
}
