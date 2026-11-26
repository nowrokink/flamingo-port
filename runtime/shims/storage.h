// Persistent local storage (see storage.cpp).
#pragma once
#include <filesystem>

namespace rt::storage {

// Reads the title's quota from <game dir>\MicrosoftGame.Config and mounts `folder` as its storage drive.
void init(const std::filesystem::path& game_dir, const std::filesystem::path& folder);

}  // namespace rt::storage
