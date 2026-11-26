// The launcher's look and vocabulary: the palette and type of the "Flamingo — pronto" design, the window's sizes, the
// stages it shows, the clickable things and the modes it can be in.
//
// The design's fonts are Inter (text), Space Grotesk (titles and numbers) and JetBrains Mono (paths and the log);
// Windows' own stand in for them, so nothing is embedded: Segoe UI, Bahnschrift and Consolas.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

#include "core/text.h"
#include "ui/canvas.h"

namespace fp::launcher {

using ui::Align;
using ui::Font;

// ---- colours
constexpr uint32_t BG = 0x140b20, CARD = 0x1d1129, EDGE = 0x2e1f42, TILE = 0x251636, LOGBG = 0x110918;
constexpr uint32_t TEXT = 0xf3eefa, MUTED = 0xb9aacc, FAINT = 0xa495b8, DIM = 0x7d6e92, SUBTLE = 0xd9cfe6;
constexpr uint32_t LABEL = 0xc4a6ff, VIOLET = 0x9b6bff, TRACK = 0x33224a, PINK = 0xff8fb3;
constexpr uint32_t GREEN = 0x5fd394, GREEN_T = 0x7fe0a8, PILL_BG = 0x1f2a22, PILL_EDGE = 0x2f5a3e;
constexpr uint32_t WARN_BG = 0x2d2014, WARN_EDGE = 0x5c4220, WARN = 0xf5c07a, WARN_V = 0xffd59c;
constexpr uint32_t DANGER_EDGE = 0x6a2c3e, DANGER = 0xff9fb3, BTN_EDGE = 0x45305f, BTN_HOVER = 0x2c1c40;
constexpr uint32_t OFF_BG = 0x2f2240, OFF_FG = 0x8e7fa3, RED = 0xff7088, PATH = 0xd9cfe6, LOGTEXT = 0xcfc3de;
constexpr uint32_t PLAY = 0xff6f9f, PLAY_HOVER = 0xff86b0, PLAY_TEXT = 0x1a0b22, LINK_HOVER = 0xe2d4ff;
constexpr uint32_t DRAWER_BG = 0x1a0f26, DRAWER_EDGE = 0x3a2752, SELECTED = 0x2a1840, SWITCH_OFF = 0x3a2752;
constexpr uint32_t HDR_FILL = 0x140b20, HDR_EDGE = 0x5a5066, HDR_TEXT = 0xe6dcf2, BADGE = 0x33224a;
constexpr uint32_t SCRIM = 0x08040e;

// ---- type (sizes in the design's CSS pixels: the canvas takes points, 3/4 of a pixel)
inline const wchar_t* const HEAD = L"Bahnschrift";
inline const wchar_t* const BODY = L"Segoe UI";
inline const wchar_t* const MONO = L"Consolas";
inline Font T(float px, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, float spacing = 0) {
    return {BODY, px * 0.75f, w, false, spacing};
}
inline Font H(float px, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_SEMI_BOLD, float spacing = 0) {
    return {HEAD, px * 0.75f, w, false, spacing};
}
inline Font M(float px) { return {MONO, px * 0.75f}; }
// The design's section labels: 12 px, semibold, upper case, 0.08 em apart.
inline Font CAPS() { return T(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12 * 0.08f); }
// Older point sizes (the splash).
inline Font head(float pt) { return {HEAD, pt, DWRITE_FONT_WEIGHT_SEMI_BOLD}; }
inline Font body(float pt, bool bold = false) { return {BODY, pt, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL}; }
inline Font mono(float pt, bool bold = false) { return {MONO, pt, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL}; }

// ---- sizes (DIPs)
constexpr float COVER_H = 200;       // the header with the cover art
constexpr float CONTENT_MAX = 1100;  // the page's column, padding included
constexpr float DRAWER_W = 440;      // the side panel (settings, monitoring)
constexpr float MIN_W = 600, MIN_H = 480;
constexpr float START_W = 1280, START_H = 880;  // the design's size: the first window, if the screen has room
inline const wchar_t* const TITLE = L"Flamingo Port";

constexpr UINT WM_RUN = WM_APP + 1;
// From the game's window inside ours (runtime/shims/window.h has the same numbers).
constexpr UINT WM_GAME_FULLSCREEN = WM_APP + 2;  // Alt+Enter or F11 pressed in the game
constexpr UINT WM_GAME_SHOWN = WM_APP + 3;       // lParam: the game's window, on screen with its first frame

// The stages the port needs and their share of the whole; "heavy" (the largest shaders, nearly all ray tracing)
// runs after them while the game can already be played.
struct Stage {
    const char* key;
    const wchar_t* short_name;
    const wchar_t* long_name;
    float weight;
};
inline const Stage kStages[] = {{"check", L"Verificar cópia", L"Verificando a cópia", 4},
                                {"shaders", L"Traduzir shaders", L"Traduzindo os shaders para DXIL", 80},
                                {"pipelines", L"Compilar pipelines", L"Compilando os pipelines no driver", 16}};
constexpr int kStageCount = 3;
inline const char* const HEAVY = "heavy";

enum Id {
    NONE,
    PRIMARY,
    PAUSE,
    CANCEL,
    LOG_TOGGLE,
    PICK_GAME,
    FIND_GAME,
    OPEN_OUT,
    PICK_OUT,
    OPT_FULL_CHECK,
    OPT_HELPERS,
    OPT_LOCAL,
    QUALITY_FHD,
    QUALITY_4K,
    HELPER_FOLDER,
    EXPLAIN,
    OPEN_MONITOR,
    OPEN_CONFIG,
    DRAWER_CLOSE,  // the dimmed page around the side panel, and its close button
    DRAWER_BODY,   // the panel itself: takes clicks that hit nothing in it
};

enum class Kind { play, primary, outline, filled, danger, link, header };
enum class Icon { none, play, pulse, gear, terminal, refresh, close, check };
enum class Mode { idle, running, paused, error, ready };
enum class Drawer { none, config, monitor };

inline std::wstring w(const std::string& s) { return widen(s); }

inline std::wstring num(int64_t n) { return w(format_thousands(n)); }

}  // namespace fp::launcher
