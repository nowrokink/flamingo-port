// FlamingoPort.exe: the launcher, the runtime and the preparation's tools in one Windows-subsystem program (no
// console window of its own).
//
//   FlamingoPort.exe [--prepare | --play]           the launcher (ui/); --prepare starts preparing at once, --play
//                                                   starts the game (splash first) when the port is ready
//   FlamingoPort.exe --run|--dry-run ...            the runtime (src/main.cpp)
//   FlamingoPort.exe --prep|--xlat|--build-cache|--pending|--literal-samplers ...   the tools (app/tools_cli.cpp)
//
// Run from a terminal, output goes where it is redirected; not redirected, to the terminal's console (attached), and
// with neither, nowhere: the launcher starts the game with stdout and stderr in logs\game.log.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "app/cli.h"
#include "ui/launcher.h"

int rt_cli_main(int argc, wchar_t** argv);  // src/main.cpp

namespace fp::app {
namespace {
bool usable(HANDLE h) { return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN; }
}  // namespace

void attach_output() {
    const bool out = usable(GetStdHandle(STD_OUTPUT_HANDLE)), err = usable(GetStdHandle(STD_ERROR_HANDLE));
    if (out && err) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f = nullptr;
    if (!out) freopen_s(&f, "CONOUT$", "w", stdout);
    if (!err) freopen_s(&f, "CONOUT$", "w", stderr);
}

}  // namespace fp::app

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // the C runtime's own split of the command line (shell32's CommandLineToArgvW would load shell32 here)
    int argc = __argc;
    wchar_t** argv = __wargv;
    int code = fp::app::tools_main(argc, argv);
    if (code >= 0) return code;
    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--run") || !wcscmp(argv[i], L"--dry-run") || !wcscmp(argv[i], L"--pso-time") ||
            !wcscmp(argv[i], L"--warm-pipelines")) {
            fp::app::attach_output();
            return rt_cli_main(argc, argv);
        }
    return fp::launcher::main(argc, argv);
}
