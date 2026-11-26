// The preparation's stages: checking the copy, the shader stage (carve, unpack, disassemble, translate) and the
// heavy shaders left for after the port is playable.
#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "cache/literal_samplers.h"
#include "core/fs.h"
#include "core/hash.h"
#include "core/process.h"
#include "core/system.h"
#include "core/text.h"
#include "prep/preparation.h"
#include "shaders/prep.h"

namespace fp::prep {

bool Preparation::check(const Options& o) {
    if (!is_dir(o.game)) return fail("Pasta do jogo não encontrada: " + narrow(o.game));
    const json::Value& v = o.version;
    if (v.is_null()) return fail("Esta pasta não é uma versão suportada do jogo.");
    const auto& files = v["files"].members;
    const std::string title = v["title"].str() + " " + v["version"].str();
    note("check: " + title + ", " + std::to_string(files.size()) + " files");
    json::Value step = event("step", "check");
    step.set("text", title + ": conferindo " + std::to_string(files.size()) + " arquivos");
    event_(step);
    int bad = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        if (stop_) return false;
        if ((i + 1) % 50 == 0 || i + 1 == files.size()) {
            json::Value p = event("progress", "check");
            p.set("done", static_cast<uint64_t>(i + 1));
            p.set("total", static_cast<uint64_t>(files.size()));
            event_(p);
        }
        const std::wstring path = join(o.game, widen(files[i].first));
        int64_t size = file_size(path);
        if (size < 0) {
            log_("  missing  " + files[i].first);
            ++bad;
        } else if (size != files[i].second["size"].integer(-2)) {
            log_("  wrong size  " + files[i].first);
            ++bad;
        }
    }
    if (bad)
        return fail(std::to_string(bad) + " arquivo(s) faltando ou diferentes. Use uma cópia completa e sem "
                    "alterações da versão " + v["version"].str() + ".");
    if (o.full_check) {
        json::Value s = event("step", "check");
        s.set("text", "Comparando SHA-256 de todos os arquivos (lento)");
        event_(s);
        for (size_t i = 0; i < files.size(); ++i) {
            json::Value p = event("progress", "check");
            p.set("done", static_cast<uint64_t>(i));
            p.set("total", static_cast<uint64_t>(files.size()));
            p.set("text", "SHA-256");
            event_(p);
            const std::string& want = files[i].second["sha256"].str();
            if (want.empty()) continue;
            std::string have;
            if (!sha256_file(join(o.game, widen(files[i].first)), have, &stop_)) {
                if (stop_) return false;
                have = "?";
            }
            if (have != want) {
                log_("CHANGED   " + files[i].first);
                ++bad;
            }
        }
        if (bad) return fail(std::to_string(bad) + " arquivo(s) com conteúdo diferente da versão " +
                             v["version"].str() + ": a cópia está incompleta ou danificada.");
    }
    const std::wstring exe = join(o.game, widen(v["expected_exe"].str()));
    note(std::string("check: all files present") + (o.full_check ? " and hashes match" : "") + ". " +
         (is_file(exe) ? "Executable found." : v["expected_exe"].str() + " is not in the folder: nothing to run yet."));
    return true;
}

bool Preparation::shaders(const Options& o) {
    const std::wstring sh = join(o.work, L"shaders"), dxbc = join(sh, L"dxbc");
    auto step = [&](const char* text) {
        json::Value e = event("step", "shaders");
        e.set("text", text);
        event_(e);
    };
    auto progress = [&](const char* text) {
        return [this, text](size_t done, size_t total) {
            json::Value e = event("progress", "shaders");
            e.set("done", static_cast<uint64_t>(done));
            e.set("total", static_cast<uint64_t>(std::max<size_t>(total, 1)));
            e.set("text", text);
            event_(e);
        };
    };
    auto log = [this](const std::string& l) { note("shaders: " + l); };
    // The translator expects <dir>/<name>.dxbc next to <dir>/fssm/ and <dir>/disasm/.
    if (!is_file(join(dxbc, L"index.json"))) {
        const std::string want = o.version["files"]["shaders.rpf"]["sha256"].str();
        if (!want.empty()) {
            note("checking shaders.rpf (SHA-256)");
            step("Conferindo o shaders.rpf (SHA-256)");
            std::string have;
            if (!sha256_file(join(o.game, L"shaders.rpf"), have, &stop_)) return stop_ ? false : fail("Não deu para ler o shaders.rpf.");
            if (have != want)
                return fail("O shaders.rpf tem o tamanho certo mas conteúdo diferente da versão " +
                            o.version["version"].str() + ": a cópia está incompleta ou danificada. Copie de novo.");
        }
        note("shaders: carving shaders.rpf");
        step("Extraindo os shaders do shaders.rpf");
        if (!shaders::carve(join(o.game, L"shaders.rpf"), dxbc, stop_, progress("Extraindo os shaders"), log))
            return stop_ ? false : fail("A extração dos shaders do shaders.rpf falhou.");
    }
    // fssm_index.json is written last: without it an interrupted unpack starts over.
    if (!is_file(join(dxbc, L"fssm_index.json"))) {
        note("shaders: unpacking native code");
        step("Descompactando o código nativo");
        if (!shaders::unpack(dxbc, stop_, progress("Descompactando o código nativo"), log))
            return stop_ ? false : fail("A descompactação do código nativo falhou.");
    }
    const std::wstring disasm = join(dxbc, L"disasm");
    if (!is_dir(disasm) || count_dir(disasm, 1000) < 1000) {
        std::wstring llvm = env(L"LLVM_MC");
        llvm = which(llvm.empty() ? L"llvm-mc" : llvm, path_);
        if (llvm.empty()) return fail("llvm-mc não foi encontrado. Instale o LLVM 21+ com o alvo AMDGPU ou defina LLVM_MC.");
        note("shaders: disassembling (gfx1013)");
        step("Desmontando o código RDNA2");
        if (!shaders::disassemble(join(dxbc, L"fssm"), disasm, llvm, L"gfx1013", o.jobs, stop_,
                                  progress("Desmontando o código RDNA2"), log))
            return stop_ ? false : fail("A desmontagem do código RDNA2 falhou.");
    }
    // The cache the runtime reads: <out>/shaders/<digest>.dxil. Each shader goes RDNA2 -> SPIR-V -> HLSL -> DXIL (the
    // longest step); an interrupted build continues. The largest shaders are left to the heavy stage.
    const std::wstring cache = join(o.out, L"shaders");
    if (!is_file(join(cache, L"index.json"))) {
        note("shaders: translating to DXIL (the longest step)");
        step("Traduzindo os shaders para DXIL");
        if (!translate(o, disasm, dxbc, cache, true)) return false;
    }
    // Shaders the runtime met at run time and saved (not in shaders.rpf): translated into the same cache.
    if (is_dir(join(cache, L"pending"))) {
        note("shaders: translating shaders found while running");
        step("Traduzindo shaders encontrados durante o jogo");
        if (!translate(o, L"", L"", cache, false, true)) return false;
    }
    const std::wstring literal = join(sh, L"literal_samplers.txt");
    if (!is_file(literal)) {
        note("shaders: listing samplers built from immediates");
        step("Listando samplers");
        if (!cache::write_literal_samplers(disasm, literal, log)) return fail("A lista de samplers não foi gravada.");
    }
    note("shaders: done");
    return true;
}

bool Preparation::heavy(const Options& o) {
    const std::wstring dxbc = join(o.work, L"shaders\\dxbc");
    note("heavy: the largest shaders");
    json::Value e = event("step", "heavy");
    e.set("text", "Traduzindo os shaders pesados (ray tracing)");
    event_(e);
    if (!translate(o, join(dxbc, L"disasm"), dxbc, join(o.out, L"shaders"), false)) return false;
    note("heavy: done");
    return true;
}
// The pipelines the game has asked for (<cache>\pipelines.txt, written by the runtime, a line a combination) made by
// the driver ahead of the game: FlamingoPort.exe --warm-pipelines in a process of its own (a device of its own), which
// prints "warm: <done> of <total> (...)" every 5 s. The driver keeps what it made, so pipelines made before pass in
// milliseconds and only new ones cost (2-100 s each); after the shaders change, all of them are made again. The list
// exists once the game has run, so the first preparation has nothing to do here. Never while the game runs (they'd
// fight for the GPU and the CPU); a failure here doesn't stop the preparation (the game makes what's missing).
bool Preparation::pipelines(const Options& o) {
    if (stop_) return false;
    if (o.game_running && o.game_running->load()) {
        note("pipelines: skipped while the game runs");
        return true;
    }
    const std::wstring cache = join(o.out, L"shaders");
    json::Value e = event("step", "pipelines");
    e.set("text", "Compilando os pipelines no driver");
    event_(e);
    note("pipelines: making the ones the game has asked for");
    Spawn s;
    s.exe = self_path();
    s.args = {L"--warm-pipelines", L"--jobs", std::to_wstring(std::max(1, o.jobs))};
    s.env = {{L"RT_SHADER_CACHE", cache}};
    s.job = &job_;
    int64_t total = -1;
    const int code = run_lines(s, [&](const std::string& line) {
        log_("pipelines: " + line);
        if (line.rfind("warm: ", 0) != 0) return;
        // "warm: <M> pipelines in ..." first, then "warm: <done> of <M> (...)"
        long long a = 0, b = 0;
        if (sscanf_s(line.c_str(), "warm: %lld of %lld", &a, &b) == 2) {
            json::Value p = event("progress", "pipelines");
            p.set("done", static_cast<int64_t>(a));
            p.set("total", static_cast<int64_t>(std::max(1LL, b)));
            p.set("text", "Compilando pipelines");
            event_(p);
        } else if (sscanf_s(line.c_str(), "warm: %lld pipelines", &a) == 1) {
            total = a;
        }
    });
    if (stop_) return false;
    if (code < 0) {
        note("pipelines: the warm-up couldn't start");
    } else if (code != 0) {
        note("pipelines: the warm-up ended with code " + std::to_string(code) + " (the game makes them as it goes)");
    } else if (total == 0) {
        note("pipelines: none listed yet (the list is written while the game runs)");
    }
    return true;
}

}  // namespace fp::prep
