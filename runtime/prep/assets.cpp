// The assets stage (make_game.py's): models (OBJ), texture dictionaries and texture pools (DDS) and the audio banks
// (AWC), exported from the user's copy by tools\rpf9dump, a small CLI over rpf9-explorer's Core (vendored in
// tools\rpf9-explorer; see docs/design.md, "rpf9dump"). It is built here with the .NET 8 SDK the first time, into
// <work>\tools\rpf9dump. An export whose manifest exists is done already, so a stopped run continues.
#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "core/fs.h"
#include "core/system.h"
#include "core/text.h"
#include "prep/preparation.h"

namespace fp::prep {
namespace {

// The archives models and texture dictionaries come from, as make_game.py listed them.
const wchar_t* const kModelArchives[] = {
    L"common.rpf", L"textures.rpf", L"data.rpf",
    L"pack\\story000_restofworld\\story000_restofworld.rpf", L"pack\\shared_minigame000\\shared_minigame000.rpf",
    L"pack\\core_restofworld\\core_restofworld.rpf", L"pack\\core_restofworld001\\core_restofworld001.rpf",
    L"packs.rpf", L"pack\\story000\\story000.rpf", L"levels.rpf",
};

constexpr double kReserveGb = 15;  // an export doesn't start with less than this free

std::wstring stem(const std::wstring& path) {
    std::wstring b = file_name(path);
    size_t dot = b.rfind(L'.');
    return dot == std::wstring::npos || dot == 0 ? b : b.substr(0, dot);
}

bool ends_with_ci(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// Every .ttpf under data\ttdb (os.walk), smallest first.
std::vector<std::wstring> texture_pools(const std::wstring& game) {
    std::vector<std::wstring> out, stack{join(game, L"data\\ttdb")};
    while (!stack.empty()) {
        const std::wstring dir = stack.back();
        stack.pop_back();
        for (const auto& f : list_dir(dir))
            if (ends_with_ci(f, L".ttpf")) out.push_back(join(dir, f));
        for (const auto& d : list_dir(dir, false, true)) stack.push_back(join(dir, d));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::wstring& a, const std::wstring& b) { return file_size(a) < file_size(b); });
    return out;
}

}  // namespace

static std::wstring rpf9dump_core(const std::wstring& root) {
    return join(root, L"tools\\rpf9-explorer\\Core\\RpfPreview.Core.csproj");
}

bool Preparation::assets(const Options& o) {
    const std::wstring root = repo_root();
    const std::wstring exe = join(o.work, L"tools\\rpf9dump\\rpf9dump.exe");
    auto step = [&](const char* text) {
        json::Value e = event("step", "assets");
        e.set("text", text);
        event_(e);
    };
    // A program's output goes to the log; false (and the error shown) if it fails.
    auto run = [&](const std::wstring& program, std::vector<std::wstring> args) {
        if (stop_) return false;
        Spawn s;
        s.exe = program;
        s.args = std::move(args);
        s.job = &job_;
        std::string cmd = "  " + narrow(file_name(program));
        for (const auto& a : s.args) cmd += " " + narrow(a);
        log_(cmd);
        const int code = run_lines(s, [&](const std::string& l) { log_(l); });
        if (stop_) return false;
        if (code != 0) return fail(narrow(file_name(program)) + " saiu com o código " + std::to_string(code) + ".");
        return true;
    };
    auto ensure_free = [&](const std::string& what) {
        const double free = free_gb(o.work);
        if (free >= 0 && free < kReserveGb) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.0f", free);
            return fail(std::string("Só ") + buf + " GB livres no disco de " + narrow(o.work) + ", parando antes de " +
                        what + ". O que já foi exportado fica: libere espaço (ou desligue o áudio) e prepare de novo "
                               "para continuar.");
        }
        return true;
    };

    step("Compilando o extrator (rpf9dump)");
    if (!is_file(exe)) {
        if (!is_file(rpf9dump_core(root)))
            return fail("tools\\rpf9-explorer\\Core está faltando ou incompleto: restaure-o do projeto.");
        const std::wstring dotnet = which(L"dotnet", path_);
        if (dotnet.empty()) return fail("dotnet não foi encontrado: instale o .NET 8 SDK.");
        note("assets: building rpf9dump");
        if (!run(dotnet, {L"build", join(root, L"tools\\rpf9dump\\rpf9dump.csproj"), L"-c", L"Release", L"-o",
                          parent(exe), L"--nologo", L"-v", L"q"}))
            return false;
    }

    const std::wstring out = join(o.work, L"assets");
    const auto pools = texture_pools(o.game);
    const size_t total = 2 * std::size(kModelArchives) + pools.size() + (o.no_audio ? 0 : 1);
    size_t done = 0;
    auto item = [&](const std::string& text) {
        json::Value e = event("progress", "assets");
        e.set("done", static_cast<uint64_t>(done));
        e.set("total", static_cast<uint64_t>(total));
        e.set("text", text);
        event_(e);
        ++done;
    };
    for (const wchar_t* rel : kModelArchives) {
        const std::wstring src = join(o.game, rel), name = stem(rel);
        for (const wchar_t* mode : {L"models", L"textures"}) {
            const bool models = !wcscmp(mode, L"models");
            item(std::string(models ? "Modelos" : "Texturas") + " de " + narrow(rel));
            if (is_file(join(join(join(out, name), mode), L"manifest.json"))) continue;
            if (!ensure_free(narrow(mode) + " de " + narrow(rel))) return false;
            note("assets: " + narrow(mode) + " from " + narrow(rel));
            if (!run(exe, {mode, src, out})) return false;
        }
    }
    for (const auto& p : pools) {
        const std::wstring name = stem(p);
        item("Pool de texturas " + narrow(name));
        if (is_file(join(join(join(out, L"ttpf"), name), L"manifest.json"))) continue;
        if (!ensure_free("o pool de texturas " + narrow(name))) return false;
        note("assets: texture pool " + narrow(p.substr(std::min(p.size(), o.game.size() + 1))));
        if (!run(exe, {L"ttpf", p, join(out, L"ttpf")})) return false;
    }
    if (o.no_audio) {
        note("assets: audio banks skipped (no audio)");
    } else if (!is_file(join(out, L"audio\\audio_manifest.json"))) {
        item("Bancos de áudio");
        if (!ensure_free("os bancos de áudio")) return false;
        note("assets: audio banks");
        if (!run(exe, {L"audio", join(o.game, L"audio.rpf"), out})) return false;
    }
    json::Value e = event("progress", "assets");
    e.set("done", static_cast<uint64_t>(total));
    e.set("total", static_cast<uint64_t>(total));
    event_(e);
    note("assets: done");
    return true;
}

}  // namespace fp::prep
