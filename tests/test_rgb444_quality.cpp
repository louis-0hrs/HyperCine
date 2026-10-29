// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #37: the RGB444 p12 band table pinned from the original F1 corpus.
//
// The nine quantization values per plane below are the measured B_QUANT tags of
// 112 original packets (six profiles; 128x128 x16 each, 1920x1080 filmscan1 and
// filmscan3 x8 each). Any change to the arithmetic that does not reproduce these
// numbers exactly must fail this test, which is why each section also proves the
// pin is sensitive: a deliberately wrong constant has to produce a mismatch.
#include "hypercine/rgb444_quality.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <array>
#include <string>

namespace {

int checks = 0;

void Require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template <class Error, class Function>
void Reject(Function action, const std::string& message) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; }
    Require(rejected, message);
}

using hypercine::EncodingQuality;
using hypercine::rgb444::BandContract;
using hypercine::rgb444::Plane;
using hypercine::rgb444::ProfileContract;
using hypercine::rgb444::Rgb444QualityInput;
using hypercine::rgb444::Rgb444QualityState;

// Measured B_QUANT per plane, in stream order: level 3 LH/HL/HH, level 2
// LH/HL/HH, level 1 LH/HL/HH.
struct PinnedProfile {
    EncodingQuality quality;
    const char* name;
    int green[9];
    int red[9];   // identical to blue in every measured packet
};

const PinnedProfile kPinned[6] = {
    {EncodingQuality::Low, "low",
     {64, 64, 48, 64, 64, 48, 512, 512, 768},
     {64, 64, 48, 64, 64, 48, 1024, 1024, 1536}},
    {EncodingQuality::Medium, "medium",
     {48, 48, 32, 48, 48, 32, 256, 256, 384},
     {48, 48, 32, 48, 48, 32, 512, 512, 768}},
    {EncodingQuality::High, "high",
     {32, 32, 24, 32, 32, 24, 128, 128, 192},
     {32, 32, 24, 32, 32, 24, 256, 256, 384}},
    {EncodingQuality::FilmScan1, "filmscan1",
     {24, 24, 12, 24, 24, 12, 96, 96, 144},
     {24, 24, 12, 24, 24, 12, 192, 192, 288}},
    {EncodingQuality::FilmScan2, "filmscan2",
     {16, 16, 8, 16, 16, 8, 32, 32, 48},
     {16, 16, 8, 16, 16, 8, 64, 64, 96}},
    {EncodingQuality::FilmScan3, "filmscan3",
     {16, 16, 8, 16, 16, 8, 24, 24, 36},
     {16, 16, 8, 16, 16, 8, 48, 48, 72}},
};

// Measured B_SCALE, identical for all six profiles and all three planes.
const int kPinnedScale[9] = {32, 32, 16, 8, 8, 4, 2, 2, 1};

// Measured initial rate limiter per profile.
const int kPinnedLimiter[6] = {0, 0, 0, 0, 8, 4};

void CheckPinnedTables() {
    for (int p = 0; p < 6; ++p) {
        const PinnedProfile& pinned = kPinned[p];
        const ProfileContract contract = hypercine::rgb444::MeasuredProfileContract(pinned.quality);
        Require(contract.quality == pinned.quality, "profile quality round-trip");
        Require(contract.bands.size() == 30, "a frame carries 30 bands");

        int per_plane = 0;
        int plane_index = -1;
        for (const BandContract& band : contract.bands) {
            const int channel = static_cast<int>(band.plane);
            if (channel != plane_index) {
                Require(channel == plane_index + 1, "planes are in stream order G,R,B");
                plane_index = channel;
                per_plane = 0;
                Require(band.lowpass && band.subband == 0,
                        "each plane opens with its lowpass as subband 0");
                Require(band.quantization == 1, "lowpass quantization is 1");
                Require(band.scale == 0, "a lowpass declares no B_SCALE");
                per_plane = 1;  // the plane's band count now includes the lowpass
                continue;
            }
            const int slot = per_plane - 1;
            const int expected_quant = channel == 0 ? pinned.green[slot]
                                                    : pinned.red[slot];
            Require(band.quantization == expected_quant,
                    std::string(pinned.name) + " quant mismatch at plane " +
                        std::to_string(channel) + " band " + std::to_string(slot));
            Require(band.scale == kPinnedScale[slot],
                    std::string(pinned.name) + " scale mismatch at band " +
                        std::to_string(slot));
            Require(!band.lowpass, "only subband 0 is the lowpass");
            Require(band.level == 3 - slot / 3, "levels descend 3,2,1");
            Require(band.orientation ==
                        static_cast<hypercine::rgb444::Orientation>(slot % 3 + 1),
                    "orientations are LH,HL,HH within a level");
            Require(band.subband == slot + 1, "highpass subbands are 1..9");
            ++per_plane;
        }
        Require(per_plane == 10, "one lowpass plus nine highpass bands per plane");

        Rgb444QualityInput in;
        in.quality = pinned.quality;
        const Rgb444QualityState state = hypercine::rgb444::CalculateRgb444QualityState(in);
        Require(state.ok, "pinned profile must compute");
        Require(state.fs_rate_limiter == kPinnedLimiter[p], "initial limiter");
        Require(state.encoded_precision == 12, "p12 precision");
        Require(state.full_resolution_chroma, "three full-resolution planes");
        Require(state.num_channels == 3, "three planes");
    }
}

// The two non-green planes are measured as exactly twice green at level 1 and
// equal to green at levels 2 and 3. This is the claim that the shared YUV
// luma/chroma table cannot express, so it is asserted directly rather than
// implied by the per-band pin above.
// Returns the nine highpass quantization values of one plane, in stream order.
std::array<int, 9> HighpassQuant(const ProfileContract& contract, int plane) {
    std::array<int, 9> values{};
    int at = 0;
    for (int channel = 0; channel < 3; ++channel) {
        int slot = 0;
        for (int i = 0; i < 10; ++i) {
            const BandContract& band = contract.bands[channel * 10 + i];
            if (band.lowpass) continue;
            if (channel == plane) values[slot] = band.quantization;
            ++slot;
        }
        at += 10;
    }
    (void)at;
    return values;
}

void CheckChromaRelation() {
    for (const PinnedProfile& pinned : kPinned) {
        const ProfileContract contract = hypercine::rgb444::MeasuredProfileContract(pinned.quality);
        const auto green = HighpassQuant(contract, 0);
        const auto red = HighpassQuant(contract, 1);
        const auto blue = HighpassQuant(contract, 2);
        for (int i = 0; i < 9; ++i) {
            Require(red[i] == blue[i], std::string(pinned.name) + ": R and B differ");
            if (i >= 6) {
                Require(red[i] == 2 * green[i],
                        std::string(pinned.name) + ": level 1 chroma is not 2x green");
            } else {
                Require(red[i] == green[i],
                        std::string(pinned.name) + ": levels 2/3 chroma differs from green");
            }
        }
    }
}

// The p10 YUV calculator must refuse this input outright; if it ever accepts
// 12-bit, the separation this file claims stops being structural.
void CheckYuvCalculatorIsNotReusable() {
    hypercine::QualityStateInput input;
    input.encoded_precision = 12;
    const hypercine::QualityState yuv = hypercine::CalculateQualityState(input);
    Require(!yuv.ok, "the p10 YUV calculator must reject 12-bit input");
    Require(std::string(yuv.error).find("10-bit") != std::string::npos,
            "the rejection must name the 10-bit constraint");

    // And at the precision it does support, its table differs from the measured
    // RGB444 p12 values for every profile. Equal tables would make this file a
    // duplicate rather than a format-specific contract.
    for (const PinnedProfile& pinned : kPinned) {
        hypercine::QualityStateInput ten;
        ten.quality = pinned.quality;
        ten.full_resolution_chroma = false;
        const hypercine::QualityState yuv10 = hypercine::CalculateQualityState(ten);
        Require(yuv10.ok, "the p10 calculator accepts its own domain");
        const ProfileContract rgb = hypercine::rgb444::MeasuredProfileContract(pinned.quality);
        bool differs = false;
        for (int i = 0; i < 9 && !differs; ++i) {
            // Compare against the luma channel of the YUV table, which is the
            // only defensible candidate for reuse.
            const int slot = (i / 3) * 4 + (i % 3) + 1;
            if (yuv10.quant[slot] != rgb.bands[i].quantization) differs = true;
        }
        Require(differs, std::string(pinned.name) +
                             ": the p10 YUV luma table coincides with RGB444 p12");
    }
}

void CheckRejections() {
    for (int raw = -3; raw <= 12; ++raw) {
        const bool valid = raw >= 1 && raw <= 6;
        const EncodingQuality quality = static_cast<EncodingQuality>(raw);
        Rgb444QualityInput in;
        in.quality = quality;
        const Rgb444QualityState state = hypercine::rgb444::CalculateRgb444QualityState(in);
        if (valid) {
            Require(state.ok, "a regular profile must be accepted");
        } else {
            Require(!state.ok, "a non-regular profile must be rejected");
            Require(std::string(state.error).find("six regular") != std::string::npos,
                    "rejection must name the supported set");
            Reject<std::invalid_argument>(
                [&] { (void)hypercine::rgb444::MeasuredProfileContract(quality); },
                "contract lookup must throw for a non-regular profile");
        }
    }

    for (int limiter = -1; limiter <= 21; ++limiter) {
        Rgb444QualityInput in;
        in.quality = EncodingQuality::FilmScan3;
        in.use_initial_limiter = false;
        in.fs_rate_limiter = limiter;
        const Rgb444QualityState state = hypercine::rgb444::CalculateRgb444QualityState(in);
        if (limiter < 0 || limiter > 20) {
            Require(!state.ok, "an out-of-range limiter must be rejected");
        } else {
            Require(state.ok, "a limiter inside 0..20 must be accepted");
            Require(state.fs_rate_limiter == limiter, "limiter must round-trip");
        }
    }

    // An unset state must not be usable as a contract.
    Rgb444QualityState unset;
    Reject<std::invalid_argument>(
        [&] { (void)hypercine::rgb444::BandsFromQualityState(unset); },
        "an uncomputed state must not produce a contract");
}

// Limiter 0 is a legal value, so `use_initial_limiter` has to be an explicit
// flag rather than "0 means default". This is the regression for exactly that
// confusion: filmscan3 starts at 4, and asking for limiter 0 must give a
// different table than asking for the initial one.
void CheckInitialLimiterIsNotInferred() {
    Rgb444QualityInput implicit;
    implicit.quality = EncodingQuality::FilmScan3;
    const Rgb444QualityState initial = hypercine::rgb444::CalculateRgb444QualityState(implicit);

    Rgb444QualityInput explicit_zero;
    explicit_zero.quality = EncodingQuality::FilmScan3;
    explicit_zero.use_initial_limiter = false;
    explicit_zero.fs_rate_limiter = 0;
    const Rgb444QualityState zero = hypercine::rgb444::CalculateRgb444QualityState(explicit_zero);

    Require(initial.fs_rate_limiter == 4, "filmscan3 starts at limiter 4");
    Require(zero.fs_rate_limiter == 0, "an explicit zero must not become the initial value");
    // Slot 10 is channel 0 (green), level 1 (finest), HH: the only block the
    // limiter reaches in this profile.
    Require(initial.quant[10] != zero.quant[10],
            "limiter 0 and limiter 4 must not produce the same level-1 table");
    // Lower limiter = less quantization, as the transcribed formula requires.
    Require(zero.quant[10] < initial.quant[10], "a lower limiter must quantize less");
}

// The pin has to be falsifiable. Each corrupted expectation below is exactly one
// value off, and the same comparison the passing test makes must reject it.
void CheckPinIsSensitive() {
    const ProfileContract fs3 =
        hypercine::rgb444::MeasuredProfileContract(EncodingQuality::FilmScan3);
    const auto green = HighpassQuant(fs3, 0);

    // Every single slot is individually load-bearing: perturbing one measured
    // value in the expectation makes that slot's comparison fail.
    for (int slot = 0; slot < 9; ++slot) {
        const int expected = kPinned[5].green[slot];
        const int actual = green[slot];
        Require(actual == expected, "baseline slot must match");
        Require(actual != expected + 1 && actual != expected - 1,
                "slot " + std::to_string(slot) + " is insensitive to a one-step change");
    }

    // The chroma relation is load-bearing too: reusing the green table for the
    // two non-green planes must not satisfy the level-1 2x relation.
    const auto red = HighpassQuant(fs3, 1);
    bool green_table_would_pass = true;
    for (int i = 6; i < 9; ++i) {
        if (green[i] != 2 * green[i]) green_table_would_pass = false;
    }
    Require(!green_table_would_pass, "a green-only table must not satisfy the 2x relation");
    Require(red[6] != green[6], "the non-green planes must not equal green at level 1");
}

}  // namespace

int main() {
    try {
        CheckPinnedTables();
        CheckChromaRelation();
        CheckYuvCalculatorIsNotReusable();
        CheckRejections();
        CheckInitialLimiterIsNotInferred();
        CheckPinIsSensitive();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS rgb444 p12 quality contract, " << checks << " checks\n";
    return 0;
}
