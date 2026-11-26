// Content hashes (BCrypt).
#pragma once
#include <atomic>
#include <string>

namespace fp {

// SHA-1 of bytes in memory, as hex (comparisons with the Python tools' hashlib.sha1).
std::string sha1_hex(const std::string& data);

bool sha256_file(const std::wstring& path, std::string& hex, const std::atomic<bool>* stop = nullptr);

}  // namespace fp
