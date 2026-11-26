// Drawing for the launcher's window: Direct2D shapes and images, DirectWrite text, in DIPs (1/96 inch). A measuring
// pass runs the same layout code with drawing off, so a panel knows its height before its background is drawn.
#pragma once
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>

#include <map>
#include <string>
#include <tuple>

namespace fp::ui {

struct Font {
    const wchar_t* family;
    float pt;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    bool underline = false;
    float spacing = 0;  // extra space after each character, in DIPs (CSS letter-spacing)
};

struct Point {
    float x, y;
};

enum class Align { left, center, right };

struct Size {
    float w, h;
};

class Canvas {
public:
    bool init(HWND hwnd);
    void resize();
    void set_dpi(float dpi);
    float scale() const { return dpi_ / 96.f; }
    bool begin();  // false: nothing to draw into
    void end();
    void discard();  // after a device loss

    bool measuring = false;  // drawing calls do nothing

    void clear(uint32_t rgb);
    void rect(float x0, float y0, float x1, float y1, uint32_t fill, float alpha = 1.f);
    void rrect(float x0, float y0, float x1, float y1, float r, uint32_t fill, uint32_t edge = 0, bool has_fill = true,
               bool has_edge = false, float alpha = 1.f);
    // A rounded rectangle filled left to right from `a` to `b`, lightened toward white by `lift`.
    void rrect_gradient(float x0, float y0, float x1, float y1, float r, uint32_t a, uint32_t b, float lift = 0.f);
    // Vertical gradient: `top` colour at y0 to `bottom` at y1, alphas given.
    void fade(float x0, float y0, float x1, float y1, uint32_t rgb, float alpha_top, float alpha_bottom);
    void line(float x0, float y0, float x1, float y1, uint32_t rgb, float width = 1.f);
    void ellipse(float cx, float cy, float rx, float ry, uint32_t fill);
    void ellipse_edge(float cx, float cy, float rx, float ry, uint32_t rgb, float width = 1.f);
    // A dashed rounded outline (an empty state's box).
    void rrect_dashed(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float dash = 4.f);
    // Lines through `pts` (round caps and joins), stroked; or the polygon filled.
    void polyline(const Point* pts, size_t n, uint32_t rgb, float width, bool closed = false);
    void polygon(const Point* pts, size_t n, uint32_t rgb);
    void check_mark(float x, float y, uint32_t rgb, float size = 12.f, float width = 2.4f);
    void play_icon(float x, float cy, uint32_t rgb);

    Size measure(const std::wstring& text, const Font& f, float max_w = 100000.f);
    // Draws text with its top-left at (x, y) (or aligned in [x, x + w]); returns its height.
    float text(float x, float y, const std::wstring& text, const Font& f, uint32_t rgb, float w = 0.f,
               Align a = Align::left, bool wrap = false, float alpha = 1.f);

    // An image file (PNG, JPEG...) as a bitmap; nullptr if it can't be read.
    ID2D1Bitmap* image(const std::wstring& path);
    // An image in this exe's resources (RCDATA `id`, a PNG); nullptr without it.
    ID2D1Bitmap* image(int id);
    void bitmap(ID2D1Bitmap* b, D2D1_RECT_F dest, D2D1_RECT_F src, float alpha = 1.f);
    // An icon from an image file (for the window).
    HICON icon(const std::wstring& path, int size);

    void push_clip(float x0, float y0, float x1, float y1);
    void pop_clip();
    void translate(float dx, float dy);

private:
    ID2D1SolidColorBrush* brush(uint32_t rgb, float alpha);
    IDWriteTextFormat* format(const Font& f);
    IWICBitmapSource* decode(const std::wstring& path);
    IWICBitmapSource* decode_resource(int id);
    void spacing(IDWriteTextLayout* layout, const Font& f, size_t length);

    HWND hwnd_ = nullptr;
    float dpi_ = 96.f;
    float dx_ = 0, dy_ = 0;
    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    ID2D1SolidColorBrush* solid_ = nullptr;
    std::map<std::tuple<std::wstring, float, int>, IDWriteTextFormat*> formats_;
    std::map<std::wstring, ID2D1Bitmap*> images_;
    std::map<int, ID2D1Bitmap*> resources_;
};

}  // namespace fp::ui
