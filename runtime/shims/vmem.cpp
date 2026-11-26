// The title's own VirtualAlloc / VirtualFree / VirtualQueryEx, for addresses in unified memory (uma.h).
//
// The title also manages memory it got from XMemVirtualAlloc with the kernel32 calls: it splits placeholder
// reservations (VirtualFree with MEM_PRESERVE_PLACEHOLDER) before claiming parts of them, commits, decommits and
// releases. In R those are our bookkeeping (R is split lazily, page by page, as pages get mapped), so here:
//   VirtualFree   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER (a split) or MEM_COALESCE_PLACEHOLDERS: nothing to do;
//                 MEM_RELEASE of a reservation's start: release it; MEM_DECOMMIT: unmap the pages
//   VirtualAlloc  MEM_COMMIT inside a reservation: map pool pages; MEM_RESERVE at an address in R: reserve there;
//                 MEM_RESET: nothing to do
//   VirtualQueryEx  what R's bookkeeping says
// Everything else, and every address outside R, goes to the host unchanged.
#include <windows.h>

#include <cstdio>

#include "shims.h"
#include "uma.h"

namespace {

bool in_r(const void* p) { return p && rt::uma::contains(reinterpret_cast<uintptr_t>(p)); }

LPVOID WINAPI VirtualAlloc_(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (!in_r(address)) return VirtualAlloc(address, size, type, protect);
    auto a = reinterpret_cast<uintptr_t>(address);
    if (type & MEM_RESET) return address;
    if ((type & MEM_RESERVE) && !rt::uma::reservation(a, nullptr, nullptr)) {
        if (!rt::uma::reserve(size, rt::uma::kPage, a & ~(rt::uma::kPage - 1))) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
    }
    if (type & MEM_COMMIT) {
        if (!rt::uma::reservation(a, nullptr, nullptr) || !rt::uma::commit(a, size, protect & 0xFF)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
    }
    return address;
}

BOOL WINAPI VirtualFree_(LPVOID address, SIZE_T size, DWORD type) {
    if (!in_r(address)) return VirtualFree(address, size, type);
    auto a = reinterpret_cast<uintptr_t>(address);
    if ((type & MEM_PRESERVE_PLACEHOLDER) || (type & MEM_COALESCE_PLACEHOLDERS)) return TRUE;
    uintptr_t start;
    uint64_t bytes;
    if (!rt::uma::reservation(a, &start, &bytes)) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    if (type & MEM_RELEASE) {
        if (a != start || size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        return rt::uma::release(start);
    }
    if (type & MEM_DECOMMIT) return rt::uma::decommit(a, size ? size : start + bytes - a);
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

SIZE_T WINAPI VirtualQueryEx_(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
    if (GetProcessId(process) == GetCurrentProcessId() && length >= sizeof(MEMORY_BASIC_INFORMATION) &&
        rt::uma::query(reinterpret_cast<uintptr_t>(address), info))
        return sizeof(MEMORY_BASIC_INFORMATION);
    return VirtualQueryEx(process, address, info, length);
}

SIZE_T WINAPI VirtualQuery_(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
    return VirtualQueryEx_(GetCurrentProcess(), address, info, length);
}

}  // namespace

#define FN(dll, name) {dll, #name, reinterpret_cast<void*>(&name##_)}
SHIM_TABLE(vmem,
    FN("api-ms-win-core-memory-l1-1-0.dll", VirtualAlloc),
    FN("api-ms-win-core-memory-l1-1-0.dll", VirtualFree),
    FN("api-ms-win-core-memory-l1-1-0.dll", VirtualQueryEx),
    FN("api-ms-win-core-memory-l1-1-0.dll", VirtualQuery),
    FN("kernel32.dll", VirtualAlloc),
    FN("kernel32.dll", VirtualFree),
    FN("kernel32.dll", VirtualQueryEx),
    FN("kernel32.dll", VirtualQuery),
);
