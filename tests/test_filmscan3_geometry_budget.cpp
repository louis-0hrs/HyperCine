// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Format legality vs resource budget.
//
// The bug this locks down: 4096x2160 and 8K were reported as *invalid geometry*
// although they meet the alignment required by our current 3-level YUV422 pipeline.
// They were merely too large for this build's session budget. A caller could not
// tell "this pipeline refuses the shape" from "this build cannot afford it", and the
// error text claimed "[128,4096] ... 8K unqualified" as if 8K were not a format.
//
// Contract proven here:
//   current pipeline shape -> no throw from the shape gate
//   legal but too big  -> std::length_error (budget)
//   unsupported shape  -> std::invalid_argument (current implementation)
//   width not %128     -> invalid_argument, even if it would fit the budget

#include "hypercine/filmscan3.h"
#include "hypercine/standalone.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using hypercine::standalone::FilmScan3Geometry;

int checks = 0;
int failures = 0;

void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// Returns "legal", "budget", "format" or the unexpected exception text.
std::string Classify(int width, int height) {
    FilmScan3Geometry g{};
    g.width = width;
    g.height = height;
    g.sample_capacity_bytes = 0; // let the capacity bound stay out of the way
    try {
        hypercine::standalone::ValidateFilmScan3Geometry(g);
        return "legal";
    } catch (const std::length_error&) {
        return "budget";
    } catch (const std::invalid_argument&) {
        return "format";
    } catch (const std::exception& e) {
        return std::string("unexpected: ") + e.what();
    }
}

} // namespace

int main() {
    // --- Legal and affordable: must pass the gate without throwing -----------
    Check(Classify(128, 24) == "legal", "128x24 legal");
    Check(Classify(1920, 1080) == "legal", "1920x1080 legal");
    Check(Classify(2048, 1080) == "legal", "2048x1080 legal");
    Check(Classify(3840, 2160) == "legal", "3840x2160 legal (exactly the budget)");

    // --- Legal format, unaffordable: budget error, NOT a format error --------
    // 4096x2160 = 8,847,360 px. Legal shape (4096%128==0, 2160%8==0), but above
    // the 8,294,400-pixel budget. This case is the whole point of the change.
    Check(Classify(4096, 2160) == "budget", "4096x2160 legal shape, budget error");
    Check(Classify(4096, 2024) == "legal", "4096x2024 legal and inside budget");

    // DCI/UHD 8K and beyond: legal shapes that no session here can allocate.
    Check(Classify(7680, 4320) == "budget", "7680x4320 legal shape, budget error");
    Check(Classify(8192, 4320) == "budget", "8192x4320 legal shape, budget error");
    Check(Classify(12288, 8040) == "budget", "12288x8040 legal shape, budget error");

    // 17520 is NOT a multiple of 128 (17520 = 136*128 + 112), so our
    // CURRENT FilmScan3 pipeline rejects it. Do not infer that an original
    // CineForm encoder cannot pad or represent this camera raster.
    Check(Classify(17520, 8040) == "format", "17520x8040 width not %128 -> format error");
    // The nearest legal widths on either side are.
    Check(Classify(17408, 8040) == "budget", "17408x8040 legal shape, budget error");
    Check(Classify(17536, 8040) == "budget", "17536x8040 legal shape, budget error");

    // --- Illegal shapes: format error ---------------------------------------
    Check(Classify(4095, 2160) == "format", "4095 width not %128 -> format");
    Check(Classify(3840, 2161) == "format", "3840x2161 height not %8 -> format");
    Check(Classify(0, 2160) == "format", "zero width -> format");
    Check(Classify(128, 16) == "format", "height below 24 -> format");
    Check(Classify(-128, 1080) == "format", "negative width -> format");

    // Above the 16-bit wire field: format-illegal, not merely large.
    Check(Classify(65664, 2160) == "format", "width above 65535 -> format (wire bound)");
    Check(Classify(65536, 2160) == "format", "width 65536 -> format (wire bound)");

    // Legacy Fixed-Q2 retains <=4096 and invalid_argument rejection,
    // even for shapes whose pixels would otherwise be under kMaxPixels.
    bool legacy_width = false, legacy_height = false;
    try { hypercine::standalone::ValidateGeometry(4224, 24); }
    catch (const std::invalid_argument&) { legacy_width = true; }
    try { hypercine::standalone::ValidateGeometry(128, 4104); }
    catch (const std::invalid_argument&) { legacy_height = true; }
    Check(legacy_width, "legacy fixed-Q2 width>4096 remains invalid_argument");
    Check(legacy_height, "legacy fixed-Q2 height>4096 remains invalid_argument");

    // --- The two failures must be distinguishable by type, not just by text --
    bool budget_is_length_error = false;
    bool format_is_invalid_argument = false;
    {
        FilmScan3Geometry big{};
        big.width = 7680;
        big.height = 4320;
        try {
            hypercine::standalone::ValidateFilmScan3Geometry(big);
        } catch (const std::length_error&) {
            budget_is_length_error = true;
        } catch (...) {
        }
    }
    {
        FilmScan3Geometry bad{};
        bad.width = 4095;
        bad.height = 2160;
        try {
            hypercine::standalone::ValidateFilmScan3Geometry(bad);
        } catch (const std::invalid_argument&) {
            format_is_invalid_argument = true;
        } catch (...) {
        }
    }
    Check(budget_is_length_error, "budget failure is std::length_error");
    Check(format_is_invalid_argument, "format failure is std::invalid_argument");

    std::printf("GEOMETRY-BUDGET %s checks=%d failures=%d\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}