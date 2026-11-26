// Virtual drives over host folders (see files.cpp).
#pragma once
#include <filesystem>
#include <string>

namespace rt::files {

// Mounts host folder `root` as drive `letter` for the title (e.g. 'G' for the package).
void mount(wchar_t letter, const std::filesystem::path& root);

// Mounts `root` on the highest letter neither the host nor another mount uses (the same letter if `root` is
// already mounted); 0 if none is left.
wchar_t mount_free(const std::filesystem::path& root);

// The host path for a title path: rewritten if it's on a mounted drive, else unchanged.
std::wstring to_host(const wchar_t* path);
std::wstring to_host(const char* path);  // ANSI code page

}  // namespace rt::files
