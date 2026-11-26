// Console-CPU (Zen 2) instructions emulated on PC CPUs that lack them (see cpu_emul.cpp).
#pragma once
#include <windows.h>

namespace rt {

// For an EXCEPTION_ILLEGAL_INSTRUCTION on one of them: performs it, steps over it, returns true (continue
// execution). Anything else: false (an unknown illegal instruction is logged with its bytes).
bool emulate_console_instruction(EXCEPTION_POINTERS* ep);

// Instructions emulated so far, in all (each one is an exception, a few microseconds).
unsigned long long emulated_instructions();

}  // namespace rt
