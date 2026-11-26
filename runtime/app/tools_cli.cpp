// The preparation's tools from a terminal (what the Python tools did as scripts), for running a step on its own and
// checking it against earlier outputs:
//
//   --prep carve <shaders.rpf> <out> | unpack <dxbc dir> | disasm <fssm dir> <out> [llvm-mc]
//         | assets <game> <work> [--no-audio]   (models, textures, texture pools, audio banks: tools\rpf9dump)
//   --xlat <.s or folder> [--target vulkan|d3d12] [--cs-regs DIR] [--out DIR] [--hashes FILE] [--limit N] [--jobs N]
//         the translator alone (rdna2spirv.py): .spvasm files and/or one line per shader "name<TAB>sha1<TAB>length"
//         ("name<TAB>ERR<TAB>reason" when it isn't translated)
//   --build-cache <disasm> <dxbc> <out> [--jobs N] [--limit N] [--timeout S] [--opt 0|1|3] [--serve PORT]
//         [--token T] [--keep DIR] [--defer-big] [--big-kb N] [--local-dxc N] [--sidecars-only]
//         the DXIL cache (dxil_cache.py); FLAMINGO_SERVE, FLAMINGO_TOKEN and FLAMINGO_LOCAL_DXC as defaults
//   --pending <cache> [--jobs N]                     the shaders the runtime saved at run time (pending_shaders.py)
//   --literal-samplers <.s or folder> --out FILE     the S#s shaders build from immediates (literal_samplers.py)
#include "app/cli.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cache/builder.h"
#include "cache/literal_samplers.h"
#include "cache/pending.h"
#include "core/fs.h"
#include "core/hash.h"
#include "core/system.h"
#include "core/json.h"
#include "core/text.h"
#include "prep/preparation.h"
#include "shaders/prep.h"
#include "xlat/translator.h"

namespace fp::app {
namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

void print(const std::string& l) {
    std::printf("%s\n", l.c_str());
    std::fflush(stdout);
}

// --name value / --flag
struct ArgList {
    std::vector<std::wstring> positional;
    std::vector<std::pair<std::wstring, std::wstring>> named;
    bool flag(const wchar_t* n) const {
        for (const auto& [k, v] : named)
            if (k == n) return true;
        return false;
    }
    std::wstring get(const wchar_t* n, const std::wstring& fallback = L"") const {
        for (const auto& [k, v] : named)
            if (k == n) return v;
        return fallback;
    }
    int num(const wchar_t* n, int fallback) const {
        std::wstring v = get(n);
        return v.empty() ? fallback : _wtoi(v.c_str());
    }
};

ArgList parse(int argc, wchar_t** argv, int from, std::initializer_list<const wchar_t*> flags) {
    ArgList a;
    for (int i = from; i < argc; ++i) {
        std::wstring s = argv[i];
        if (s.size() > 2 && s.compare(0, 2, L"--") == 0) {
            bool is_flag = false;
            for (const wchar_t* f : flags) is_flag = is_flag || s == f;
            if (is_flag || i + 1 >= argc)
                a.named.emplace_back(s, L"1");
            else
                a.named.emplace_back(s, argv[++i]);
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

int default_jobs() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return std::max(1, static_cast<int>(si.dwNumberOfProcessors) / 2);
}

std::vector<std::wstring> s_files(const std::wstring& src, int limit) {
    std::vector<std::wstring> files;
    if (src.size() > 2 && src.compare(src.size() - 2, 2, L".s") == 0) {
        files.push_back(src);
    } else {
        for (const auto& f : list_dir(src))
            if (f.size() > 2 && f.compare(f.size() - 2, 2, L".s") == 0) files.push_back(join(src, f));
    }
    if (limit > 0 && files.size() > static_cast<size_t>(limit)) files.resize(static_cast<size_t>(limit));
    return files;
}

std::wstring stem(const std::wstring& path) {
    std::wstring b = file_name(path);
    size_t dot = b.rfind(L'.');
    return dot == std::wstring::npos || dot == 0 ? b : b.substr(0, dot);
}

int prep_step(int argc, wchar_t** argv) {
    auto progress = [](size_t, size_t) {};
    const std::wstring step = argc > 2 ? argv[2] : L"";
    bool ok = false;
    if (step == L"carve" && argc > 4) {
        ok = shaders::carve(argv[3], argv[4], g_stop, progress, print);
    } else if (step == L"unpack" && argc > 3) {
        ok = shaders::unpack(argv[3], g_stop, progress, print);
    } else if (step == L"disasm" && argc > 4) {
        std::wstring llvm = argc > 5 ? argv[5] : which(L"llvm-mc", tool_path());
        ok = shaders::disassemble(argv[3], argv[4], llvm, L"gfx1013", 4, g_stop, progress, print);
    } else if (step == L"assets" && argc > 4) {
        // make_game.py's assets stage: rpf9dump over the copy, into <work>\assets
        prep::Options o;
        o.game = argv[3];
        o.work = argv[4];
        o.out = argv[4];
        for (int i = 5; i < argc; ++i) o.no_audio = o.no_audio || !wcscmp(argv[i], L"--no-audio");
        prep::Preparation p;
        std::atomic<bool> done{false};
        std::thread watch([&] {  // Ctrl+C stops the stage and the programs it runs
            while (!done && !g_stop) Sleep(200);
            if (g_stop) p.stop();
        });
        auto event = [](const json::Value& e) {
            if (e["event"].str() == "progress" && !e["text"].str().empty())
                print("[" + std::to_string(e["done"].integer() + 1) + "/" + std::to_string(e["total"].integer()) +
                      "] " + e["text"].str());
            else if (e["event"].str() == "error")
                print("ERROR: " + e["text"].str());
        };
        ok = p.run(o, {"assets"}, event, print);
        done = true;
        watch.join();
    } else {
        std::fprintf(stderr, "usage: --prep carve <rpf> <out> | unpack <dir> | disasm <fssm dir> <out> [llvm-mc] | "
                             "assets <game> <work> [--no-audio]\n");
        return 2;
    }
    return ok ? 0 : 1;
}

int xlat_cli(int argc, wchar_t** argv) {
    const ArgList a = parse(argc, argv, 2, {});
    if (a.positional.empty()) {
        std::fprintf(stderr, "usage: --xlat <.s or folder> [--target vulkan|d3d12] [--cs-regs DIR] [--out DIR] "
                             "[--hashes FILE] [--limit N] [--jobs N]\n");
        return 2;
    }
    xlat::Options opt;
    opt.target = a.get(L"--target", L"vulkan") == L"d3d12" ? xlat::Target::d3d12 : xlat::Target::vulkan;
    opt.cs_regs = a.get(L"--cs-regs");
    xlat::Context ctx(opt);
    const auto files = s_files(a.positional[0], a.num(L"--limit", 0));
    const std::wstring out = a.get(L"--out"), hashes = a.get(L"--hashes");
    if (!out.empty()) make_dirs(out);
    std::vector<std::string> lines(files.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; !g_stop && (i = next++) < files.size();) {
            const std::string name = narrow(stem(files[i]));
            try {
                const std::string text = xlat::translate(files[i], ctx);
                if (!out.empty()) write_file(join(out, stem(files[i]) + L".spvasm"), text.data(), text.size());
                lines[i] = name + "\t" + sha1_hex(text) + "\t" + std::to_string(text.size());
            } catch (const xlat::Unsupported& e) {
                lines[i] = name + "\tERR\t" + e.what();
            } catch (const xlat::PyError& e) {
                const std::string t = e.what();
                const bool caught = t == "IndexError" || t == "KeyError" || t == "ValueError";
                lines[i] = name + "\tERR\t" + (caught ? "translator error " : "crash ") + t;
            } catch (const std::exception& e) {
                lines[i] = name + "\tERR\tcrash " + e.what();
            }
        }
    };
    std::vector<std::thread> threads;
    const int jobs = a.num(L"--jobs", default_jobs());
    for (int i = 0; i < jobs; ++i) threads.emplace_back(work);
    for (auto& t : threads) t.join();
    std::string text;
    size_t translated = 0;
    for (const auto& l : lines) {
        text += l + "\n";
        translated += l.find("\tERR\t") == std::string::npos;
    }
    if (!hashes.empty()) write_file(hashes, text.data(), text.size());
    print(std::to_string(files.size()) + " shaders: " + std::to_string(translated) + " translated");
    return 0;
}

int build_cli(int argc, wchar_t** argv) {
    const ArgList a = parse(argc, argv, 2, {L"--defer-big", L"--sidecars-only"});
    if (a.positional.size() < 3) {
        std::fprintf(stderr, "usage: --build-cache <disasm> <dxbc> <out> [--jobs N] [--limit N] [--timeout S] "
                             "[--opt 0|1|3] [--serve PORT] [--token T] [--keep DIR] [--defer-big] [--big-kb N] "
                             "[--local-dxc N] [--sidecars-only]\n");
        return 2;
    }
    cache::BuildOptions o;
    o.disasm = a.positional[0];
    o.dxbc = a.positional[1];
    o.out = a.positional[2];
    o.jobs = a.num(L"--jobs", default_jobs());
    o.limit = a.num(L"--limit", 0);
    o.timeout = a.num(L"--timeout", 900);
    o.opt = narrow(a.get(L"--opt", L"0"));
    o.serve = a.num(L"--serve", _wtoi(env(L"FLAMINGO_SERVE").c_str()));
    o.token = narrow(a.get(L"--token", env(L"FLAMINGO_TOKEN")));
    o.keep = a.get(L"--keep");
    o.defer_big = a.flag(L"--defer-big");
    o.big_kb = a.num(L"--big-kb", 2500);
    const std::wstring local = env(L"FLAMINGO_LOCAL_DXC");
    o.local_dxc = a.num(L"--local-dxc", local.empty() ? -1 : _wtoi(local.c_str()));
    o.sidecars_only = a.flag(L"--sidecars-only");
    Job job;
    o.job = &job;
    return cache::build(o, g_stop, nullptr, print);
}

int pending_cli(int argc, wchar_t** argv) {
    const ArgList a = parse(argc, argv, 2, {});
    if (a.positional.empty()) {
        std::fprintf(stderr, "usage: --pending <cache> [--jobs N]\n");
        return 2;
    }
    Job job;
    cache::BuildOptions o;
    o.jobs = a.num(L"--jobs", default_jobs());
    o.serve = _wtoi(env(L"FLAMINGO_SERVE").c_str());
    o.token = narrow(env(L"FLAMINGO_TOKEN"));
    o.job = &job;
    return cache::translate_pending(a.positional[0], o, g_stop, nullptr, print);
}

int literal_cli(int argc, wchar_t** argv) {
    const ArgList a = parse(argc, argv, 2, {});
    if (a.positional.empty() || a.get(L"--out").empty()) {
        std::fprintf(stderr, "usage: --literal-samplers <.s or folder> --out FILE\n");
        return 2;
    }
    return cache::write_literal_samplers(a.positional[0], a.get(L"--out"), print) ? 0 : 1;
}

}  // namespace

int tools_main(int argc, wchar_t** argv) {
    if (argc < 2) return -1;
    const std::wstring cmd = argv[1];
    int (*fn)(int, wchar_t**) = cmd == L"--prep"               ? prep_step
                                : cmd == L"--xlat"             ? xlat_cli
                                : cmd == L"--build-cache"      ? build_cli
                                : cmd == L"--pending"          ? pending_cli
                                : cmd == L"--literal-samplers" ? literal_cli
                                                               : nullptr;
    if (!fn) return -1;
    attach_output();
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    return fn(argc, argv);
}

}  // namespace fp::app
