// The player's controller as one console gamepad: a DualSense read straight from HID (Windows gives no usable gamepad
// for it: XInput doesn't see it and GameInput reports it neutral), and Xbox controllers through XInput, combined.
//
// DualSense mapping (Xbox names): Cross A, Circle B, Square X, Triangle Y, L1 / R1 shoulders, L2 / R2 triggers,
// Options Menu, Create View, touchpad click View, L3 / R3 thumbsticks; the PS button does nothing.
#pragma once
#include <cstddef>
#include <cstdint>

namespace rt::pad {

// GameInputGamepadButtons bits (the console's and the PC SDK's GameInput.h agree).
enum Buttons : uint32_t {
    kMenu = 0x1, kView = 0x2, kA = 0x4, kB = 0x8, kX = 0x10, kY = 0x20,
    kUp = 0x40, kDown = 0x80, kLeft = 0x100, kRight = 0x200,
    kLeftShoulder = 0x400, kRightShoulder = 0x800, kLeftThumb = 0x1000, kRightThumb = 0x2000,
};

// GameInputGamepadState's layout: buttons, then triggers in [0, 1] and thumbsticks in [-1, 1] (+y up).
struct State {
    uint32_t buttons = 0;
    float left_trigger = 0, right_trigger = 0;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};

// One DualSense input report (report ID first): USB 0x01, Bluetooth's default 0x01 and full 0x31. False for any
// other report.
bool parse_dualsense(const uint8_t* report, size_t size, State& out);

// The combined state now; false when no controller is connected. The first call starts the reader.
bool current(State& out, uint64_t* sequence = nullptr);

}  // namespace rt::pad
