// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "decode_phase3a_rc_support.h"

namespace DecodeRC {
// Direct tests feed metadata and coefficients to the existing kernel. They do
// not fabricate a CineForm bitstream or claim progressive production coverage.
inline int PeakUnits() {
    DeviceBuffer<uint8_t> sample(16); DeviceBuffer<int16_t> coeff(8);
    DeviceBuffer<GpuEntropyBandDescriptor> descriptors(2); DeviceBuffer<uint32_t> error(1);
    const std::vector<int16_t> original{-252, 250, 251, -250, 999, 0, -999, 12};
    Bytes bytes(16, 0); const int16_t replacements[]{-30000, 30000, 32767, -32768};
    std::memcpy(bytes.data() + 4, replacements, sizeof(replacements));
    GpuEntropyBandDescriptor base; base.has_peaks = true; base.peak_level = 250;
    base.peak_offset = 4; base.peak_count = 4; base.coefficient_count = 8;
    base.width = 8; base.height = 1; base.pitch_samples = 8; base.pitch_bytes = 16; base.d_destination = coeff.p;
    int count = 0;
    auto run = [&](GpuEntropyBandDescriptor desc, const Bytes& payload, uint32_t guard,
                   uint32_t expected_error, const std::vector<int16_t>* expected, bool difference = false) {
        GpuCheck(cudaMemcpy(sample.p, payload.data(), payload.size(), cudaMemcpyHostToDevice));
        GpuCheck(cudaMemcpy(coeff.p, original.data(), 16, cudaMemcpyHostToDevice));
        GpuCheck(cudaMemcpy(error.p, &guard, 4, cudaMemcpyHostToDevice));
        // Include an inactive band to exercise multiple descriptor blocks.
        GpuEntropyBandDescriptor pair[]{GpuEntropyBandDescriptor{}, desc};
        GpuCheck(cudaMemcpy(descriptors.p, pair, sizeof(pair), cudaMemcpyHostToDevice));
        GpuCheck(LaunchRestoreDecodePeaks(sample.p, payload.size(), descriptors.p, 2, nullptr, error.p));
        if (difference) GpuCheck(LaunchCooperativeDifferenceCodingGpu(descriptors.p + 1, 1, nullptr, error.p));
        uint32_t actual_error = 0; GpuCheck(cudaMemcpy(&actual_error, error.p, 4, cudaMemcpyDeviceToHost));
        Require(actual_error == expected_error, "RC peak error/guard exact");
        if (expected) {
            std::vector<int16_t> actual(8); GpuCheck(cudaMemcpy(actual.data(), coeff.p, 16, cudaMemcpyDeviceToHost));
            Require(actual == *expected, "RC restoration unit exact");
        }
        ++count;
    };
    auto restored = original; restored[0] = -30000; restored[2] = 30000; restored[4] = 32767; restored[6] = -32768;
    Require(restored != original, "historical omitted restoration differs");
    run(base, bytes, 0, 0, &restored);
    auto desc = base; desc.has_peaks = false; run(desc, bytes, 0, 0, &original);
    desc = base; desc.peak_level = 0; run(desc, bytes, 0, 0, &original);
    run(base, bytes, 73, 73, &original);
    const auto invalid = uint32_t(GpuBandDecodeStatus::InvalidDescriptor);
    desc = base; desc.d_destination = nullptr; run(desc, bytes, 0, invalid, &original);
    desc = base; desc.peak_count = 0; run(desc, bytes, 0, invalid, &original);
    desc = base; desc.peak_count = -1; run(desc, bytes, 0, invalid, &original);
    desc = base; desc.peak_offset = 17; run(desc, bytes, 0, invalid, &original);
    desc = base; desc.peak_offset = 16; run(desc, bytes, 0, invalid, &original);
    desc = base; desc.peak_count = 7; run(desc, bytes, 0, invalid, &original);
    desc = base; desc.peak_count = 2; run(desc, bytes, 0, uint32_t(GpuBandDecodeStatus::InputTruncated), nullptr);
    // Exactly one trailing zero padding entry is permitted.
    desc = base; desc.peak_level = 251; Bytes odd = bytes;
    std::memset(odd.data() + 10, 0, 2);
    auto odd_expected = original; odd_expected[0] = -30000; odd_expected[4] = 30000; odd_expected[6] = 32767;
    // Three escapes plus one zero pad produce the encoder's four-entry table.
    // Two unused entries are rejected even when both are zero.
    desc.peak_count = 5;
    run(desc, odd, 0, invalid, nullptr);
    desc.peak_count = 4;
    run(desc, odd, 0, 0, &odd_expected);
    odd[10] = 1; run(desc, odd, 0, invalid, nullptr);
    desc = base; desc.difference_coding = true;
    auto prefix = restored; for (size_t i = 1; i < prefix.size(); ++i) prefix[i] = int16_t(int(prefix[i]) + prefix[i - 1]);
    run(desc, bytes, 0, 0, &prefix, true);
    Require(LaunchRestoreDecodePeaks(nullptr, 16, descriptors.p, 2, nullptr, error.p) == cudaErrorInvalidValue, "RC null peak launch"); ++count;
    std::cout << "Peak restoration units " << count << " exact PASS\n" << std::flush;
    return count;
}

inline Bytes PeakInput(int w, int h, int p, bool ten_bit) {
    Bytes input(size_t(w) * h * (ten_bit ? 4 : 2));
    const int maximum = ten_bit ? 1023 : 255;
    auto value = [&](int x, int y, int channel) {
        switch (p) {
        case 0: return (x & 1) ? maximum : 0;
        case 1: return ((x + y) & 1) ? maximum : 0;
        case 2: return x % 127 == 1 && y % 113 == 1 ? maximum : 0;
        case 3: return x < w / 2 ? 0 : maximum;
        case 4: return (Noise(x, y, HoldoutSeed + uint32_t(channel)) & 1) ? maximum : 0;
        default: return channel ? (((x / 3 + y / 5) & 1) ? maximum : 0) : maximum / 2;
        }
    };
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
        const int y0 = value(x, y, 0), y1 = value(x + 1, y, 0), u = value(x, y, 1), v = value(x, y, 2);
        if (ten_bit) {
            uint16_t words[]{uint16_t(y0 << 6), uint16_t(v << 6), uint16_t(y1 << 6), uint16_t(u << 6)};
            std::memcpy(input.data() + (size_t(y) * w + x) * 4, words, 8);
        } else {
            const size_t at = (size_t(y) * w + x) * 2;
            input[at] = uint8_t(y0); input[at + 1] = uint8_t(u); input[at + 2] = uint8_t(y1); input[at + 3] = uint8_t(v);
        }
    }
    return input;
}

// Reproduce historical omission using raw entropy with difference disabled.
// Restore on the real table, then compare against CPU (before differencing for
// progressive samples). No production switch or public diagnostic is added.
inline size_t ProveRealPeaks(Sample& s, const std::vector<GpuEntropyBandDescriptor>& bands) {
    GpuFsmTableManager tables; Require(tables.Initialize(), "RC raw FSM init");
    DeviceBuffer<uint8_t> sample(s.compressed.size()); DeviceBuffer<GpuEntropyBandDescriptor> descriptor(1);
    DeviceBuffer<GpuBandDecodeResult> result(1); DeviceBuffer<uint32_t> error(1);
    GpuCheck(cudaMemcpy(sample.p, s.compressed.data(), s.compressed.size(), cudaMemcpyHostToDevice));
    size_t used = 0, changed = 0;
    for (auto b : bands) if (b.has_peaks && b.peak_level > 0) {
        DeviceBuffer<int16_t> destination(b.coefficient_count); b.d_destination = destination.p;
        b.difference_coding = false; // Observe escape sentinels before prefix sums.
        GpuCheck(cudaMemcpy(descriptor.p, &b, sizeof(b), cudaMemcpyHostToDevice));
        GpuCheck(LaunchDecodeEntropyGpu(sample.p, s.compressed.size(), descriptor.p, 1, tables.GetDeviceTable(), result.p, nullptr));
        GpuBandDecodeResult status; GpuCheck(cudaMemcpy(&status, result.p, sizeof(status), cudaMemcpyDeviceToHost));
        Require(status.status == GpuBandDecodeStatus::Complete, "RC raw entropy exact framing");
        std::vector<int16_t> old(b.coefficient_count), restored(b.coefficient_count);
        GpuCheck(cudaMemcpy(old.data(), destination.p, old.size() * 2, cudaMemcpyDeviceToHost));
        used += size_t(std::count_if(old.begin(), old.end(), [&](int16_t v) { return v > b.peak_level || v < -b.peak_level; }));
        GpuCheck(cudaMemset(error.p, 0, 4));
        GpuCheck(LaunchRestoreDecodePeaks(sample.p, s.compressed.size(), descriptor.p, 1, nullptr, error.p));
        uint32_t code; GpuCheck(cudaMemcpy(&code, error.p, 4, cudaMemcpyDeviceToHost)); Require(code == 0, "RC real peak table consumed");
        GpuCheck(cudaMemcpy(restored.data(), destination.p, restored.size() * 2, cudaMemcpyDeviceToHost));
        for (size_t i = 0; i < old.size(); ++i) if (old[i] != restored[i]) ++changed;
    }
    Require(used > 0 && changed > 0, "real peak escape and historical omission proven"); return used;
}

inline void PeakSearch(const std::filesystem::path& output) {
    const int units = PeakUnits();
    std::ofstream json(output); Require(bool(json), "RC peak JSON open");
    json << "{\"license\":\"MIT OR Apache-2.0\",\"unit_cases\":" << units
         << ",\"restoration_units_exact\":true,\"unit_old_omission_differences\":4,\"restoration_before_difference_exact\":true,\"attempts\":[\n";
    int attempts = 0, found = 0; size_t escapes = 0;
    const char* patterns[]{"alternating_extremes", "checkerboard", "impulses", "hard_transition", "random_extremes", "saturated_chroma"};
    for (int size = 0; size < 2; ++size) for (int fmt = 0; fmt < 2; ++fmt)
        for (int p = 0; p < 6; ++p) for (int q = 1; q <= 6; ++q) {
        const int w = size ? 1920 : 64, h = size ? 1080 : 64;
        std::cout << "peak attempt " << (attempts + 1) << ' ' << w << 'x' << h << ' '
                  << (fmt ? "YU64" : "YUY2") << ' ' << patterns[p] << " quality=" << q << '\n' << std::flush;
        auto input = PeakInput(w, h, p, fmt != 0);
        // The public SDK flag reserves extra output capacity for extreme data.
        // It changes no quantizer or coding policy and keeps progressive mode.
        auto s = EncodeInput(input, w, h, fmt ? CFHD_PIXEL_FORMAT_YU64 : CFHD_PIXEL_FORMAT_YUY2,
            CFHD_EncodingQuality(q), p, CFHD_ENCODING_FLAGS_LARGER_OUTPUT);
        std::vector<GpuEntropyBandDescriptor> bands; std::string reason; DecodeError status;
        const bool supported = ParseCineFormSampleValidated(s.compressed.data(), s.compressed.size(), 24, bands, &reason, &status);
        const int tables = int(std::count_if(bands.begin(), bands.end(), [](const auto& b) { return b.has_peaks && b.peak_level > 0; }));
        size_t count = 0;
        if (supported && tables) {
            count = ProveRealPeaks(s, bands); escapes += count;
            DecoderConfig cfg; cfg.maximum_width = w; cfg.maximum_height = h; cfg.slot_count = 1;
            Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "RC progressive peaks init"); Exact(s, d); ++found;
        }
        if (attempts++) json << ",\n";
        json << "{\"width\":" << w << ",\"height\":" << h << ",\"source_pattern\":\"" << patterns[p]
             << "\",\"input_format\":\"" << (fmt ? "YU64_10bit_values" : "YUY2") << "\",\"quality_enum\":" << q
             << ",\"encoding_flags\":2048,\"progressive\":true,\"supported\":" << (supported ? "true" : "false") << ",\"parser_error\":" << int(status)
             << ",\"compressed_bytes\":" << s.compressed.size() << ",\"source_hash_fnv1a64\":\"" << s.input_hash
             << "\",\"compressed_hash_fnv1a64\":\"" << Hash(s.compressed.data(), s.compressed.size())
             << "\",\"peak_tables\":" << tables << ",\"peak_escape_count\":" << count << ",\"end_to_end_exact\":" << (count ? "true" : "null") << '}';
        json.flush();
    }
    json << "],\"attempt_count\":" << attempts << ",\"progressive_samples_found\":" << found << ",\"peak_escape_count\":" << escapes
         << ",\"production_vector\":\"" << (found ? "FOUND" : "NOT FOUND") << "\",\"status\":\"PASS\"}\n";
    std::cout << "Progressive Peak-Escape search " << attempts << " attempts, " << found << " found\n" << std::flush;
}
} // namespace DecodeRC
