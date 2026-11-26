// Text: UTF-8 <-> UTF-16, and the launcher's number and time formats.
#pragma once
#include <cstdint>
#include <string>

namespace fp {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
std::wstring timestamp();                     // HH:MM:SS
std::string format_thousands(int64_t n);      // 12.345
std::string format_duration(double seconds);  // "12 min" / "1 h 05 min" / "calculando…"

}  // namespace fp
