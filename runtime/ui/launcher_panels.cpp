// The page's panels: the status card (the "pronto" form: play first, with the numbers as chips; or the progress form:
// stage, percentage, the stage bar, four tiles, the actions), the two folder cards, and the log. Everything wraps as
// the design's flex rows do when the column is narrow.
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <vector>

#include "core/text.h"
#include "ui/app.h"

namespace fp::launcher {

namespace {

// Lays items of the given widths in rows of `w` (gap between them), as flex-wrap does; returns each one's (row, x).
struct Flow {
    std::vector<int> row;
    std::vector<float> x;
    int rows = 0;
};
Flow flow(const std::vector<float>& widths, float w, float gap) {
    Flow f;
    float at = 0;
    int r = 0;
    for (float iw : widths) {
        if (at > 0 && at + iw > w) {
            ++r;
            at = 0;
        }
        f.row.push_back(r);
        f.x.push_back(at);
        at += iw + gap;
    }
    f.rows = widths.empty() ? 0 : r + 1;
    return f;
}

}  // namespace

float App::paint_status(float x, float y, float w, const Summary& s) {
    const float pad = w < 520 ? 20.f : 28.f;
    return card(x, y, w, CARD, EDGE, 16, pad, pad, [&](float ix, float iy, float iw) {
        return s.ready_card ? paint_ready(ix, iy, iw, s) : paint_progress(ix, iy, iw, s);
    });
}

// The kicker, the headline and the line under it; returns their height.
static float headline(App&, ui::Canvas& g, float x, float y, float w, const App::Summary& s, float big,
                      const std::function<void(float, float, float, uint32_t)>& check) {
    const Font kf = CAPS();
    float h = 0;
    float kx = x;
    if (s.kicker_check) {
        const float kh = g.measure(L"X", kf).h;
        check(x, y + (kh - 14) / 2, 14, s.kicker_colour);
        kx += 20;
    }
    h += g.text(kx, y, s.kicker, kf, s.kicker_colour, std::max(20.f, w - (kx - x)), Align::left, true);
    h += 6;
    h += g.text(x, y + h, s.headline, H(big), s.head_colour, w, Align::left, true);
    h += 6;
    h += g.text(x, y + h, s.subline, T(14), MUTED, w, Align::left, true);
    return h;
}

// The numbers as chips (shaders, from the cache, without a translation, waiting for the game), wrapping in `w`.
float App::paint_chips(float x, float y, float w) {
    const json::Value& st = stats_;
    if (st.members.empty()) return 0;
    struct Chip {
        std::wstring cap, val;
        bool warn;
    };
    std::vector<Chip> chips{{L"Shaders", num(st["total"].integer()), false},
                            {L"Do cache", num(st["cached"].integer()), false}};
    const int64_t bad = missing_count();
    if (bad) chips.push_back({L"Sem tradução ›", num(bad), true});
    if (st["unmet"].integer() > 0) chips.push_back({L"Aguardam o jogo", num(st["unmet"].integer()), false});
    std::vector<float> widths;
    for (const auto& c : chips)
        widths.push_back(std::max(110.f, std::max(g_.measure(c.cap, T(12)).w, g_.measure(c.val, H(18)).w) + 28));
    const Flow f = flow(widths, w, 10);
    const float ch = 10 + g_.measure(L"X", T(12)).h + 2 + g_.measure(L"0", H(18)).h + 10;
    for (size_t i = 0; i < chips.size(); ++i) {
        const float cx = x + f.x[i], cy = y + f.row[i] * (ch + 10);
        const Chip& c = chips[i];
        if (c.warn) {
            g_.rrect(cx, cy, cx + widths[i], cy + ch, 12, hover_ == EXPLAIN ? 0x3a2a18 : WARN_BG, WARN_EDGE, true, true);
            hit(EXPLAIN, cx, cy, cx + widths[i], cy + ch);
        } else {
            g_.rrect(cx, cy, cx + widths[i], cy + ch, 12, TILE);
        }
        const float th = g_.text(cx + 14, cy + 10, c.cap, T(12), c.warn ? WARN : MUTED);
        g_.text(cx + 14, cy + 10 + th + 2, c.val, H(18), c.warn ? WARN_V : TEXT);
    }
    actions_[EXPLAIN] = [this] { open_drawer(Drawer::monitor); };
    return f.rows * ch + (f.rows - 1) * 10;
}

// "Pronto": the state and the numbers on top, then play (and preparing again) under a line.
float App::paint_ready(float x, float y, float w, const Summary& s) {
    auto check = [this](float cx, float cy, float size, uint32_t c) { icon(Icon::check, cx, cy, size, c); };
    // the numbers beside the text when both fit (the text keeps at least 260), else under it
    // the chips' width in one row (each at least 110 wide; paint_chips wraps them if the guess is short)
    float chips_w = 0;
    if (!stats_.members.empty()) {
        const int n = 2 + (missing_count() ? 1 : 0) + (stats_["unmet"].integer() > 0 ? 1 : 0);
        chips_w = n * 120.f + (n - 1) * 10;
    }
    float h;
    if (chips_w > 0 && 260 + 16 + chips_w <= w) {
        const float tw = w - chips_w - 16;
        const float lh = headline(*this, g_, x, y, tw, s, 30, check);
        const float chh = paint_chips(x + tw + 16, y, chips_w);
        h = std::max(lh, chh);
    } else {
        h = headline(*this, g_, x, y, w, s, 30, check);
        if (chips_w > 0) h += 16 + paint_chips(x, y + h + 16, w);
    }
    if (s.background) {
        // the heavy shaders' progress, slim
        const int64_t total = heavy_["total"].integer(), done = heavy_["done"].integer();
        const float by = y + h + 16;
        g_.rrect(x, by, x + w, by + 6, 3, TRACK);
        if (total > 0)
            g_.rrect(x, by, x + std::max(6.f, w * static_cast<float>(done) / static_cast<float>(total)), by + 6, 3,
                     VIOLET);
        h += 16 + 6;
    }
    h += 22;
    g_.rect(x, y + h, x + w, y + h + 1, EDGE);
    h += 1 + 22;
    h += paint_actions(x, y + h, w, s);
    return h;
}

// The progress form: the stage and the big percentage, the bar a segment a stage, the tiles, the actions.
float App::paint_progress(float x, float y, float w, const Summary& s) {
    auto check = [this](float cx, float cy, float size, uint32_t c) { icon(Icon::check, cx, cy, size, c); };
    const bool show_pct = s.m != Mode::idle || !watching().is_null();
    const std::wstring pn = std::to_wstring(s.shown);
    const Font big = H(56, DWRITE_FONT_WEIGHT_BOLD), pct = H(24);
    const ui::Size bs = g_.measure(pn, big), ps = g_.measure(L"%", pct);
    const float pct_w = show_pct ? bs.w + ps.w + 4 : 0;
    float h;
    if (pct_w && 240 + 16 + pct_w <= w) {
        const float lh = headline(*this, g_, x, y, w - pct_w - 16, s, 28, check);
        const float block = std::max(lh, bs.h);
        g_.text(x + w - pct_w, y + block - bs.h, pn, big, TEXT);
        g_.text(x + w - ps.w, y + block - ps.h - 6, L"%", pct, MUTED);
        h = block;
    } else {
        h = headline(*this, g_, x, y, w, s, 28, check);
        if (pct_w) {
            g_.text(x, y + h + 8, pn, big, TEXT);
            g_.text(x + bs.w + 4, y + h + 8 + bs.h - ps.h - 6, L"%", pct, MUTED);
            h += 8 + bs.h;
        }
    }
    h += 24;
    segbar(x, y + h, w, 10);
    h += 10 + 10;
    h += paint_stage_labels(x, y + h, w) + 24;
    h += paint_tiles(x, y + h, w, s) + 20;
    g_.rect(x, y + h, x + w, y + h + 1, EDGE);
    h += 1 + 20;
    h += paint_actions(x, y + h, w, s);
    return h;
}

// The stage names under their segments of the bar; returns their height.
float App::paint_stage_labels(float x, float yy, float cw) {
    float total_w = 0;
    for (const auto& st : kStages) total_w += st.weight;
    float sx = x, label_h = 0, free_x = x;  // free_x: where the last label ended (labels never overlap)
    for (int i = 0; i < kStageCount; ++i) {
        const auto [st, f] = stage_view(kStages[i].key);
        const float seg_w = (cw - 4 * (kStageCount - 1)) * kStages[i].weight / total_w;
        std::wstring extra;
        uint32_t colour = FAINT;
        Font nf = T(13);
        if (st == "done") {
            colour = GREEN_T;
        } else if (st == "running" || st == "paused" || st == "error") {
            colour = TEXT;
            nf = T(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            extra = L" · " + std::to_wstring(static_cast<int>(f * 100 + 0.5)) + L"%";
        }
        const float name_w = g_.measure(kStages[i].short_name, nf).w, extra_w = g_.measure(extra, T(13)).w;
        const float item_w = 20 + name_w + extra_w;
        const float ix = (i == kStageCount - 1) ? std::max(free_x, x + cw - item_w) : std::max(sx, free_x);
        free_x = ix + item_w + 16;
        const float th = g_.measure(kStages[i].short_name, nf).h;
        if (st == "done")
            icon(Icon::check, ix, yy + (th - 14) / 2, 14, GREEN_T);
        else if (st == "running" || st == "paused" || st == "error")
            g_.ellipse(ix + 7, yy + th / 2, 4, 4, st == "running" ? VIOLET : st == "paused" ? WARN : RED);
        g_.text(ix + 20, yy, kStages[i].short_name, nf, colour);
        g_.text(ix + 20 + name_w, yy, extra, T(13), MUTED);
        label_h = std::max(label_h, th);
        sx += seg_w + 4;
    }
    return label_h;
}

// The four tiles (processed, from the cache, without a translation, time left): one row, or 2x2 when narrow.
float App::paint_tiles(float x, float yy, float cw, const Summary& sum) {
    const Mode m = sum.m;
    const json::Value& wt = watching();
    const json::Value& s = stats_;
    const bool has = !s.members.empty();
    const int64_t bad = missing_count();
    const int cols = cw >= 560 ? 4 : 2;
    const float gap = 12, tw = (cw - (cols - 1) * gap) / cols;
    float row_y = yy, row_h = 0;
    for (int t = 0; t < 4; ++t) {
        if (t && t % cols == 0) {
            row_y += row_h + gap;
            row_h = 0;
        }
        const float tx = x + (t % cols) * (tw + gap);
        const bool warn = t == 2 && bad;
        std::wstring cap, val, after;
        if (t == 0) {
            cap = L"Processados";
            val = has ? num(s["done"].integer()) : L"—";
            after = has ? L"/ " + num(s["total"].integer()) : L"";
        } else if (t == 1) {
            cap = L"Do cache";
            val = has ? num(s["cached"].integer()) : L"—";
        } else if (t == 2) {
            cap = L"Sem tradução";
            val = has ? num(bad) : L"—";
        } else {
            cap = L"Tempo restante";
            if (!wt.is_null() && wt["eta"].num() > 0)
                val = widen(format_duration(wt["eta"].num()));
            else if (m == Mode::running && (current_ == "shaders" || current_ == HEAVY))
                val = widen(format_duration(eta_));
            else
                val = m == Mode::ready ? L"pronto" : L"—";
        }
        const float h = card(tx, row_y, tw, warn ? WARN_BG : TILE, warn ? WARN_EDGE : TILE, 12, 16, 14,
                             [&](float ix, float iy, float iw) {
                                 float hh = g_.text(ix, iy, cap, T(12), warn ? WARN : MUTED, iw);
                                 hh += 4;
                                 const ui::Size vs = g_.measure(val, H(20));
                                 g_.text(ix, iy + hh, val, H(20), warn ? WARN_V : TEXT);
                                 if (!after.empty()) {
                                     const float ah = g_.measure(after, T(13, DWRITE_FONT_WEIGHT_MEDIUM)).h;
                                     g_.text(ix + vs.w + 6, iy + hh + vs.h - ah - 2, after,
                                             T(13, DWRITE_FONT_WEIGHT_MEDIUM), FAINT, std::max(10.f, iw - vs.w - 6));
                                 }
                                 if (t == 2 && bad) {
                                     Font lf = T(12);
                                     lf.underline = true;
                                     const ui::Size ls = g_.measure(L"ver quais", lf);
                                     const float lx = ix + iw - ls.w, ly = iy + hh + vs.h - ls.h - 3;
                                     g_.text(lx, ly, L"ver quais", lf, WARN);
                                     hit(EXPLAIN, lx, ly, lx + ls.w, ly + ls.h);
                                 }
                                 float total = hh + vs.h;
                                 // compute shaders waiting for the game: not failures, shown apart from the count
                                 if (t == 2 && has && s["unmet"].integer() > 0)
                                     total += 2 + g_.text(ix, iy + total + 2,
                                                          num(s["unmet"].integer()) + L" aguardam o jogo", T(12),
                                                          MUTED, iw);
                                 return total;
                             });
        row_h = std::max(row_h, h);
    }
    actions_[EXPLAIN] = [this] { open_drawer(Drawer::monitor); };
    return row_y + row_h - yy;
}

// The actions: the main button (play, prepare, continue), what it needs said beside it, and pause / cancel /
// prepare again at the right. Rows wrap when the column is narrow. Returns their height.
float App::paint_actions(float x, float yy, float cw, const Summary& s) {
    const Mode m = s.m;
    const json::Value& wt = watching();
    std::wstring ptext, hint, hint2;
    bool penabled = !version_.is_null();
    Kind pkind = Kind::primary;
    Icon picon = Icon::none;
    std::function<void()> paction;
    struct Side {
        int id;
        std::wstring text;
        Kind kind;
        Icon icon;
        std::function<void()> act;
    };
    std::vector<Side> side;
    const bool playing = game_proc_ != nullptr;
    auto pause = Side{PAUSE, L"Pausar", Kind::outline, Icon::none, [this] { stop_preparation("pause"); }};
    auto cancel = Side{CANCEL, L"Cancelar", Kind::danger, Icon::none, [this] { stop_preparation("cancel"); }};
    if (s.background) {
        pkind = Kind::play, picon = Icon::play;
        ptext = playing ? L"Jogando…" : L"Jogar";
        penabled = !playing && !version_.is_null();
        paction = [this] { play(); };
        const int64_t total = heavy_["total"].integer();
        hint = L"Abre nesta janela · F11 para tela cheia";
        hint2 = total ? L"Pesados: " + num(heavy_["done"].integer()) + L" de " + num(total) : L"Pesados: começando…";
        side = {pause, cancel};
    } else if (m == Mode::running) {
        pkind = Kind::play, picon = Icon::play;
        ptext = L"Jogar";
        penabled = false;
        hint = L"Fica disponível quando as " + s.stages_n + L" etapas terminarem.";
        side = {pause, cancel};
    } else if (m == Mode::ready) {
        pkind = Kind::play, picon = Icon::play;
        ptext = !playing ? L"Jogar" : game_closing_ ? L"Encerrando…" : L"Jogando…";
        penabled = !playing && !version_.is_null();
        paction = [this] { play(); };
        hint = game_closing_ ? L"O jogo fechou; ele termina de devolver a memória" : L"Abre nesta janela · F11 para tela cheia";
        hint2 = L"Registro do jogo em logs\\game.log";
        side = {{PAUSE, L"Preparar de novo", Kind::outline, Icon::refresh, [this] { prepare(); }}};
        if (pipelines_new_ > 0 && !playing)  // what the last game session asked for, made ahead for the next one
            side.insert(side.begin(), {CANCEL, L"Compilar " + num(pipelines_new_) + L" pipelines novos", Kind::filled,
                                       Icon::none, [this] { prepare({"pipelines"}); }});
    } else {
        ptext = m == Mode::paused ? L"Continuar" : m == Mode::error ? L"Tentar de novo" : L"Preparar";
        paction = [this] { prepare(); };
        hint = version_.is_null() ? L"Escolha a pasta da sua cópia do jogo abaixo." : L"";
    }
    if (!wt.is_null()) {
        // A second preparation would fight the running one for the cache and the helpers' port, and the game can't
        // start either: the cache is half old, half new (shaders that may not link with each other).
        penabled = false;
        hint = L"A tradução está em andamento fora do launcher; o Jogar volta quando ela terminar.";
        hint2.clear();
        side.clear();
    }
    actions_[PRIMARY] = paction;
    for (const auto& b : side) actions_[b.id] = b.act;

    const float ph = pkind == Kind::play && penabled ? 56.f : 48.f, sh = 44, gap = 16;
    const float pw = button_width(ptext, pkind, pkind == Kind::play ? 16.f : 15.f, picon);
    float side_w = 0;
    for (const auto& b : side) side_w += button_width(b.text, b.kind, 14, b.icon) + 12;
    if (side_w) side_w -= 12;
    // the hint takes what's left between (at least 200), else it goes under the button; the side buttons go to a
    // row of their own when they don't fit either
    const bool side_inline = pw + gap + 200 + gap + side_w <= cw;
    const float hint_x = x + pw + gap;
    const float hint_w = side_inline ? cw - pw - gap - (side_w ? side_w + gap : 0) : cw - pw - gap;
    const bool hint_inline = hint_w >= 160;
    button(PRIMARY, x, yy, ptext, pkind, ph, pkind == Kind::play ? 16.f : 15.f, picon, penabled && paction != nullptr);
    float h = ph;
    auto draw_hint = [&](float hx, float hy, float hw, float box_h) {
        const float h1 = hint.empty() ? 0 : g_.measure(hint, T(13), hw).h;
        const float h2 = hint2.empty() ? 0 : g_.measure(hint2, T(12), hw).h;
        const float total = h1 + (h2 ? 2 + h2 : 0);
        float ty = box_h > 0 ? hy + (box_h - total) / 2 : hy;
        if (!hint.empty()) ty += g_.text(hx, ty, hint, T(13), hint2.empty() ? FAINT : SUBTLE, hw, Align::left, true);
        if (!hint2.empty()) g_.text(hx, ty + 2, hint2, T(12), FAINT, hw, Align::left, true);
        return total;
    };
    if (hint_inline) {
        draw_hint(hint_x, yy, hint_w, ph);
    } else if (!hint.empty()) {
        h += 10 + draw_hint(x, yy + h + 10, cw, 0);
    }
    if (!side.empty()) {
        float bx, by;
        if (side_inline) {
            bx = x + cw - side_w;
            by = yy + (ph - sh) / 2;
        } else {
            bx = x;
            by = yy + h + 12;
            h += 12 + sh;
        }
        for (const auto& b : side) {
            if (!side_inline && bx > x && bx + button_width(b.text, b.kind, 14, b.icon) > x + cw) {  // wrap again
                bx = x;
                by += sh + 8;
                h += sh + 8;
            }
            bx += button(b.id, bx, by, b.text, b.kind, sh, 14, b.icon) + 12;
        }
    }
    return h;
}

// The game's copy and the port's folder: side by side, or one above the other when the column is narrow.
float App::paint_folders(float x, float y, float w) {
    const bool two = w >= 2 * 320 + 16;
    const float cw = two ? (w - 16) / 2 : w;
    auto content = [&](int which, float ix, float iy, float iw) {
        // buttons at the right, the text in what's left
        std::vector<std::tuple<int, std::wstring, Kind>> buttons;
        if (which == 0) {
            if (version_.is_null() && !searching_) buttons.emplace_back(FIND_GAME, L"Procurar", Kind::link);
            buttons.emplace_back(PICK_GAME, L"Trocar", Kind::outline);
        } else {
            buttons.emplace_back(PICK_OUT, L"Alterar", Kind::link);
            buttons.emplace_back(OPEN_OUT, L"Abrir", Kind::outline);
        }
        float bw_total = 0;
        for (const auto& [id, t, k] : buttons) bw_total += button_width(t, k, 13) + 6;
        const float tw = std::max(80.f, iw - bw_total - 14);
        float h = 0;
        const std::wstring cap = which == 0 ? L"CÓPIA DO JOGO" : L"PASTA DO PORT";
        const float caph = section(ix, iy, cap);
        if (which == 1 && out_free_ >= 0) {
            wchar_t buf[48];
            swprintf_s(buf, L"%.0f GB livres", out_free_);
            const ui::Size fs = g_.measure(buf, T(12));
            const double need = std::max(10.0, out_used_);
            g_.text(ix + tw - fs.w, iy, buf, T(12), out_free_ <= need - out_used_ + 2 ? WARN : FAINT);
        }
        h += caph + 4;
        const std::wstring path = which == 0 ? (game_.empty() ? L"—" : game_) : out_;
        h += g_.text(ix, iy + h, path, M(12.5f), PATH, tw);
        std::wstring info;
        uint32_t ic = MUTED;
        if (which == 0) {
            if (searching_ && version_.is_null()) {
                info = L"Procurando em Downloads, Documentos, Games e na raiz dos discos…";
            } else if (!version_.is_null()) {
                info = L"Pacote Xbox Series " + widen(version_["version"].str()) + L" · " +
                       std::to_wstring(version_["files"].members.size()) + L" arquivos";
            } else if (!game_.empty()) {
                info = L"Esta pasta não é uma versão suportada (1.0.2.1).";
                ic = WARN;
            } else {
                info = L"Não encontrada. Use \"Trocar\" e aponte a pasta que tem MicrosoftGame.Config.";
                ic = WARN;
            }
        } else if (out_used_ > 0) {
            wchar_t buf[64];
            swprintf_s(buf, L"O port usa ~%.0f GB", out_used_);
            info = buf;
        }
        if (!info.empty()) h += 4 + g_.text(ix, iy + h + 4, info, T(12), ic, tw, Align::left, true);
        // the buttons, centred on the card's height
        const float bh = 40, by = iy + std::max(0.f, (h - bh) / 2);
        float bx = ix + iw;
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
            const auto& [id, t, k] = *it;
            bx -= button_width(t, k, 13);
            button(id, bx, by, t, k, bh, 13);
            bx -= 6;
        }
        return std::max(h, bh);
    };
    // both cards as tall as the taller one
    float hs[2];
    {
        const bool was = g_.measuring;
        g_.measuring = true;
        for (int i = 0; i < 2; ++i) hs[i] = content(i, 0, 0, cw - 36);
        g_.measuring = was;
    }
    float total = 0;
    for (int i = 0; i < 2; ++i) {
        const float cx = two ? x + i * (cw + 16) : x, cy = two ? y : y + total;
        const float ch = (two ? std::max(hs[0], hs[1]) : hs[i]) + 32;
        g_.rrect(cx, cy, cx + cw, cy + ch, 14, CARD, EDGE, true, true);
        content(i, cx + 18, cy + 16, cw - 36);
        total = two ? ch : total + ch + (i == 0 ? 16 : 0);
    }
    actions_[PICK_GAME] = [this] { pick_game(); };
    actions_[FIND_GAME] = [this] { find_game(); };
    actions_[OPEN_OUT] = [this] { open_folder(out_); };
    actions_[PICK_OUT] = [this] { pick_out(); };
    return total;
}

// The log: a bar with its last line, opening to the lines themselves.
float App::paint_log(float x, float y, float width) {
    const float h = card(x, y, width, LOGBG, EDGE, 14, 18, 0, [&](float ix, float y0, float cw) {
        float yy = y0;
        const float hh = 48;
        icon(Icon::terminal, ix, yy + (hh - 16) / 2, 16, TEXT);
        const Font rf = T(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const ui::Size rs = g_.measure(L"Registro", rf);
        g_.text(ix + 16 + 12, yy + (hh - rs.h) / 2, L"Registro", rf, TEXT);
        const std::wstring toggle = log_open_ ? L"Ocultar" : L"Mostrar";
        const Font tf = T(13, DWRITE_FONT_WEIGHT_MEDIUM);
        const ui::Size ts = g_.measure(toggle, tf);
        g_.text(ix + cw - ts.w, yy + (hh - ts.h) / 2, toggle, tf, hover_ == LOG_TOGGLE ? LINK_HOVER : LABEL);
        std::wstring last;
        if (!log_.empty()) last = log_.back().first + L"  " + widen(log_.back().second);
        const float lx = ix + 16 + 12 + rs.w + 12;
        const float lh = g_.measure(L"0", M(12)).h;
        g_.text(lx, yy + (hh - lh) / 2, last, M(12), FAINT, std::max(10.f, ix + cw - ts.w - 12 - lx));
        hit(LOG_TOGGLE, ix - 18, yy, ix + cw + 18, yy + hh);
        yy += hh;
        if (log_open_) {
            g_.rect(ix - 18, yy, ix + cw + 18, yy + 1, EDGE);
            yy += 1 + 14;
            const int lines = 9;
            const float line_h = g_.measure(L"0", M(12.5f)).h + 4;
            const int count = static_cast<int>(log_.size());
            const int last_i = count - std::min(log_scroll_, std::max(0, count - lines));
            const int first = std::max(0, last_i - lines);
            if (!g_.measuring) {
                log_box_y0_ = yy - scroll_;
                log_box_y1_ = yy + lines * line_h - scroll_;
            }
            for (int i = first; i < last_i; ++i) {
                const float ly = yy + (i - first) * line_h;
                const auto& [stamp, text] = log_[i];
                const float sw = g_.measure(stamp + L"  ", M(12.5f)).w;
                g_.text(ix, ly, stamp, M(12.5f), DIM);
                g_.text(ix + sw, ly, widen(text), M(12.5f), LOGTEXT, cw - sw);
            }
            yy += lines * line_h + 14;
        }
        return yy - y0;
    });
    actions_[LOG_TOGGLE] = [this] {
        log_open_ = !log_open_;
        log_scroll_ = 0;
    };
    return h;
}

}  // namespace fp::launcher
