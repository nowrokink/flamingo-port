// The launcher's drawing helpers (cards measured before their background is drawn, the design's buttons and icons,
// switches, choice cards, the stage bar) and what a click on one of them does.
#include <algorithm>
#include <cmath>

#include "ui/app.h"

namespace fp::launcher {

void App::hit(int id, float x0, float y0, float x1, float y1) {
    if (g_.measuring) return;
    const float top = std::max(y0 + hit_dy_, hit_top_), bottom = y1 + hit_dy_;
    if (bottom > top) hits_.push_back({id, x0, top, x1, bottom});
}

float App::card(float x, float y, float w, uint32_t fill, uint32_t edge, float radius, float padx, float pady,
                const std::function<float(float, float, float)>& body) {
    const bool was = g_.measuring;
    g_.measuring = true;
    const float h = body(x + padx, y + pady, w - 2 * padx) + 2 * pady;
    g_.measuring = was;
    g_.rrect(x, y, x + w, y + h, radius, fill, edge, true, edge != fill);
    body(x + padx, y + pady, w - 2 * padx);
    return h;
}

namespace {

Font button_font(Kind kind, float px) {
    if (kind == Kind::play) return T(px, DWRITE_FONT_WEIGHT_BOLD);
    if (kind == Kind::primary) return T(px, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    return T(px, DWRITE_FONT_WEIGHT_MEDIUM);
}

float pad(Kind kind) {
    switch (kind) {
        case Kind::play: return 36;
        case Kind::primary: return 28;
        case Kind::header: return 14;
        case Kind::link: return 10;
        default: return 16;
    }
}

}  // namespace

float App::button_width(const std::wstring& text, Kind kind, float px, Icon ic) {
    const float tw = text.empty() ? 0 : g_.measure(text, button_font(kind, px)).w;
    const float iw = ic == Icon::none ? 0 : (ic == Icon::play ? 18.f : 16.f) + (text.empty() ? 0 : 8);
    return tw + iw + 2 * pad(kind);
}

float App::button(int id, float x, float y, const std::wstring& text, Kind kind, float h, float px, Icon ic,
                  bool enabled) {
    const Font f = button_font(kind, px);
    const float bw = button_width(text, kind, px, ic);
    const bool hover = enabled && hover_ == id;
    uint32_t fg = TEXT;
    switch (kind) {
        case Kind::play:
        case Kind::primary:
            if (enabled) {
                g_.rrect(x, y, x + bw, y + h, kind == Kind::play ? 14.f : 12.f, hover ? PLAY_HOVER : PLAY);
                fg = PLAY_TEXT;
            } else {
                g_.rrect(x, y, x + bw, y + h, kind == Kind::play ? 14.f : 12.f, OFF_BG);
                fg = OFF_FG;
            }
            break;
        case Kind::outline:
            g_.rrect(x, y, x + bw, y + h, 10, BTN_HOVER, BTN_EDGE, hover, true);
            if (!enabled) fg = OFF_FG;
            break;
        case Kind::filled:
            g_.rrect(x, y, x + bw, y + h, 10, hover ? BTN_HOVER : TILE, BTN_EDGE, true, true);
            break;
        case Kind::danger:
            g_.rrect(x, y, x + bw, y + h, 10, 0x34172a, DANGER_EDGE, hover, true);
            fg = DANGER;
            break;
        case Kind::header:
            // the design's glass buttons over the cover: the page's colour at 60 %, 85 % under the pointer
            g_.rrect(x, y, x + bw, y + h, 10, HDR_FILL, HDR_EDGE, true, false, hover ? 0.85f : 0.6f);
            g_.rrect(x, y, x + bw, y + h, 10, 0, HDR_EDGE, false, true, 0.55f);
            break;
        case Kind::link:
            fg = !enabled ? OFF_FG : hover ? LINK_HOVER : LABEL;
            break;
    }
    const float th = text.empty() ? 0 : g_.measure(text, f).h;
    float cx = x + pad(kind);
    if (ic != Icon::none) {
        const float is = ic == Icon::play ? 18.f : 16.f;
        icon(ic, cx, y + (h - is) / 2, is, fg);
        cx += is + 8;
    }
    if (!text.empty()) g_.text(cx, y + (h - th) / 2, text, f, fg);
    if (enabled) hit(id, x, y, x + bw, y + h);
    return bw;
}

// The design's icons (Feather-style strokes on a 24-unit box), drawn at `size` DIPs from (x, y).
void App::icon(Icon i, float x, float y, float size, uint32_t rgb) {
    const float s = size / 24.f, sw = std::max(1.2f, 2.f * s);
    auto P = [&](float px, float py) { return ui::Point{x + px * s, y + py * s}; };
    switch (i) {
        case Icon::none:
            break;
        case Icon::play: {
            const ui::Point t[] = {P(6, 4), P(20, 12), P(6, 20)};
            g_.polygon(t, 3, rgb);
            break;
        }
        case Icon::pulse: {
            const ui::Point p[] = {P(3, 12), P(7, 12), P(10, 20), P(14, 4), P(17, 12), P(21, 12)};
            g_.polyline(p, 6, rgb, sw);
            break;
        }
        case Icon::gear: {
            // a cog: the hub, the wheel and eight teeth
            g_.ellipse_edge(x + 12 * s, y + 12 * s, 3 * s, 3 * s, rgb, sw);
            g_.ellipse_edge(x + 12 * s, y + 12 * s, 6.5f * s, 6.5f * s, rgb, sw);
            for (int k = 0; k < 8; ++k) {
                const float a = k * 3.14159265f / 4;
                const ui::Point t[] = {P(12 + 6.5f * std::cos(a), 12 + 6.5f * std::sin(a)),
                                       P(12 + 9.5f * std::cos(a), 12 + 9.5f * std::sin(a))};
                g_.polyline(t, 2, rgb, sw * 1.3f);
            }
            break;
        }
        case Icon::terminal: {
            const ui::Point a[] = {P(4, 17), P(10, 11), P(4, 5)};
            const ui::Point b[] = {P(12, 19), P(20, 19)};
            g_.polyline(a, 3, rgb, sw);
            g_.polyline(b, 2, rgb, sw);
            break;
        }
        case Icon::refresh: {
            // most of a circle (from the top left, clockwise round to the left) and its arrowhead
            ui::Point arc[25];
            for (int k = 0; k <= 24; ++k) {
                const float a = 3.14159265f * (1.25f - 1.6f * k / 24.f);
                arc[k] = P(12 + 9 * std::cos(a), 12 - 9 * std::sin(a));
            }
            g_.polyline(arc, 25, rgb, sw);
            const ui::Point head[] = {P(3, 3), P(3, 8), P(8, 8)};
            g_.polyline(head, 3, rgb, sw);
            break;
        }
        case Icon::close: {
            const ui::Point a[] = {P(18, 6), P(6, 18)};
            const ui::Point b[] = {P(6, 6), P(18, 18)};
            g_.polyline(a, 2, rgb, sw);
            g_.polyline(b, 2, rgb, sw);
            break;
        }
        case Icon::check: {
            const ui::Point p[] = {P(20, 6), P(9, 17), P(4, 12)};
            g_.polyline(p, 3, rgb, std::max(1.5f, 2.5f * s));
            break;
        }
    }
}

float App::section(float x, float y, const std::wstring& caps, uint32_t rgb) {
    return g_.text(x, y, caps, CAPS(), rgb);
}

float App::switch_row(int id, float x, float y, float w, bool on, const std::wstring& label, const std::wstring& hint) {
    const float tw = 40, th = 24, pady = 12, gap = 14;
    const float text_w = std::max(40.f, w - tw - gap);
    float h = pady;
    h += g_.text(x, y + h, label, T(14, DWRITE_FONT_WEIGHT_MEDIUM), TEXT, text_w, Align::left, true);
    h += 3;
    h += g_.text(x, y + h, hint, T(12), MUTED, text_w, Align::left, true);
    h += pady;
    // the switch: a pink track with the knob right when on
    const float sx = x + w - tw, sy = y + pady + 2;
    g_.rrect(sx, sy, sx + tw, sy + th, th / 2, on ? PLAY : (hover_ == id ? BTN_EDGE : SWITCH_OFF));
    g_.ellipse(sx + (on ? 19.f : 3.f) + 9, sy + 3 + 9, 9, 9, 0xffffff);
    g_.rect(x, y + h - 1, x + w, y + h, EDGE);
    hit(id, x, y, x + w, y + h);
    return h;
}

float App::choice(int id, float x, float y, float w, bool on, const std::wstring& title, const std::wstring& sub) {
    const float h = card(x, y, w, on ? SELECTED : LOGBG, on ? PLAY : (hover_ == id ? BTN_EDGE : EDGE), 12, 14, 14,
                         [&](float ix, float iy, float iw) {
                             float th = g_.text(ix, iy, title, T(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), TEXT, iw);
                             th += 4;
                             th += g_.text(ix, iy + th, sub, T(12), MUTED, iw, Align::left, true);
                             return th;
                         });
    if (on) g_.rrect(x + 1, y + 1, x + w - 1, y + h - 1, 11, 0, PLAY, false, true);  // the design's 2 px border
    hit(id, x, y, x + w, y + h);
    return h;
}

std::pair<std::string, double> App::stage_view(const std::string& key) const {
    const json::Value& wt = watching();
    if (!wt.is_null()) {
        // a translation started elsewhere: the copy was checked, the shaders are where its file says, the rest waits
        // (this window's own states say "done" for a port that was ready when it opened)
        if (key == "check") return {"done", 1.0};
        if (key == "shaders")
            return {"running", std::clamp(wt["done"].num() / std::max(1.0, wt["total"].num(1)), 0.0, 1.0)};
        return {"idle", 0.0};
    }
    auto st = state_.find(key);
    auto fr = frac_.find(key);
    return {st == state_.end() ? "idle" : st->second,
            fr == frac_.end() ? 0.0 : std::clamp(fr->second, 0.0, 1.0)};
}

double App::overall() const {
    double total = 0, done = 0;
    for (const auto& st : kStages) {
        total += st.weight;
        auto it = frac_.find(st.key);
        done += st.weight * (it == frac_.end() ? 0 : it->second);
    }
    return done / total;
}

int64_t App::missing_count() const {
    return stats_.members.empty() ? 0 : stats_["failed"].integer() + stats_["skipped"].integer();
}

// The one bar, a segment a stage (sized by its weight): done green, the running one filling violet.
void App::segbar(float x, float y, float w, float h) {
    const float gap = 4, r = h / 2;
    float total = 0;
    for (const auto& st : kStages) total += st.weight;
    const float usable = w - gap * (kStageCount - 1);
    float at = x;
    for (int i = 0; i < kStageCount; ++i) {
        const float x0 = at, x1 = at + usable * kStages[i].weight / total;
        at = x1 + gap;
        const auto [st, f] = stage_view(kStages[i].key);
        g_.rrect(x0, y, x1, y + h, r, st == "done" ? GREEN : TRACK);
        if ((st == "running" || st == "paused" || st == "error") && f > 0) {
            const uint32_t c = st == "running" ? VIOLET : st == "paused" ? WARN : RED;
            g_.rrect(x0, y, x0 + std::max<float>(h, static_cast<float>((x1 - x0) * f)), y + h, r, c);
        } else if (st == "running") {
            const float span = x1 - x0, band = std::min(120.f, span / 3);
            const float b0 = x0 + (phase_ / 400.f) * (span + band) - band;
            g_.push_clip(x0, y, x1, y + h);
            g_.rrect(std::max(x0, b0), y, std::min(x1, b0 + band), y + h, r, VIOLET);
            g_.pop_clip();
        }
    }
}

void App::click(int id) {
    auto it = actions_.find(id);
    if (it != actions_.end()) {
        auto fn = it->second;  // the action may repaint and replace the map
        if (fn) fn();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace fp::launcher
