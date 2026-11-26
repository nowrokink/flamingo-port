// The shaders the runtime found at run time and saved for translation (pending_shaders.py). Some of the title's
// pipelines (slot 86 stream pipelines) carry console code that isn't in shaders.rpf: DXBC containers whose DXIL part
// has no bitcode, only XDAT -> XBCH -> FSSM, the same format as shaders.rpf's. The runtime saves each one it can't
// find in the cache as <cache>/pending/<HASH digest>.dxbc; this runs them through the same chain as shaders.rpf's (an
// index with each one's stage, unpack, disassembly, the cache build), so their DXIL lands in the cache under the same
// digest names and the next run finds it.
#pragma once
#include <atomic>
#include <string>

#include "cache/builder.h"
#include "cache/progress.h"

namespace fp::cache {

// 0 when done (or nothing to do), else non-zero. `base` gives the jobs, the helpers' port and token and the process
// job; its folders are ignored.
int translate_pending(const std::wstring& cache, const BuildOptions& base, const std::atomic<bool>& stop,
                      const Event& event, const Log& log);

}  // namespace fp::cache
