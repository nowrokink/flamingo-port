#include "compile.h"

#include <cstdlib>

#include <vector>

#include <optional>

#include <map>

#include <windows.h>

#include "core/fs.h"
#include "core/text.h"

namespace fp::cache {
namespace {

std::string first_of_stripped(const std::string& err) {
    // (r.stderr.strip().splitlines() or ["?"])[0][:90]
    size_t a = err.find_first_not_of(" \t\r\n\v\f");
    if (a == std::string::npos) return "?";
    size_t e = err.find_first_of("\r\n", a);
    return err.substr(a, e == std::string::npos ? std::string::npos : e - a).substr(0, 90);
}

int run(const std::wstring& exe, std::vector<std::wstring> args, std::string& out, std::string& err, Job* job,
        DWORD priority, DWORD timeout_ms = INFINITE) {
    Spawn s;
    s.exe = exe;
    s.args = std::move(args);
    s.job = job;
    s.timeout_ms = timeout_ms;
    s.priority = priority;
    return run_capture(s, "", out, err);
}

std::wstring stem(const std::wstring& path) {
    std::wstring b = file_name(path);
    size_t dot = b.rfind(L'.');
    return dot == std::wstring::npos || dot == 0 ? b : b.substr(0, dot);
}

// translate_one: the .spv in `dir` when it passes spirv-val, else why not.
std::optional<std::string> translate_one(const std::wstring& s_path, const std::wstring& dir, xlat::Context& ctx,
                                         const Tools& tools, Job* job, DWORD priority) {
    const std::wstring name = stem(s_path);
    std::string text;
    try {
        text = xlat::translate(s_path, ctx);
    } catch (const xlat::Unsupported& e) {
        return std::string(e.what());
    } catch (const xlat::PyError& e) {
        return "translator error " + std::string(e.what());
    } catch (const std::exception& e) {  // a fault of the port itself (the Python would have stopped the build)
        return "translator error " + std::string(e.what());
    }
    const std::wstring sa = join(dir, name + L".spvasm"), sv = join(dir, name + L".spv");
    if (!write_file(sa, text.data(), text.size())) return std::string("translator error cannot write");
    std::string out, err;
    int r = run(tools.spirv_as, {sa, L"-o", sv, L"--target-env", L"vulkan1.2"}, out, err, job, priority);
    if (r == 0) {
        err.clear();
        r = run(tools.spirv_val, {L"--target-env", L"vulkan1.2", sv}, out, err, job, priority);
    }
    if (r == 0) return std::nullopt;
    DeleteFileW(sv.c_str());
    return "INVALID: " + first_of_stripped(err);
}

}  // namespace

const char* profile(const std::string& stage) {
    if (stage == "vs") return "vs_6_6";
    if (stage == "ps") return "ps_6_6";
    if (stage == "cs") return "cs_6_6";
    if (stage == "ms") return "ms_6_5";
    return "";
}

Outcome to_hlsl(const std::wstring& s_path, xlat::Context& ctx, const Tools& tools, const std::wstring& keep,
                Job* job, DWORD priority) {
    ScratchDir scratch(L"dxil_");
    const std::wstring dir = keep.empty() ? scratch.path() : keep;
    if (dir.empty()) return {std::nullopt, "translate: no scratch folder"};
    if (auto why = translate_one(s_path, dir, ctx, tools, job, priority)) return {std::nullopt, "translate: " + *why};
    const std::wstring name = stem(s_path);
    const std::wstring spv = join(dir, name + L".spv"), hlsl = join(dir, name + L".hlsl");
    std::string out, err;
    if (run(tools.spirv_cross, {L"--hlsl", L"--shader-model", L"66", spv, L"--output", hlsl}, out, err, job, priority))
        return {std::nullopt, "spirv-cross: " + first_line(err)};
    std::string text;
    if (!read_file(hlsl, text)) return {std::nullopt, "spirv-cross: no output"};
    return {link_signatures(text, ctx.stage_of(narrow(name), s_path)), {}};
}

Outcome run_dxc(const std::string& hlsl, const std::string& prof, const std::string& opt, int timeout_s,
                const Tools& tools, Job* job, DWORD priority) {
    ScratchDir scratch(L"dxc_");
    if (!scratch.ok()) return {std::nullopt, "dxc: no scratch folder"};
    const std::wstring src = join(scratch.path(), L"s.hlsl"), dst = join(scratch.path(), L"s.dxil");
    if (!write_file(src, hlsl.data(), hlsl.size())) return {std::nullopt, "dxc: cannot write the HLSL"};
    std::string out, err;
    int r = run(tools.dxc, {L"-T", widen(prof), L"-E", L"main", widen(opt), src, L"-Fo", dst}, out, err, job, priority,
                timeout_s > 0 ? static_cast<DWORD>(timeout_s) * 1000 : INFINITE);
    if (r == -2) return {std::nullopt, "dxc: timed out"};
    if (r) return {std::nullopt, "dxc: " + first_line(err)};
    std::string dxil;
    if (!read_file(dst, dxil)) return {std::nullopt, "dxc: no output"};
    return {std::move(dxil), {}};
}

}  // namespace fp::cache
