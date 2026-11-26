// The supported versions of the game (the version files in <repo>\game) and the copies of them on this PC.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"

namespace fp::prep {

// The version file lists in <repo>\game (title, version, expected_exe, files: {path: {size, sha256}}).
std::vector<json::Value> versions();
// The supported version a folder holds (by the size of a few small files), or a null value.
json::Value match_version(const std::wstring& game);
// Folders on this PC holding a supported version: Downloads, Desktop, Documents, Games, the home folder and the top
// of each drive, two folder levels down. Only names and a few file sizes are read.
std::vector<std::pair<std::wstring, json::Value>> find_copies();

std::wstring default_out();  // Documents\Flamingo

}  // namespace fp::prep
