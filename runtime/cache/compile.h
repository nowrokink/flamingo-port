// One shader's way to DXIL, in two halves (dxil_cache.to_hlsl / run_dxc): the translator and SPIRV-Cross make HLSL
// here; DXC compiles it here or on a helper PC (cache/server.h).
#pragma once
#include <optional>
#include <string>

#include "cache/signatures.h"
#include "cache/toolchain.h"
#include "core/process.h"
#include "xlat/translator.h"

namespace fp::cache {

struct Outcome {
    std::optional<std::string> data;  // the HLSL / DXIL
    std::string why;                  // else why not ("translate: ...", "spirv-cross: ...", "dxc: ...")
};

// The DXC profile of a stage ("vs" -> vs_6_6, "ps", "cs", "ms" -> ms_6_5), or "" for stages the cache doesn't take.
const char* profile(const std::string& stage);

// rdna2spirv.translate_one + to_hlsl: SPIR-V assembly, spirv-as, spirv-val (Vulkan 1.2), spirv-cross --hlsl (SM 6.6).
// `keep`: a folder for the intermediates (they are deleted otherwise).
// `priority`: the tools' priority class (0: this process's).
Outcome to_hlsl(const std::wstring& s_path, xlat::Context& ctx, const Tools& tools, const std::wstring& keep,
                Job* job, DWORD priority = 0);

// dxc -T <profile> -E main <opt> with a time limit in seconds. DXC's optimiser takes minutes on large HLSL and the
// driver optimises again when it makes the pipeline, so the default is -O0 (a third of -O3's time on a sample).
Outcome run_dxc(const std::string& hlsl, const std::string& profile, const std::string& opt, int timeout_s,
                const Tools& tools, Job* job, DWORD priority = 0);

}  // namespace fp::cache
