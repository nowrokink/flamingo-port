#include "hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

namespace fp {

std::string sha1_hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[20] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return {};
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                             static_cast<ULONG>(data.size()), 0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool sha256_file(const std::wstring& path, std::string& hex, const std::atomic<bool>* stop) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    HANDLE h = ok ? CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr)
                  : INVALID_HANDLE_VALUE;
    ok = ok && h != INVALID_HANDLE_VALUE;
    std::vector<uint8_t> buf(8u << 20);
    while (ok) {
        if (stop && stop->load()) {
            ok = false;
            break;
        }
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) ok = false;
        if (!got) break;
        if (BCryptHashData(hash, buf.data(), got, 0) != 0) ok = false;
    }
    uint8_t digest[32];
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    static const char* digits = "0123456789abcdef";
    hex.clear();
    for (uint8_t b : digest) {
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    return true;
}

}  // namespace fp
