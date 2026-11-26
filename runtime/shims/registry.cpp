#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "shims.h"

namespace rt::shims {
namespace {

std::map<std::string, void*>& table() {
    static std::map<std::string, void*> t;  // "dll!name"
    return t;
}

std::mutex g_called_mutex;
std::set<std::string> g_called;

// Called by the thunks with the import's "dll!name" string.
extern "C" uint64_t rt_unimplemented(const char* what) {
    std::lock_guard lock(g_called_mutex);
    if (g_called.insert(what).second) std::fprintf(stderr, "[rt] unimplemented: %s\n", what);
    return 0;
}

// Thunk layout (x86-64):
//   48 B9 <imm64>   mov rcx, what
//   48 B8 <imm64>   mov rax, rt_unimplemented
//   0F 57 C0        xorps xmm0, xmm0
//   FF E0           jmp rax              (tail call: returns straight to the game's caller)
constexpr size_t kThunkSize = 32;

uint8_t* g_pool = nullptr;
size_t g_pool_used = 0, g_pool_size = 0;
std::mutex g_pool_mutex;

// Executable memory for a thunk of `size` bytes (rounded up to 16). Caller holds g_pool_mutex.
uint8_t* alloc_thunk(size_t size) {
    size = (size + 15) & ~size_t{15};
    if (!g_pool || g_pool_used + size > g_pool_size) {
        g_pool_size = 1 << 20;
        g_pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_pool_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        g_pool_used = 0;
        if (!g_pool) return nullptr;
    }
    uint8_t* t = g_pool + g_pool_used;
    g_pool_used += size;
    return t;
}

std::mutex g_traced_mutex;
std::set<std::string> g_traced;

// Called by the trace thunks before the real host function.
extern "C" void rt_host_trace(const char* what) {
    DWORD err = GetLastError();  // some callers read it right after the call; the logging mustn't change it
    {
        std::lock_guard lock(g_traced_mutex);
        if (g_traced.insert(what).second) std::fprintf(stderr, "[rt] call: %s (thread %lu)\n", what, GetCurrentThreadId());
    }
    SetLastError(err);
}

}  // namespace

Table::Table(const Entry* entries, size_t count) {
    for (size_t i = 0; i < count; ++i) table()[std::string(entries[i].dll) + "!" + entries[i].name] = entries[i].fn;
}

void* find(const std::string& dll_lower, const std::string& name) {
    auto it = table().find(dll_lower + "!" + name);
    return it == table().end() ? nullptr : it->second;
}

void* make_unimplemented_thunk(const std::string& dll, const std::string& name) {
    std::lock_guard lock(g_pool_mutex);
    uint8_t* t = alloc_thunk(kThunkSize);
    if (!t) return nullptr;
    const char* what = _strdup((dll + "!" + name).c_str());  // lives for the process
    uint64_t a = reinterpret_cast<uint64_t>(what), f = reinterpret_cast<uint64_t>(&rt_unimplemented);
    uint8_t code[kThunkSize] = {0x48, 0xB9};
    std::memcpy(code + 2, &a, 8);
    code[10] = 0x48; code[11] = 0xB8;
    std::memcpy(code + 12, &f, 8);
    code[20] = 0x0F; code[21] = 0x57; code[22] = 0xC0;
    code[23] = 0xFF; code[24] = 0xE0;
    std::memset(code + 25, 0xCC, kThunkSize - 25);
    std::memcpy(t, code, kThunkSize);
    FlushInstructionCache(GetCurrentProcess(), t, kThunkSize);
    return t;
}

void* make_trace_thunk(const std::string& dll, const std::string& name, void* real) {
    // Saves the argument registers (rcx, rdx, r8, r9, xmm0-3), calls rt_host_trace(what), restores them and jumps
    // to the real function, so the caller's return address and stack arguments are untouched. On entry rsp is
    // 8 mod 16; 4 pushes + 0x68 leave it 16-aligned for the call, with 0x20 bytes of shadow space.
    static const uint8_t prologue[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51,  // push rcx, rdx, r8, r9
        0x48, 0x83, 0xEC, 0x68,              // sub rsp, 0x68
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,  // movdqu [rsp+0x20], xmm0
        0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,  // movdqu [rsp+0x30], xmm1
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,  // movdqu [rsp+0x40], xmm2
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,  // movdqu [rsp+0x50], xmm3
    };
    static const uint8_t epilogue[] = {
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,  // movdqu xmm0, [rsp+0x20]
        0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,  // movdqu xmm1, [rsp+0x30]
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,  // movdqu xmm2, [rsp+0x40]
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,  // movdqu xmm3, [rsp+0x50]
        0x48, 0x83, 0xC4, 0x68,              // add rsp, 0x68
        0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,  // pop r9, r8, rdx, rcx
    };
    uint8_t code[128];
    size_t n = 0;
    auto put = [&](const void* p, size_t len) { std::memcpy(code + n, p, len); n += len; };
    auto imm64 = [&](uint8_t reg_op, uint64_t v) {  // mov r64, imm64
        uint8_t op[2] = {0x48, reg_op};
        put(op, 2);
        put(&v, 8);
    };
    const char* what = _strdup((dll + "!" + name).c_str());
    put(prologue, sizeof(prologue));
    imm64(0xB9, reinterpret_cast<uint64_t>(what));           // mov rcx, what
    imm64(0xB8, reinterpret_cast<uint64_t>(&rt_host_trace));  // mov rax, rt_host_trace
    static const uint8_t call_rax[] = {0xFF, 0xD0};
    put(call_rax, 2);
    put(epilogue, sizeof(epilogue));
    imm64(0xB8, reinterpret_cast<uint64_t>(real));            // mov rax, real
    static const uint8_t jmp_rax[] = {0xFF, 0xE0};
    put(jmp_rax, 2);

    std::lock_guard lock(g_pool_mutex);
    uint8_t* t = alloc_thunk(n);
    if (!t) return real;
    std::memcpy(t, code, n);
    FlushInstructionCache(GetCurrentProcess(), t, n);
    return t;
}

void report_called_unimplemented() {
    std::lock_guard lock(g_called_mutex);
    for (auto& s : g_called) std::fprintf(stderr, "  called: %s\n", s.c_str());
}

}  // namespace rt::shims
