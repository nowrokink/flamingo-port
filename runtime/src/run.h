// Runs a loaded image: unwind tables, static TLS, DllMain of the bundled modules, then the exe's entry point.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

#include "pe_loader.h"

namespace rt {

// Doesn't return when the game exits or crashes (the process ends). Returns an exit code if setup fails.
// `probes`: exe RVAs that log once when reached (FlamingoPort.exe --probe).
int run(Loader& loader, Module& exe, const std::filesystem::path& game_dir, const std::vector<uint32_t>& probes = {});

}  // namespace rt
