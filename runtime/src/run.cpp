#include "run.h"

#include "cpu_emul.h"
#include "game_patches.h"
#include "../shims/files.h"
#include "../shims/storage.h"
#include "../shims/uma.h"

#include <windows.h>
#include <intrin.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../shims/shims.h"
#include "../shims/com_probe.h"

namespace rt {
namespace {

constexpr int kMaxTlsSlots = 4;  // rt_tls_slot0.dll .. rt_tls_slot3.dll, built next to FlamingoPort.exe

std::vector<Module*> g_modules;  // for the crash report

IMAGE_NT_HEADERS64* nt_headers(Module& m) {
    return reinterpret_cast<IMAGE_NT_HEADERS64*>(m.base + reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
}

IMAGE_DATA_DIRECTORY directory(Module& m, unsigned index) {
    auto& oh = nt_headers(m)->OptionalHeader;
    return index < oh.NumberOfRvaAndSizes ? oh.DataDirectory[index] : IMAGE_DATA_DIRECTORY{};
}

// "module+0xRVA" for an address inside a loaded module, else the bare address.
std::string where(const void* address) {
    auto a = reinterpret_cast<const uint8_t*>(address);
    char buf[160];
    for (auto* m : g_modules)
        if (a >= m->base && a < m->base + m->size) {
            std::snprintf(buf, sizeof(buf), "%s+0x%llx", m->name.c_str(), static_cast<unsigned long long>(a - m->base));
            return buf;
        }
    HMODULE host = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &host)) {
        char name[MAX_PATH] = {};
        GetModuleFileNameA(host, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        int n = std::snprintf(buf, sizeof(buf), "%s+0x%llx (host)", base ? base + 1 : name,
                              static_cast<unsigned long long>(a - reinterpret_cast<const uint8_t*>(host)));
        // Our own frames: the function from this exe's PDB.
        if (host == GetModuleHandleW(nullptr) && n > 0 && n < static_cast<int>(sizeof(buf))) {
            static bool symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 128] = {};
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 127;
            DWORD64 off = 0;
            if (symbols && SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &off, sym))
                std::snprintf(buf + n, sizeof(buf) - n, " %s+0x%llx", sym->Name, static_cast<unsigned long long>(off));
        }
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%p", address);
    return buf;
}

// Walks the stack from an exception's context (a copy: the original is left alone) and prints each frame.
void print_backtrace(const CONTEXT* from, int max_frames = 16) {
    CONTEXT c = *from;
    for (int i = 0; i < max_frames && c.Rip; ++i) {
        std::fprintf(stderr, "[rt]     #%-2d %s\n", i, where(reinterpret_cast<void*>(c.Rip)).c_str());
        DWORD64 image_base = 0;
        auto* fe = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!fe) {  // leaf function: the return address is at rsp
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
    }
}

void safe_backtrace(const CONTEXT* c, int frames) {
    __try {
        print_backtrace(c, frames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[rt]     (the stack couldn't be walked further)\n");
    }
}

// Where every other thread of the process is: for the title's own "something hung" crash (a thread of its that
// writes to null on purpose when a wait takes too long), whose own stack says nothing about what hung.
void dump_other_threads() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    int shown = 0;
    for (BOOL ok = Thread32First(snap, &te); ok && shown < 96; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(t, &c)) {
                std::fprintf(stderr, "[rt]   thread %lu:\n", te.th32ThreadID);
                safe_backtrace(&c, 12);
                ++shown;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

LONG WINAPI crash_report(EXCEPTION_POINTERS* ep) {
    auto* r = ep->ExceptionRecord;
    std::fprintf(stderr, "\n[rt] unhandled exception 0x%08lX at %s (thread %lu)\n", r->ExceptionCode,
                 where(r->ExceptionAddress).c_str(), GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        const char* op = r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing";
        std::fprintf(stderr, "[rt]   %s %p\n", op, reinterpret_cast<void*>(r->ExceptionInformation[1]));
    }
    auto* c = ep->ContextRecord;
    std::fprintf(stderr, "[rt]   rax=%016llx rcx=%016llx rdx=%016llx r8=%016llx r9=%016llx\n", c->Rax, c->Rcx, c->Rdx,
                 c->R8, c->R9);
    std::fprintf(stderr, "[rt]   rsp=%016llx, backtrace:\n", c->Rsp);
    print_backtrace(c);
    std::fprintf(stderr, "[rt] unimplemented imports called so far:\n");
    shims::report_called_unimplemented();
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0xC0DE);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Logs first-chance exceptions (the first few) without handling them, so a crash that never reaches the
// unhandled filter (a fail-fast, a handler that terminates) still leaves a trace. C++ throws, thread naming and
// debug prints are skipped: they're routine.
// --probe: one-shot breakpoints on exe RVAs. Each logs when it's first reached (with its arguments and caller),
// then puts the original byte back and lets the code run on.
struct Probe {
    uint8_t* at;
    uint32_t rva;
    uint8_t saved;
};
std::vector<Probe> g_probes;
SRWLOCK g_probe_lock = SRWLOCK_INIT;

void patch_byte(uint8_t* at, uint8_t value) {  // code pages are read-only once the loader has protected them
    DWORD old;
    VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old);
    *at = value;
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
}

bool hit_probe(EXCEPTION_POINTERS* ep) {
    auto* at = static_cast<uint8_t*>(ep->ExceptionRecord->ExceptionAddress);
    AcquireSRWLockExclusive(&g_probe_lock);
    for (auto& p : g_probes)
        if (p.at == at && *at == 0xCC) {
            patch_byte(at, p.saved);
            ReleaseSRWLockExclusive(&g_probe_lock);
            auto* c = ep->ContextRecord;
            std::fprintf(stderr, "[rt] probe +%x reached (thread %lu): rcx=%s rdx=%llx r8=%llx r9=%llx, from %s\n",
                         p.rva, GetCurrentThreadId(), where(reinterpret_cast<void*>(c->Rcx)).c_str(), c->Rdx, c->R8,
                         c->R9, where(*reinterpret_cast<void**>(c->Rsp)).c_str());
            // what rcx and rdx point at (16 qwords each, when committed and readable)
            for (DWORD64 reg : {c->Rcx, c->Rdx}) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (reg < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(reg), &mbi, sizeof mbi) ||
                    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                    reg + 128 > reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize)
                    continue;
                auto* q = reinterpret_cast<const uint64_t*>(reg);
                std::fprintf(stderr, "[rt]   [%llx]", reg);
                for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %llx", q[i]);
                std::fprintf(stderr, "\n");
            }
            c->Rip = reinterpret_cast<DWORD64>(at);
            return true;
        }
    ReleaseSRWLockExclusive(&g_probe_lock);
    return false;
}

void arm_probes(Module& exe, const std::vector<uint32_t>& rvas) {
    for (uint32_t rva : rvas) {
        if (rva >= exe.size) throw std::runtime_error("probe RVA outside the exe");
        uint8_t* at = exe.base + rva;
        g_probes.push_back({at, rva, *at});
        patch_byte(at, 0xCC);
    }
    if (!rvas.empty()) std::fprintf(stderr, "[rt] %zu probe(s) armed\n", rvas.size());
}

LONG WINAPI first_chance(EXCEPTION_POINTERS* ep) {
    static LONG count = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT && hit_probe(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    if (emulate_console_instruction(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code == 0xE06D7363) {  // a C++ throw: routine, but the first few are worth seeing (who throws, from where)
        static LONG throws = 0;
        if (InterlockedIncrement(&throws) <= 6) {
            std::fprintf(stderr, "[rt] C++ exception thrown (thread %lu):\n", GetCurrentThreadId());
            CONTEXT copy = *ep->ContextRecord;
            safe_backtrace(&copy, 8);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C)
        return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&count);
    if (n <= 20)
        std::fprintf(stderr, "[rt] first-chance exception 0x%08lX at %s\n", code,
                     where(ep->ExceptionRecord->ExceptionAddress).c_str());
    // The game handles its own breakpoints (a failed check) and crashes (its crash handler) and exits; show how it
    // got there.
    if (n <= 3 && (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ACCESS_VIOLATION ||
                   code == EXCEPTION_INT_DIVIDE_BY_ZERO)) {
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            std::fprintf(stderr, "[rt]   %s %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                         reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));
        auto* c = ep->ContextRecord;
        std::fprintf(stderr, "[rt]   rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%s rdi=%llx rbp=%llx\n", c->Rax, c->Rbx,
                     c->Rcx, c->Rdx, where(reinterpret_cast<void*>(c->Rsi)).c_str(), c->Rdi, c->Rbp);
        std::fprintf(stderr, "[rt]   r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx rsp=%llx\n",
                     c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15, c->Rsp);
        if (code == EXCEPTION_ACCESS_VIOLATION && c->Rbx > 0x10000) {  // the object a null field was read from: often rbx
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(c->Rbx), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                c->Rbx + 0xC0 <= reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize) {
                const auto* q = reinterpret_cast<const uint32_t*>(c->Rbx);
                for (int i = 0; i < 0xC0 / 4; i += 8)
                    std::fprintf(stderr, "[rt]   [rbx+%03x] %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4, q[i], q[i + 1],
                                 q[i + 2], q[i + 3], q[i + 4], q[i + 5], q[i + 6], q[i + 7]);
                std::fprintf(stderr, "[rt]   [rbx] is %s\n", where(*reinterpret_cast<void* const*>(c->Rbx)).c_str());
            }
        }
        if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {  // the object the divisor is read from is usually in rdi
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(c->Rdi), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                c->Rdi + 0x180 <= reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize) {
                const auto* q = reinterpret_cast<const uint32_t*>(c->Rdi);
                for (int i = 0; i < 0x180 / 4; i += 8)
                    std::fprintf(stderr, "[rt]   [rdi+%03x] %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4, q[i], q[i + 1],
                                 q[i + 2], q[i + 3], q[i + 4], q[i + 5], q[i + 6], q[i + 7]);
                std::fprintf(stderr, "[rt]   [rdi] is %s\n", where(*reinterpret_cast<void* const*>(c->Rdi)).c_str());
            }
            std::fprintf(stderr, "[rt]   backtrace:\n");
            CONTEXT copy = *c;
            print_backtrace(&copy);
        }
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 &&
            ep->ExceptionRecord->ExceptionInformation[0] == 1 && ep->ExceptionRecord->ExceptionInformation[1] == 0) {
            std::fprintf(stderr, "[rt]   a write to null (the title's own fatal stop); the other threads:\n");
            shims::report_waits();
            dump_other_threads();
            // RT_NO_WATCHDOG=1 (for diagnostics that hold a frame, like a whole-frame capture inside the open
            // world): the thread that stops the game stays here instead. It is the title's watchdog when a
            // frame has taken 30 s; after any other fatal stop the game goes on in whatever state it was.
            static const bool park = [] {
                char v[4];
                return GetEnvironmentVariableA("RT_NO_WATCHDOG", v, sizeof v) && v[0] == '1';
            }();
            if (park) {
                std::fprintf(stderr, "[rt]   RT_NO_WATCHDOG=1: thread %lu stays parked here; the game goes on\n",
                             GetCurrentThreadId());
                std::fflush(stderr);
                Sleep(INFINITE);
            }
        }
        if (c->Rip < 0x10000)  // a call through a null pointer: the caller is the return address on the stack
            std::fprintf(stderr, "[rt]   called from %s\n", where(*reinterpret_cast<void**>(c->Rsp)).c_str());
        {
            const DWORD64 regs[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->R8,
                                    c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
            const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8",
                                   "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
            for (int i = 0; i < 15; ++i) {
                uint64_t start = 0, bytes = 0;
                if (shims::in_blank_bcpack(regs[i], &start, &bytes))
                    std::fprintf(stderr, "[rt]   %s points into a BCPack request left blank (%llx + %llu bytes)\n", names[i],
                                 static_cast<unsigned long long>(start), static_cast<unsigned long long>(bytes));
                if (const char* obj = shims::probe_object_name(reinterpret_cast<const void*>(regs[i])))
                    std::fprintf(stderr, "[rt]   %s is one of our objects: %s\n", names[i], obj);
            }
        }
        print_backtrace(c);
        // Readable text in the failing frame (strings the game decodes on its stack before a check).
        auto* sp = reinterpret_cast<const unsigned char*>(c->Rsp);
        std::string run;
        std::fprintf(stderr, "[rt]   text on stack:");
        for (int i = 0; i < 512; ++i) {
            unsigned char ch = sp[i];
            if (ch >= 0x20 && ch < 0x7F) run += static_cast<char>(ch);
            else {
                if (run.size() >= 4) std::fprintf(stderr, " \"%s\"", run.c_str());
                run.clear();
            }
        }
        std::fprintf(stderr, "\n");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Only for images mapped by hand: Windows already knows the unwind data of the ones it mapped.
void register_unwind(Module& m) {
    if (m.os_mapped) return;
    auto dir = directory(m, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (!dir.Size) return;
    auto* table = reinterpret_cast<RUNTIME_FUNCTION*>(m.base + dir.VirtualAddress);
    if (!RtlAddFunctionTable(table, dir.Size / sizeof(RUNTIME_FUNCTION), reinterpret_cast<DWORD64>(m.base)))
        throw std::runtime_error(m.name + ": RtlAddFunctionTable failed");
}

std::vector<PIMAGE_TLS_CALLBACK> tls_callbacks(Module& m) {
    std::vector<PIMAGE_TLS_CALLBACK> out;
    auto dir = directory(m, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!dir.Size) return out;
    auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(m.base + dir.VirtualAddress);
    if (auto* cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks))
        for (; *cb; ++cb) out.push_back(*cb);
    return out;
}

// Per-module state for thread attach/detach, kept for the life of the process.
struct TlsModule {
    Module* module;
    std::vector<PIMAGE_TLS_CALLBACK> callbacks;
};

void on_thread(void* ctx, DWORD reason) {
    auto* t = static_cast<TlsModule*>(ctx);
    for (auto cb : t->callbacks) cb(t->module->base, reason, nullptr);
}

using SlotIndexFn = unsigned long (*)();
using SlotCapacityFn = size_t (*)();
using SlotBlockFn = void* (*)();
using SlotInitFn = bool (*)(const void*, size_t, void (*)(void*, DWORD), void*);

// ---- the game exe's TLS lives in TLS index 0
//
// MSVC compiles EXEs assuming their TLS index is 0 (/GA): parts of game_scarlett_master.exe read gs:[0x58][0]
// directly instead of going through _tls_index. Index 0 belongs to the process's own exe, FlamingoPort.exe, so the game
// exe gets this exe's TLS block: it reserves 16 KiB of TLS it never touches (it's built with
// /Zc:threadSafeInit-, so function-local statics don't use TLS either), and on every thread the game's template is
// copied over the whole block, CRT bookkeeping included (the runtime has no thread_local data that it reads).
constexpr size_t kExeTlsReserve = 16 * 1024;
struct ExeTls {
    const uint8_t* tmpl = nullptr;
    size_t tmpl_size = 0;
    size_t total = 0;  // template + zero fill
    TlsModule* module = nullptr;
} g_exe_tls;

uint8_t* exe_tls_block() { return static_cast<uint8_t*>(reinterpret_cast<void**>(__readgsqword(0x58))[0]); }

void fill_exe_tls() {
    uint8_t* block = exe_tls_block();
    std::memcpy(block, g_exe_tls.tmpl, g_exe_tls.tmpl_size);
    std::memset(block + g_exe_tls.tmpl_size, 0, g_exe_tls.total - g_exe_tls.tmpl_size);
}

}  // namespace
}  // namespace rt

extern "C" {
extern unsigned long _tls_index;  // FlamingoPort.exe's own TLS index: 0, as for any exe
__declspec(thread) unsigned char rt_exe_tls_reserve[rt::kExeTlsReserve];

// Runs on every thread attach/detach after the CRT's TLS callbacks (.CRT$XLC): refills index 0 for the game.
void NTAPI rt_exe_tls_callback(PVOID, DWORD reason, PVOID) {
    if (!rt::g_exe_tls.tmpl) return;
    if (reason == DLL_THREAD_ATTACH) rt::fill_exe_tls();
    if (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH) rt::on_thread(rt::g_exe_tls.module, reason);
}
#pragma section(".CRT$XLY", long, read)
__declspec(allocate(".CRT$XLY")) PIMAGE_TLS_CALLBACK rt_exe_tls_callback_ptr = rt_exe_tls_callback;
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:rt_exe_tls_callback_ptr")

namespace rt {
namespace {

void setup_exe_tls(Module& m, IMAGE_TLS_DIRECTORY64* tls) {
    if (_tls_index != 0) throw std::runtime_error("FlamingoPort.exe doesn't have TLS index 0");
    // The reserve must be inside index 0's block, and the block must hold the game's data from offset 0.
    uint8_t* block = exe_tls_block();
    size_t room = static_cast<size_t>(rt_exe_tls_reserve + kExeTlsReserve - block);
    g_exe_tls.tmpl = reinterpret_cast<const uint8_t*>(tls->StartAddressOfRawData);
    g_exe_tls.tmpl_size = tls->EndAddressOfRawData - tls->StartAddressOfRawData;
    g_exe_tls.total = g_exe_tls.tmpl_size + tls->SizeOfZeroFill;
    if (rt_exe_tls_reserve < block || room > (1u << 20) || g_exe_tls.total > room)
        throw std::runtime_error(m.name + ": TLS doesn't fit in FlamingoPort.exe's TLS block");
    *reinterpret_cast<uint32_t*>(tls->AddressOfIndex) = 0;
    g_exe_tls.module = new TlsModule{&m, tls_callbacks(m)};
    fill_exe_tls();
    std::fprintf(stderr, "[rt] %s: TLS %zu bytes in FlamingoPort.exe's block (index 0, block %p), %zu callback(s)\n",
                 m.name.c_str(), g_exe_tls.total, static_cast<void*>(block), g_exe_tls.module->callbacks.size());
}

// Gives a module with static TLS a TLS index: index 0 for an exe, else one of the rt_tls_slot DLLs' indices
// (see tls_slot.cpp).
void setup_tls(Module& m, int& next_slot) {
    auto dir = directory(m, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!dir.Size) return;
    auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(m.base + dir.VirtualAddress);
    if (!(nt_headers(m)->FileHeader.Characteristics & IMAGE_FILE_DLL)) return setup_exe_tls(m, tls);
    size_t tmpl = tls->EndAddressOfRawData - tls->StartAddressOfRawData;
    size_t size = tmpl + tls->SizeOfZeroFill;
    if (next_slot >= kMaxTlsSlots) throw std::runtime_error(m.name + ": no rt_tls_slot DLL left");

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    auto path = std::filesystem::path(self).parent_path() / (L"rt_tls_slot" + std::to_wstring(next_slot++) + L".dll");
    HMODULE slot = LoadLibraryW(path.c_str());
    if (!slot) throw std::runtime_error("can't load " + path.string());
    auto index = reinterpret_cast<SlotIndexFn>(GetProcAddress(slot, "rt_tls_slot_index"));
    auto capacity = reinterpret_cast<SlotCapacityFn>(GetProcAddress(slot, "rt_tls_slot_capacity"));
    auto block = reinterpret_cast<SlotBlockFn>(GetProcAddress(slot, "rt_tls_slot_block"));
    auto init = reinterpret_cast<SlotInitFn>(GetProcAddress(slot, "rt_tls_slot_init"));
    if (!index || !capacity || !block || !init) throw std::runtime_error(path.string() + ": missing exports");
    if (size > capacity()) throw std::runtime_error(m.name + ": TLS block too big for a slot");

    // The slot must really own a TLS block of that size, all zero fill (so it starts at the slot's TLS base, where
    // the module addresses its data from), and ntdll must have allocated it.
    auto* sb = reinterpret_cast<uint8_t*>(slot);
    auto* snt = reinterpret_cast<IMAGE_NT_HEADERS64*>(sb + reinterpret_cast<IMAGE_DOS_HEADER*>(sb)->e_lfanew);
    auto sdir = snt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    auto* stls = sdir.Size ? reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(sb + sdir.VirtualAddress) : nullptr;
    if (!stls || stls->EndAddressOfRawData - stls->StartAddressOfRawData + stls->SizeOfZeroFill < capacity())
        throw std::runtime_error(path.string() + ": has no usable TLS directory");
    // A fresh block is all zero. (When ntdll ignored the directory, index 0 pointed at another module's block.)
    auto** tls_array = reinterpret_cast<void**>(__readgsqword(0x58));
    auto* fresh = tls_array ? static_cast<const uint64_t*>(tls_array[index()]) : nullptr;
    if (!fresh || fresh != block() || reinterpret_cast<uintptr_t>(fresh) % 16 || fresh[0] || fresh[1] ||
        fresh[capacity() / 8 - 1]) {
        char why[160];
        std::snprintf(why, sizeof(why), ": TLS block isn't set up (index %lu, block %p, first %llx)", index(),
                      static_cast<const void*>(fresh), fresh ? static_cast<unsigned long long>(fresh[0]) : 0ull);
        throw std::runtime_error(path.string() + why);
    }

    *reinterpret_cast<uint32_t*>(tls->AddressOfIndex) = index();
    auto* state = new TlsModule{&m, tls_callbacks(m)};
    if (!init(reinterpret_cast<const void*>(tls->StartAddressOfRawData), tmpl, &on_thread, state))
        throw std::runtime_error(m.name + ": rt_tls_slot_init failed");
    std::fprintf(stderr, "[rt] %s: TLS %zu bytes in slot %ls (index %lu, block %p), %zu callback(s)\n",
                 m.name.c_str(), size, path.filename().c_str(), index(), block(), state->callbacks.size());
}

}  // namespace

// The finest timer resolution Windows offers (usually 0.5 ms), for the title's whole run: the console's timers are
// precise, and with Windows' default 15.6 ms tick every short sleep rounds up; the title's audio mixer then made a
// third of the sound it should (one 10 ms buffer in three, the others silent). Windows 11 drops a process' timer
// request while its window is hidden unless told not to.
void fine_timer() {
    PROCESS_POWER_THROTTLING_STATE keep{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                        PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    BOOL kept = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &keep, sizeof keep);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    using QueryFn = LONG(NTAPI*)(ULONG*, ULONG*, ULONG*);
    using SetFn = LONG(NTAPI*)(ULONG, BOOLEAN, ULONG*);
    auto query = nt ? reinterpret_cast<QueryFn>(GetProcAddress(nt, "NtQueryTimerResolution")) : nullptr;
    auto set = nt ? reinterpret_cast<SetFn>(GetProcAddress(nt, "NtSetTimerResolution")) : nullptr;
    ULONG coarsest = 0, finest = 0, was = 0, now = 0;
    if (!query || !set || query(&coarsest, &finest, &was) < 0 || !finest) {
        std::fprintf(stderr, "[rt] timer: the resolution couldn't be raised\n");
        return;
    }
    LONG status = set(finest, TRUE, &now);
    std::fprintf(stderr, "[rt] timer: %s (was %.2f ms, now %.2f ms; kept while hidden: %s)\n",
                 status >= 0 ? "the finest resolution" : "raising the resolution failed", was / 10000.0, now / 10000.0,
                 kept ? "yes" : "no");
}

// Every thread's stack, on request: creating %LOCALAPPDATA%\Flamingo\diag\threads (the file is deleted) logs each
// thread's name and frames. For a title that stops making progress (a load that never ends): where each thread
// waits. Threads are suspended one at a time only to copy and unwind their context (unwinding takes no locks);
// names and symbols are printed after every thread is running again, so a thread suspended inside the heap or
// stderr can't deadlock the dump.
struct ThreadStack {
    DWORD id;
    std::wstring name;
    DWORD64 frames[24];
    int count;
    DWORD64 r10, rdx;  // a wait syscall's arguments: NtWaitForSingleObject(handle = r10, ...),
                       // NtWaitForMultipleObjects(count = r10, handles = rdx, ...)
};

// "Event \"name\"" for a handle (type and name from NtQueryObject), or "" if it isn't one.
std::string describe_handle(HANDLE h) {
    using QueryFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    if (!query || !h) return {};
    struct Unicode {
        USHORT length, max;
        PWSTR buffer;
    };
    alignas(8) unsigned char type[512] = {}, name[1024] = {};
    if (query(h, 2, type, sizeof type, nullptr) < 0) return {};  // ObjectTypeInformation: a UNICODE_STRING first
    std::string out;
    auto* t = reinterpret_cast<Unicode*>(type);
    for (USHORT i = 0; t->buffer && i < t->length / 2; ++i) out += static_cast<char>(t->buffer[i]);
    if (query(h, 1, name, sizeof name, nullptr) >= 0) {  // ObjectNameInformation
        auto* n = reinterpret_cast<Unicode*>(name);
        if (n->buffer && n->length) {
            out += " \"";
            for (USHORT i = 0; i < n->length / 2; ++i) out += static_cast<char>(n->buffer[i] < 128 ? n->buffer[i] : '?');
            out += "\"";
        }
    }
    char hv[32];
    std::snprintf(hv, sizeof hv, " (handle %llx)", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    return out + hv;
}

void dump_threads() {
    std::vector<ThreadStack> stacks;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<DWORD> ids;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) ids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    stacks.reserve(ids.size());  // no allocation while a thread is suspended
    for (DWORD id : ids) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (!t) continue;
        ThreadStack s{id, {}, {}, 0, 0, 0};
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc) {
            s.name = desc;
            LocalFree(desc);
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            if (GetThreadContext(t, &c)) {
                s.r10 = c.R10;
                s.rdx = c.Rdx;
                for (; s.count < 24 && c.Rip; ++s.count) {
                    s.frames[s.count] = c.Rip;
                    DWORD64 image_base = 0;
                    auto* fe = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
                    if (!fe) {
                        if (IsBadReadPtr(reinterpret_cast<void*>(c.Rsp), 8)) break;
                        c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                        c.Rsp += 8;
                        continue;
                    }
                    void* handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        stacks.push_back(std::move(s));
    }
    std::fprintf(stderr, "[rt] threads: %zu\n", stacks.size());
    for (const ThreadStack& s : stacks) {
        std::fprintf(stderr, "[rt] thread %lu \"%ls\":\n", s.id, s.name.c_str());
        // what it waits on: r10 as one handle, or r10 handles at rdx
        std::string one = describe_handle(reinterpret_cast<HANDLE>(s.r10));
        if (!one.empty()) {
            std::fprintf(stderr, "[rt]     waits on %s\n", one.c_str());
        } else if (s.r10 > 0 && s.r10 <= 64 && !IsBadReadPtr(reinterpret_cast<void*>(s.rdx), s.r10 * sizeof(HANDLE))) {
            const HANDLE* hs = reinterpret_cast<const HANDLE*>(s.rdx);
            for (DWORD64 i = 0; i < s.r10; ++i) {
                std::string d = describe_handle(hs[i]);
                if (!d.empty()) std::fprintf(stderr, "[rt]     waits on [%llu] %s\n", (unsigned long long)i, d.c_str());
            }
        }
        for (int i = 0; i < s.count; ++i)
            std::fprintf(stderr, "[rt]     #%-2d %s\n", i, where(reinterpret_cast<void*>(s.frames[i])).c_str());
    }
}

// A sampling profile, on request (%LOCALAPPDATA%\Flamingo\diag\profile): every thread's stack 400 times, 5 ms apart,
// then the chains of title code seen most, with how often. A title that runs but makes no progress (a load that
// polls for something each frame) shows the code that polls: threads parked in a wait give one chain every time,
// the working ones give the functions of their loop.
// Each sample whole (thread, frames), for the second part of the report: where the time goes outside the title.
struct Sample {
    DWORD thread;
    int count;
    DWORD64 frames[32];
};

// "module!function" (the runtime's from its PDB, the system's from their exports), or the title module's name alone:
// what the profile's second part groups samples by.
std::string symbol_of(DWORD64 address, std::map<DWORD64, std::string>& cache) {
    if (auto it = cache.find(address); it != cache.end()) return it->second;
    std::string out;
    auto a = reinterpret_cast<const uint8_t*>(address);
    for (auto* m : g_modules)
        if (a >= m->base && a < m->base + m->size) out = m->name;
    HMODULE host = nullptr;
    if (out.empty() &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &host)) {
        char name[MAX_PATH] = {};
        GetModuleFileNameA(host, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        out = base ? base + 1 : name;
        static bool symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 off = 0;
        if (symbols && SymFromAddr(GetCurrentProcess(), address, &off, sym)) {
            // an export that is far behind is a function without a name of its own: keep the module only
            if (host == GetModuleHandleW(nullptr) || off < 0x2000) out += std::string("!") + sym->Name;
        }
    }
    if (out.empty()) out = "?";
    cache[address] = out;
    return out;
}

// A sample whose innermost frame is a system call that waits: the thread isn't using a CPU.
bool waiting_symbol(const std::string& s) {
    const auto bang = s.find('!');
    if (bang == std::string::npos || s.compare(0, bang, "ntdll.dll") != 0) return false;
    const std::string f = s.substr(bang + 1);
    for (const char* w : {"Wait", "Delay", "RemoveIoCompletion", "ReplyWaitReceive", "WorkerFactory", "Yield"})
        if (f.find(w) != std::string::npos) return true;
    return false;
}

void profile_report_host(const std::vector<Sample>& samples, int rounds) {
    std::map<DWORD64, std::string> cache;
    std::map<std::string, int> self, inclusive;
    std::map<DWORD, std::pair<int, int>> threads;  // thread -> (busy samples, all samples)
    int busy = 0;
    for (const Sample& s : samples) {
        if (!s.count) continue;
        const std::string leaf = symbol_of(s.frames[0], cache);
        auto& t = threads[s.thread];
        ++t.second;
        if (waiting_symbol(leaf)) continue;
        ++busy;
        ++t.first;
        ++self[leaf];
        std::vector<std::string> seen;  // the runtime's functions on the stack, once each
        for (int i = 0; i < s.count; ++i) {
            std::string f = symbol_of(s.frames[i], cache);
            if (f.rfind("FlamingoPort.exe!", 0) != 0) continue;
            if (std::find(seen.begin(), seen.end(), f) == seen.end()) seen.push_back(f);
        }
        for (const std::string& f : seen) ++inclusive[f];
    }
    auto top = [](const std::map<std::string, int>& m) {
        std::vector<std::pair<int, std::string>> v;
        for (auto& [k, n] : m) v.push_back({n, k});
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
        return v;
    };
    std::fprintf(stderr, "[rt] profile: CPUs busy on average %.2f (%d busy samples in %d rounds)\n",
                 rounds ? static_cast<double>(busy) / rounds : 0.0, busy, rounds);
    auto s = top(self);
    for (size_t i = 0; i < s.size() && i < 50; ++i)
        std::fprintf(stderr, "[rt] profile self: %5.1f%%  %s\n", 100.0 * s[i].first / std::max(busy, 1), s[i].second.c_str());
    auto in = top(inclusive);
    for (size_t i = 0; i < in.size() && i < 50; ++i)
        std::fprintf(stderr, "[rt] profile runtime: %5.1f%%  %s\n", 100.0 * in[i].first / std::max(busy, 1),
                     in[i].second.c_str());
    std::vector<std::pair<int, DWORD>> by_thread;
    for (auto& [id, n] : threads) by_thread.push_back({n.first, id});
    std::sort(by_thread.begin(), by_thread.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < by_thread.size() && i < 32 && by_thread[i].first; ++i) {
        std::wstring name;
        if (HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, by_thread[i].second)) {
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc) {
                name = desc;
                LocalFree(desc);
            }
            CloseHandle(t);
        }
        std::fprintf(stderr, "[rt] profile thread: %5.1f%% of a CPU  thread %lu \"%ls\"\n",
                     rounds ? 100.0 * by_thread[i].first / rounds : 0.0, by_thread[i].second, name.c_str());
    }
}

void profile_threads() {
    std::map<std::vector<DWORD64>, int> chains;  // the title's frames of a stack (innermost first) -> samples
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<Sample> samples;
    samples.reserve(400 * 160);
    for (int round = 0; round < 400; ++round) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te{sizeof te};
        std::vector<DWORD> ids;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) ids.push_back(te.th32ThreadID);
        CloseHandle(snap);
        for (DWORD id : ids) {
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
            if (!t) continue;
            DWORD64 frames[32];
            int count = 0;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(t) != static_cast<DWORD>(-1)) {
                if (GetThreadContext(t, &c)) {
                    for (; count < 32 && c.Rip; ++count) {
                        frames[count] = c.Rip;
                        DWORD64 image_base = 0;
                        auto* fe = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
                        if (!fe) {
                            if (IsBadReadPtr(reinterpret_cast<void*>(c.Rsp), 8)) break;
                            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                            c.Rsp += 8;
                            continue;
                        }
                        void* handler_data = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &handler_data, &establisher,
                                         nullptr);
                    }
                }
                ResumeThread(t);
            }
            CloseHandle(t);
            if (count) {
                samples.push_back(Sample{id, count, {}});
                std::memcpy(samples.back().frames, frames, sizeof(DWORD64) * count);
            }
            // the frames outside the system's and our own modules: the title's
            std::vector<DWORD64> chain;
            bool waiting = false;
            for (int i = 0; i < count && chain.size() < 8; ++i) {
                auto a = reinterpret_cast<const uint8_t*>(frames[i]);
                bool title = false;  // inside a module our loader mapped (the title's exe and DLLs)
                for (auto* m : g_modules)
                    if (a >= m->base && a < m->base + m->size) title = true;
                if (!title) {
                    if (i == 0) waiting = true;  // sampled in the system or in the runtime
                    continue;
                }
                chain.push_back(frames[i]);
            }
            if (!chain.empty()) {
                if (waiting) chain.insert(chain.begin(), 0);  // 0 first: sampled inside a system call
                ++chains[chain];
            }
        }
        Sleep(5);
    }
    std::vector<std::pair<int, const std::vector<DWORD64>*>> top;
    for (auto& [k, v] : chains) top.push_back({v, &k});
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[rt] profile: 400 samples, %zu chains of title code\n", top.size());
    for (size_t i = 0; i < top.size() && i < 60; ++i) {
        std::string line;
        for (DWORD64 f : *top[i].second) {
            line += f ? where(reinterpret_cast<void*>(f)) : std::string("(syscall)");
            line += " < ";
        }
        std::fprintf(stderr, "[rt] profile: %4d  %s\n", top[i].first, line.c_str());
    }
    profile_report_host(samples, 400);
}

// Where the process's memory is, on request (%LOCALAPPDATA%\Flamingo\diag\memory): every committed region by kind,
// with what of it is resident (QueryWorkingSetEx, page by page) and how much of that is shared (a page of a section
// seen through two views, as uma's pool is: the title's view and the chunk view the GPU heap is opened on, counts
// twice in the working set and once in RAM). The private allocations outside uma, biggest first.
void memory_report() {
    struct Sum {
        uint64_t committed = 0, resident = 0, shared = 0;
    };
    std::map<std::string, Sum> kinds;
    std::map<uintptr_t, Sum> privates;  // by allocation base
    std::vector<HANDLE> heaps(256);
    heaps.resize(GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data()));
    const uintptr_t ub = uma::base(), ue = ub + uma::size();
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages;
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t at = 0x10000; VirtualQuery(reinterpret_cast<void*>(at), &m, sizeof m); ) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress);
        if (base + m.RegionSize <= at) break;
        at = base + m.RegionSize;
        if (m.State != MEM_COMMIT) continue;
        std::string kind = base >= ub && base < ue ? "uma (the title's memory)"
                           : m.Type == MEM_IMAGE   ? "images (exe and DLLs)"
                           : m.Type == MEM_MAPPED  ? (m.RegionSize >= (1ull << 30) ? "mapped 1 GiB views (uma chunks for the GPU)"
                                                                                  : "mapped, other")
                                                   : "private";
        Sum s;
        s.committed = m.RegionSize;
        // residency, 4 KiB at a time, in batches
        const uint64_t count = m.RegionSize / 4096;
        for (uint64_t done = 0; done < count;) {
            const uint64_t n = std::min<uint64_t>(count - done, 1 << 16);
            pages.resize(n);
            for (uint64_t i = 0; i < n; ++i) pages[i].VirtualAddress = reinterpret_cast<void*>(base + (done + i) * 4096);
            if (QueryWorkingSetEx(GetCurrentProcess(), pages.data(), static_cast<DWORD>(n * sizeof pages[0])))
                for (uint64_t i = 0; i < n; ++i)
                    if (pages[i].VirtualAttributes.Valid) {
                        s.resident += 4096;
                        if (pages[i].VirtualAttributes.ShareCount > 1 || pages[i].VirtualAttributes.Shared)
                            s.shared += 4096;
                    }
            done += n;
        }
        Sum& k = kinds[kind];
        k.committed += s.committed, k.resident += s.resident, k.shared += s.shared;
        if (kind == "private") {
            Sum& p = privates[reinterpret_cast<uintptr_t>(m.AllocationBase)];
            p.committed += s.committed, p.resident += s.resident;
        }
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    std::fprintf(stderr, "[rt] memory: working set %llu MiB, private (commit) %llu MiB; uma pool in use %llu MiB\n",
                 static_cast<unsigned long long>(pmc.WorkingSetSize >> 20),
                 static_cast<unsigned long long>(pmc.PrivateUsage >> 20),
                 static_cast<unsigned long long>(uma::pool_bytes_used() >> 20));
    for (auto& [k, s] : kinds)
        std::fprintf(stderr, "[rt] memory:   %-45s committed %6llu MiB, resident %6llu MiB (%llu MiB shared)\n", k.c_str(),
                     static_cast<unsigned long long>(s.committed >> 20), static_cast<unsigned long long>(s.resident >> 20),
                     static_cast<unsigned long long>(s.shared >> 20));
    std::fprintf(stderr, "[rt] memory:   of the private: the title's CPU memory (XMemAlloc) %llu MiB\n",
                 static_cast<unsigned long long>(shims::xmem_cpu_bytes() >> 20));
    // the private allocations by size: many middle-sized ones point at an allocator, a few big ones at buffers
    const uint64_t limits[] = {1ull << 20, 4ull << 20, 16ull << 20, 64ull << 20, 256ull << 20, ~0ull};
    uint64_t count[6] = {}, resident[6] = {};
    for (auto& [b, s] : privates)
        for (int k = 0; k < 6; ++k)
            if (s.committed < limits[k]) {
                ++count[k], resident[k] += s.resident;
                break;
            }
    const char* names[] = {"< 1 MiB", "1-4 MiB", "4-16 MiB", "16-64 MiB", "64-256 MiB", ">= 256 MiB"};
    for (int k = 0; k < 6; ++k)
        std::fprintf(stderr, "[rt] memory:   private allocations %-10s: %6llu, resident %6llu MiB\n", names[k],
                     static_cast<unsigned long long>(count[k]), static_cast<unsigned long long>(resident[k] >> 20));
    std::vector<std::pair<uint64_t, uintptr_t>> top;
    for (auto& [b, s] : privates) top.push_back({s.resident, b});
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < top.size() && i < 20; ++i) {
        const Sum& s = privates[top[i].second];
        const bool heap = std::find(heaps.begin(), heaps.end(), reinterpret_cast<HANDLE>(top[i].second)) != heaps.end();
        std::fprintf(stderr, "[rt] memory:   private at %p: committed %llu MiB, resident %llu MiB%s\n",
                     reinterpret_cast<void*>(top[i].second), static_cast<unsigned long long>(s.committed >> 20),
                     static_cast<unsigned long long>(s.resident >> 20), heap ? " (a heap)" : "");
    }
}

DWORD WINAPI thread_dump_watch(void*) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n > MAX_PATH - 40) return 0;
    std::wstring threads = std::wstring(dir) + L"\\Flamingo\\diag\\threads";
    std::wstring profile = std::wstring(dir) + L"\\Flamingo\\diag\\profile";
    std::wstring memory = std::wstring(dir) + L"\\Flamingo\\diag\\memory";
    for (;;) {
        Sleep(500);
        if (GetFileAttributesW(threads.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(threads.c_str());
            dump_threads();
        }
        if (GetFileAttributesW(profile.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(profile.c_str());
            profile_threads();
        }
        if (GetFileAttributesW(memory.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(memory.c_str());
            memory_report();
        }
    }
}

int run(Loader& loader, Module& exe, const std::filesystem::path& game_dir, const std::vector<uint32_t>& probes) {
    fine_timer();
    if (HANDLE t = CreateThread(nullptr, 0, thread_dump_watch, nullptr, 0, nullptr)) CloseHandle(t);
    g_modules = loader.modules();
    std::setvbuf(stderr, nullptr, _IONBF, 0);  // a fail-fast ends the process without flushing
    SetUnhandledExceptionFilter(&crash_report);
    AddVectoredExceptionHandler(1, &first_chance);
    SetCurrentDirectoryW(game_dir.c_str());
    // On the console the title's package is drive G:, and its executable sits at the top of it. T: is the title's
    // temporary space and persistent local storage another drive; here folders under %LOCALAPPDATA%\Flamingo.
    files::mount(L'G', game_dir);
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::filesystem::path data = std::filesystem::path(local[0] ? local : L".") / L"Flamingo";
    std::error_code ec;
    std::filesystem::create_directories(data / L"temp", ec);
    files::mount(L'T', data / L"temp");
    storage::init(game_dir, data / L"storage");
    shims::set_save_root((data / L"saves").wstring());
    shims::set_title_path(L"G:\\" + std::filesystem::path(exe.name).wstring());
    shims::set_game_identity((std::filesystem::path(game_dir) / L"MicrosoftGame.Config").wstring());
    if (auto hooks = exe.exports.find("__XMemAllocationHooks__"); hooks != exe.exports.end())
        shims::set_xmem_hooks(hooks->second);
    apply_game_patches(exe, game_dir, loader.trace_host());
    arm_probes(exe, probes);

    int next_slot = 0;
    for (auto* m : g_modules) {
        register_unwind(*m);
        setup_tls(*m, next_slot);
    }
    std::fprintf(stderr, "[rt] %zu module(s) ready\n", g_modules.size());
    for (auto* m : g_modules)
        std::fprintf(stderr, "[rt]   %s at %p (%u bytes)\n", m->name.c_str(), static_cast<void*>(m->base), m->size);

    // Bundled DLLs first, last loaded first (their dependencies were loaded after them).
    for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
        Module& m = **it;
        if (&m == &exe) continue;
        for (auto cb : tls_callbacks(m)) cb(m.base, DLL_PROCESS_ATTACH, nullptr);
        if (!m.entry_rva) continue;
        std::fprintf(stderr, "[rt] %s: DllMain\n", m.name.c_str());
        auto dll_main = reinterpret_cast<BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID)>(m.base + m.entry_rva);
        if (!dll_main(reinterpret_cast<HINSTANCE>(m.base), DLL_PROCESS_ATTACH, nullptr)) {
            std::fprintf(stderr, "[rt] %s: DllMain returned FALSE\n", m.name.c_str());
            return 1;
        }
    }

    for (auto cb : tls_callbacks(exe)) cb(exe.base, DLL_PROCESS_ATTACH, nullptr);
    std::fprintf(stderr, "[rt] %s: entry point\n", exe.name.c_str());
    std::fflush(stderr);
    auto entry = reinterpret_cast<int (*)()>(exe.base + exe.entry_rva);
    int code = entry();  // normally doesn't return: the CRT calls exit()
    std::fprintf(stderr, "[rt] entry point returned %d\n", code);
    shims::report_called_unimplemented();
    return code;
}

}  // namespace rt
