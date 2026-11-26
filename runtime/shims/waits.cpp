// api-ms-win-core-synch-l1-1-0.dll: the title's waits and signals, passed through to the system and remembered, so
// that when the title stops itself for a hang (a watchdog thread of its that writes to null) the log can say which
// thread waits on what, who made that event or semaphore, and who signalled it last. Every thread's stack at that
// point ends in a wait, which alone says nothing.
//
// What is kept: per thread, the wait it is in (handle, since when, called from where); per handle the title made
// through these imports, where it was made and its last signal. Handles the title got elsewhere (from the runtime's
// own APIs) show as "not made here".
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "shims.h"

namespace {

struct Waiting {
    std::atomic<DWORD> thread{0};
    HANDLE handle;
    ULONGLONG since;
    void* from;
};
constexpr int kThreads = 512;
Waiting g_waiting[kThreads];

struct Made {
    HANDLE handle;
    void* from;          // who made it
    const char* kind;
    void* signalled_from;
    ULONGLONG signalled_at;
    uint64_t signals;
};
constexpr int kHandles = 1 << 14;
Made g_made[kHandles];
SRWLOCK g_made_lock = SRWLOCK_INIT;

Made* slot_of(HANDLE h, bool claim) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(h);
    for (int i = 0; i < 64; ++i) {
        Made& m = g_made[((v >> 2) * 2654435761u + i) & (kHandles - 1)];
        if (m.handle == h) return &m;
        if (!m.handle) {
            if (!claim) return nullptr;
            m.handle = h;
            return &m;
        }
    }
    return nullptr;
}

void note_made(HANDLE h, void* from, const char* kind) {
    if (!h) return;
    AcquireSRWLockExclusive(&g_made_lock);
    if (Made* m = slot_of(h, true)) *m = {h, from, kind, nullptr, 0, 0};  // a reused handle value starts over
    ReleaseSRWLockExclusive(&g_made_lock);
}

void note_signal(HANDLE h, void* from) {
    AcquireSRWLockExclusive(&g_made_lock);
    if (Made* m = slot_of(h, false)) {
        m->signalled_from = from;
        m->signalled_at = GetTickCount64();
        ++m->signals;
    }
    ReleaseSRWLockExclusive(&g_made_lock);
}

Waiting* enter_wait(HANDLE h, void* from) {
    const DWORD me = GetCurrentThreadId();
    for (int i = 0; i < kThreads; ++i) {
        Waiting& w = g_waiting[(me / 4 + i) % kThreads];
        DWORD none = 0;
        if (w.thread.compare_exchange_strong(none, me)) {
            w.handle = h;
            w.since = GetTickCount64();
            w.from = from;
            return &w;
        }
    }
    return nullptr;
}

DWORD WINAPI WaitForSingleObjectEx_(HANDLE h, DWORD ms, BOOL alertable) {
    if (ms != INFINITE && ms < 1000) return WaitForSingleObjectEx(h, ms, alertable);  // polls: nothing to remember
    Waiting* w = enter_wait(h, _ReturnAddress());
    const DWORD r = WaitForSingleObjectEx(h, ms, alertable);
    if (w) w->thread = 0;
    return r;
}
DWORD WINAPI WaitForSingleObject_(HANDLE h, DWORD ms) {
    if (ms != INFINITE && ms < 1000) return WaitForSingleObject(h, ms);
    Waiting* w = enter_wait(h, _ReturnAddress());
    const DWORD r = WaitForSingleObject(h, ms);
    if (w) w->thread = 0;
    return r;
}
BOOL WINAPI SetEvent_(HANDLE h) {
    note_signal(h, _ReturnAddress());
    return SetEvent(h);
}
BOOL WINAPI ReleaseSemaphore_(HANDLE h, LONG count, LONG* previous) {
    note_signal(h, _ReturnAddress());
    return ReleaseSemaphore(h, count, previous);
}
HANDLE WINAPI CreateEventA_(SECURITY_ATTRIBUTES* sa, BOOL manual, BOOL initial, const char* name) {
    HANDLE h = CreateEventA(sa, manual, initial, name);
    note_made(h, _ReturnAddress(), manual ? "manual event" : "auto event");
    return h;
}
HANDLE WINAPI CreateEventW_(SECURITY_ATTRIBUTES* sa, BOOL manual, BOOL initial, const wchar_t* name) {
    HANDLE h = CreateEventW(sa, manual, initial, name);
    note_made(h, _ReturnAddress(), manual ? "manual event" : "auto event");
    return h;
}
HANDLE WINAPI CreateSemaphoreExW_(SECURITY_ATTRIBUTES* sa, LONG initial, LONG maximum, const wchar_t* name, DWORD flags,
                                  DWORD access) {
    HANDLE h = CreateSemaphoreExW(sa, initial, maximum, name, flags, access);
    note_made(h, _ReturnAddress(), "semaphore");
    return h;
}

#define SYNCH "api-ms-win-core-synch-l1-1-0.dll"

}  // namespace

namespace rt::shims {

// The waits longer than two seconds, oldest first is not attempted: the list is short.
void report_waits() {
    const ULONGLONG now = GetTickCount64();
    std::fprintf(stderr, "[rt] waits: the title's waits of two seconds and more:\n");
    for (const Waiting& w : g_waiting) {
        const DWORD thread = w.thread;
        if (!thread || w.since > now || now - w.since < 2000) continue;  // (a slot being filled reads as the future)
        Made m{};
        bool known = false;
        AcquireSRWLockShared(&g_made_lock);
        if (Made* found = slot_of(w.handle, false)) {
            m = *found;
            known = true;
        }
        ReleaseSRWLockShared(&g_made_lock);
        std::fprintf(stderr, "[rt] waits:   thread %lu, %.1f s on %p from %p: ", thread, (now - w.since) / 1000.0, w.handle,
                     w.from);
        if (known && !m.signals) {  // never signalled: are the others made at the same place ever signalled?
            uint64_t siblings = 0, signalled = 0, signals = 0;
            uint64_t before = 0, before_signalled = 0;  // those with a smaller handle value (made earlier, mostly)
            void* last = nullptr;
            AcquireSRWLockShared(&g_made_lock);
            for (const Made& o : g_made)
                if (o.handle && o.from == m.from) {
                    ++siblings;
                    if (o.handle < m.handle) {
                        ++before;
                        if (o.signals) ++before_signalled;
                    }
                    if (o.signals) {
                        ++signalled;
                        signals += o.signals;
                        last = o.signalled_from;
                    }
                }
            ReleaseSRWLockShared(&g_made_lock);
            std::fprintf(stderr, "%s made at %p, never signalled (of the %llu made there, %llu were ever signalled, "
                                 "%llu time(s), one from %p; %llu have a smaller handle, %llu of those signalled)\n",
                         m.kind, m.from, static_cast<unsigned long long>(siblings),
                         static_cast<unsigned long long>(signalled), static_cast<unsigned long long>(signals), last,
                         static_cast<unsigned long long>(before), static_cast<unsigned long long>(before_signalled));
            continue;
        }
        if (!known) std::fprintf(stderr, "not made here\n");
        else if (!m.signals) std::fprintf(stderr, "%s made at %p, never signalled\n", m.kind, m.from);
        else
            std::fprintf(stderr, "%s made at %p, signalled %llu time(s), last %.1f s ago from %p\n", m.kind, m.from,
                         static_cast<unsigned long long>(m.signals), (now - m.signalled_at) / 1000.0, m.signalled_from);
    }
}

}  // namespace rt::shims

SHIM_TABLE(waits,
    {SYNCH, "WaitForSingleObject", reinterpret_cast<void*>(&WaitForSingleObject_)},
    {SYNCH, "WaitForSingleObjectEx", reinterpret_cast<void*>(&WaitForSingleObjectEx_)},
    {SYNCH, "SetEvent", reinterpret_cast<void*>(&SetEvent_)},
    {SYNCH, "ReleaseSemaphore", reinterpret_cast<void*>(&ReleaseSemaphore_)},
    {SYNCH, "CreateEventA", reinterpret_cast<void*>(&CreateEventA_)},
    {SYNCH, "CreateEventW", reinterpret_cast<void*>(&CreateEventW_)},
    {SYNCH, "CreateSemaphoreExW", reinterpret_cast<void*>(&CreateSemaphoreExW_)},
);
