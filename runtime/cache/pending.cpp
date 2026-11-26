#include "pending.h"

#include "core/fs.h"
#include "core/json.h"
#include "core/system.h"
#include "core/text.h"
#include "shaders/dxbc.h"
#include "shaders/prep.h"

namespace fp::cache {

int translate_pending(const std::wstring& cache, const BuildOptions& base, const std::atomic<bool>& stop,
                      const Event& event, const Log& log) {
    auto say = [&](const std::string& s) {
        if (log) log(s);
    };
    const std::wstring pending = join(cache, L"pending");
    std::vector<std::wstring> names;
    if (is_dir(pending))
        for (const auto& f : list_dir(pending))
            if (f.size() > 5 && f.compare(f.size() - 5, 5, L".dxbc") == 0) names.push_back(f);
    size_t todo = 0;
    for (const auto& f : names) todo += !is_file(join(cache, f.substr(0, f.size() - 5) + L".dxil"));
    if (!todo) {
        say("pending shaders: none");
        return 0;
    }
    // the stage of each, as the carve index has it (the translator reads it from there)
    json::Value index = json::Value::array();
    for (const auto& f : names) {
        std::string c;
        read_file(join(pending, f), c);
        json::Value e = json::Value::object();
        e.set("file", narrow(f));
        e.set("stage", dxbc::stage(c));
        index.push(std::move(e));
    }
    json::save_file(join(pending, L"index.json"), index, 1);
    say("pending shaders: " + std::to_string(todo) + " to translate");

    auto progress = [](size_t, size_t) {};
    if (!shaders::unpack(pending, stop, progress, log)) return 1;
    const std::wstring llvm = which(L"llvm-mc", tool_path());
    if (llvm.empty()) {
        say("llvm-mc wasn't found (the Vulkan SDK or LLVM)");
        return 1;
    }
    if (!shaders::disassemble(join(pending, L"fssm"), join(pending, L"disasm"), llvm, L"gfx1013", base.jobs, stop, progress,
                              log))
        return 1;
    BuildOptions o = base;
    o.disasm = join(pending, L"disasm");
    o.dxbc = pending;
    o.out = cache;
    return build(o, stop, event, log);
}

}  // namespace fp::cache
