// Build-specific patches inside the game exe (see game_patches.cpp).
#pragma once
#include <filesystem>

#include "pe_loader.h"

namespace rt {

// Applies the patches for this exact build, if there are any. Call before any game code runs. `trace` also wraps
// the patched objects' other methods in logging thunks (FlamingoPort.exe --trace-host).
void apply_game_patches(Module& exe, const std::filesystem::path& game_dir, bool trace = false);

}  // namespace rt
