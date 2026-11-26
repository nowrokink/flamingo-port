// The runtime's command line: native runtime for the Xbox Series build.
//
//   FlamingoPort.exe --dry-run <image> [--game-dir DIR]
//       Maps the image and its bundled modules, applies relocations, binds every import and prints a report.
//       No code from the image runs.
//
//   FlamingoPort.exe --run [--trace-host] [--probe RVA,RVA...] <image> [--game-dir DIR]
//       Loads as above, then runs it: DllMain of the bundled modules, then the exe's entry point. Unimplemented
//       imports log once and return 0; an unhandled exception prints where it happened and which unimplemented
//       imports were called. For finding the next thing to implement, not for playing. --trace-host logs the first
//       call to each Windows function and shim the game imports. --probe logs the first time each exe RVA (hex)
//       is reached, with its register arguments and caller.
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <vector>
#include <map>
#include <string>

#include "pe_loader.h"
#include "run.h"
#include "../shims/uma.h"
#include "../shims/d3d12x.h"

namespace {

const char* binding_name(rt::Binding b) {
    switch (b) {
        case rt::Binding::Shim: return "shim";
        case rt::Binding::Host: return "host";
        case rt::Binding::Module: return "module";
        default: return "UNIMPLEMENTED";
    }
}

int usage() {
    std::fprintf(stderr, "usage: FlamingoPort.exe --dry-run|--run [--trace-host] [--probe RVA,...] <image> [--game-dir DIR]\n");
    return 2;
}

}  // namespace

// The command line above: app/entry.cpp calls it for --run / --dry-run.
int rt_cli_main(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"--pso-time")) return rt::d3d12x::pso_time(argc, argv);
    if (argc > 1 && !wcscmp(argv[1], L"--warm-pipelines")) return rt::d3d12x::warm_pipelines(argc, argv);
    std::filesystem::path image, game_dir;
    bool dry_run = false, run = false, trace_host = false;
    std::vector<uint32_t> probes;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--dry-run") dry_run = true;
        else if (a == L"--run") run = true;
        else if (a == L"--trace-host") trace_host = true;
        else if (a == L"--probe" && i + 1 < argc) {
            for (const wchar_t* p = argv[++i]; *p;) {
                wchar_t* end = nullptr;
                unsigned long v = std::wcstoul(p, &end, 16);
                if (end == p) return usage();
                probes.push_back(static_cast<uint32_t>(v));
                p = *end == L',' ? end + 1 : end;
            }
        }
        else if (a == L"--game-dir" && i + 1 < argc) game_dir = argv[++i];
        else if (image.empty()) image = a;
        else return usage();
    }
    if (image.empty() || dry_run == run) return usage();
    if (game_dir.empty()) game_dir = image.parent_path();
    if (run && rt::uma::size() < (192ull << 30)) {
        // Something Windows placed at process start (by ASLR) is in the way of the GPU-visible range the title needs;
        // a new process gets a new layout. Up to 3 times.
        wchar_t tries[8] = {};
        GetEnvironmentVariableW(L"RT_RELAUNCH", tries, 8);
        int n = tries[0] ? tries[0] - L'0' : 0;
        if (n < 3) {
            std::fprintf(stderr, "[rt] the GPU-visible range came out small (%llu GiB): starting again\n",
                         static_cast<unsigned long long>(rt::uma::size() >> 30));
            wchar_t next[2] = {static_cast<wchar_t>(L'1' + n), 0};
            SetEnvironmentVariableW(L"RT_RELAUNCH", next);
            STARTUPINFOW si{sizeof si};
            PROCESS_INFORMATION pi{};
            std::wstring cmd = GetCommandLineW();
            // the new process ends with this one (a job closed when this process goes)
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si,
                               &pi)) {
                if (job) AssignProcessToJobObject(job, pi.hProcess);
                ResumeThread(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                return static_cast<int>(code);
            }
        }
    }

    try {
        rt::Loader loader(game_dir);
        loader.set_trace_host(trace_host);
        rt::Module& exe = loader.load(image);
        if (run) return rt::run(loader, exe, game_dir, probes);
        int unimplemented = 0;
        for (auto* m : loader.modules()) {
            std::map<rt::Binding, int> counts;
            for (auto& r : m->imports) counts[r.binding]++;
            std::printf("%-36s base=%p%s size=%u entry=+%#x  shim=%d host=%d module=%d unimplemented=%d\n",
                        m->name.c_str(), m->base, m->relocated ? " (relocated)" : "", m->size, m->entry_rva,
                        counts[rt::Binding::Shim], counts[rt::Binding::Host], counts[rt::Binding::Module],
                        counts[rt::Binding::Unimplemented]);
            for (auto& r : m->imports)
                if (r.binding == rt::Binding::Unimplemented || r.binding == rt::Binding::Shim) {
                    std::printf("    %-13s %s!%s\n", binding_name(r.binding), r.dll.c_str(), r.name.c_str());
                    unimplemented += r.binding == rt::Binding::Unimplemented;
                }
        }
        std::printf("%zu module(s) mapped, %d unimplemented import(s)\n", loader.modules().size(), unimplemented);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
