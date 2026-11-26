// The window frames are presented in (window.cpp).
#pragma once
#include <windows.h>

namespace rt::window {

// The window, made the first time with a client area of `width` x `height` (later calls return the same one).
HWND get(int width, int height);

// Called after every presented frame. The first time it signals the event named by RT_SPLASH_EVENT, if set: the
// launcher (FlamingoPort.exe) shows a splash until the game's first frame is on screen.
void frame_shown();

// Whether the window lives inside the launcher's (RT_PARENT_WINDOW): a child window there, shown at the first frame.
bool embedded();

// Messages a child window sends its launcher (the launcher's ui/theme.h has the same numbers).
constexpr UINT WM_GAME_SHOWN = WM_APP + 3;       // lParam: the game's window, now on screen
constexpr UINT WM_GAME_FULLSCREEN = WM_APP + 2;  // Alt+Enter or F11 in the game: the launcher toggles full screen

}  // namespace rt::window
