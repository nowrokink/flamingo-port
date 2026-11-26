// Probe COM objects: stand-ins for console interfaces whose layout isn't known yet.
//
// Every vtable slot logs its index and arguments (rcx = this, rdx, r8, r9 and the next 4 stack arguments) and
// returns S_OK, except slots a caller fills in with a real implementation. Hand one to the game, run it with
// FlamingoPort.exe --run, and read off which methods it calls, with what, in which order; then replace those slots. The
// table only has to come first in the object: the interface name is kept just before the table.
#pragma once
#include <cstdint>

namespace rt::shims {

constexpr int kProbeSlots = 200;

using ProbeMethod = void*;  // any function pointer with the method's real signature

struct ProbeObject {
    void* const* vtbl;  // must stay first: the game reads it as a COM object
    const char* name;
    void* user;         // for the real implementations
};

// A fresh vtable for interface `name` whose slots all log. Slots 0-2 behave as IUnknown (QueryInterface returns
// the object itself for any IID, AddRef/Release count). Change entries with set_probe_method before creating
// objects from it.
void** make_probe_vtable(const char* name);
void set_probe_method(void** vtbl, int slot, ProbeMethod fn);

// The interface name of one of our probe-table objects at `p`, else null (bad pointers too).
const char* probe_object_name(const void* p);

ProbeObject* make_probe_object(void** vtbl, const char* name, void* user = nullptr);

// The next `calls` probe calls are all logged, past the per-slot limit (for a frame being reported).
void probe_burst(long calls);

// The most recent probe calls on object `self` (kept in a ring of the last 4096 calls of all objects), printed
// oldest first with `prefix`: what the title called on it, unlogged, before something of interest.
void print_recent_probes(const void* self, const char* prefix);

}  // namespace rt::shims
