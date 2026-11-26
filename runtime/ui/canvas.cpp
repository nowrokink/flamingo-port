#include "ui/canvas.h"

#include <dwrite_1.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fp::ui {
namespace {

D2D1_COLOR_F colour(uint32_t rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        float x = static_cast<float>((a >> s) & 255), y = static_cast<float>((b >> s) & 255);
        out |= static_cast<uint32_t>(std::lround(x + (y - x) * t)) << s;
    }
    return out;
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

bool Canvas::init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwrite_))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    return true;
}

void Canvas::set_dpi(float dpi) {
    dpi_ = dpi;
    if (target_) target_->SetDpi(dpi, dpi);
}

void Canvas::discard() {
    release(solid_);
    for (auto& [_, b] : images_) release(b);
    images_.clear();
    for (auto& [_, b] : resources_) release(b);
    resources_.clear();
    release(target_);
}

void Canvas::resize() {
    if (!target_) return;
    RECT r;
    GetClientRect(hwnd_, &r);
    target_->Resize(D2D1::SizeU(r.right, r.bottom));
}

bool Canvas::begin() {
    if (!target_) {
        RECT r;
        GetClientRect(hwnd_, &r);
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
        p.dpiX = p.dpiY = dpi_;
        if (FAILED(factory_->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom)),
                                                    &target_)))
            return false;
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target_->CreateSolidColorBrush(colour(0), &solid_);
    }
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    dx_ = dy_ = 0;
    return true;
}

void Canvas::end() {
    if (!target_) return;
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) discard();
}

void Canvas::translate(float dx, float dy) {
    dx_ += dx;
    dy_ += dy;
    if (target_) target_->SetTransform(D2D1::Matrix3x2F::Translation(dx_, dy_));
}

ID2D1SolidColorBrush* Canvas::brush(uint32_t rgb, float alpha) {
    solid_->SetColor(colour(rgb, alpha));
    return solid_;
}

void Canvas::clear(uint32_t rgb) {
    if (!measuring && target_) target_->Clear(colour(rgb));
}

void Canvas::rect(float x0, float y0, float x1, float y1, uint32_t fill, float alpha) {
    if (measuring || !target_ || x1 <= x0 || y1 <= y0) return;
    target_->FillRectangle(D2D1::RectF(x0, y0, x1, y1), brush(fill, alpha));
}

void Canvas::rrect(float x0, float y0, float x1, float y1, float r, uint32_t fill, uint32_t edge, bool has_fill,
                   bool has_edge, float alpha) {
    if (measuring || !target_ || x1 - x0 < 0.5f || y1 - y0 < 0.5f) return;
    r = std::max(0.f, std::min({r, (x1 - x0) / 2, (y1 - y0) / 2}));
    if (has_fill) target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), r, r), brush(fill, alpha));
    if (has_edge)
        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 + .5f, y0 + .5f, x1 - .5f, y1 - .5f), r, r),
                                      brush(edge, alpha), 1.f);
}

void Canvas::rrect_gradient(float x0, float y0, float x1, float y1, float r, uint32_t a, uint32_t b, float lift) {
    if (measuring || !target_) return;
    D2D1_GRADIENT_STOP stops[2] = {{0.f, colour(mix(a, 0xffffff, lift))}, {1.f, colour(mix(b, 0xffffff, lift))}};
    ID2D1GradientStopCollection* coll = nullptr;
    ID2D1LinearGradientBrush* g = nullptr;
    if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 2, &coll)) &&
        SUCCEEDED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y0)), coll, &g)))
        target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), r, r), g);
    release(g);
    release(coll);
}

void Canvas::fade(float x0, float y0, float x1, float y1, uint32_t rgb, float alpha_top, float alpha_bottom) {
    if (measuring || !target_) return;
    // eased like the Tk launcher's ((i + 1) / rows) ^ 1.5 blend
    D2D1_GRADIENT_STOP stops[5];
    for (int k = 0; k < 5; ++k) {
        float t = k / 4.f;
        stops[k] = {t, colour(rgb, alpha_top + (alpha_bottom - alpha_top) * std::pow(t, 1.5f))};
    }
    ID2D1GradientStopCollection* coll = nullptr;
    ID2D1LinearGradientBrush* g = nullptr;
    if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 5, &coll)) &&
        SUCCEEDED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x0, y1)), coll, &g)))
        target_->FillRectangle(D2D1::RectF(x0, y0, x1, y1), g);
    release(g);
    release(coll);
}

void Canvas::line(float x0, float y0, float x1, float y1, uint32_t rgb, float width) {
    if (measuring || !target_) return;
    target_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(rgb, 1.f), width);
}

void Canvas::ellipse(float cx, float cy, float rx, float ry, uint32_t fill) {
    if (measuring || !target_) return;
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush(fill, 1.f));
}

void Canvas::check_mark(float x, float y, uint32_t rgb, float size, float width) {
    if (measuring || !target_) return;
    float s = size / 24.f;
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(factory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(x + 4 * s, y + 12 * s), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(x + 9 * s, y + 17 * s));
        sink->AddLine(D2D1::Point2F(x + 20 * s, y + 6 * s));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        ID2D1StrokeStyle* style = nullptr;
        factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &style);
        target_->DrawGeometry(path, brush(rgb, 1.f), width, style);
        release(style);
    }
    release(sink);
    release(path);
}

void Canvas::play_icon(float x, float cy, uint32_t rgb) {
    if (measuring || !target_) return;
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(factory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(x + 2, cy - 7), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(x + 15, cy));
        sink->AddLine(D2D1::Point2F(x + 2, cy + 7));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target_->FillGeometry(path, brush(rgb, 1.f));
    }
    release(sink);
    release(path);
}

IDWriteTextFormat* Canvas::format(const Font& f) {
    auto key = std::make_tuple(std::wstring(f.family), f.pt, static_cast<int>(f.weight));
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second;
    IDWriteTextFormat* fmt = nullptr;
    dwrite_->CreateTextFormat(f.family, nullptr, f.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              f.pt * 96.f / 72.f, L"pt-br", &fmt);
    formats_[key] = fmt;
    return fmt;
}

Size Canvas::measure(const std::wstring& text, const Font& f, float max_w) {
    IDWriteTextLayout* layout = nullptr;
    Size s{0, 0};
    IDWriteTextFormat* fmt = format(f);
    if (fmt && SUCCEEDED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, max_w, 100000.f,
                                                   &layout))) {
        layout->SetWordWrapping(max_w < 99999.f ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        spacing(layout, f, text.size());
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        s = {m.widthIncludingTrailingWhitespace, m.height};
    }
    release(layout);
    return s;
}

float Canvas::text(float x, float y, const std::wstring& text, const Font& f, uint32_t rgb, float w, Align a, bool wrap,
                   float alpha) {
    IDWriteTextFormat* fmt = format(f);
    if (!fmt) return 0;
    const float box = w > 0 ? w : 100000.f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, box, 100000.f, &layout)))
        return 0;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(a == Align::center ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : a == Align::right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                 : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (f.underline) layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
    spacing(layout, f, text.size());
    if (!wrap && w > 0) {
        // one line, cut with an ellipsis
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* dots = nullptr;
        dwrite_->CreateEllipsisTrimmingSign(fmt, &dots);
        layout->SetTrimming(&trim, dots);
        release(dots);
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    if (!measuring && target_)
        target_->DrawTextLayout(D2D1::Point2F(a == Align::left || w > 0 ? x : x, y), layout, brush(rgb, alpha),
                                D2D1_DRAW_TEXT_OPTIONS_CLIP);
    release(layout);
    return m.height;
}

void Canvas::spacing(IDWriteTextLayout* layout, const Font& f, size_t length) {
    if (f.spacing <= 0) return;
    IDWriteTextLayout1* l1 = nullptr;
    if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&l1)))) {
        l1->SetCharacterSpacing(0, f.spacing, 0, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(length)});
        l1->Release();
    }
}

void Canvas::ellipse_edge(float cx, float cy, float rx, float ry, uint32_t rgb, float width) {
    if (measuring || !target_) return;
    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush(rgb, 1.f), width);
}

void Canvas::rrect_dashed(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float dash) {
    if (measuring || !target_) return;
    ID2D1StrokeStyle* style = nullptr;
    const float dashes[] = {dash, dash};
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                            D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f,
                                                            D2D1_DASH_STYLE_CUSTOM, 0.f),
                                dashes, 2, &style);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 + .5f, y0 + .5f, x1 - .5f, y1 - .5f), r, r),
                                  brush(rgb, 1.f), 1.f, style);
    release(style);
}

void Canvas::polyline(const Point* pts, size_t n, uint32_t rgb, float width, bool closed) {
    if (measuring || !target_ || n < 2) return;
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(factory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < n; ++i) sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        ID2D1StrokeStyle* style = nullptr;
        factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &style);
        target_->DrawGeometry(path, brush(rgb, 1.f), width, style);
        release(style);
    }
    release(sink);
    release(path);
}

void Canvas::polygon(const Point* pts, size_t n, uint32_t rgb) {
    if (measuring || !target_ || n < 3) return;
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(factory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i < n; ++i) sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target_->FillGeometry(path, brush(rgb, 1.f));
    }
    release(sink);
    release(path);
}

IWICBitmapSource* Canvas::decode_resource(int id) {
    if (!wic_) return nullptr;
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL h = r ? LoadResource(self, r) : nullptr;
    const void* data = h ? LockResource(h) : nullptr;
    if (!data) return nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmapSource* out = nullptr;
    if (SUCCEEDED(wic_->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), SizeofResource(self, r))) &&
        SUCCEEDED(wic_->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut))) {
        out = conv;
        conv = nullptr;
    }
    release(conv);
    release(frame);
    release(dec);
    release(stream);
    return out;
}

ID2D1Bitmap* Canvas::image(int id) {
    if (!target_) return nullptr;
    auto it = resources_.find(id);
    if (it != resources_.end()) return it->second;
    ID2D1Bitmap* b = nullptr;
    if (IWICBitmapSource* src = decode_resource(id)) {
        target_->CreateBitmapFromWicBitmap(src, nullptr, &b);
        src->Release();
    }
    resources_[id] = b;
    return b;
}

IWICBitmapSource* Canvas::decode(const std::wstring& path) {
    if (!wic_) return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                  &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut))) {
        release(frame);
        release(dec);
        return conv;
    }
    release(conv);
    release(frame);
    release(dec);
    return nullptr;
}

ID2D1Bitmap* Canvas::image(const std::wstring& path) {
    if (!target_) return nullptr;
    auto it = images_.find(path);
    if (it != images_.end()) return it->second;
    ID2D1Bitmap* b = nullptr;
    if (IWICBitmapSource* src = decode(path)) {
        target_->CreateBitmapFromWicBitmap(src, nullptr, &b);
        src->Release();
    }
    images_[path] = b;
    return b;
}

void Canvas::bitmap(ID2D1Bitmap* b, D2D1_RECT_F dest, D2D1_RECT_F src, float alpha) {
    if (measuring || !target_ || !b) return;
    target_->DrawBitmap(b, dest, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
}

HICON Canvas::icon(const std::wstring& path, int size) {
    IWICBitmapSource* src = decode(path);
    if (!src) return nullptr;
    IWICBitmapScaler* scaler = nullptr;
    HICON h = nullptr;
    if (SUCCEEDED(wic_->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(src, size, size, WICBitmapInterpolationModeFant))) {
        std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4);
        if (SUCCEEDED(scaler->CopyPixels(nullptr, size * 4, static_cast<UINT>(px.size()), px.data()))) {
            BITMAPV5HEADER bh{};
            bh.bV5Size = sizeof bh;
            bh.bV5Width = size;
            bh.bV5Height = -size;
            bh.bV5Planes = 1;
            bh.bV5BitCount = 32;
            bh.bV5Compression = BI_BITFIELDS;
            bh.bV5RedMask = 0x00FF0000;
            bh.bV5GreenMask = 0x0000FF00;
            bh.bV5BlueMask = 0x000000FF;
            bh.bV5AlphaMask = 0xFF000000;
            void* bits = nullptr;
            HDC dc = GetDC(nullptr);
            HBITMAP colour_bm = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (colour_bm && bits) {
                // the pixels came premultiplied: undo it for the icon
                for (size_t i = 0; i < px.size(); i += 4) {
                    uint8_t a = px[i + 3];
                    if (a && a < 255)
                        for (int c = 0; c < 3; ++c) px[i + c] = static_cast<uint8_t>(std::min(255, px[i + c] * 255 / a));
                }
                memcpy(bits, px.data(), px.size());
                HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
                ICONINFO ii{TRUE, 0, 0, mask, colour_bm};
                h = CreateIconIndirect(&ii);
                DeleteObject(mask);
            }
            if (colour_bm) DeleteObject(colour_bm);
        }
    }
    release(scaler);
    src->Release();
    return h;
}

void Canvas::push_clip(float x0, float y0, float x1, float y1) {
    if (measuring || !target_) return;
    target_->PushAxisAlignedClip(D2D1::RectF(x0, y0, x1, y1), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Canvas::pop_clip() {
    if (measuring || !target_) return;
    target_->PopAxisAlignedClip();
}

}  // namespace fp::ui
