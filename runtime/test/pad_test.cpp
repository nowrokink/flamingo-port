// Checks the DualSense report parser (shims/pad.cpp) on hand-made reports in its three layouts, and the mapping to
// the console gamepad. `pad_test --live` instead prints the connected controller's state for 30 seconds.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../shims/pad.h"

namespace {

int g_failures = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

bool close_to(float a, float b) { return std::fabs(a - b) < 0.01f; }

// One report of `layout` (0 USB, 1 Bluetooth simple, 2 Bluetooth full) with these fields.
size_t report(int layout, uint8_t* r, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2, uint8_t r2, uint8_t b0,
              uint8_t b1, uint8_t b2) {
    std::memset(r, 0, 78);
    switch (layout) {
    case 0:
        r[0] = 0x01, r[1] = lx, r[2] = ly, r[3] = rx, r[4] = ry, r[5] = l2, r[6] = r2, r[8] = b0, r[9] = b1, r[10] = b2;
        return 64;
    case 1:
    case 3:  // as Windows hands it over Bluetooth: padded to the 78 bytes of the longest report
        r[0] = 0x01, r[1] = lx, r[2] = ly, r[3] = rx, r[4] = ry, r[5] = b0, r[6] = b1, r[7] = b2, r[8] = l2, r[9] = r2;
        return layout == 1 ? 10 : 78;
    default:
        r[0] = 0x31, r[2] = lx, r[3] = ly, r[4] = rx, r[5] = ry, r[6] = l2, r[7] = r2, r[9] = b0, r[10] = b1, r[11] = b2;
        return 78;
    }
}

int live() {
    uint32_t last = ~0u;
    float shown[6] = {};
    DWORD start = GetTickCount();
    std::printf("pad_test: 30 s of the controller's state (press buttons)\n");
    while (GetTickCount() - start < 30000) {
        rt::pad::State s;
        bool on = rt::pad::current(s);
        uint32_t key = on ? s.buttons : 0xFFFFFFFF;
        const float now[6] = {s.left_x, s.left_y, s.right_x, s.right_y, s.left_trigger, s.right_trigger};
        bool moved = false;
        for (int i = 0; i < 6; ++i) moved |= std::fabs(now[i] - shown[i]) > 0.25f;
        if (key != last || moved) {
            std::memcpy(shown, now, sizeof shown);
            if (on)
                std::printf("buttons %#06x  L (%.2f %.2f) R (%.2f %.2f) LT %.2f RT %.2f\n", s.buttons, s.left_x,
                            s.left_y, s.right_x, s.right_y, s.left_trigger, s.right_trigger);
            else
                std::printf("no controller\n");
            std::fflush(stdout);
            last = key;
        }
        Sleep(16);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--live") == 0) return live();
    using namespace rt::pad;
    uint8_t r[80];
    for (int layout = 0; layout < 4; ++layout) {
        State s;
        // centred sticks, nothing pressed, hat released (8)
        CHECK(parse_dualsense(r, report(layout, r, 128, 128, 128, 128, 0, 0, 0x08, 0, 0), s));
        CHECK(s.buttons == 0);
        CHECK(close_to(s.left_x, 0) && close_to(s.left_y, 0) && close_to(s.right_x, 0) && close_to(s.right_y, 0));
        // left stick up-left, right stick down-right, triggers, Cross + Triangle, hat up-right, L1, Options, touchpad
        CHECK(parse_dualsense(r, report(layout, r, 0, 0, 255, 255, 255, 128, 0xA0 | 1, 0x01 | 0x20, 0x02 | 0x01), s));
        CHECK(close_to(s.left_x, -1) && close_to(s.left_y, 1));    // +y up
        CHECK(close_to(s.right_x, 1) && close_to(s.right_y, -1));
        CHECK(close_to(s.left_trigger, 1) && close_to(s.right_trigger, 128 / 255.0f));
        CHECK(s.buttons == (kA | kY | kUp | kRight | kLeftShoulder | kMenu | kView));  // PS (0x01 in buttons2) ignored
        // Square, Circle, R1, Create, L3, R3, hat left
        CHECK(parse_dualsense(r, report(layout, r, 128, 128, 128, 128, 0, 0, 0x50 | 6, 0x02 | 0x10 | 0x40 | 0x80, 0), s));
        CHECK(s.buttons == (kX | kB | kLeft | kRightShoulder | kView | kLeftThumb | kRightThumb));
    }
    State s;
    uint8_t other[16] = {0x05};
    CHECK(!parse_dualsense(other, sizeof other, s));  // another report
    CHECK(!parse_dualsense(r, 4, s));                 // too short
    if (g_failures) {
        std::fprintf(stderr, "pad_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pad_test: ok\n");
    return 0;
}
