// The launcher's window (FlamingoPort.exe with no --run): finds the user's copy of the game, prepares the port with
// progress (prep/preparation.h), and starts the game in a second process of this same exe, turning into a splash until the
// game's first frame is on screen.
#pragma once

namespace fp::launcher {

int main(int argc, wchar_t** argv);

}  // namespace fp::launcher
