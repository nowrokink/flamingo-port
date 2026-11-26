#include "text.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace fp {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[16];
    swprintf_s(buf, L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return buf;
}

std::string format_thousands(int64_t n) {
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += '.';
        out += digits[i];
    }
    return n < 0 ? "-" + out : out;
}

std::string format_duration(double seconds) {
    if (seconds < 0) return "calculando\xE2\x80\xA6";
    int m = std::max(1, static_cast<int>(seconds) / 60);
    char buf[32];
    if (m >= 60)
        std::snprintf(buf, sizeof buf, "%d h %02d min", m / 60, m % 60);
    else
        std::snprintf(buf, sizeof buf, "%d min", m);
    return buf;
}

}  // namespace fp
