// BCPack decoder (shims/bcpack.cpp): self-checks, then, given a folder of samples, a byte-for-byte comparison.
//
//   bcpack_test.exe                 # self-checks only
//   bcpack_test.exe <folder>        # also every <name>.f<format>.s<size>.pack there (tools/bcpack_samples.py),
//                                   # against <name>.expected: the same chunk decoded by an independent decoder
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "../shims/bcpack.h"

namespace fs = std::filesystem;
using rt::bcpack::Result;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

void self_checks() {
    check(rt::bcpack::block_bytes(1) == 8 && rt::bcpack::block_bytes(3) == 16 && rt::bcpack::block_bytes(4) == 8 &&
              rt::bcpack::block_bytes(5) == 16 && rt::bcpack::block_bytes(6) == 16 &&
              rt::bcpack::block_bytes(7) == 16 && rt::bcpack::block_bytes(2) == 0,
          "block sizes");
    // supported() also says whether every rANS model of the tables tiles 65536.
    for (int f : {1, 3, 4, 5, 6, 7}) check(rt::bcpack::supported(f), "BC1/3/4/5/6H/7 supported (tables valid)");
    check(!rt::bcpack::supported(2) && !rt::bcpack::supported(0) && !rt::bcpack::supported(9),
          "BC2 and out-of-range formats unsupported");

    uint8_t out[256];
    const uint8_t junk[64] = {0xFF, 0x13, 0x77};
    check(rt::bcpack::decode(junk, sizeof junk, 2, out, 256) == Result::unsupported, "BC2 refused");
    check(rt::bcpack::decode(junk, sizeof junk, 1, out, 12) == Result::unsupported, "partial block refused");
    check(rt::bcpack::decode(junk, 0, 1, out, 256) == Result::bad_stream, "empty input refused");
    // Arbitrary bytes must fail cleanly (or, rarely, decode), never read past the input or crash.
    uint32_t seed = 12345;
    std::vector<uint8_t> noise(4096);
    for (int round = 0; round < 200; ++round) {
        for (auto& b : noise) b = static_cast<uint8_t>((seed = seed * 1664525 + 1013904223) >> 24);
        for (int format : {1, 3, 4, 5, 6, 7}) {
            std::vector<uint8_t> dest(4096);
            rt::bcpack::decode(noise.data(), noise.size() - (round % 97), format, dest.data(), dest.size());
        }
    }
}

void samples(const fs::path& dir) {
    const std::regex name(R"(\.f(\d)\.s(\d+)\.pack$)");
    int matched[8] = {}, differ[8] = {}, failed[8] = {};
    double format_seconds[8] = {}, format_bytes[8] = {};
    uint64_t bytes = 0;
    double seconds = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string file = entry.path().filename().string();
        std::smatch m;
        if (!std::regex_search(file, m, name)) continue;
        const int format = std::stoi(m[1]);
        const size_t size = std::stoull(m[2]);
        const fs::path expected_path = fs::path(entry.path()).replace_extension(".expected");
        if (!fs::exists(expected_path) || format < 0 || format > 7) continue;
        const auto packed = read_all(entry.path());
        const auto expected = read_all(expected_path);
        std::vector<uint8_t> out(size);
        const auto t0 = std::chrono::steady_clock::now();
        const Result r = rt::bcpack::decode(packed.data(), packed.size(), format, out.data(), out.size());
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        seconds += took;
        format_seconds[format] += took;
        format_bytes[format] += static_cast<double>(size);
        if (r != Result::ok) {
            std::fprintf(stderr, "FAIL: %s: not decoded (%d)\n", file.c_str(), static_cast<int>(r));
            ++failed[format];
        } else if (expected != out) {
            size_t at = 0;
            while (at < out.size() && at < expected.size() && out[at] == expected[at]) ++at;
            if (differ[format]++ < 3)
                std::fprintf(stderr, "FAIL: %s: differs at byte %zu (block %zu)\n", file.c_str(), at,
                             at / rt::bcpack::block_bytes(format));
        } else {
            ++matched[format];
            bytes += size;
        }
    }
    int total = 0;
    for (int f = 1; f < 8; ++f) {
        if (!matched[f] && !differ[f] && !failed[f]) continue;
        std::printf("samples %-4s: %d identical, %d differ, %d not decoded, %.0f MiB/s\n", rt::bcpack::name(f),
                    matched[f], differ[f], failed[f],
                    format_seconds[f] > 0 ? format_bytes[f] / 1048576.0 / format_seconds[f] : 0.0);
        g_failures += differ[f] + failed[f];
        total += matched[f];
    }
    std::printf("samples: %.1f MiB identical, %.0f MiB/s on one thread\n", bytes / 1048576.0,
                seconds > 0 ? bytes / 1048576.0 / seconds : 0.0);
    check(total > 0, "at least one sample compared");
}

}  // namespace

int main(int argc, char** argv) {
    self_checks();
    if (argc > 1) samples(argv[1]);
    std::printf(g_failures ? "bcpack_test: %d FAILED\n" : "bcpack_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
