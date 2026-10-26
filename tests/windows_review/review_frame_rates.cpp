// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
// Negative review: non-30fps requests must not promise an applied timebase.
#include "encoder_session_config.h"
#include <iostream>
using namespace hypercine::standalone;
int main() {
    bool failed = false;
    for (auto fps : {Rational{30,1}, Rational{24,1}, Rational{25,1},
                     Rational{50,1}, Rational{60000,1001}, Rational{0,1}, Rational{30,0}}) {
        EncoderSessionConfig config;
        config.frame_rate = fps;
        const auto result = CheckEncoderCapability(config, EncoderDevice{});
        const bool expected = fps.numerator == 30 && fps.denominator == 1;
        std::cout << fps.numerator << '/' << fps.denominator
                  << " allocation=" << result.allocation_permitted
                  << " reason=" << CapabilityReasonName(result.primary) << '\n';
        if (result.allocation_permitted != expected) failed = true;
    }
    if (failed) std::cerr << "FAIL: unsupported writer timebase advertised as available\n";
    return failed ? 1 : 0;
}
