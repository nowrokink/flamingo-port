// Checks the emulated console instructions (src/cpu_emul.cpp) against the AMD manual's definitions, by handing
// emulate_console_instruction the context an illegal-instruction exception would carry.
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "../src/cpu_emul.h"

namespace {

int g_failures = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

// Runs the instruction in `code` on `c`; true if it was emulated and RIP moved by `length`.
bool run(CONTEXT& c, const uint8_t* code, size_t length) {
    EXCEPTION_RECORD r{};
    r.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
    r.ExceptionAddress = const_cast<uint8_t*>(code);
    c.Rip = reinterpret_cast<DWORD64>(code);
    EXCEPTION_POINTERS ep{&r, &c};
    return rt::emulate_console_instruction(&ep) && c.Rip == reinterpret_cast<DWORD64>(code) + length;
}

}  // namespace

int main() {
    CONTEXT c{};

    // insertq xmm0, xmm1, 16, 16 (what the title runs): xmm1's low 16 bits into xmm0 bits [16, 32)
    const uint8_t insertq_imm[] = {0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10};
    c.Xmm0 = {0x1111222233334444ull, 0x5555666677778888ull};
    c.Xmm1 = {0xAAAABBBBCCCCDDDDull, 0x9999ull};
    CHECK(run(c, insertq_imm, sizeof insertq_imm));
    CHECK(c.Xmm0.Low == 0x11112222DDDD4444ull);
    CHECK(c.Xmm0.High == 0x5555666677778888ll);  // left alone
    CHECK(c.Xmm1.Low == static_cast<ULONGLONG>(0xAAAABBBBCCCCDDDDull));

    // insertq with a length of 0 (64) at 0: the whole quadword
    const uint8_t insertq_all[] = {0xF2, 0x0F, 0x78, 0xC1, 0x00, 0x00};
    c.Xmm0.Low = 0;
    CHECK(run(c, insertq_all, sizeof insertq_all));
    CHECK(c.Xmm0.Low == static_cast<ULONGLONG>(0xAAAABBBBCCCCDDDDull));

    // insertq xmm2, xmm9 (REX.B), lengths from xmm9's high quadword: 8 bits at 4
    const uint8_t insertq_reg[] = {0xF2, 0x41, 0x0F, 0x79, 0xD1};
    c.Xmm2 = {0, 0};
    c.Xmm9 = {0xFFull, 0x0408ull};  // [69:64] = 8, [77:72] = 4
    CHECK(run(c, insertq_reg, sizeof insertq_reg));
    CHECK(c.Xmm2.Low == 0xFF0ull);

    // extrq xmm3, 12, 8: bits [8, 20) to the bottom, zero-extended
    const uint8_t extrq_imm[] = {0x66, 0x0F, 0x78, 0xC3, 0x0C, 0x08};
    c.Xmm3 = {0x0000000000ABCDEFull, 0x1234ull};
    CHECK(run(c, extrq_imm, sizeof extrq_imm));
    CHECK(c.Xmm3.Low == 0xBCDull);
    CHECK(c.Xmm3.High == 0x1234ll);

    // extrq xmm12 (REX.R), xmm5: lengths from xmm5's low quadword: 4 bits at 4
    const uint8_t extrq_reg[] = {0x66, 0x44, 0x0F, 0x79, 0xE5};
    c.Xmm12 = {0xF0ull, 0};
    c.Xmm5 = {0x0404ull, 0};
    CHECK(run(c, extrq_reg, sizeof extrq_reg));
    CHECK(c.Xmm12.Low == 0xFull);

    // something else stays illegal
    const uint8_t ud2[] = {0x0F, 0x0B};
    CHECK(!run(c, ud2, sizeof ud2));

    if (g_failures) {
        std::fprintf(stderr, "cpu_emul_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("cpu_emul_test: ok\n");
    return 0;
}
