// The resource overlay: frame rate, CPU, memory and video memory as the port measures them, drawn over the top-left
// of the presented frame (d3d12x.cpp's present), with the runtime's details in a bar across the top. On by default;
// RT_OVERLAY=0 starts it off, F10 turns it on and off.
//
// "Real" memory is the port's own count: its private working set plus the console memory pool once. The pool is a
// section seen through two views (the title's addresses and the chunk views the GPU heaps are opened on), and the
// working set Task Manager and other overlays show counts those pages twice.
#pragma once

#include <cstdint>
#include <vector>

struct ID3D12Device;

namespace rt::overlay {

// What only the runtime knows, handed over by the present (every 32 frames is enough).
struct RuntimeNumbers {
    double gpu_wait_ms = 0;          // a frame's time blocked on the GPU
    double draw_ms = 0;              // a frame's time recording draws
    uint64_t pipelines_making = 0;   // PC pipelines the driver is still compiling
};

bool enabled();
// Once a present: F10 (while the game's window, or the launcher around it, has the keyboard) turns it on and off.
void poll_toggle();
// Once a present, for the frame rate (`width`: the back buffer's, which the top bar spans); starts the thread
// that measures and draws the image.
void frame_presented(ID3D12Device* device, uint32_t width);
void set_runtime(const RuntimeNumbers& n);
// The latest image: B8G8R8A8 with straight alpha, `w` x `h`, rows packed. False before the first one. `version`
// changes with each new image.
bool copy_image(std::vector<uint32_t>& out, uint32_t* w, uint32_t* h, uint64_t* version);

}  // namespace rt::overlay
