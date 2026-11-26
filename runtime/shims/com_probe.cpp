#include "com_probe.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstdio>
#include <utility>

namespace rt::shims {
namespace {

// The interface name lives in a hidden entry just before the method table, so probes work with any object layout
// (all they need is the table pointer first). Some console methods don't take the object in rcx (those returning a
// struct get the result buffer there), so the table is only trusted if it's one of ours.
constexpr int kMaxTables = 256;
void** g_tables[kMaxTables];
long g_table_count = 0;

bool is_probe_table(void* const* vtbl) {
    for (long i = 0; i < g_table_count; ++i)
        if (g_tables[i] == vtbl) return true;
    return false;
}

// Calls logged per table and slot: the first few only (every call is a console line, and a frame makes hundreds).
constexpr long kLoggedCalls = 8;
long g_calls[kMaxTables][kProbeSlots];

volatile long g_burst = 0;

struct Recent {
    const void* self;
    int slot;
    uint64_t a1, a2, a3;
    void* from;
};
constexpr long kRecent = 4096;
Recent g_recent[kRecent];
volatile long g_recent_next = 0;

// Whether this call is one to log (and a note when a slot stops being logged).
bool count_call(const void* self, int slot) {
    if (g_burst > 0 && InterlockedDecrement(&g_burst) >= 0) return true;
    void* const* vtbl = nullptr;
    __try {
        vtbl = *static_cast<void* const* const*>(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    for (long i = 0; i < g_table_count && i < kMaxTables; ++i)
        if (g_tables[i] == vtbl) {
            long n = InterlockedIncrement(&g_calls[i][slot]);
            if (n == kLoggedCalls + 1)
                std::fprintf(stderr, "[rt] probe %s::slot%d: called more; not logged any further\n",
                             static_cast<const char*>(vtbl[-1]), slot);
            if (n >= 1024 && (n & (n - 1)) == 0)
                std::fprintf(stderr, "[rt] probe %s::slot%d: %ld calls so far\n",
                             static_cast<const char*>(vtbl[-1]), slot, n);
            return n <= kLoggedCalls;
        }
    return true;
}

const char* name_of(const void* self) {
    if (!self) return "?";
    void* const* vtbl = nullptr;
    __try {
        vtbl = *static_cast<void* const* const*>(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
    return is_probe_table(vtbl) ? static_cast<const char*>(vtbl[-1]) : "?(rcx is not the object)";
}

// Eight integer parameters: the method's own (rcx, rdx, r8, r9) plus four stack slots, read whether or not the
// caller passed them (they're in the caller's frame either way).
template <int Slot>
uint64_t probe_slot(void* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t s4, uint64_t s5, uint64_t s6,
                    uint64_t s7) {
    long k = InterlockedIncrement(&g_recent_next) - 1;
    g_recent[k & (kRecent - 1)] = {self, Slot, a1, a2, a3, _ReturnAddress()};
    if (!count_call(self, Slot)) return 0;
    std::fprintf(stderr,
                 "[rt] probe %s(%p)::slot%d(rdx=%llx r8=%llx r9=%llx | stack %llx %llx %llx %llx) from %p\n",
                 name_of(self), self, Slot, a1, a2, a3, s4, s5, s6, s7, _ReturnAddress());
    // what the pointer arguments are: one of our objects, or the first words there
    const uint64_t args[3] = {a1, a2, a3};
    static const char* const kReg[3] = {"rdx", "r8", "r9"};
    for (int i = 0; i < 3; ++i) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (args[i] < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(args[i]), &mbi, sizeof mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            args[i] + 64 > reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize)
            continue;
        const uint64_t* q = reinterpret_cast<const uint64_t*>(args[i]);
        const char* name = name_of(q);
        if (name[0] != '?') std::fprintf(stderr, "[rt] probe   %s is a %s\n", kReg[i], name);
        else
            std::fprintf(stderr, "[rt] probe   %s -> %llx %llx %llx %llx %llx %llx %llx %llx\n", kReg[i], q[0], q[1], q[2],
                         q[3], q[4], q[5], q[6], q[7]);
    }
    return 0;  // S_OK
}

template <int... I>
std::array<void*, sizeof...(I)> probe_table(std::integer_sequence<int, I...>) {
    return {reinterpret_cast<void*>(&probe_slot<I>)...};
}

HRESULT STDMETHODCALLTYPE query_interface(void* self, const GUID* iid, void** out) {
    std::fprintf(stderr,
                 "[rt] probe %s::QueryInterface({%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}) -> itself\n",
                 name_of(self), iid->Data1, iid->Data2, iid->Data3, iid->Data4[0], iid->Data4[1], iid->Data4[2],
                 iid->Data4[3], iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
    if (!out) return E_POINTER;
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE add_ref(void*) { return 2; }
ULONG STDMETHODCALLTYPE release(void*) { return 1; }  // probe objects live for the process

}  // namespace

void** make_probe_vtable(const char* name) {
    const auto table = probe_table(std::make_integer_sequence<int, kProbeSlots>{});
    auto** block = new void*[kProbeSlots + 1];
    block[0] = const_cast<char*>(name);  // see name_of
    void** vtbl = block + 1;
    for (int i = 0; i < kProbeSlots; ++i) vtbl[i] = table[i];
    vtbl[0] = reinterpret_cast<void*>(&query_interface);
    vtbl[1] = reinterpret_cast<void*>(&add_ref);
    vtbl[2] = reinterpret_cast<void*>(&release);
    long i = InterlockedIncrement(&g_table_count) - 1;
    if (i < kMaxTables) g_tables[i] = vtbl;
    return vtbl;
}

void set_probe_method(void** vtbl, int slot, ProbeMethod fn) { vtbl[slot] = fn; }

void probe_burst(long calls) { InterlockedExchange(&g_burst, calls); }

void print_recent_probes(const void* self, const char* prefix) {
    long end = g_recent_next;
    for (long k = end > kRecent ? end - kRecent : 0; k < end; ++k) {
        const Recent& r = g_recent[k & (kRecent - 1)];
        if (r.self != self) continue;
        std::fprintf(stderr, "%s slot%d(rdx=%llx r8=%llx r9=%llx) from %p\n", prefix, r.slot,
                     static_cast<unsigned long long>(r.a1), static_cast<unsigned long long>(r.a2),
                     static_cast<unsigned long long>(r.a3), r.from);
    }
}

ProbeObject* make_probe_object(void** vtbl, const char* name, void* user) {
    return new ProbeObject{vtbl, name, user};
}

const char* probe_object_name(const void* p) {
    // VirtualQuery, not __try: crash reports call this from the exception handler, which sees nested faults first.
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000 || reinterpret_cast<uintptr_t>(p) % 8 ||
        !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return nullptr;
    void* const* vtbl = *static_cast<void* const* const*>(p);
    return is_probe_table(vtbl) ? static_cast<const char*>(vtbl[-1]) : nullptr;
}

}  // namespace rt::shims
