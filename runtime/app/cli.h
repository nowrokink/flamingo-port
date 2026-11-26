// The program's terminal side: output for a Windows-subsystem program, and the preparation's tools as commands.
#pragma once

namespace fp::app {

// A GUI-subsystem program gets the C runtime's stdio bound to inherited (redirected) handles; with none, the console
// of the terminal it was started from, if any.
void attach_output();

// --prep / --xlat / --build-cache / --pending / --literal-samplers (tools_cli.cpp): the exit code, or -1 when the
// command line names none of them.
int tools_main(int argc, wchar_t** argv);

}  // namespace fp::app
