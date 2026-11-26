// Network: offline by design. The title (and libHttpClient beside it) must not reach the network from this PC: its
// crash reporter uploads dumps and machine details over HTTP, and online services expect a console. Every way out
// fails as an unplugged network would, and each first attempt is logged.
//
// WinHTTP: opening a session, connecting and sending fail. Winsock: creating a socket, connecting, sending to an
// address and name lookups fail. (The exe imports Winsock by ordinal.)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "shims.h"

namespace {

void blocked(const char* what) {
    static std::mutex m;
    static std::set<std::string> seen;
    std::lock_guard lock(m);
    if (seen.insert(what).second) std::fprintf(stderr, "[rt] net: %s blocked (offline)\n", what);
}

// ---------------------------------------------------------------- WinHTTP

HINTERNET WINAPI WinHttpOpen_(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) {
    blocked("WinHttpOpen");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return nullptr;
}
HINTERNET WINAPI WinHttpConnect_(HINTERNET, LPCWSTR host, INTERNET_PORT, DWORD) {
    blocked("WinHttpConnect");
    if (host) std::fprintf(stderr, "[rt] net: refused connection to %ls\n", host);
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return nullptr;
}
HINTERNET WINAPI WinHttpOpenRequest_(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD) {
    blocked("WinHttpOpenRequest");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return nullptr;
}
BOOL WINAPI WinHttpSendRequest_(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) {
    blocked("WinHttpSendRequest");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE;
}
BOOL WINAPI WinHttpWriteData_(HINTERNET, LPCVOID, DWORD, LPDWORD written) {
    blocked("WinHttpWriteData");
    if (written) *written = 0;
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE;
}

// ---------------------------------------------------------------- Winsock

SOCKET WSAAPI socket_(int, int, int) {
    blocked("socket");
    WSASetLastError(WSAENETDOWN);
    return INVALID_SOCKET;
}
int WSAAPI connect_(SOCKET, const sockaddr*, int) {
    blocked("connect");
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}
int WSAAPI sendto_(SOCKET, const char*, int, int, const sockaddr*, int) {
    blocked("sendto");
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}
// Names answered on this machine without asking the network: none or the empty name (the machine's own addresses),
// "localhost" and IP literals. The title asks for its own addresses before anything else and, refused, waits for
// a network that never comes (1.0.2.1 stayed on the spinner after its legal screen); with them it finds the
// services unreachable and goes on offline. Every other name still fails as with no network. (The rule the ztrx
// project applies.)
bool local_name(const char* name) {
    if (!name || !*name || _stricmp(name, "localhost") == 0) return true;
    unsigned char buf[sizeof(in6_addr)];
    return inet_pton(AF_INET, name, buf) == 1 || inet_pton(AF_INET6, name, buf) == 1;
}

INT WSAAPI getaddrinfo_(PCSTR name, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
    if (local_name(name)) {
        static std::atomic<int> logged{0};
        INT r = getaddrinfo(name, service, hints, result);
        if (logged++ < 4)
            std::fprintf(stderr, "[rt] net: local lookup of \"%s\" (service %s) -> %d, from %p\n", name ? name : "",
                         service ? service : "-", r, _ReturnAddress());
        return r;
    }
    blocked("getaddrinfo");
    if (name) std::fprintf(stderr, "[rt] net: refused lookup of %s, from %p\n", name, _ReturnAddress());
    if (result) *result = nullptr;
    return WSAHOST_NOT_FOUND;
}
hostent* WSAAPI gethostbyname_(const char* name) {
    if (local_name(name)) return gethostbyname(name);
    blocked("gethostbyname");
    if (name) std::fprintf(stderr, "[rt] net: refused lookup of %s\n", name);
    WSASetLastError(WSAHOST_NOT_FOUND);
    return nullptr;
}

}  // namespace

#define FN(dll, name, fn) {dll, name, reinterpret_cast<void*>(&fn)}
SHIM_TABLE(net,
    FN("winhttp.dll", "WinHttpOpen", WinHttpOpen_),
    FN("winhttp.dll", "WinHttpConnect", WinHttpConnect_),
    FN("winhttp.dll", "WinHttpOpenRequest", WinHttpOpenRequest_),
    FN("winhttp.dll", "WinHttpSendRequest", WinHttpSendRequest_),
    FN("winhttp.dll", "WinHttpWriteData", WinHttpWriteData_),
    // ws2_32 by name and by ordinal (the exe imports ordinals: 4 connect, 20 sendto, 23 socket, 52 gethostbyname)
    FN("ws2_32.dll", "socket", socket_),
    FN("ws2_32.dll", "#23", socket_),
    FN("ws2_32.dll", "connect", connect_),
    FN("ws2_32.dll", "#4", connect_),
    FN("ws2_32.dll", "sendto", sendto_),
    FN("ws2_32.dll", "#20", sendto_),
    FN("ws2_32.dll", "gethostbyname", gethostbyname_),
    FN("ws2_32.dll", "#52", gethostbyname_),
    FN("ws2_32.dll", "getaddrinfo", getaddrinfo_),
);
