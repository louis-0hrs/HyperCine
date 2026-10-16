// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

// Call the real C symbol in the pristine SDK, rather than a copied filter.
extern "C" void FilterHorizontalRow16s(short*, short*, short*, int);
#ifdef HYPERCINE_ORACLE_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace {
constexpr int capacity = 256;
using Row = std::array<short, capacity>;
struct Result { Row low{}, high{}; };
int floor8(int n) { return n >= 0 ? n / 8 : -((-n + 7) / 8); }
short saturate(int n) { return static_cast<short>(std::clamp(n, -32768, 32767)); }
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Result run(const Row& row, int width, short a, short b, bool poison) {
    // Eight prefix samples preserve alignment. Extra suffix storage permits the
    // SDK's SIMD lookahead; this experiment varies ONLY input[-2] and input[-1].
    alignas(16) std::array<short, capacity + 32> storage{};
    alignas(16) Row low{}, high{};
    short* input = storage.data() + 8;
    std::copy_n(row.data(), width, input);
    input[-2] = a;
    input[-1] = b;
#ifdef HYPERCINE_ORACLE_ASAN
    if (poison) __asan_poison_memory_region(storage.data(), 16);
#else
    require(!poison, "--asan-prefix requires the instrumented target");
#endif
    FilterHorizontalRow16s(input, low.data(), high.data(), width);
#ifdef HYPERCINE_ORACLE_ASAN
    if (poison) __asan_unpoison_memory_region(storage.data(), 16);
#endif
    return {low, high};
}
void print(const Row& row, int count) {
    std::cout << '[';
    for (int i = 0; i < count; ++i) std::cout << (i ? "," : "") << row[i];
    std::cout << ']';
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--asan-prefix") {
            Row flat{};
            flat.fill(512);
            run(flat, 16, 512, 512, true);
            throw std::runtime_error("ASan did not detect the poisoned prefix read");
        }
        require(argc == 1, "unexpected argument");
        const std::array<int, 12> widths{12,14,16,18,20,24,32,64,120,128,192,240};
        const std::array<std::array<short, 2>, 5> guards{{
            {512,512}, {0,0}, {1024,1024}, {-1024,1024}, {4095,-4095}}};
        int cases = 0;
        std::cout << "{\"schema\":1,\"cases\":[\n";
        for (int width : widths) {
            for (int pattern = 0; pattern < 12; ++pattern) {
                Row row{};
                for (int x = 0; x < width; ++x) {
                    if (pattern == 0) row[x] = 512;
                    else if (pattern == 1) row[x] = static_cast<short>(x * 7 - 400);
                    else if (pattern == 2) row[x] = static_cast<short>(x % 2 ? 4095 : -4095);
                    else if (pattern == 3) row[x] = static_cast<short>((x * 1337 + 0xBEEF) % 8191 - 4095);
                    else if (pattern >= 4 && pattern < 10) {
                        const int positions[]{0,1,2,13,14,15};
                        row[x] = x == positions[pattern - 4] ? 1023 : 0;
                    } else row[x] = static_cast<short>(pattern == 10 ? -4095 : 4095);
                }
                const auto baseline = run(row, width, guards[0][0], guards[0][1], false);
                bool dependent = false;
                for (const auto& guard : guards) {
                    const auto result = run(row, width, guard[0], guard[1], false);
                    require(result.low == baseline.low, "prefix changed lowpass");
                    require(std::equal(result.high.begin() + 1, result.high.end(), baseline.high.begin() + 1),
                            "prefix changed a coefficient other than highpass[0]");
                    const bool small = width <= 16;
                    const int left = 5*row[0]-11*row[1]+4*row[2]+4*row[3]-row[4]-row[5];
                    const short expected = small
                        ? saturate(floor8(-guard[0]-guard[1]+row[2]+row[3]+4)+row[0]-row[1])
                        : saturate(floor8(left+4));
                    require(result.high[0] == expected, "unexpected first coefficient");
                    dependent |= result.high[0] != baseline.high[0];
                    // A repeated call must preserve every output sample.
                    const auto repeat = run(row, width, guard[0], guard[1], false);
                    require(result.low == repeat.low && result.high == repeat.high, "fixed guard nondeterminism");
                    std::cout << (cases++ ? ",\n" : "") << "{\"width\":" << width
                              << ",\"pattern\":" << pattern << ",\"guard\":[" << guard[0] << ',' << guard[1]
                              << "],\"input\":";
                    print(row, width);
                    std::cout << ",\"low\":"; print(result.low, width/2);
                    std::cout << ",\"high\":"; print(result.high, width/2);
                    std::cout << '}';
                }
                // This is a negative oracle regression, NOT a parity exemption
                // or a production geometry capability check.
                require(dependent == (width <= 16), "guard-dependence boundary changed");
            }
        }
        std::cout << "\n],\"case_count\":" << cases
                  << ",\"result\":\"PASS\",\"width16_oracle\":\"UPSTREAM_ORACLE_UNDEFINED\"}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "small-chroma oracle FAIL: " << error.what() << '\n';
        return 1;
    }
}
