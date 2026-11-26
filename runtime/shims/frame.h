// The console's frame clock (device slots 78-81; see frame.cpp).
#pragma once
#include <windows.h>

#include <cstdint>

namespace rt::frame {

// Device methods (the device object first).
HRESULT WINAPI set_frame_interval(void* device, void* output, uint32_t length_us, uint32_t periodicity, uint32_t flags);
HRESULT WINAPI schedule_frame_event(void* device, uint32_t type, uint32_t offset_us, const void* ancillary,
                                    uint32_t flags);
HRESULT WINAPI wait_frame_event(void* device, uint32_t type, uint32_t timeout_ms, const void* ancillary, uint32_t flags,
                                uint64_t* token);
HRESULT WINAPI frame_statistics(void* device, uint64_t token, uint32_t type, uint32_t* count, void* stats);

// The frame of `token` was shown (PresentX): its statistics become available.
void mark_flipped(uint64_t token);

// For tests: the clock's state back to the start (interval off, nothing scheduled, no tokens).
void reset_for_test();

}  // namespace rt::frame
