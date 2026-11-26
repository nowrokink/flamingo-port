// The translated-shader cache the runtime reads (tools/dxil_cache.py in C++): each disassembled shader through the
// translator (xlat/) -> SPIR-V -> HLSL (SPIRV-Cross) -> DXIL (DXC), one file per shader named by the 16-byte HASH
// digest of its container (what a pipeline's stage metadata names it by; see docs/gpu-memory.md).
//
// Two halves: threads here translate to HLSL (a few MB of intermediates per shader, deleted at once) and feed a queue;
// DXC compiles from that queue, here and on helper PCs on the network (cache/server.h, tools/worker). DXC is most of
// the time, so helpers need only DXC: no copy of the game's data, just HLSL in and DXIL out.
//
// Writes <out>/<digest>.dxil, <out>/index.json ({digest: {name, stage, bytes}}), <digest>.vsin beside each vertex
// shader (its inputs) and <out>/translation.json (the progress). Shaders already in the cache are skipped, so an
// interrupted build continues where it stopped. A failure is kept as <digest>.fail with the translator's identity, so
// it isn't retried until the translator changes.
#pragma once
#include <atomic>
#include <string>

#include "cache/progress.h"
#include "core/process.h"

namespace fp::cache {

struct BuildOptions {
    std::wstring disasm;  // <name>.s
    std::wstring dxbc;    // <name>.dxbc (and index.json, fssm/ beside them)
    std::wstring out;     // the cache
    int jobs = 1;         // translating threads (and DXC processes unless local_dxc says otherwise)
    int limit = 0;        // only the first N (0: all)
    int timeout = 900;    // seconds DXC may spend on one shader (the largest take minutes even at -O0)
    std::string opt = "0";  // DXC optimisation level
    int serve = 0;        // also hand HLSL to helper PCs on this port (plain HTTP, local network only)
    std::string token;    // the helpers' password
    std::wstring keep;    // keep the intermediates (SPIR-V, HLSL) in this folder
    // leave shaders with more than big_kb of disassembly for a later run (minutes each in DXC; nearly all are
    // ray-tracing compute shaders)
    bool defer_big = false;
    int big_kb = 2500;
    // DXC processes on this machine (0: helpers compile, this machine only translates; it still compiles what a
    // helper failed, and takes the queue when no helper is seen for a minute); -1: jobs
    int local_dxc = -1;
    bool sidecars_only = false;  // only the vertex shaders' .vsin files
    Job* job = nullptr;          // where the tools' processes go (stopped with it)
    // Below normal: the build's threads and the tools' processes run that way (the launcher, to keep the PC usable);
    // otherwise the tools take this process's priority class.
    bool below_normal = false;
    // While this is true (the game running from the launcher) the tools start at idle priority: a translation at
    // normal priority nearly stopped the game loading its world.
    const std::atomic<bool>* idle_while = nullptr;
};

// Runs the build. 0 when it ran to the end, 1 when it couldn't (tools missing, stopped), 2 for bad options.
int build(const BuildOptions& o, const std::atomic<bool>& stop, const Event& event, const Log& log);

// The translator's identity: kept failures of another one are retried.
const char* tools_id();

}  // namespace fp::cache
