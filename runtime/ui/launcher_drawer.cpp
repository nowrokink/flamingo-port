// The side panel over the page (the design's drawer): settings (quality, how preparing works, the helpers' folder)
// and monitoring (the computers taking part, the shaders without a translation). It scrolls on its own; a click on
// the dimmed page, its close button or Esc closes it.
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <vector>

#include "core/text.h"
#include "ui/app.h"

namespace fp::launcher {

void App::paint_drawer(float width, float height) {
    const float dw = std::min(DRAWER_W, width), x0 = width - dw;
    drawer_x_ = x0;
    // the page dimmed, and a shadow along the panel's edge
    g_.rect(0, 0, width, height, SCRIM, 0.6f);
    hit(DRAWER_CLOSE, 0, 0, x0, height);
    for (int i = 0; i < 6; ++i) g_.rect(x0 - 24 + i * 4.f, 0, x0, height, 0x000000, 0.05f);
    g_.rect(x0, 0, width, height, DRAWER_BG);
    g_.rect(x0, 0, x0 + 1, height, DRAWER_EDGE);
    hit(DRAWER_BODY, x0, 0, width, height);
    actions_[DRAWER_CLOSE] = [this] { drawer_ = Drawer::none; };
    actions_[DRAWER_BODY] = nullptr;

    // the header: the title and the close button
    const float head_h = 80;
    const std::wstring title = drawer_ == Drawer::config ? L"Configurações" : L"Monitoramento";
    const Font tf = H(20);
    g_.text(x0 + 24, (head_h - g_.measure(title, tf).h) / 2, title, tf, TEXT, dw - 24 - 72);
    const float cx = width - 24 - 40, cy = 20;
    if (hover_ == DRAWER_CLOSE) g_.rrect(cx, cy, cx + 40, cy + 40, 10, BTN_HOVER);
    icon(Icon::close, cx + 11, cy + 11, 18, SUBTLE);
    hit(DRAWER_CLOSE, cx, cy, cx + 40, cy + 40);
    g_.rect(x0, head_h, width, head_h + 1, EDGE);

    // the body, scrolled
    const float top = head_h + 1;
    drawer_view_h_ = height - top;
    drawer_scroll_ = std::clamp(drawer_scroll_, 0.f, std::max(0.f, drawer_h_ - drawer_view_h_));
    g_.push_clip(x0 + 1, top, width, height);
    g_.translate(0, -drawer_scroll_);
    hit_dy_ = -drawer_scroll_;
    hit_top_ = top;
    const float bx = x0 + 24, bw = dw - 48;
    float y = top + 8;
    y += drawer_ == Drawer::config ? paint_config(bx, y, bw) : paint_monitor(bx, y, bw);
    drawer_h_ = y + 24 - top;
    g_.translate(0, drawer_scroll_);
    hit_dy_ = 0;
    hit_top_ = -1e9f;
    g_.pop_clip();
    if (drawer_h_ > drawer_view_h_ + 2) {  // its own scrollbar
        const float thumb = std::max(24.f, drawer_view_h_ * drawer_view_h_ / drawer_h_);
        const float t0 = top + drawer_scroll_ / std::max(1.f, drawer_h_ - drawer_view_h_) * (drawer_view_h_ - thumb);
        g_.rrect(width - 8, t0 + 2, width - 3, t0 + thumb - 2, 2.5f, BTN_EDGE);
    }
}

float App::paint_config(float x, float y, float w) {
    float h = 16;
    // quality: the console the game is told it runs on
    h += section(x, y + h, L"QUALIDADE") + 12;
    const bool fhd = flag("full_hd", true);
    if (w >= 320) {
        const float cw = (w - 8) / 2;
        const float a = choice(QUALITY_FHD, x, y + h, cw, fhd, L"Full HD", L"Perfil Series S");
        const float b = choice(QUALITY_4K, x + cw + 8, y + h, cw, !fhd, L"4K",
                               L"Perfil Series X · mais de 8 GB de vídeo e 32 GB de RAM");
        h += std::max(a, b);
    } else {
        h += choice(QUALITY_FHD, x, y + h, w, fhd, L"Full HD", L"Perfil Series S") + 8;
        h += choice(QUALITY_4K, x, y + h, w, !fhd, L"4K", L"Perfil Series X · mais de 8 GB de vídeo e 32 GB de RAM");
    }
    actions_[QUALITY_FHD] = [this] { set_flag("full_hd", true); };
    actions_[QUALITY_4K] = [this] { set_flag("full_hd", false); };

    // preparing
    h += 24;
    h += section(x, y + h, L"PREPARAÇÃO") + 4;
    std::wstring hinfo = L"Divide o trabalho com quem rodar o flamingo-worker.exe.";
    const int64_t port = s_["helper_port"].integer(8765);
    if (!helpers_.empty())
        hinfo = std::to_wstring(helpers_.size()) + (helpers_.size() == 1 ? L" ajudando agora." : L" ajudando agora.");
    else if (!watch_.is_null() && watch_["port"].integer())
        hinfo = L"Esperando ajudantes em " + lan_address() + L":" + std::to_wstring(watch_["port"].integer()) + L".";
    else if (!watch_.is_null())
        hinfo = L"Esta tradução não aceita ajudantes (iniciada sem a porta).";
    else if (mode_ == Mode::running && flag("helpers", true))
        hinfo = L"Esperando ajudantes em " + lan_address() + L":" + std::to_wstring(port) + L".";
    h += switch_row(OPT_LOCAL, x, y + h, w, flag("local_compile", true), L"Este PC também compila shaders",
                    L"Se desligar, ele só traduz e deixa a compilação para os ajudantes.");
    h += switch_row(OPT_HELPERS, x, y + h, w, flag("helpers", true), L"Aceitar ajuda de outros PCs da rede", hinfo);
    h += switch_row(OPT_FULL_CHECK, x, y + h, w, flag("full_check", false), L"Conferir o SHA-256 de todos os arquivos",
                    L"Mais lento. Vale na próxima verificação da cópia.");
    actions_[OPT_LOCAL] = [this] { set_flag("local_compile", !flag("local_compile", true)); };
    actions_[OPT_HELPERS] = [this] { set_flag("helpers", !flag("helpers", true)); };
    actions_[OPT_FULL_CHECK] = [this] { set_flag("full_check", !flag("full_check", false)); };

    // the helpers' folder: what to take to another PC
    h += 24;
    h += section(x, y + h, L"PASTA DO AJUDANTE") + 8;
    const std::wstring create = L"Criar";
    const float bw = button_width(create, Kind::outline, 13);
    const std::wstring where = lan_address() + L":" + std::to_wstring(port);
    const float box_w = std::max(60.f, w - bw - 8);
    const float lh = g_.measure(where, M(12)).h;
    g_.rrect(x, y + h, x + box_w, y + h + 40, 10, LOGBG, EDGE, true, true);
    g_.text(x + 12, y + h + (40 - lh) / 2, where, M(12), PATH, box_w - 24);
    button(HELPER_FOLDER, x + w - bw, y + h, create, Kind::outline, 40, 13);
    actions_[HELPER_FOLDER] = [this] { helper_folder(); };
    h += 40 + 8;
    h += g_.text(x, y + h,
                 L"Onde os outros PCs pegam e devolvem os shaders: \"Criar\" monta uma pasta com o "
                 L"flamingo-worker.exe e o worker.txt (este endereço e a senha) para copiar até eles.",
                 T(12), FAINT, w, Align::left, true);
    return h;
}

float App::paint_monitor(float x, float y, float w) {
    float h = 16;
    // the computers taking part, with what each has compiled
    const float caps = section(x, y + h, L"COMPUTADORES");
    const ui::Size rs = g_.measure(L"shaders compilados", T(12));
    g_.text(x + w - rs.w, y + h + caps - rs.h, L"shaders compilados", T(12), FAINT);
    h += caps + 12;
    const bool working = mode_ == Mode::running || !watch_.is_null();
    struct Row {
        std::string key, label;
        bool on;
    };
    std::vector<Row> rows{{"", "Este PC", true}};
    std::vector<std::string> names;
    for (const auto& [k, _] : machines_)
        if (!k.empty()) names.push_back(k);
    for (const auto& hl : helpers_)
        if (!hl.empty() && std::find(names.begin(), names.end(), hl) == names.end()) names.push_back(hl);
    std::sort(names.begin(), names.end());
    for (const auto& k : names) {
        const bool on = std::find(helpers_.begin(), helpers_.end(), k) != helpers_.end();
        rows.push_back({k, k.substr(0, k.find('@')), on});
    }
    for (const Row& r : rows) {
        const bool busy = r.on && working;
        std::wstring sub;
        if (!r.key.empty()) {
            const size_t at = r.key.find('@');
            const std::wstring addr = at == std::string::npos ? L"" : widen(r.key.substr(at + 1));
            sub = (addr.empty() ? L"" : addr + L" · ") + (r.on ? L"conectado" : L"sem resposta");
        } else {
            sub = working ? L"Traduz e distribui" : L"Parado · nada na fila";
        }
        const int64_t count = machines_.count(r.key) ? machines_[r.key] : 0;
        h += card(x, y + h, w, TILE, TILE, 12, 16, 14, [&](float ix, float iy, float iw) {
            const std::wstring n = num(count);
            const ui::Size ns = g_.measure(n, H(18));
            const float tw = std::max(40.f, iw - 22 - ns.w - 12);
            float th = g_.text(ix + 22, iy, widen(r.label), T(14, DWRITE_FONT_WEIGHT_MEDIUM), r.on ? TEXT : FAINT, tw);
            th += 2;
            th += g_.text(ix + 22, iy + th, sub, T(12), !r.key.empty() && busy ? GREEN_T : FAINT, tw);
            g_.ellipse(ix + 5, iy + th / 2, 5, 5, busy ? GREEN : r.on ? DIM : 0x4a3a5e);
            g_.text(ix + iw - ns.w, iy + (th - ns.h) / 2, n, H(18), r.on ? TEXT : FAINT);
            return th;
        });
        h += 8;
    }
    if (names.empty()) {
        // nobody helping: how to get help
        const float pad = 16;
        const std::wstring t1 = L"Nenhum ajudante conectado";
        const std::wstring t2 = L"Rode o flamingo-worker.exe em outro PC da rede para dividir a compilação. A pasta "
                                L"para levar até ele sai em Configurações › Pasta do ajudante.";
        const float a = g_.measure(t1, T(14, DWRITE_FONT_WEIGHT_MEDIUM), w - 2 * pad).h;
        const float b = g_.measure(t2, T(13), w - 2 * pad).h;
        const float bh = 18 + a + 6 + b + 18;
        g_.rrect_dashed(x, y + h, x + w, y + h + bh, 12, BTN_EDGE);
        g_.text(x + pad, y + h + 18, t1, T(14, DWRITE_FONT_WEIGHT_MEDIUM), TEXT, w - 2 * pad, Align::left, true);
        g_.text(x + pad, y + h + 18 + a + 6, t2, T(13), MUTED, w - 2 * pad, Align::left, true);
        h += bh;
    } else {
        h -= 8;
    }

    // shaders without a translation
    h += 24;
    h += section(x, y + h, L"SHADERS SEM TRADUÇÃO") + 10;
    if (stats_.members.empty()) {
        h += g_.text(x, y + h, L"Os números aparecem durante a preparação.", T(13), MUTED, w, Align::left, true);
        return h;
    }
    const int64_t bad = missing_count(), total = std::max<int64_t>(1, stats_["total"].integer());
    const std::wstring list = L"Ver lista";
    const float lw = button_width(list, Kind::outline, 13);
    h += card(x, y + h, w, bad ? WARN_BG : TILE, bad ? WARN_EDGE : TILE, 12, 16, 14, [&](float ix, float iy, float iw) {
        const float tw = std::max(60.f, iw - lw - 12);
        float th = g_.text(ix, iy, num(bad), H(18), bad ? WARN_V : TEXT, tw);
        th += 2;
        wchar_t pct[32];
        swprintf_s(pct, L"%.0f%% do total", 100.0 * static_cast<double>(bad) / static_cast<double>(total));
        th += g_.text(ix, iy + th, std::wstring(pct) + (bad ? L" · podem causar falhas visuais" : L""), T(12),
                      bad ? WARN : MUTED, tw, Align::left, true);
        // "Ver lista": the reasons (the button in the warning's colours)
        const float by = iy + std::max(0.f, (th - 40) / 2), bx = ix + iw - lw;
        g_.rrect(bx, by, bx + lw, by + 40, 10, 0x3a2a18, WARN_EDGE, hover_ == EXPLAIN, true);
        const ui::Size ls = g_.measure(list, T(13, DWRITE_FONT_WEIGHT_MEDIUM));
        g_.text(bx, by + (40 - ls.h) / 2, list, T(13, DWRITE_FONT_WEIGHT_MEDIUM), WARN_V, lw, Align::center);
        hit(EXPLAIN, bx, by, bx + lw, by + 40);
        return std::max(th, 40.f);
    });
    actions_[EXPLAIN] = [this] { explain_missing(); };
    if (const int64_t unmet = stats_["unmet"].integer()) {
        h += 8;
        h += card(x, y + h, w, TILE, TILE, 12, 16, 14, [&](float ix, float iy, float iw) {
            float th = g_.text(ix, iy, num(unmet) + L" aguardam o jogo", H(18), TEXT, iw);
            th += 2;
            th += g_.text(ix, iy + th,
                          L"Shaders de compute traduzidos na preparação seguinte à passagem do jogo por eles. Não "
                          L"são falhas.",
                          T(12), MUTED, iw, Align::left, true);
            return th;
        });
    }
    return h;
}

}  // namespace fp::launcher
