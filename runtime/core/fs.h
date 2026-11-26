// Files, folders and disks (wide paths, Win32 underneath).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace fp {

std::wstring join(const std::wstring& a, const std::wstring& b);
std::wstring parent(const std::wstring& path);
std::wstring file_name(const std::wstring& path);
std::wstring full_path(const std::wstring& path);
// Whether `inner` is `outer` or inside it (case-insensitive).
bool inside(const std::wstring& inner, const std::wstring& outer);

bool is_file(const std::wstring& path);
bool is_dir(const std::wstring& path);
bool make_dirs(const std::wstring& path);
int64_t file_size(const std::wstring& path);  // -1 if absent
bool read_file(const std::wstring& path, std::string& out);
bool write_file(const std::wstring& path, const void* data, size_t size);
// The names in a folder (files, folders or both), sorted by code point (as Python's sorted()).
std::vector<std::wstring> list_dir(const std::wstring& dir, bool files = true, bool dirs = false);
size_t count_dir(const std::wstring& dir, size_t stop_at = SIZE_MAX);

double free_gb(const std::wstring& path);
double folder_gb(const std::wstring& path, const std::atomic<bool>* stop = nullptr);

}  // namespace fp
