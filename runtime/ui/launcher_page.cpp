// The launcher's page: what the state says (kicker, headline, percentage, the window's title), the header (the cover
// art, the title, the tools at the top right), the column of panels (launcher_panels.cpp) following the window's
// width, the scrollbar, and the side panel over it all (launcher_drawer.cpp).
#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "core/fs.h"
#include "core/text.h"
#include "ui/app.h"

namespace fp::launcher {

void App::paint() {
    if (!g_.begin()) return;
    hits_.clear();
    actions_.clear();
    RECT cr;
    GetClientRect(hwnd_, &cr);
    const float sc = g_.scale();
    const float width = cr.right / sc;
    view_h_ = cr.bottom / sc;
    g_.clear(BG);

    hit_dy_ = -scroll_;
    g_.translate(0, -scroll_);
    const Summary s = summarize();
    paint_header(width);
    // the column: at most CONTENT_MAX wide, centred; narrower windows get a narrower margin
    const float outer = std::min(width, CONTENT_MAX), pad = width < 640 ? 16.f : 28.f;
    const float x = (width - outer) / 2 + pad, cw = outer - 2 * pad;
    float y = COVER_H + 8;
    y += paint_status(x, y, cw, s) + 16;
    y += paint_folders(x, y, cw) + 16;
    y += paint_log(x, y, cw) + 16;
    y += g_.text(x, y, L"Offline · nada é gravado na pasta do jogo", T(12), FAINT, cw, Align::center);
    content_h_ = y + 24;
    g_.translate(0, scroll_);
    hit_dy_ = 0;
    paint_scrollbar(cr);
    if (drawer_ != Drawer::none) paint_drawer(width, view_h_);
    g_.end();
}

App::Summary App::summarize() {
    const Mode m = mode_;
    const bool background = m == Mode::running && current_ == HEAVY;
    const double o = overall();
    int n_stage = 1;
    for (int i = 0; i < kStageCount; ++i)
        if (current_ == kStages[i].key) n_stage = i + 1;
    Summary s{m, background, 0, {}, {}, {}, std::to_wstring(kStageCount), TEXT};
    s.shown = (m == Mode::ready || background) ? 100 : std::min(99, static_cast<int>(o * 100));
    const std::wstring copy = version_.is_null() ? L"" : L"Cópia " + w(version_["version"].str()) + L" · ";
    if (background) {
        s.ready_card = true;
        s.kicker = L"Pronto para jogar · shaders pesados em segundo plano";
        s.kicker_colour = GREEN_T;
        s.kicker_check = true;
        s.headline = L"Port pronto para jogar";
        const int64_t total = heavy_["total"].integer(), left = std::max<int64_t>(0, total - heavy_["done"].integer());
        s.subline = total ? L"Traduzindo " + num(left) + L" shaders pesados (ray tracing) em segundo plano" +
                                (eta_ > 0 ? L" · cerca de " + w(format_duration(eta_)) : L"")
                          : L"Traduzindo os shaders pesados (ray tracing) em segundo plano";
    } else if (m == Mode::running) {
        const std::string cur = current_.empty() ? "check" : current_;
        s.kicker = L"Etapa " + std::to_wstring(n_stage) + L" de " + s.stages_n;
        for (const auto& st : kStages)
            if (cur == st.key) s.headline = st.long_name;
        std::wstring sub = (cur != "shaders" || eta_ < 0) ? w(step_text_)
                                                          : L"Cerca de " + w(format_duration(eta_)) + L" restantes";
        s.subline = (sub.empty() ? L"" : sub + L" · ") + L"você pode minimizar a janela";
    } else if (m == Mode::paused) {
        s.kicker = L"Etapa " + std::to_wstring(n_stage) + L" de " + s.stages_n + L" · pausada";
        s.kicker_colour = WARN;
        s.headline = L"Preparação pausada";
        s.subline = L"O que já foi feito está guardado. Continue quando quiser.";
    } else if (m == Mode::error) {
        s.kicker = L"Etapa " + std::to_wstring(n_stage) + L" de " + s.stages_n;
        s.kicker_colour = RED;
        s.headline = L"A preparação parou";
        s.head_colour = RED;
        s.subline = w(error_.substr(0, error_.find('\n')).substr(0, 220));
    } else if (m == Mode::ready) {
        s.ready_card = true;
        s.kicker = L"Tudo pronto";
        s.kicker_colour = GREEN_T;
        s.kicker_check = true;
        s.headline = L"Port pronto para jogar";
        s.subline = copy + L"shaders traduzidos · o jogo roda deste mesmo programa";
    } else {
        s.kicker = s.stages_n + L" etapas";
        s.headline = L"Pronto para preparar";
        s.subline = L"Verificar a cópia e traduzir os shaders. Na primeira vez leva algumas horas; depois, só o que "
                    L"mudou.";
    }
    const json::Value& wt = watching();
    if (!wt.is_null()) {  // a translation started elsewhere: its numbers, whatever the mode
        const int64_t done = wt["done"].integer(), total = std::max<int64_t>(1, wt["total"].integer(1));
        s.ready_card = false;
        s.kicker = L"Tradução em andamento · iniciada fora do launcher";
        s.kicker_colour = LABEL;
        s.kicker_check = false;
        s.headline = L"Traduzindo os shaders";
        s.head_colour = TEXT;
        std::wstring parts = num(done) + L" de " + num(total);
        if (wt["rate"].num() > 0) {
            wchar_t buf[48];
            swprintf_s(buf, L" · %.1f por segundo", wt["rate"].num());
            for (wchar_t* c = buf; *c; ++c)
                if (*c == L'.') *c = L',';
            parts += buf;
        }
        if (wt["eta"].num() > 0) parts += L" · cerca de " + w(format_duration(wt["eta"].num()));
        const size_t nh = helpers_.size();
        parts += nh == 1 ? L" · 1 ajudante conectado"
                         : nh ? L" · " + std::to_wstring(nh) + L" ajudantes conectados" : L" · nenhum ajudante conectado";
        s.subline = parts;
        s.shown = static_cast<int>(std::min<int64_t>(99, done * 100 / total));
    }
    // the design writes these in upper case through CSS
    if (!s.kicker.empty()) {
        const std::wstring src = s.kicker;
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, src.c_str(), static_cast<int>(src.size()),
                      s.kicker.data(), static_cast<int>(s.kicker.size()), nullptr, nullptr, 0);
    }
    SetWindowTextW(hwnd_, (!wt.is_null() || (m == Mode::running && !background))
                              ? (std::to_wstring(s.shown) + L"% · " + TITLE).c_str()
                              : TITLE);
    return s;
}

// The header: the cover (the design's scene, drawn for this project; app/flamingo.rc), the tools at the top right,
// the title at the bottom left and the copy's status at the bottom right.
void App::paint_header(float width) {
    if (ID2D1Bitmap* art = g_.image(IDR_COVER)) {
        // as the SVG's "xMidYMax slice": as wide as the window (never smaller), the bottom edge kept
        const D2D1_SIZE_F px = art->GetPixelSize().width ? D2D1::SizeF(static_cast<float>(art->GetPixelSize().width),
                                                                       static_cast<float>(art->GetPixelSize().height))
                                                         : art->GetSize();
        const float scale = std::max(width / 1280.f, 1.f), dw = 1280 * scale, dh = COVER_H * scale;
        g_.push_clip(0, 0, width, COVER_H);
        g_.bitmap(art, D2D1::RectF((width - dw) / 2, COVER_H - dh, (width + dw) / 2, COVER_H),
                  D2D1::RectF(0, 0, px.width, px.height));
        g_.pop_clip();
    } else {
        g_.fade(0, 0, width, COVER_H, 0x4b1a63, 1.f, 0.f);
    }

    // the tools: monitoring (with how many computers take part) and settings
    const float by = 16, bh = 40;
    float bx = width - 24;
    const std::wstring settings = L"Configurações";
    bx -= button_width(settings, Kind::header, 13, Icon::gear);
    button(OPEN_CONFIG, bx, by, settings, Kind::header, bh, 13, Icon::gear);
    bx -= 8;
    const std::wstring monitor = L"Monitoramento", badge = std::to_wstring(1 + helpers_.size());
    const Font bf = T(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const ui::Size bs = g_.measure(badge, bf);
    const float badge_w = std::max(18.f, bs.w + 10);
    const float mw = button_width(monitor, Kind::header, 13, Icon::pulse) + 8 + badge_w;
    bx -= mw;
    const bool over = hover_ == OPEN_MONITOR;
    g_.rrect(bx, by, bx + mw, by + bh, 10, HDR_FILL, HDR_EDGE, true, false, over ? 0.85f : 0.6f);
    g_.rrect(bx, by, bx + mw, by + bh, 10, 0, HDR_EDGE, false, true, 0.55f);
    icon(Icon::pulse, bx + 14, by + 12, 16, TEXT);
    const Font mf = T(13, DWRITE_FONT_WEIGHT_MEDIUM);
    const ui::Size ms = g_.measure(monitor, mf);
    g_.text(bx + 14 + 16 + 8, by + (bh - ms.h) / 2, monitor, mf, TEXT);
    const float bx0 = bx + 14 + 16 + 8 + ms.w + 8;
    g_.rrect(bx0, by + 11, bx0 + badge_w, by + 29, 9, BADGE);
    g_.text(bx0, by + 20 - bs.h / 2, badge, bf, SUBTLE, badge_w, Align::center);
    hit(OPEN_MONITOR, bx, by, bx + mw, by + bh);
    actions_[OPEN_MONITOR] = [this] { open_drawer(Drawer::monitor); };
    actions_[OPEN_CONFIG] = [this] { open_drawer(Drawer::config); };

    // the title, bottom left: FLAMINGO //PORT and what it is
    const float big = width < 480 ? 30.f : 40.f;
    const Font tf = H(big, DWRITE_FONT_WEIGHT_BOLD, big * 0.14f), pf = H(big * 0.45f, DWRITE_FONT_WEIGHT_SEMI_BOLD, big * 0.045f);
    const std::wstring sub = L"Xbox Series → PC · runtime nativo";
    const ui::Size ts = g_.measure(L"FLAMINGO", tf), ss = g_.measure(sub, T(13)), ps = g_.measure(L"//PORT", pf);
    const float ty = COVER_H - 20 - ss.h - 4 - ts.h;
    g_.text(28, ty, L"FLAMINGO", tf, 0xffffff);
    g_.text(28 + ts.w + 12 - big * 0.14f, ty + (ts.h - ps.h) / 2 + 2, L"//PORT", pf, PINK);
    g_.text(28, COVER_H - 20 - ss.h, sub, T(13), HDR_TEXT);

    // the copy's status, bottom right (when there's room beside the title)
    std::wstring pill;
    uint32_t fg, fill, edge;
    if (!version_.is_null()) {
        pill = L"Cópia " + w(version_["version"].str()) + L" reconhecida";
        fg = GREEN_T, fill = PILL_BG, edge = PILL_EDGE;
    } else if (!game_.empty() && !searching_) {
        pill = L"Pasta não reconhecida";
        fg = WARN_V, fill = WARN_BG, edge = WARN_EDGE;
    } else {
        pill = L"Procurando a sua cópia…";
        fg = MUTED, fill = CARD, edge = EDGE;
    }
    const Font pillf = T(13, DWRITE_FONT_WEIGHT_MEDIUM);
    const ui::Size pls = g_.measure(pill, pillf);
    const float check = version_.is_null() ? 0.f : 22.f, pw = pls.w + 24 + check;
    if (28 + std::max(ts.w + 12 + ps.w, ss.w) + 24 + pw <= width - 28) {
        const float x1 = width - 28, y1 = COVER_H - 20, y0 = y1 - (pls.h + 12);
        g_.rrect(x1 - pw, y0, x1, y1, (y1 - y0) / 2, fill, edge, true, true);
        if (check) icon(Icon::check, x1 - pw + 12, y0 + (y1 - y0 - 14) / 2, 14, fg);
        g_.text(x1 - pw + 12 + check, y0 + 6, pill, pillf, fg);
    }
}

void App::open_drawer(Drawer d) {
    drawer_ = d;
    drawer_scroll_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The scrollbar, when the window is shorter than the page.
void App::paint_scrollbar(const RECT& cr) {
    const float sc = g_.scale();
    scrollbar_ = content_h_ > view_h_ + 2;
    if (scrollbar_) {
        scroll_ = std::clamp(scroll_, 0.f, content_h_ - view_h_);
        const float bx = cr.right / sc - 10;
        const float thumb = std::max(24.f, view_h_ * view_h_ / content_h_);
        const float t0 = scroll_ / std::max(1.f, content_h_ - view_h_) * (view_h_ - thumb);
        g_.rrect(bx + 2, t0 + 2, bx + 8, t0 + thumb - 2, 3, BTN_EDGE);
    } else {
        scroll_ = 0;
    }
}

}  // namespace fp::launcher
