// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #37: the missing transformation stage of the RGB444 p12 family.
//
// This test pins the one thing that separates the p12 transform from the already
// merged p10 writer: the SDK's prescale table for 12-bit spatial transforms
// carries a non-zero entry at a second level. The pin comes from measurement, not
// from reasoning: tools/rgb444_prescale_probe.py reads CODEC_TAG_PRESCALE_TABLE out
// of all 24 packets of the frozen upstream sample and every one declares
// [0, 2, 2, 0, 0, 0, 0, 0].
//
// Every assertion here is written so it can fail. In particular:
//   * the p12 and p10 sequences must differ, otherwise "both are right" is vacuous;
//   * a pyramid built with the p10 sequence must produce different samples at the
//     coarse level than one built with the p12 sequence, on input chosen to have
//     energy at that level;
//   * the prescale record must be checkable independently of the pixel data, so a
//     caller can prove what ran instead of inferring it.
#include "hypercine/rgb444_transform.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

using hypercine::rgb444::Prescale;
using hypercine::rgb444::PrescaleSequenceForPrecision;
using hypercine::rgb444::Rgb444Frame;
using hypercine::rgb444::Rgb444Pyramid;
using hypercine::rgb444::TransformRgb444P12;

// Deterministic asymmetric pattern with energy in every band, seeded explicitly
// so the suite is reproducible across runs (AGENTS.md s6).
constexpr std::uint32_t kSeed = 0x1337BEEFu;

std::uint16_t Sample(int x, int y, int channel) {
    std::uint32_t state = kSeed ^ (static_cast<std::uint32_t>(x) * 0x9e3779b9u)
                        ^ (static_cast<std::uint32_t>(y) * 0x85ebca6bu)
                        ^ (static_cast<std::uint32_t>(channel) * 0xc2b2ae35u);
    state = state * 1664525u + 1013904223u;
    // Mix a coarse ramp (energy at level 3), a fine checkerboard (energy at
    // level 1) and noise, so no level can be silently all-zero.
    const int coarse = ((x / 64) + (y / 64)) % 2 ? 2000 : 200;
    const int fine = ((x + y) % 2) ? 300 : 0;
    const int noise = static_cast<int>((state >> 20) & 0x1F);
    return static_cast<std::uint16_t>((coarse + fine + noise + channel * 17) & 0x0FFF);
}

Rgb444Frame MakeFrame(int width, int height) {
    // AlignedVector, not std::vector: the canonical plane contract requires 16-byte
    // aligned addresses and row strides, and a plain vector's alignment is an
    // allocator detail rather than a guarantee. The rows are tightly packed here
    // because width * sizeof(uint16_t) is already a multiple of 16 for an even
    // width, so the stride rule is satisfied without padding.
    hypercine::standalone::AlignedVector<std::uint16_t> red(
        static_cast<std::size_t>(width) * height);
    hypercine::standalone::AlignedVector<std::uint16_t> green(red.size());
    hypercine::standalone::AlignedVector<std::uint16_t> blue(red.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto at = static_cast<std::size_t>(y) * width + x;
            red[at] = Sample(x, y, 0);
            green[at] = Sample(x, y, 1);
            blue[at] = Sample(x, y, 2);
        }
    }
    const std::size_t stride = static_cast<std::size_t>(width) * sizeof(std::uint16_t);
    hypercine::rgb444::FrameView view;
    view.width = width;
    view.height = height;
    view.red = {red.data(), red.size() * sizeof(std::uint16_t), stride};
    view.green = {green.data(), green.size() * sizeof(std::uint16_t), stride};
    view.blue = {blue.data(), blue.size() * sizeof(std::uint16_t), stride};
    return Rgb444Frame::Copy(view);
}

bool SamePlaneSamples(const hypercine::standalone::Plane& a,
                      const hypercine::standalone::Plane& b) {
    if (a.width != b.width || a.height != b.height) return false;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (a.pixels[static_cast<std::size_t>(y) * a.stride + x]
                != b.pixels[static_cast<std::size_t>(y) * b.stride + x])
                return false;
        }
    }
    return true;
}

// Sum of absolute values over one band, used to prove a band carries energy
// rather than to assert a specific value.
std::uint64_t BandEnergy(const hypercine::standalone::Plane& plane) {
    std::uint64_t total = 0;
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            const int value = plane.pixels[static_cast<std::size_t>(y) * plane.stride + x];
            total += static_cast<std::uint64_t>(value < 0 ? -value : value);
        }
    }
    return total;
}

// --- 1. The measured prescale contract -------------------------------------

void TestPrescaleSequence() {
    std::array<Prescale, 3> ten{};
    std::array<Prescale, 3> twelve{};

    Require(PrescaleSequenceForPrecision(10, &ten), "10-bit sequence must be defined");
    Require(PrescaleSequenceForPrecision(12, &twelve), "12-bit sequence must be defined");

    // The measured original p12 packets declare [0, 2, 2, ...]; the level binding
    // follows the merged p10 writer's validated convention, so level 1 is 0 in
    // both sequences and the difference sits at level 3.
    Require(twelve[0] == Prescale::None, "level 1 must not be prescaled at p12");
    Require(twelve[1] == Prescale::Quarter, "level 2 must be prescaled at p12");
    Require(twelve[2] == Prescale::Quarter, "level 3 must be prescaled at p12 (the p12-only entry)");

    Require(ten[0] == Prescale::None, "level 1 must not be prescaled at p10");
    Require(ten[1] == Prescale::Quarter, "level 2 must be prescaled at p10");
    Require(ten[2] == Prescale::None, "level 3 must not be prescaled at p10");

    // Anti-vacuity: if the two sequences were equal, the whole stage would be a
    // rename of the p10 path and every parity claim below would be meaningless.
    Require(!(ten == twelve), "p10 and p12 prescale sequences must differ");

    std::array<Prescale, 3> untouched{};
    Require(!PrescaleSequenceForPrecision(8, &untouched), "8-bit must be rejected, not defaulted");
    Require(!PrescaleSequenceForPrecision(14, &untouched), "14-bit must be rejected");
    Require(!PrescaleSequenceForPrecision(0, &untouched), "zero precision must be rejected");
    Require(!PrescaleSequenceForPrecision(12, nullptr), "a null output must be rejected");
}

// --- 2. A real transform produces a complete three-level pyramid -----------

void TestTransformShape() {
    const int width = 64, height = 64;  // even, >= 6, divisible by 8 so levels nest
    const auto frame = MakeFrame(width, height);
    const auto pyramid = TransformRgb444P12(frame);

    Require(pyramid.transformed, "a successful transform must set the completion flag");

    for (int level = 0; level < 3; ++level) {
        Require(pyramid.prescale[static_cast<std::size_t>(level)]
                    == (level == 0 ? Prescale::None : Prescale::Quarter),
                "recorded prescale must match the p12 sequence at level "
                    + std::to_string(level + 1));
    }

    // Full-resolution channels: at the finest level every band is width/2 x height/2,
    // and no channel is a half-width chroma plane.
    const int expected_width = width / 2;
    const int expected_height = height / 2;
    for (int channel = 0; channel < 3; ++channel) {
        for (int band = 0; band < 4; ++band) {
            const auto& plane =
                pyramid.channels[static_cast<std::size_t>(channel)][0].bands[static_cast<std::size_t>(band)];
            Require(plane.width == expected_width && plane.height == expected_height,
                    "level 1 band geometry must be half resolution in both axes");
        }
    }

    // Coarsest level is width/8.
    for (int channel = 0; channel < 3; ++channel) {
        const auto& plane =
            pyramid.channels[static_cast<std::size_t>(channel)][2].bands[0];
        Require(plane.width == width / 8 && plane.height == height / 8,
                "level 3 lowpass must be one eighth in both axes");
    }

    // Every channel must be non-trivial: identical channels would mean the
    // R/G/B mapping collapsed.
    const auto green = BandEnergy(pyramid.channels[0][0].bands[1]);
    const auto red = BandEnergy(pyramid.channels[1][0].bands[1]);
    const auto blue = BandEnergy(pyramid.channels[2][0].bands[1]);
    Require(green > 0 && red > 0 && blue > 0, "every channel must carry energy");
    Require(!SamePlaneSamples(pyramid.channels[0][0].bands[1],
                              pyramid.channels[1][0].bands[1]),
            "green and red must not be the same plane");

    // Pin the stream channel order and prove the stage really applies the
    // sequence it records. Without this, a stage that stores the right prescale
    // record but decomposes with zeroes would pass every assertion above: the
    // record would be correct, the geometry would be correct, and the samples
    // would be silently wrong. So the pyramid is compared against an independent
    // decomposition of the same source plane, done here by hand.
    //
    // The comparison also pins the channel mapping: the encoded stream is G/R/B
    // while the public frame names its planes red/green/blue. If the stage used
    // the frame's array order directly, channel 0 would be built from red here and
    // the comparison against the green decomposition would fail.
    const std::array<hypercine::rgb444::Channel, 3> source_order = {
        hypercine::rgb444::Channel::Green, hypercine::rgb444::Channel::Red,
        hypercine::rgb444::Channel::Blue};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto& source_plane = frame.plane(source_order[channel]);
        hypercine::standalone::Plane working(width, height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                working.pixels[static_cast<std::size_t>(y) * working.stride + x] =
                    static_cast<std::int16_t>(
                        source_plane[static_cast<std::size_t>(y) * (frame.stride_bytes() / 2) + x]);
            }
        }
        const auto level0 = hypercine::standalone::ForwardLevel(
            working, static_cast<int>(pyramid.prescale[0]));
        const auto level1 = hypercine::standalone::ForwardLevel(
            level0.bands[0], static_cast<int>(pyramid.prescale[1]));
        const auto level2 = hypercine::standalone::ForwardLevel(
            level1.bands[0], static_cast<int>(pyramid.prescale[2]));
        Require(SamePlaneSamples(pyramid.channels[channel][2].bands[1], level2.bands[1]),
                "the pyramid must equal an independent decomposition with the recorded "
                "prescale, at stream channel " + std::to_string(channel));
    }
}

// --- 3. The p12-only prescale entry has to change the coarse level ---------

void TestPrescaleIsDiscriminating() {
    const int width = 64, height = 64;
    const auto frame = MakeFrame(width, height);

    // Same decomposition, but forced through the p10 sequence. If level 3's
    // prescale were irrelevant the two pyramids would be identical and the whole
    // distinction this stage exists for would be unobservable.
    std::array<Prescale, 3> ten{};
    Require(PrescaleSequenceForPrecision(10, &ten), "10-bit sequence must be defined");

    const auto p12 = TransformRgb444P12(frame);
    Require(p12.transformed, "p12 transform must complete");

    const auto& p12_coarse = p12.channels[0][2].bands[1];
    // A prescale of 2 divides by four with rounding, so the coarse highpass of a
    // prescaled level is systematically smaller in magnitude than an unscaled one.
    // Compare against the unscaled decomposition of the same source: the level-3
    // highpass must differ, and the prescaled one must be the smaller of the two.
    Require(BandEnergy(p12_coarse) > 0, "coarse highpass must carry energy to be comparable");

    // Rebuild the coarse level without prescale by calling the stage sequence
    // contract: 10-bit prescales only level 2, so level 3 is unscaled there.
    // We cannot ask TransformRgb444P12 for a 10-bit run (it is p12 by contract),
    // so the discrimination is proven through the sequence itself plus a direct
    // ForwardLevel comparison on the same working data.
    hypercine::standalone::Plane working(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            working.pixels[static_cast<std::size_t>(y) * working.stride + x] =
                static_cast<std::int16_t>(Sample(x, y, 1));
        }
    }
    const auto level0 = hypercine::standalone::ForwardLevel(working, static_cast<int>(ten[0]));
    const auto level1 = hypercine::standalone::ForwardLevel(level0.bands[0], static_cast<int>(ten[1]));
    const auto level2_scaled = hypercine::standalone::ForwardLevel(
        level1.bands[0], static_cast<int>(ten[2]));
    const auto level2_p12 = hypercine::standalone::ForwardLevel(
        level1.bands[0], static_cast<int>(p12.prescale[2]));

    Require(!SamePlaneSamples(level2_scaled.bands[1], level2_p12.bands[1]),
            "level-3 prescale must change the decomposition, not just the record");
    Require(BandEnergy(level2_p12.bands[1]) < BandEnergy(level2_scaled.bands[1]),
            "a prescaled level-3 highpass must be smaller in magnitude than an unscaled one");
}

// --- 4. Rejection, with a reason ------------------------------------------

void TestRejection() {
    // Odd height cannot be decomposed by the 2-6 filter bank. The frame itself is
    // constructible (its rows are still 16-byte aligned), so the rejection is the
    // transform's own guard, not an artefact of building the input.
    const auto odd = MakeFrame(64, 63);
    Reject<std::invalid_argument>([&] { TransformRgb444P12(odd); },
                                   "an odd height must be rejected");

    // A height below the filter's minimum support of 6. Width must stay even and
    // keep its rows 16-byte aligned, otherwise the canonical frame cannot even be
    // built and the assertion would test the wrong guard.
    const auto tiny = MakeFrame(8, 4);
    Reject<std::invalid_argument>([&] { TransformRgb444P12(tiny); },
                                   "a frame under 6 samples must be rejected");
}

} // namespace

int main() {
    try {
        TestPrescaleSequence();
        TestTransformShape();
        TestPrescaleIsDiscriminating();
        TestRejection();
    } catch (const std::exception& error) {
        std::cerr << "rgb444 p12 transform contract failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "rgb444 p12 transform contract: " << checks << " checks passed\n";
    return 0;
}
