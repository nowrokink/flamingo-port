// Zen 2 instructions the console's CPU has and some PC CPUs lack, emulated when the title's code raises
// EXCEPTION_ILLEGAL_INSTRUCTION on them (the i5-9400F here has no clwb; AMD-only ones fail on every Intel CPU):
//   clwb m8        66 [REX] 0F AE /6   nothing to do: the PC's memory is coherent
//   clzero         0F 01 FC            zeroes the 64-byte line at rAX
//   monitorx       0F 01 FA            nothing to do
//   mwaitx         0F 01 FB            yields the CPU
//   rdpru          0F 01 FD            the performance counter in EDX:EAX (ECX picks MPERF / APERF on the console)
//   rdpid r64      F3 [REX] 0F C7 /7   the current processor number
//   extrq / insertq (SSE4a), on the low quadword of an XMM register (the high one is left; AMD leaves it undefined):
//     extrq xmm, len, idx           66 [REX] 0F 78 /0 ib ib   bits [idx, idx + len) to the bottom, zero-extended
//     extrq xmm, xmm2               66 [REX] 0F 79 /r         len = xmm2[5:0], idx = xmm2[13:8]
//     insertq xmm, xmm2, len, idx   F2 [REX] 0F 78 /r ib ib   xmm2's low len bits into bits [idx, idx + len)
//     insertq xmm, xmm2             F2 [REX] 0F 79 /r         len = xmm2[69:64], idx = xmm2[77:72]
//   (a length of 0 means 64)
// Each is logged the first time. Anything else stays an illegal instruction, logged with its code bytes.
#include "cpu_emul.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace rt {
namespace {

// Bytes of a ModRM operand (ModRM itself, SIB, displacement) starting at `m`.
int modrm_length(const uint8_t* m) {
    uint8_t mod = m[0] >> 6, rm = m[0] & 7;
    int n = 1;
    if (mod != 3 && rm == 4) {
        ++n;
        if (mod == 0 && (m[1] & 7) == 5) n += 4;
    }
    if (mod == 0 && rm == 5) n += 4;  // RIP-relative
    if (mod == 1) n += 1;
    if (mod == 2) n += 4;
    return n;
}

volatile long long g_emulated = 0;

void note(const char* what) {
    InterlockedIncrement64(&g_emulated);
    static const char* seen[8] = {};
    for (auto& s : seen) {
        if (s == what) return;
        if (!s) {
            s = what;
            std::fprintf(stderr, "[rt] cpu: emulating %s (this CPU lacks it)\n", what);
            return;
        }
    }
}

DWORD64* reg(CONTEXT* c, int index) { return &c->Rax + index; }  // Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8-R15
M128A* xmm(CONTEXT* c, int index) { return &c->Xmm0 + index; }

uint64_t field_mask(unsigned len) { return len >= 64 ? ~uint64_t{0} : (uint64_t{1} << len) - 1; }

uint64_t extract(uint64_t v, unsigned len, unsigned idx) {
    len = len ? len : 64;
    return (v >> (idx & 63)) & field_mask(len);
}
uint64_t insert(uint64_t dst, uint64_t src, unsigned len, unsigned idx) {
    len = len ? len : 64;
    idx &= 63;
    uint64_t m = field_mask(len) << idx;
    return (dst & ~m) | ((src << idx) & m);
}

bool emulate(CONTEXT* c, const uint8_t* code) {
    const uint8_t* p = code;
    bool opsize = false, rep = false, repne = false;
    for (;; ++p) {  // legacy prefixes
        uint8_t b = *p;
        if (b == 0x66) opsize = true;
        else if (b == 0xF3) rep = true;
        else if (b == 0xF2) repne = true;
        else if (b == 0xF0 || b == 0x67 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
                 b == 0x64 || b == 0x65) {
        } else break;
    }
    uint8_t rex = (*p & 0xF0) == 0x40 ? *p++ : 0;
    if (p[0] != 0x0F) return false;
    if (p[1] == 0xAE && opsize && (p[2] >> 6) != 3 && ((p[2] >> 3) & 7) == 6) {
        note("clwb");
        c->Rip += (p - code) + 2 + modrm_length(p + 2);
        return true;
    }
    if (p[1] == 0x01) {
        switch (p[2]) {
            case 0xFC: {
                note("clzero");
                std::memset(reinterpret_cast<void*>(c->Rax & ~DWORD64{63}), 0, 64);
                break;
            }
            case 0xFA: note("monitorx"); break;
            case 0xFB:
                note("mwaitx");
                YieldProcessor();
                break;
            case 0xFD: {
                note("rdpru");
                LARGE_INTEGER t;
                QueryPerformanceCounter(&t);
                c->Rax = static_cast<uint32_t>(t.QuadPart);
                c->Rdx = static_cast<uint32_t>(t.QuadPart >> 32);
                c->EFlags |= 1;  // CF: a valid counter
                break;
            }
            default: return false;
        }
        c->Rip += (p - code) + 3;
        return true;
    }
    if ((p[1] == 0x78 || p[1] == 0x79) && (opsize || repne) && (p[2] >> 6) == 3) {
        int r = ((p[2] >> 3) & 7) | ((rex & 4) << 1), m = (p[2] & 7) | ((rex & 1) << 3);
        bool imm = p[1] == 0x78;
        if (opsize) {  // extrq: the immediate form names its register in rm
            note("extrq");
            M128A* d = xmm(c, imm ? m : r);
            unsigned len = imm ? p[3] & 63 : xmm(c, m)->Low & 63, idx = imm ? p[4] & 63 : (xmm(c, m)->Low >> 8) & 63;
            d->Low = extract(static_cast<uint64_t>(d->Low), len, idx);
        } else {
            note("insertq");
            M128A* d = xmm(c, r);
            const M128A* s = xmm(c, m);
            unsigned len = imm ? p[3] & 63 : s->High & 63, idx = imm ? p[4] & 63 : (s->High >> 8) & 63;
            d->Low = insert(static_cast<uint64_t>(d->Low), static_cast<uint64_t>(s->Low), len, idx);
        }
        c->Rip += (p - code) + 3 + (imm ? 2 : 0);
        return true;
    }
    if (p[1] == 0xC7 && rep && (p[2] >> 6) == 3 && ((p[2] >> 3) & 7) == 7) {
        note("rdpid");
        *reg(c, (p[2] & 7) | ((rex & 1) << 3)) = GetCurrentProcessorNumber();
        c->Rip += (p - code) + 3;
        return true;
    }
    return false;
}

// ---- patching hot sites: an emulation costs an exception (microseconds); a site run often is rewritten in place,
// once, the first time it's emulated:
//   monitorx                     -> nop (0F 1F 00)
//   mwaitx                       -> pause; nop
//   the title's clzero loop      clzero; add rax, 40h; cmp rax, <r>; jb back  (0F 01 FC 48 83 C0 40 xx xx xx 72 F4)
//                                -> jmp to a stub doing the same with ordinary stores, then back after the loop
// A rewrite must stay inside one aligned 8-byte word, written at once, so another thread running the code never
// sees half an instruction; otherwise the site stays emulated.

// Writes `bytes` over `at` atomically (they must lie in one aligned qword); false if they don't, or it can't.
bool patch(uint8_t* at, const uint8_t* bytes, size_t n) {
    auto word = reinterpret_cast<uintptr_t>(at) & ~uintptr_t{7};
    size_t off = reinterpret_cast<uintptr_t>(at) - word;
    if (off + n > 8) return false;
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(word), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    auto* w = reinterpret_cast<volatile long long*>(word);
    long long before, after;
    do {
        before = *w;
        after = before;
        std::memcpy(reinterpret_cast<uint8_t*>(&after) + off, bytes, n);
    } while (InterlockedCompareExchange64(w, after, before) != before);
    VirtualProtect(reinterpret_cast<void*>(word), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(word), 8);
    return true;
}

// patch() with every other thread of the process suspended, and any of them stopped inside the rewritten bytes
// (after the first) moved back to `at`: a thread just past an emulated clzero would otherwise resume mid-jmp.
bool patch_stopped(uint8_t* at, const uint8_t* bytes, size_t n) {
    std::vector<HANDLE> stopped;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        stopped.reserve(1024);  // no allocating once threads are stopped (one may hold the heap's lock)
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok && stopped.size() < 1024; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!t) continue;
            if (SuspendThread(t) == static_cast<DWORD>(-1)) {
                CloseHandle(t);
                continue;
            }
            stopped.push_back(t);
        }
        CloseHandle(snap);
    }
    for (HANDLE t : stopped) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(t, &c)) continue;
        auto rip = reinterpret_cast<uint8_t*>(c.Rip);
        if (rip > at && rip < at + n) {
            c.Rip = reinterpret_cast<DWORD64>(at);
            SetThreadContext(t, &c);
        }
    }
    bool done = patch(at, bytes, n);
    if (!done) {  // across an aligned qword: a plain write, safe while the other threads are stopped
        DWORD old;
        if (VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(at, bytes, n);
            VirtualProtect(at, n, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, n);
            done = true;
        }
    }
    for (HANDLE t : stopped) {
        ResumeThread(t);
        CloseHandle(t);
    }
    return done;
}

// Executable memory within 2 GiB of `origin` (for rel32 jumps to it), handed out in pieces.
uint8_t* stub_memory(const uint8_t* origin, size_t n) {
    static uint8_t* block = nullptr;  // only touched under g_patch_lock
    static size_t used = 0, size = 0;
    auto in_reach = [&](const uint8_t* p) {
        auto d = static_cast<long long>(p - origin);
        return d > -0x70000000ll && d < 0x70000000ll;
    };
    if (!block || used + n > size || !in_reach(block)) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const size_t grain = si.dwAllocationGranularity;
        block = nullptr;
        for (uintptr_t a = (reinterpret_cast<uintptr_t>(origin) & ~(grain - 1)) - 0x10000000; !block; a -= grain * 16) {
            if (!in_reach(reinterpret_cast<uint8_t*>(a))) return nullptr;
            block = static_cast<uint8_t*>(
                VirtualAlloc(reinterpret_cast<void*>(a), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        }
        used = 0, size = 0x10000;
    }
    uint8_t* p = block + used;
    used += (n + 15) & ~size_t{15};
    return p;
}

SRWLOCK g_patch_lock = SRWLOCK_INIT;

// Loop sites rewritten to a jmp: a thread emulating the clzero there when it happened (its handler returns past the
// clzero, into the jmp's bytes) is sent back to the loop's start instead, which only zeroes the same line again.
const uint8_t* volatile g_loops[64];
volatile long g_loop_count = 0;
bool rewritten_loop(const uint8_t* code) {
    for (long i = 0; i < g_loop_count && i < 64; ++i)
        if (g_loops[i] == code) return true;
    return false;
}

// The clzero loop at `site` (see above): a stub, and a jmp to it over the loop's first five bytes.
bool patch_clzero_loop(uint8_t* site) {
    static const uint8_t head[] = {0x0F, 0x01, 0xFC, 0x48, 0x83, 0xC0, 0x40};
    // cmp rax, <any 64-bit register>: REX.W (+R), 39, ModRM 11 reg 000
    if (std::memcmp(site, head, sizeof head) != 0 || (site[7] != 0x48 && site[7] != 0x4C) || site[8] != 0x39 ||
        (site[9] & 0xC7) != 0xC0 || site[10] != 0x72 || site[11] != 0xF4)
        return false;
    uint8_t s[96];
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) {
        for (uint8_t x : b) s[n++] = x;
    };
    size_t loop = n;
    put({0x51});                          // push rcx
    put({0x48, 0x89, 0xC1});              // mov rcx, rax
    put({0x48, 0x83, 0xE1, 0xC0});        // and rcx, -64
    put({0x48, 0xC7, 0x01, 0, 0, 0, 0});  // mov qword [rcx], 0
    for (uint8_t d = 8; d < 64; d += 8) put({0x48, 0xC7, 0x41, d, 0, 0, 0, 0});  // mov qword [rcx + d], 0
    put({0x59});                          // pop rcx
    put({0x48, 0x83, 0xC0, 0x40});        // add rax, 40h
    put({site[7], site[8], site[9]});     // the loop's own cmp rax, <r>
    put({0x72, static_cast<uint8_t>(loop - (n + 2))});  // jb loop
    put({0xFF, 0x25, 0, 0, 0, 0});        // jmp [rip]: back after the loop
    uint64_t back = reinterpret_cast<uint64_t>(site + 12);
    std::memcpy(s + n, &back, 8);
    n += 8;
    uint8_t* stub = stub_memory(site, n);
    if (!stub) return false;
    std::memcpy(stub, s, n);
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    uint8_t jmp[5] = {0xE9};
    int32_t rel = static_cast<int32_t>(stub - (site + 5));
    std::memcpy(jmp + 1, &rel, 4);
    if (!patch_stopped(site, jmp, sizeof jmp)) return false;
    long i = InterlockedIncrement(&g_loop_count) - 1;
    if (i < 64) g_loops[i] = site;
    return true;
}

// A same-length rewrite: atomic when it fits one aligned qword, else with the other threads stopped.
bool patch_in_place(uint8_t* at, const uint8_t* bytes, size_t n) {
    return patch(at, bytes, n) || patch_stopped(at, bytes, n);
}

// Sites already tried (rewritten or not): each is looked at once.
const uint8_t* volatile g_tried[1024];
volatile long g_tried_count = 0;

// Rewrites a site just emulated, when it's one of the kinds above (once: a rewritten site raises nothing more).
void patch_site(uint8_t* code) {
    for (long i = 0; i < g_tried_count && i < 1024; ++i)
        if (g_tried[i] == code) return;
    AcquireSRWLockExclusive(&g_patch_lock);
    bool seen = false;
    for (long i = 0; i < g_tried_count && i < 1024; ++i) seen |= g_tried[i] == code;
    if (seen) {
        ReleaseSRWLockExclusive(&g_patch_lock);
        return;
    }
    long slot = InterlockedIncrement(&g_tried_count) - 1;
    if (slot < 1024) g_tried[slot] = code;
    bool done = false;
    const char* what = nullptr;
    if (code[0] == 0x0F && code[1] == 0x01 && code[2] == 0xFA) {
        static const uint8_t nop3[] = {0x0F, 0x1F, 0x00};
        done = patch_in_place(code, nop3, 3), what = "monitorx";
    } else if (code[0] == 0x0F && code[1] == 0x01 && code[2] == 0xFB) {
        static const uint8_t pause_nop[] = {0xF3, 0x90, 0x90};
        done = patch_in_place(code, pause_nop, 3), what = "mwaitx";
    } else if (code[0] == 0x0F && code[1] == 0x01 && code[2] == 0xFC) {
        done = patch_clzero_loop(code), what = "clzero loop";
    } else if (code[0] == 0x66) {  // clwb: 66 [REX] 0F AE /6 -> a NOP of the same length
        const uint8_t* p = code + 1;
        if ((*p & 0xF0) == 0x40) ++p;
        if (p[0] == 0x0F && p[1] == 0xAE && (p[2] >> 6) != 3 && ((p[2] >> 3) & 7) == 6) {
            size_t n = static_cast<size_t>(p + 2 + modrm_length(p + 2) - code);
            // the recommended multi-byte NOPs (0F 1F /0), 3 to 9 bytes
            static const uint8_t nops[10][9] = {
                {}, {}, {},
                {0x0F, 0x1F, 0x00},
                {0x0F, 0x1F, 0x40, 0x00},
                {0x0F, 0x1F, 0x44, 0x00, 0x00},
                {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
                {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
                {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
                {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}};
            what = "clwb";
            if (n >= 3 && n <= 9) done = patch_in_place(code, nops[n], n);
        }
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
    static volatile long logged = 0;
    if (what && InterlockedIncrement(&logged) <= 64)
        std::fprintf(stderr, "[rt] cpu: %s at %p %s\n", what, static_cast<void*>(code),
                     done ? "rewritten (no more exceptions there)" : "left emulated");
}

}  // namespace

unsigned long long emulated_instructions() { return static_cast<unsigned long long>(g_emulated); }

bool emulate_console_instruction(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return false;
    auto* code = static_cast<const uint8_t*>(ep->ExceptionRecord->ExceptionAddress);
    if (rewritten_loop(code)) {  // its clzero raised before the rewrite; the loop runs from the stub now
        ep->ContextRecord->Rip = reinterpret_cast<DWORD64>(code);
        return true;
    }
    if (emulate(ep->ContextRecord, code)) {
        patch_site(const_cast<uint8_t*>(code));
        if (rewritten_loop(code)) ep->ContextRecord->Rip = reinterpret_cast<DWORD64>(code);
        // hot sites: one line when a site reaches 10000 emulations (a fixed table; collisions just share a count)
        static volatile long hits[4096];
        size_t slot = (reinterpret_cast<uintptr_t>(code) >> 1) % 4096;
        if (InterlockedIncrement(&hits[slot]) == 10000) {
            std::fprintf(stderr, "[rt] cpu: hot site %p:", static_cast<const void*>(code));
            for (int i = 0; i < 12; ++i) std::fprintf(stderr, " %02x", code[i]);
            std::fprintf(stderr, "\n");
        }
        return true;
    }
    std::fprintf(stderr, "[rt] cpu: illegal instruction at %p:", static_cast<const void*>(code));
    for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %02x", code[i]);
    std::fprintf(stderr, "\n");
    return false;
}

}  // namespace rt
