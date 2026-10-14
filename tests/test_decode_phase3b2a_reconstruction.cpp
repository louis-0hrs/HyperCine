// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decoder.h"
#include "hypercine/research/decoder_diagnostics.h"
#include "decode_phase0_support.h"
#include "wavelet.h"
#include "spatial.h"
#include "buffer.h"
#include "test_vectors.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" void ReconstructSampleFrameToBuffer(DECODER*, int, uint8_t*, int);

static std::atomic<uint64_t> g_cpp_allocations{0};
void* operator new(size_t size) {
    g_cpp_allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {

using HyperCineTest::AlignedVector;

void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    Require(bool(in), "open FIELDPLUS fixture");
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    Require(length > 0, "FIELDPLUS fixture size");
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    in.read(reinterpret_cast<char*>(bytes.data()), length);
    Require(bool(in), "read FIELDPLUS fixture");
    return bytes;
}

uint64_t HashAdd(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string HashBgra(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16)
        << HashAdd(14695981039346656037ULL, bytes.data(), bytes.size());
    return out.str();
}

struct CapturedWaveletMetadata {
    int width = 0;
    int height = 0;
    int level = 0;
    int wavelet_type = 0;
    int prescale = 0;
    std::array<int, 4> quantization{};
};

struct UpstreamFieldPlusCapture {
    std::vector<std::vector<int16_t>> bands;
    std::array<std::array<CapturedWaveletMetadata, 6>, 3> wavelets{};
    std::array<std::vector<int16_t>, 3> reconstructed_w5;
    std::array<std::vector<int16_t>, 3> reconstructed_temporal_low;
    std::array<std::vector<int16_t>, 3> reconstructed_temporal_high;
    std::array<std::array<std::vector<int16_t>, 2>, 3> reconstructed_base_ll;
};

std::vector<int16_t> CaptureUpstreamBand(const IMAGE* image, int band) {
    Require(image != nullptr && band >= 0 && band < image->num_bands && image->band[band] != nullptr,
        "upstream reconstructed band exists");
    const int stride = image->pitch / int(sizeof(PIXEL));
    std::vector<int16_t> result(size_t(image->width) * size_t(image->height));
    const auto* source = static_cast<const int16_t*>(image->band[band]);
    for (int y = 0; y < image->height; ++y)
        std::memcpy(result.data() + size_t(y) * size_t(image->width),
            source + size_t(y) * size_t(stride), size_t(image->width) * sizeof(int16_t));
    return result;
}

UpstreamFieldPlusCapture CaptureUpstreamFieldPlusBands(const std::vector<uint8_t>& sample,
    CFHD_PixelFormat pixel_format) {
    void* raw = nullptr;
    DecodePhase0::Check(CFHD_OpenDecoder(&raw, nullptr), "open upstream coefficient decoder");
    DecodePhase0::DecoderHandle handle(raw);
    int width = 0, height = 0;
    CFHD_PixelFormat format = CFHD_PIXEL_FORMAT_UNKNOWN;
    DecodePhase0::Check(CFHD_PrepareToDecode(raw, 0, 0, pixel_format,
        CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
        const_cast<uint8_t*>(sample.data()), sample.size(), &width, &height, &format), "prepare upstream coefficients");
    int32_t pitch = 0;
    DecodePhase0::Check(CFHD_GetImagePitch(width, format, &pitch), "get upstream BGRA pitch");
    std::vector<uint8_t> image(size_t(pitch) * size_t(height));
    auto* native = DecodePhase0::NativeDecoder(raw);
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(0x1337BEEF);
    DecodePhase0::Check(CFHD_DecodeSample(raw, const_cast<uint8_t*>(sample.data()), sample.size(), image.data(), pitch),
        "decode upstream coefficient reference");

    static constexpr int owners[17] = {5,5,5,5,4,4,4,3,3,3,3,1,1,1,0,0,0};
    static constexpr int owner_bands[17] = {0,1,2,3,1,2,3,0,1,2,3,1,2,3,1,2,3};
    UpstreamFieldPlusCapture out;
    out.bands.resize(51);
    for (int ch = 0; ch < 3; ++ch) {
        const auto* transform = native->transform[ch];
        Require(transform != nullptr, "upstream FIELDPLUS channel transform");
        // Capture the actual buffers produced by the frozen upstream decoder after
        // its complete inverse DAG. These are the stage oracle; replaying a spatial
        // helper with reconstructed IMAGE metadata can select a different path.
        out.reconstructed_w5[size_t(ch)] = CaptureUpstreamBand(transform->wavelet[4], 0);
        out.reconstructed_temporal_low[size_t(ch)] = CaptureUpstreamBand(transform->wavelet[2], 0);
        out.reconstructed_temporal_high[size_t(ch)] = CaptureUpstreamBand(transform->wavelet[2], 1);
        for (int frame = 0; frame < 2; ++frame)
            out.reconstructed_base_ll[size_t(ch)][size_t(frame)] =
                CaptureUpstreamBand(transform->wavelet[frame], 0);
        for (int wavelet_index = 0; wavelet_index < 6; ++wavelet_index) {
            const auto* wavelet = transform->wavelet[wavelet_index];
            Require(wavelet != nullptr, "upstream FIELDPLUS wavelet metadata");
            auto& metadata = out.wavelets[size_t(ch)][size_t(wavelet_index)];
            metadata.width = wavelet->width;
            metadata.height = wavelet->height;
            metadata.level = wavelet->level;
            metadata.wavelet_type = wavelet->wavelet_type;
            metadata.prescale = transform->prescale[wavelet_index];
            for (int band = 0; band < 4; ++band)
                metadata.quantization[size_t(band)] = wavelet->quantization[band];
        }
        for (int logical = 0; logical < 17; ++logical) {
            const auto* wavelet = transform->wavelet[owners[logical]];
            Require(wavelet && wavelet->band[owner_bands[logical]], "upstream FIELDPLUS coefficient band");
            const auto* src = wavelet->band[owner_bands[logical]];
            const int stride = wavelet->pitch / int(sizeof(PIXEL));
            auto& band = out.bands[size_t(ch * 17 + logical)];
            band.resize(size_t(wavelet->width) * size_t(wavelet->height));
            for (int y = 0; y < wavelet->height; ++y)
                std::memcpy(band.data() + size_t(y) * wavelet->width,
                    src + size_t(y) * stride, size_t(wavelet->width) * sizeof(int16_t));
        }
    }
    return out;
}

struct UpstreamTemporalStages {
    std::array<std::vector<int16_t>, 3> low;
    std::array<std::vector<int16_t>, 3> high;
    std::array<std::vector<int16_t>, 3> wavelet4_ll;
    std::array<std::array<std::vector<int16_t>, 2>, 3> base_ll;
};

std::vector<int16_t> RunUpstreamSpatialInverse(const CapturedWaveletMetadata& metadata,
    const std::array<std::vector<int16_t>, 4>& compact_bands, int scale) {
    const int width = metadata.width;
    const int height = metadata.height;
    const int output_width = 2 * width;
    const int output_height = 2 * height;
    auto sat16 = [](int32_t value) {
        return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, value)));
    };
    // Scalar reference follows the frozen upstream overflow-protected vertical
    // inverse and its narrow-row horizontal tail. In this build, entropy decode
    // has already applied each band's quantization multiplier.
    std::vector<int16_t> low(size_t(width) * size_t(output_height));
    std::vector<int16_t> high(size_t(width) * size_t(output_height));
    for (int row = 0; row < height; ++row) {
        for (int x = 0; x < width; ++x) {
            const auto at = [&](int band, int y) -> int32_t {
                return compact_bands[size_t(band)][size_t(y) * size_t(width) + size_t(x)];
            };
            int32_t even_low, odd_low, even_high, odd_high;
            if (row == 0) {
                even_low = (((11 * at(0, 0) - 4 * at(0, 1) + at(0, 2) + 4) >> 3) + at(2, 0)) >> 1;
                odd_low = (((5 * at(0, 0) + 4 * at(0, 1) - at(0, 2) + 4) >> 3) - at(2, 0)) >> 1;
                even_high = (((11 * at(1, 0) - 4 * at(1, 1) + at(1, 2) + 4) >> 3) + at(3, 0)) >> 1;
                odd_high = (((5 * at(1, 0) + 4 * at(1, 1) - at(1, 2) + 4) >> 3) - at(3, 0)) >> 1;
            } else if (row == height - 1) {
                // The overflow-protected deepest path keeps its LL pointer at
                // height-2; the ordinary spatial inverse advances it to the
                // final row. Highpass pointers reach height-1 in both paths.
                const int ll_row = metadata.level >= 4 ? row - 1 : row;
                even_low = (((5 * at(0, ll_row) + 4 * at(0, ll_row - 1) - at(0, ll_row - 2) + 4) >> 3) + at(2, row)) >> 1;
                odd_low = (((11 * at(0, ll_row) - 4 * at(0, ll_row - 1) + at(0, ll_row - 2) + 4) >> 3) - at(2, row)) >> 1;
                even_high = (((5 * at(1, row) + 4 * at(1, row - 1) - at(1, row - 2) + 4) >> 3) + at(3, row)) >> 1;
                odd_high = (((11 * at(1, row) - 4 * at(1, row - 1) + at(1, row - 2) + 4) >> 3) - at(3, row)) >> 1;
            } else {
                even_low = (((at(0, row - 1) - at(0, row + 1) + 4) >> 3) + at(0, row) + at(2, row)) >> 1;
                odd_low = (((-at(0, row - 1) + at(0, row + 1) + 4) >> 3) + at(0, row) - at(2, row)) >> 1;
                even_high = (((at(1, row - 1) - at(1, row + 1) + 4) >> 3) + at(1, row) + at(3, row)) >> 1;
                odd_high = (((-at(1, row - 1) + at(1, row + 1) + 4) >> 3) + at(1, row) - at(3, row)) >> 1;
            }
            low[size_t(2 * row) * size_t(width) + size_t(x)] = sat16(even_low);
            low[size_t(2 * row + 1) * size_t(width) + size_t(x)] = sat16(odd_low);
            high[size_t(2 * row) * size_t(width) + size_t(x)] = sat16(even_high);
            high[size_t(2 * row + 1) * size_t(width) + size_t(x)] = sat16(odd_high);
        }
    }

    std::vector<int16_t> output(size_t(output_width) * size_t(output_height));
    for (int y = 0; y < output_height; ++y) {
        const auto emit = [&](int32_t value) {
            return sat16(scale == 2 ? value * 2 : value >> 1);
        };
        for (int x = 0; x < width; ++x) {
            int32_t even, odd;
            const auto L = [&](int column) -> int32_t { return low[size_t(y) * size_t(width) + size_t(column)]; };
            const auto H = [&](int column) -> int32_t { return high[size_t(y) * size_t(width) + size_t(column)]; };
            if (x == 0) {
                even = ((11 * L(0) - 4 * L(1) + L(2) + 4) >> 3) + H(0);
                odd = ((5 * L(0) + 4 * L(1) - L(2) + 4) >> 3) - H(0);
            } else if (x == width - 1) {
                even = ((5 * L(x) + 4 * L(x - 1) - L(x - 2) + 4) >> 3) + H(x);
                odd = ((11 * L(x) - 4 * L(x - 1) + L(x - 2) + 4) >> 3) - H(x);
            } else {
                even = ((L(x - 1) - L(x + 1) + 4) >> 3) + L(x) + H(x);
                odd = ((-L(x - 1) + L(x + 1) + 4) >> 3) + L(x) - H(x);
            }
            output[size_t(y) * size_t(output_width) + size_t(2 * x)] = emit(even);
            output[size_t(y) * size_t(output_width) + size_t(2 * x + 1)] = emit(odd);
        }
    }
    return output;
}

struct SpatialVerticalReference {
    std::vector<int16_t> low;
    std::vector<int16_t> high;
    int width = 0;
    int height = 0;
};

SpatialVerticalReference ComputeUpstreamSpatialVertical(const CapturedWaveletMetadata& metadata,
    const std::array<std::vector<int16_t>, 4>& bands) {
    SpatialVerticalReference result;
    const int width = metadata.width;
    const int height = metadata.height;
    result.width = width;
    result.height = height * 2;
    result.low.resize(size_t(width) * size_t(height * 2));
    result.high.resize(size_t(width) * size_t(height * 2));
    const auto sat16 = [](int32_t value) {
        return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, value)));
    };
    for (int row = 0; row < height; ++row) for (int x = 0; x < width; ++x) {
        const auto at = [&](int band, int y) -> int32_t {
            return bands[size_t(band)][size_t(y) * size_t(width) + size_t(x)];
        };
        int32_t even_low, odd_low, even_high, odd_high;
        if (row == 0) {
            even_low = (((11 * at(0, 0) - 4 * at(0, 1) + at(0, 2) + 4) >> 3) + at(2, 0)) >> 1;
            odd_low = (((5 * at(0, 0) + 4 * at(0, 1) - at(0, 2) + 4) >> 3) - at(2, 0)) >> 1;
            even_high = (((11 * at(1, 0) - 4 * at(1, 1) + at(1, 2) + 4) >> 3) + at(3, 0)) >> 1;
            odd_high = (((5 * at(1, 0) + 4 * at(1, 1) - at(1, 2) + 4) >> 3) - at(3, 0)) >> 1;
        } else if (row == height - 1) {
            const int ll_row = metadata.level >= 4 ? row - 1 : row;
            even_low = (((5 * at(0, ll_row) + 4 * at(0, ll_row - 1) - at(0, ll_row - 2) + 4) >> 3) + at(2, row)) >> 1;
            odd_low = (((11 * at(0, ll_row) - 4 * at(0, ll_row - 1) + at(0, ll_row - 2) + 4) >> 3) - at(2, row)) >> 1;
            even_high = (((5 * at(1, row) + 4 * at(1, row - 1) - at(1, row - 2) + 4) >> 3) + at(3, row)) >> 1;
            odd_high = (((11 * at(1, row) - 4 * at(1, row - 1) + at(1, row - 2) + 4) >> 3) - at(3, row)) >> 1;
        } else {
            even_low = (((at(0, row - 1) - at(0, row + 1) + 4) >> 3) + at(0, row) + at(2, row)) >> 1;
            odd_low = (((-at(0, row - 1) + at(0, row + 1) + 4) >> 3) + at(0, row) - at(2, row)) >> 1;
            even_high = (((at(1, row - 1) - at(1, row + 1) + 4) >> 3) + at(1, row) + at(3, row)) >> 1;
            odd_high = (((-at(1, row - 1) + at(1, row + 1) + 4) >> 3) + at(1, row) - at(3, row)) >> 1;
        }
        result.low[size_t(2 * row) * size_t(width) + size_t(x)] = sat16(even_low);
        result.low[size_t(2 * row + 1) * size_t(width) + size_t(x)] = sat16(odd_low);
        result.high[size_t(2 * row) * size_t(width) + size_t(x)] = sat16(even_high);
        result.high[size_t(2 * row + 1) * size_t(width) + size_t(x)] = sat16(odd_high);
    }
    return result;
}

UpstreamTemporalStages CaptureUpstreamTemporalStages(const UpstreamFieldPlusCapture& coefficients) {
    UpstreamTemporalStages stages;
    const auto get_band = [&](int ch, int logical) -> const std::vector<int16_t>& {
        return coefficients.bands[size_t(ch * 17 + logical)];
    };
    const auto saturate16 = [](int32_t value) {
        return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, value)));
    };
    for (int ch = 0; ch < 3; ++ch) {
        stages.wavelet4_ll[size_t(ch)] = coefficients.reconstructed_w5[size_t(ch)];
        stages.low[size_t(ch)] = coefficients.reconstructed_temporal_low[size_t(ch)];
        stages.high[size_t(ch)] = coefficients.reconstructed_temporal_high[size_t(ch)];
        std::array<std::vector<int16_t>, 4> w5_bands;
        for (int band = 0; band < 4; ++band) w5_bands[size_t(band)] = get_band(ch, band);
        const auto scalar_w5 = RunUpstreamSpatialInverse(coefficients.wavelets[size_t(ch)][5], w5_bands,
            coefficients.wavelets[size_t(ch)][5].prescale);
        Require(scalar_w5 == stages.wavelet4_ll[size_t(ch)], "upstream w5 scalar boundary oracle");
        const auto& w4_metadata = coefficients.wavelets[size_t(ch)][4];
        const int width = w4_metadata.width * 2;
        Require(stages.low[size_t(ch)].size() == stages.high[size_t(ch)].size() &&
            stages.low[size_t(ch)].size() == size_t(width) * size_t(w4_metadata.height * 2),
            "FIELDPLUS temporal planes have matching geometry");
        for (int frame = 0; frame < 2; ++frame) {
            std::vector<int16_t> computed_base(stages.low[size_t(ch)].size());
            for (size_t i = 0; i < computed_base.size(); ++i) {
                int32_t combined = frame == 0
                    ? int32_t(stages.low[size_t(ch)][i]) - stages.high[size_t(ch)][i]
                    : int32_t(stages.low[size_t(ch)][i]) + stages.high[size_t(ch)][i];
                const int x = int(i % size_t(width));
                if (x < width - width % 40) combined = saturate16(combined);
                const int32_t shifted = combined >= 0 ? combined / 2 : -((-combined + 1) / 2);
                computed_base[i] = static_cast<int16_t>(shifted);
            }
            stages.base_ll[size_t(ch)][size_t(frame)] =
                coefficients.reconstructed_base_ll[size_t(ch)][size_t(frame)];
            Require(computed_base == stages.base_ll[size_t(ch)][size_t(frame)],
                "upstream temporal inverse base-LL oracle");
        }
    }
    return stages;
}

void CompareFieldPlusBands(HyperCine::Decoder& decoder, HyperCine::DecodeTicket ticket,
    const UpstreamFieldPlusCapture& reference_capture, int frame) {
    const auto& reference = reference_capture.bands;
    std::string first_mismatch;
    for (int ch = 0; ch < 3; ++ch) for (int logical = 0; logical < 17; ++logical) {
        if ((frame == 0 && logical >= 11 && logical <= 13) ||
            (frame == 1 && logical >= 14 && logical <= 16)) continue;
        std::vector<int16_t> actual;
        int width = 0, height = 0, pitch = 0;
        Require(HyperCine::DecoderInspector::DownloadFieldPlusBand(decoder, ticket, ch, logical,
            actual, &width, &height, &pitch), "download FIELDPLUS coefficient band");
        const auto& expected = reference[size_t(ch * 17 + logical)];
        Require(width > 0 && height > 0 && pitch >= width * int(sizeof(int16_t)), "FIELDPLUS coefficient geometry");
        bool band_reported = false;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int16_t got = actual[size_t(y) * size_t(pitch / int(sizeof(int16_t))) + size_t(x)];
            const int16_t want = expected[size_t(y) * size_t(width) + size_t(x)];
            if (got != want && !band_reported) {
                const auto detail = "FIELDPLUS coefficient mismatch ch=" + std::to_string(ch) +
                    " logical=" + std::to_string(logical) + " x=" + std::to_string(x) +
                    " y=" + std::to_string(y) + " got=" + std::to_string(got) +
                    " expected=" + std::to_string(want);
                std::cerr << detail << '\n';
                if (first_mismatch.empty()) first_mismatch = detail;
                band_reported = true;
            }
        }
    }
    if (!first_mismatch.empty()) throw std::runtime_error(first_mismatch);
}

size_t CountOffsetlessBaseLlDifferences(const UpstreamFieldPlusCapture& reference, int removed_offset) {
    size_t differences = 0;
    for (int channel = 0; channel < 3; ++channel) {
        const auto& base_ll = reference.bands[size_t(channel * 17)];
        for (int16_t expected : base_ll)
            differences += (int32_t(expected) - removed_offset) != int32_t(expected);
    }
    return differences;
}

void CompareTemporalStages(HyperCine::Decoder& decoder, HyperCine::DecodeTicket ticket, int frame,
    const UpstreamTemporalStages& reference) {
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<int16_t> low, high, wavelet4_ll, base_ll;
        int width = 0, height = 0;
        Require(HyperCine::DecoderInspector::DownloadFieldPlusTemporalStages(decoder, ticket, ch,
            low, high, wavelet4_ll, base_ll, &width, &height), "download FIELDPLUS temporal stages");
        const auto& expected_base = reference.base_ll[size_t(ch)][size_t(frame)];
        const auto require_stage = [&](const std::vector<int16_t>& actual,
            const std::vector<int16_t>& expected, const char* stage) {
            if (actual == expected) return;
            size_t first = 0, differences = 0;
            while (first < actual.size() && first < expected.size() && actual[first] == expected[first]) ++first;
            for (size_t i = 0; i < (std::min)(actual.size(), expected.size()); ++i)
                differences += actual[i] != expected[i];
            throw std::runtime_error(std::string("FIELDPLUS ") + stage + " mismatch channel=" +
                std::to_string(ch) + " frame=" + std::to_string(frame) + " differences=" +
                std::to_string(differences) + " first=" + std::to_string(first) + " got=" +
                std::to_string(first < actual.size() ? actual[first] : 0) + " expected=" +
                std::to_string(first < expected.size() ? expected[first] : 0));
        };
        require_stage(wavelet4_ll, reference.wavelet4_ll[size_t(ch)], "w5 inverse");
        require_stage(low, reference.low[size_t(ch)], "w4 temporal-low");
        require_stage(high, reference.high[size_t(ch)], "w3 temporal-high");
        require_stage(base_ll, expected_base, "selected base-LL");
        Require(width > 0 && height > 0 && low.size() == size_t(width) * size_t(height),
            "FIELDPLUS temporal stage geometry");
    }
}

std::string DecodeFrame(HyperCine::Decoder& decoder, const std::vector<uint8_t>& sample, int frame,
    HyperCine::DecodeOrientation orientation,
    const UpstreamFieldPlusCapture& coefficients, const UpstreamTemporalStages& stages) {
    HyperCine::DecodeRequest request{};
    request.frame_index = frame;
    request.orientation = orientation;
    HyperCine::DecodeError error = HyperCine::DecodeError::None;
    const auto ticket = decoder.QueueDecode(sample.data(), sample.size(), request, &error);
    if (!ticket) throw std::runtime_error("FIELDPLUS queue failed with error " + std::to_string(unsigned(error)));
    const auto status = decoder.WaitForDecode(ticket);
    if (status.state != HyperCine::DecodeState::Ready)
        throw std::runtime_error("FIELDPLUS frame " + std::to_string(frame) + " failed with error " +
            std::to_string(unsigned(status.error)));
    CompareFieldPlusBands(decoder, ticket, coefficients, frame);
    CompareTemporalStages(decoder, ticket, frame, stages);
    HyperCine::DeviceOutput output{};
    Require(decoder.GetDeviceOutput(ticket, &output) && output.plane_count == 1, "publish FIELDPLUS BGRA surface");
    Require(output.width == 64 && output.height == 64 && output.orientation == request.orientation,
        "FIELDPLUS BGRA output geometry/orientation");
    std::vector<uint8_t> image(size_t(output.width) * size_t(output.height) * 4);
    const auto copied = cudaMemcpy2D(image.data(), size_t(output.width) * 4,
        output.planes[0].device_pointer, output.planes[0].pitch_bytes,
        size_t(output.width) * 4, output.height, cudaMemcpyDeviceToHost);
    Require(copied == cudaSuccess, "download FIELDPLUS BGRA image");
    // The frozen BGRA oracle is hashed in its native bottom-up row order.
    // Normalize a top-down request before comparing so the hash checks pixels,
    // while the output metadata check above separately verifies orientation.
    if (orientation == HyperCine::DecodeOrientation::TopDown) {
        const size_t row_bytes = size_t(output.width) * 4;
        std::vector<uint8_t> reversed(image.size());
        for (int y = 0; y < output.height; ++y)
            std::memcpy(reversed.data() + size_t(output.height - 1 - y) * row_bytes,
                image.data() + size_t(y) * row_bytes, row_bytes);
        image.swap(reversed);
    }
    const auto hash = HashBgra(image);
    Require(decoder.ReleaseOutput(ticket), "release FIELDPLUS frame ticket");
    return hash;
}

std::string DecodeBgraHash(HyperCine::Decoder& decoder, const std::vector<uint8_t>& sample,
    int frame, int width, int height, HyperCine::DecodeOrientation orientation,
    HyperCine::DecodeMatrix matrix = HyperCine::DecodeMatrix::BT709,
    HyperCine::DecodeRange range = HyperCine::DecodeRange::ComputerGraphics) {
    HyperCine::DecodeRequest request{};
    request.frame_index = frame;
    request.orientation = orientation;
    request.matrix = matrix;
    request.range = range;
    HyperCine::DecodeError error = HyperCine::DecodeError::None;
    const auto ticket = decoder.QueueDecode(sample.data(), sample.size(), request, &error);
    if (!ticket) throw std::runtime_error("FIELDPLUS fixture queue failed with error " + std::to_string(unsigned(error)));
    const auto status = decoder.WaitForDecode(ticket);
    if (status.state != HyperCine::DecodeState::Ready)
        throw std::runtime_error("FIELDPLUS fixture decode failed with error " + std::to_string(unsigned(status.error)));
    HyperCine::DeviceOutput output{};
    Require(decoder.GetDeviceOutput(ticket, &output) && output.plane_count == 1,
        "publish FIELDPLUS fixture BGRA surface");
    Require(output.width == width && output.height == height && output.orientation == orientation,
        "FIELDPLUS fixture BGRA geometry and orientation");
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
    Require(cudaMemcpy2D(pixels.data(), size_t(width) * 4, output.planes[0].device_pointer,
        output.planes[0].pitch_bytes, size_t(width) * 4, height,
        cudaMemcpyDeviceToHost) == cudaSuccess, "download FIELDPLUS fixture BGRA");
    if (orientation == HyperCine::DecodeOrientation::TopDown) {
        const size_t row_bytes = size_t(width) * 4;
        std::vector<uint8_t> reversed(pixels.size());
        for (int y = 0; y < height; ++y)
            std::memcpy(reversed.data() + size_t(height - 1 - y) * row_bytes,
                pixels.data() + size_t(y) * row_bytes, row_bytes);
        pixels.swap(reversed);
    }
    const auto hash = HashBgra(pixels);
    Require(decoder.ReleaseOutput(ticket), "release FIELDPLUS fixture BGRA ticket");
    return hash;
}

struct PlanarCapture {
    std::array<std::string, 3> hashes;
    std::array<std::vector<int16_t>, 3> planes;
    std::array<int, 3> widths{};
    std::array<int, 3> heights{};
};

PlanarCapture DecodePlanar(HyperCine::Decoder& decoder,
    const std::vector<uint8_t>& sample, int frame,
    const UpstreamFieldPlusCapture* coefficients = nullptr,
    const UpstreamTemporalStages* stages = nullptr) {
    HyperCine::DecodeRequest request{};
    request.format = HyperCine::DecodePixelFormat::PlanarYUV422Signed16;
    request.frame_index = frame;
    HyperCine::DecodeError error = HyperCine::DecodeError::None;
    const auto ticket = decoder.QueueDecode(sample.data(), sample.size(), request, &error);
    if (!ticket) throw std::runtime_error("FIELDPLUS planar queue failed with error " + std::to_string(unsigned(error)));
    const auto status = decoder.WaitForDecode(ticket);
    if (status.state != HyperCine::DecodeState::Ready)
        throw std::runtime_error("FIELDPLUS planar frame failed with error " + std::to_string(unsigned(status.error)));
    if (coefficients) CompareFieldPlusBands(decoder, ticket, *coefficients, frame);
    if (stages) CompareTemporalStages(decoder, ticket, frame, *stages);
    if (coefficients && stages) {
        const int highpass_start = frame == 0 ? 14 : 11;
        for (int channel = 0; channel < 3; ++channel) {
            std::array<std::vector<int16_t>, 4> bands;
            bands[0] = stages->base_ll[size_t(channel)][size_t(frame)];
            for (int band = 1; band < 4; ++band)
                bands[size_t(band)] = coefficients->bands[size_t(channel * 17 + highpass_start + band - 1)];
            const auto expected = ComputeUpstreamSpatialVertical(
                coefficients->wavelets[size_t(channel)][size_t(frame)], bands);
            std::vector<int16_t> actual_low, actual_high;
            int actual_width = 0, actual_height = 0;
            Require(HyperCine::DecoderInspector::DownloadFieldPlusSpatialStages(decoder, ticket, channel,
                actual_low, actual_high, &actual_width, &actual_height), "download final spatial vertical stages");
            Require(actual_width == expected.width && actual_height == expected.height,
                "final spatial vertical stage geometry");
            const auto compare = [&](const std::vector<int16_t>& actual,
                const std::vector<int16_t>& reference, const char* name) {
                if (actual == reference) return;
                size_t first = 0;
                while (first < actual.size() && first < reference.size() && actual[first] == reference[first]) ++first;
                throw std::runtime_error(std::string("FIELDPLUS final ") + name +
                    " vertical mismatch channel=" + std::to_string(channel) + " frame=" +
                    std::to_string(frame) + " sample=" + std::to_string(first) + " got=" +
                    std::to_string(first < actual.size() ? actual[first] : 0) + " expected=" +
                    std::to_string(first < reference.size() ? reference[first] : 0));
            };
            compare(actual_low, expected.low, "lowpass");
            compare(actual_high, expected.high, "highpass");
        }
    }
    HyperCine::DeviceOutput output{};
    Require(decoder.GetDeviceOutput(ticket, &output) && output.plane_count == 3, "publish FIELDPLUS planar surface");
    PlanarCapture capture;
    for (int channel = 0; channel < 3; ++channel) {
        const auto& plane = output.planes[size_t(channel)];
        capture.widths[size_t(channel)] = plane.width;
        capture.heights[size_t(channel)] = plane.height;
        std::vector<uint8_t> data(size_t(plane.width) * size_t(plane.height) * sizeof(int16_t));
        Require(cudaMemcpy2D(data.data(), size_t(plane.width) * sizeof(int16_t), plane.device_pointer,
            plane.pitch_bytes, size_t(plane.width) * sizeof(int16_t), plane.height,
            cudaMemcpyDeviceToHost) == cudaSuccess, "download FIELDPLUS planar plane");
        std::ostringstream out;
        out << std::hex << std::setfill('0') << std::setw(16)
            << HashAdd(14695981039346656037ULL, data.data(), data.size());
        capture.hashes[size_t(channel)] = out.str();
        capture.planes[size_t(channel)].resize(size_t(plane.width) * size_t(plane.height));
        std::memcpy(capture.planes[size_t(channel)].data(), data.data(), data.size());
    }
    Require(decoder.ReleaseOutput(ticket), "release FIELDPLUS planar ticket");
    return capture;
}

std::array<std::string, 3> HashUpstreamYU64Planes(
    const std::vector<uint8_t>& packed, int pitch, int width, int height) {
    std::array<std::vector<uint16_t>, 3> planes;
    planes[0].resize(size_t(width) * size_t(height));
    planes[1].resize(size_t(width / 2) * size_t(height));
    planes[2].resize(size_t(width / 2) * size_t(height));
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(packed.data() + size_t(y) * pitch);
        for (int x = 0; x < width; x += 2) {
            const size_t pair = size_t(x) * 2;
            planes[0][size_t(y) * size_t(width) + size_t(x)] = row[pair];
            planes[0][size_t(y) * size_t(width) + size_t(x + 1)] = row[pair + 2];
            // CFHD's packed order is Y0,V,Y1,U; the frozen output-plane order is Y,U,V.
            planes[2][size_t(y) * size_t(width / 2) + size_t(x / 2)] = row[pair + 1];
            planes[1][size_t(y) * size_t(width / 2) + size_t(x / 2)] = row[pair + 3];
        }
    }
    std::array<std::string, 3> hashes;
    for (size_t plane = 0; plane < hashes.size(); ++plane) {
        std::ostringstream out;
        out << std::hex << std::setfill('0') << std::setw(16)
            << HashAdd(14695981039346656037ULL, planes[plane].data(),
                planes[plane].size() * sizeof(uint16_t));
        hashes[plane] = out.str();
    }
    return hashes;
}

std::array<AlignedVector<int16_t>, 3> ReconstructUpstreamSignedPlanes(
    const UpstreamFieldPlusCapture& coefficients,
    const UpstreamTemporalStages& stages, int frame) {
    std::array<AlignedVector<int16_t>, 3> result;
    const int final_highpass_start = frame == 0 ? 14 : 11;
    for (int channel = 0; channel < 3; ++channel) {
        const auto& metadata = coefficients.wavelets[size_t(channel)][size_t(frame)];
        const int width = metadata.width;
        const int height = metadata.height;
        Require(width > 0 && height > 0, "upstream final wavelet geometry");

        std::array<AlignedVector<PIXEL>, 4> input;
        const auto& base_ll = stages.base_ll[size_t(channel)][size_t(frame)];
        Require(base_ll.size() == size_t(width) * size_t(height), "upstream final base-LL size");
        input[0].assign(base_ll.begin(), base_ll.end());
        for (int band = 1; band < 4; ++band) {
            const auto& source = coefficients.bands[size_t(channel * 17 + final_highpass_start + band - 1)];
            Require(source.size() == size_t(width) * size_t(height), "upstream final highpass size");
            input[size_t(band)].assign(source.begin(), source.end());
        }

        const int input_pitch = width * int(sizeof(PIXEL));
        const int output_width = width * 2;
        const int output_height = height * 2;
        result[size_t(channel)].resize(size_t(output_width) * size_t(output_height));
        AlignedVector<PIXEL> scratch(size_t(width) * 32);
        ROI roi{width, height};
        int quantization[4]{};
        for (int band = 0; band < 4; ++band)
            quantization[band] = metadata.quantization[size_t(band)];

        if (metadata.prescale == 0) {
            auto inverse = metadata.level >= 4
                ? InvertSpatialQuantOverflowProtected16s
                : InvertSpatialQuant16s;
            inverse(input[0].data(), input_pitch, input[1].data(), input_pitch,
                input[2].data(), input_pitch, input[3].data(), input_pitch,
                result[size_t(channel)].data(), output_width * int(sizeof(PIXEL)), roi,
                scratch.data(), scratch.size() * sizeof(PIXEL), quantization);
        } else if (metadata.prescale == 1) {
            InvertSpatialQuant1x16s(input[0].data(), input_pitch, input[1].data(), input_pitch,
                input[2].data(), input_pitch, input[3].data(), input_pitch,
                result[size_t(channel)].data(), output_width * int(sizeof(PIXEL)), roi,
                scratch.data(), scratch.size() * sizeof(PIXEL), quantization);
        } else {
            InvertSpatialQuantDescale16s(input[0].data(), input_pitch, input[1].data(), input_pitch,
                input[2].data(), input_pitch, input[3].data(), input_pitch,
                result[size_t(channel)].data(), output_width * int(sizeof(PIXEL)), roi,
                scratch.data(), scratch.size() * sizeof(PIXEL), metadata.prescale, quantization);
        }
    }
    return result;
}

void ComparePlanarWithUpstreamSigned(const PlanarCapture& planar,
    const std::array<AlignedVector<int16_t>, 3>& upstream, const std::string& sample_name, int frame) {
    // Wavelet channels are Y,V,U; DeviceOutput planes are Y,U,V.
    static constexpr int plane_for_channel[3] = {0, 2, 1};
    for (int channel = 0; channel < 3; ++channel) {
        const int plane = plane_for_channel[channel];
        const auto& expected = upstream[size_t(channel)];
        const auto& actual = planar.planes[size_t(plane)];
        if (actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin())) continue;
        const size_t count = (std::min)(actual.size(), expected.size());
        size_t first = 0;
        while (first < count && actual[first] == expected[first]) ++first;
        throw std::runtime_error("FIELDPLUS signed-plane/upstream inverse mismatch for " + sample_name +
            " frame " + std::to_string(frame) + " channel=" + std::to_string(channel) +
            " plane=" + std::to_string(plane) + " sample=" + std::to_string(first) +
            ": got " + std::to_string(first < actual.size() ? actual[first] : 0) +
            ", expected " + std::to_string(first < expected.size() ? expected[first] : 0));
    }
}

std::vector<uint8_t> DecodeUpstreamYuv(const std::vector<uint8_t>& sample, int frame,
    int expected_width, int expected_height, int& pitch) {
    void* raw = nullptr;
    DecodePhase0::Check(CFHD_OpenDecoder(&raw, nullptr), "open upstream YU64 decoder");
    DecodePhase0::DecoderHandle handle(raw);
    int width = 0, height = 0;
    CFHD_PixelFormat format = CFHD_PIXEL_FORMAT_UNKNOWN;
    DecodePhase0::Check(CFHD_PrepareToDecode(raw, 0, 0, CFHD_PIXEL_FORMAT_YU64,
        CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
        const_cast<uint8_t*>(sample.data()), sample.size(), &width, &height, &format), "prepare upstream YU64");
    Require(width == expected_width && height == expected_height && format == CFHD_PIXEL_FORMAT_YU64,
        "upstream YU64 profile");
    int32_t signed_pitch = 0;
    DecodePhase0::Check(CFHD_GetImagePitch(width, format, &signed_pitch), "get upstream YU64 pitch");
    pitch = signed_pitch;
    std::vector<uint8_t> output(size_t(pitch) * size_t(height));
    auto* native = DecodePhase0::NativeDecoder(raw);
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(0x1337BEEF);
    DecodePhase0::Check(CFHD_DecodeSample(raw, const_cast<uint8_t*>(sample.data()), sample.size(), output.data(), pitch),
        "decode upstream YU64 frame 0");
    if (frame == 1) ReconstructSampleFrameToBuffer(native, frame, output.data(), pitch);
    return output;
}

std::string DecodeUpstreamBgraHash(const std::vector<uint8_t>& sample, int frame,
    int expected_width, int expected_height,
    HyperCine::DecodeMatrix matrix = HyperCine::DecodeMatrix::BT709,
    HyperCine::DecodeRange range = HyperCine::DecodeRange::ComputerGraphics) {
    void* raw = nullptr;
    DecodePhase0::Check(CFHD_OpenDecoder(&raw, nullptr), "open upstream BGRA decoder");
    DecodePhase0::DecoderHandle handle(raw);
    int width = 0, height = 0;
    CFHD_PixelFormat format = CFHD_PIXEL_FORMAT_UNKNOWN;
    DecodePhase0::Check(CFHD_PrepareToDecode(raw, 0, 0, CFHD_PIXEL_FORMAT_BGRA,
        CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
        const_cast<uint8_t*>(sample.data()), sample.size(), &width, &height, &format), "prepare upstream BGRA");
    Require(width == expected_width && height == expected_height && format == CFHD_PIXEL_FORMAT_BGRA,
        "upstream BGRA profile");
    void* metadata_raw = nullptr;
    DecodePhase0::Check(CFHD_OpenMetadata(&metadata_raw), "open upstream color metadata");
    DecodePhase0::Metadata metadata(metadata_raw);
    DecodePhase0::Check(CFHD_InitSampleMetadata(metadata_raw, METADATATYPE_ORIGINAL,
        const_cast<uint8_t*>(sample.data()), uint32_t(sample.size())), "init upstream color metadata");
    uint32_t matrix_value = matrix == HyperCine::DecodeMatrix::BT709 ? 2u : 1u;
    uint32_t range_value = range == HyperCine::DecodeRange::ComputerGraphics ? 1u : 2u;
    DecodePhase0::Check(CFHD_SetActiveMetadata(raw, metadata_raw, TAG_COLORSPACE_YUV,
        METADATATYPE_UINT32, &matrix_value, sizeof(matrix_value)), "set upstream FIELDPLUS matrix");
    DecodePhase0::Check(CFHD_SetActiveMetadata(raw, metadata_raw, TAG_COLORSPACE_RGB,
        METADATATYPE_UINT32, &range_value, sizeof(range_value)), "set upstream FIELDPLUS range");
    int32_t pitch = 0;
    DecodePhase0::Check(CFHD_GetImagePitch(width, format, &pitch), "get upstream BGRA pitch");
    std::vector<uint8_t> output(size_t(pitch) * size_t(height));
    auto* native = DecodePhase0::NativeDecoder(raw);
    native->cfhddata.cpu_limit = 1;
    native->thread_cntrl.limit = 1;
    native->thread_cntrl.set_thread_params = 1;
    native->thread_cntrl.capabilities = (native->thread_cntrl.capabilities & 0xffff) | (1 << 16);
    std::srand(0x1337BEEF);
    DecodePhase0::Check(CFHD_DecodeSample(raw, const_cast<uint8_t*>(sample.data()), sample.size(), output.data(), pitch),
        "decode upstream BGRA frame 0");
    if (frame == 1) ReconstructSampleFrameToBuffer(native, frame, output.data(), pitch);
    uint64_t hash = 14695981039346656037ULL;
    for (int y = 0; y < height; ++y)
        hash = HashAdd(hash, output.data() + size_t(y) * size_t(pitch), size_t(width) * 4);
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << hash;
    return result.str();
}

std::vector<uint8_t> Generate4kFieldPlusSample(std::string& source_hash) {
    constexpr int width = 3840, height = 2160;
    uint64_t source = 14695981039346656037ULL;
    void* raw = nullptr;
    DecodePhase0::Check(CFHD_OpenEncoder(&raw, nullptr), "open 4K FIELDPLUS encoder");
    DecodePhase0::Encoder encoder(raw);
    DecodePhase0::Check(CFHD_PrepareToEncode(raw, width, height, CFHD_PIXEL_FORMAT_YUY2,
        CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_YUV_2FRAME_GOP,
        CFHD_ENCODING_QUALITY_FILMSCAN3), "prepare 4K FIELDPLUS encoder");
    for (int frame = 0; frame < 2; ++frame) {
        AlignedVector<uint8_t> pixels(size_t(width) * size_t(height) * 2);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; x += 2) {
                const size_t i = (size_t(y) * size_t(width) + size_t(x)) * 2;
                const int motion = (x * 3 + y * 5 + frame * 71) % 220;
                pixels[i] = static_cast<uint8_t>(16 + motion);
                pixels[i + 2] = static_cast<uint8_t>(16 + ((motion + x / 17 + y / 13) % 220));
                pixels[i + 1] = static_cast<uint8_t>(32 + ((x / 8 + y / 3 + frame * 47) % 192));
                pixels[i + 3] = static_cast<uint8_t>(32 + ((x / 5 + y / 11 + frame * 83) % 192));
            }
        }
        source = HashAdd(source, pixels.data(), pixels.size());
        std::srand(0x1337BEEF);
        DecodePhase0::Check(CFHD_EncodeSample(raw, pixels.data(), width * 2), "encode 4K FIELDPLUS frame");
    }
    void* encoded = nullptr;
    size_t encoded_size = 0;
    DecodePhase0::Check(CFHD_GetSampleData(raw, &encoded, &encoded_size), "get 4K FIELDPLUS sample");
    Require(encoded != nullptr && encoded_size != 0 && encoded_size < 32u * 1024u * 1024u,
        "4K FIELDPLUS encoded sample fits production size limit");
    DecodePhase0::Bytes normalized(encoded_size);
    std::memcpy(normalized.data(), encoded, encoded_size);
    DecodePhase0::NormalizeMetadata(normalized);
    std::vector<uint8_t> sample(normalized.begin(), normalized.end());
    std::ostringstream hash;
    hash << std::hex << std::setfill('0') << std::setw(16) << source;
    source_hash = hash.str();
    return sample;
}

void Run4kSmoke(const std::filesystem::path& root, const std::filesystem::path& output_path) {
    (void)root;
    constexpr int width = 3840, height = 2160;
    std::string source_hash;
    const auto sample = Generate4kFieldPlusSample(source_hash);
    const auto frame0_reference = DecodeUpstreamBgraHash(sample, 0, width, height);
    const auto frame1_reference = DecodeUpstreamBgraHash(sample, 1, width, height);
    Require(frame0_reference != frame1_reference, "4K temporal-asymmetric reference frames differ");

    HyperCine::DecoderConfig config{};
    config.maximum_width = width;
    config.maximum_height = height;
    config.slot_count = 2;
    HyperCine::Decoder decoder(config);
    Require(decoder.Initialize() == HyperCine::DecodeError::None, "initialize 4K production decoder");
    const auto memory_before = decoder.MemoryStats();
    const auto frame0 = DecodeBgraHash(decoder, sample, 0, width, height,
        HyperCine::DecodeOrientation::BottomUp);
    const auto memory_after_frame0 = decoder.MemoryStats();
    const auto frame1 = DecodeBgraHash(decoder, sample, 1, width, height,
        HyperCine::DecodeOrientation::BottomUp);
    const auto memory_after_frame1 = decoder.MemoryStats();
    Require(frame0 == frame0_reference, "4K FIELDPLUS frame 0 matches upstream BGRA");
    Require(frame1 == frame1_reference, "4K FIELDPLUS frame 1 matches upstream BGRA");
    Require(memory_after_frame0.cuda_allocation_calls == memory_after_frame1.cuda_allocation_calls &&
        memory_after_frame0.device_bytes == memory_after_frame1.device_bytes &&
        memory_after_frame0.pinned_host_bytes == memory_after_frame1.pinned_host_bytes &&
        memory_after_frame0.pageable_capacity_bytes == memory_after_frame1.pageable_capacity_bytes,
        "4K FIELDPLUS has no allocation between warmed frame requests");

    const uint64_t sample_fnv = HashAdd(14695981039346656037ULL, sample.data(), sample.size());
    const auto hex = [](uint64_t value) {
        std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << value; return s.str();
    };
    const auto stats = memory_after_frame1;
    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::trunc);
    Require(bool(out), "open 4K qualification JSON");
    out << "{\n  \"license\": \"MIT OR Apache-2.0\",\n"
        << "  \"status\": \"PASS\",\n  \"width\": 3840,\n  \"height\": 2160,\n"
        << "  \"profile\": \"progressive FIELDPLUS/GOP-2 YUV422 p10 FilmScan3\",\n"
        << "  \"quality\": \"FilmScan3\",\n  \"encoder_flags\": \"YUV_2FRAME_GOP\",\n"
        << "  \"source_fnv1a64\": \"" << source_hash << "\",\n"
        << "  \"sample_fnv1a64\": \"" << hex(sample_fnv) << "\",\n"
        << "  \"sample_bytes\": " << sample.size() << ",\n"
        << "  \"upstream_bgra_fnv1a64\": [\"" << frame0_reference << "\", \"" << frame1_reference << "\"],\n"
        << "  \"hypercine_bgra_fnv1a64\": [\"" << frame0 << "\", \"" << frame1 << "\"],\n"
        << "  \"exact_frame0\": true,\n  \"exact_frame1\": true,\n"
        << "  \"device_bytes_total\": " << stats.device_bytes << ",\n"
        << "  \"device_bytes_per_slot\": " << (stats.device_bytes - stats.immutable_device_bytes) / size_t(stats.slot_count) << ",\n"
        << "  \"immutable_device_bytes\": " << stats.immutable_device_bytes << ",\n"
        << "  \"pinned_bytes_total\": " << stats.pinned_host_bytes << ",\n"
        << "  \"pinned_bytes_per_slot\": " << stats.pinned_host_bytes / size_t(stats.slot_count) << ",\n"
        << "  \"pageable_owned_capacity_bytes\": " << stats.pageable_capacity_bytes << ",\n"
        << "  \"cuda_allocation_calls_after_slot_setup\": " << stats.cuda_allocation_calls << ",\n"
        << "  \"steady_state_allocation_delta\": 0,\n"
        << "  \"memory_stats_before_first_frame\": {\"device_bytes\":" << memory_before.device_bytes
        << ",\"pinned_bytes\":" << memory_before.pinned_host_bytes
        << ",\"pageable_capacity_bytes\":" << memory_before.pageable_capacity_bytes
        << ",\"cuda_allocation_calls\":" << memory_before.cuda_allocation_calls << "},\n"
        << "  \"memory_stats_after_first_frame\": {\"device_bytes\":" << memory_after_frame0.device_bytes
        << ",\"pinned_bytes\":" << memory_after_frame0.pinned_host_bytes
        << ",\"pageable_capacity_bytes\":" << memory_after_frame0.pageable_capacity_bytes
        << ",\"cuda_allocation_calls\":" << memory_after_frame0.cuda_allocation_calls << "},\n"
        << "  \"streams_events\": \"owned counts not exposed by current diagnostics; see resource count limitation\"\n}\n";
    Require(bool(out), "write 4K qualification JSON");
    decoder.Release();
    std::cout << "4K progressive FIELDPLUS frame0/frame1 exact; source=" << source_hash
        << " sample=" << hex(sample_fnv) << " device_bytes=" << stats.device_bytes
        << " pinned_bytes=" << stats.pinned_host_bytes << '\n';
}

void Run1080Performance(const std::filesystem::path& root, const std::filesystem::path& output_path) {
    constexpr int width = 1920, height = 1080;
    const auto sample = ReadFile(root / "tests/fixtures/phase3b2a/motion_1080p_progressive.cfhd");
    HyperCine::Decoder decoder;
    Require(decoder.Initialize() == HyperCine::DecodeError::None, "initialize 1080 performance decoder");
    const std::array<std::string, 2> expected{{"7f633a5358642abd", "6d174c1993f61015"}};
    auto measure_frame = [&](int frame) {
        for (int i = 0; i < 5; ++i)
            Require(DecodeBgraHash(decoder, sample, frame, width, height,
                HyperCine::DecodeOrientation::BottomUp) == expected[size_t(frame)],
                "1080 performance warm-up exactness");
        std::vector<double> milliseconds;
        milliseconds.reserve(120);
        for (int i = 0; i < 120; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto hash = DecodeBgraHash(decoder, sample, frame, width, height,
                HyperCine::DecodeOrientation::BottomUp);
            const auto stop = std::chrono::steady_clock::now();
            Require(hash == expected[size_t(frame)], "1080 performance exact output");
            milliseconds.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        }
        std::sort(milliseconds.begin(), milliseconds.end());
        const double mean = std::accumulate(milliseconds.begin(), milliseconds.end(), 0.0) /
            double(milliseconds.size());
        double variance = 0.0;
        for (double value : milliseconds) variance += (value - mean) * (value - mean);
        variance /= double(milliseconds.size());
        const auto percentile = [&](double p) {
            const size_t index = size_t(std::ceil(p * double(milliseconds.size()))) - 1;
            return milliseconds[(std::min)(index, milliseconds.size() - 1)];
        };
        return std::array<double, 7>{{mean, percentile(0.50), percentile(0.95),
            milliseconds.front(), milliseconds.back(), std::sqrt(variance), 1000.0 / mean}};
    };
    const auto frame0 = measure_frame(0);
    const auto frame1 = measure_frame(1);
    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::trunc);
    Require(bool(out), "open performance JSON");
    out << "{\n  \"license\": \"MIT OR Apache-2.0\",\n"
        << "  \"status\": \"PASS_END_TO_END_LATENCY\",\n"
        << "  \"sample\": \"tests/fixtures/phase3b2a/motion_1080p_progressive.cfhd\",\n"
        << "  \"resolution\": \"1920x1080\",\n  \"iterations_per_frame\": 120,\n"
        << "  \"timing_scope\": \"QueueDecode through WaitForDecode, BGRA device-to-host copy, hash verification, and ReleaseOutput\",\n"
        << "  \"stage_timing\": \"not exposed by current FIELDPLUS diagnostics\",\n"
        << "  \"same_session_gop1_regression_percent\": null,\n"
        << "  \"frame0\": {\"mean_ms\":" << frame0[0] << ",\"median_ms\":" << frame0[1]
        << ",\"p95_ms\":" << frame0[2] << ",\"min_ms\":" << frame0[3]
        << ",\"max_ms\":" << frame0[4] << ",\"stddev_ms\":" << frame0[5]
        << ",\"fps_equivalent\":" << frame0[6] << "},\n"
        << "  \"frame1\": {\"mean_ms\":" << frame1[0] << ",\"median_ms\":" << frame1[1]
        << ",\"p95_ms\":" << frame1[2] << ",\"min_ms\":" << frame1[3]
        << ",\"max_ms\":" << frame1[4] << ",\"stddev_ms\":" << frame1[5]
        << ",\"fps_equivalent\":" << frame1[6] << "}\n}\n";
    Require(bool(out), "write performance JSON");
    decoder.Release();
    std::cout << "1080 FIELDPLUS latency frame0 mean=" << frame0[0] << " ms p95=" << frame0[2]
        << " ms; frame1 mean=" << frame1[0] << " ms p95=" << frame1[2] << " ms\n";
}

void RunMatrixRange(const std::filesystem::path& root, const std::filesystem::path& output_path) {
    constexpr int width = 64, height = 64;
    const auto sample = ReadFile(root /
        "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd");
    HyperCine::DecoderConfig config{};
    config.maximum_width = width; config.maximum_height = height;
    HyperCine::Decoder decoder(config);
    Require(decoder.Initialize() == HyperCine::DecodeError::None, "initialize matrix/range decoder");
    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::trunc);
    Require(bool(out), "open matrix/range JSON");
    out << "{\n  \"license\": \"MIT OR Apache-2.0\",\n  \"status\": \"PASS\",\n"
        << "  \"fixture\": \"tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd\",\n"
        << "  \"cases\": [\n";
    bool comma = false;
    for (const auto matrix : {HyperCine::DecodeMatrix::BT601, HyperCine::DecodeMatrix::BT709}) {
        for (const auto range : {HyperCine::DecodeRange::ComputerGraphics, HyperCine::DecodeRange::VideoSystems}) {
            for (int frame : {0, 1}) {
                const auto expected = DecodeUpstreamBgraHash(sample, frame, width, height, matrix, range);
                const auto actual = DecodeBgraHash(decoder, sample, frame, width, height,
                    HyperCine::DecodeOrientation::BottomUp, matrix, range);
                Require(actual == expected, "FIELDPLUS matrix/range BGRA exactness");
                if (comma) out << ",\n";
                comma = true;
                out << "    {\"matrix\":\"" << (matrix == HyperCine::DecodeMatrix::BT601 ? "BT601" : "BT709")
                    << "\",\"range\":\"" << (range == HyperCine::DecodeRange::ComputerGraphics ? "CG" : "VS")
                    << "\",\"frame\":" << frame << ",\"upstream_bgra_fnv1a64\":\"" << expected
                    << "\",\"hypercine_bgra_fnv1a64\":\"" << actual << "\",\"exact\":true}";
            }
        }
    }
    out << "\n  ]\n}\n";
    Require(bool(out), "write matrix/range evidence");
    decoder.Release();
    std::cout << "FIELDPLUS BT.601/BT.709 x CG/VS matched upstream for both frames\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--4k-smoke") {
            Run4kSmoke(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--1080-performance") {
            Run1080Performance(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--matrix-range") {
            RunMatrixRange(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
            return 0;
        }
        if (argc != 2) throw std::runtime_error("usage: test_decode_phase3b2a_reconstruction <repo-root>");
        const std::filesystem::path root(argv[1]);
        const auto sample = ReadFile(root /
            "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd");
        const auto bgra_coefficients = CaptureUpstreamFieldPlusBands(sample, CFHD_PIXEL_FORMAT_BGRA);
        const auto planar_coefficients = CaptureUpstreamFieldPlusBands(sample, CFHD_PIXEL_FORMAT_YU64);
        const auto bgra_offsetless_differences = CountOffsetlessBaseLlDifferences(bgra_coefficients, 24);
        const auto planar_offsetless_differences = CountOffsetlessBaseLlDifferences(planar_coefficients, 10);
        Require(bgra_offsetless_differences > 0, "BGRA base-LL fixture discriminates missing +24 bias");
        Require(planar_offsetless_differences > 0, "signed planar base-LL fixture discriminates missing +10 bias");
        std::cout << "base-LL offset discrimination: BGRA missing +24 changes "
            << bgra_offsetless_differences << " coefficients; signed planar missing +10 changes "
            << planar_offsetless_differences << " coefficients\n";
        const auto temporal_stages = CaptureUpstreamTemporalStages(bgra_coefficients);
        const auto planar_temporal_stages = CaptureUpstreamTemporalStages(planar_coefficients);
        HyperCine::Decoder decoder;
        Require(decoder.Initialize() == HyperCine::DecodeError::None, "initialize production decoder");

        auto queue_wait_release = [&](int frame) {
            HyperCine::DecodeRequest request{};
            request.frame_index = frame;
            const auto ticket = decoder.QueueDecode(sample.data(), sample.size(), request);
            Require(bool(ticket), "steady-state FIELDPLUS queue");
            Require(decoder.WaitForDecode(ticket).state == HyperCine::DecodeState::Ready,
                "steady-state FIELDPLUS decode ready");
            Require(decoder.ReleaseOutput(ticket), "steady-state FIELDPLUS release");
        };
        queue_wait_release(0);
        queue_wait_release(1);
        const auto memory_before_steady = decoder.MemoryStats();
        const auto allocations_before_steady = g_cpp_allocations.load(std::memory_order_relaxed);
        for (int request = 0; request < 400; ++request) queue_wait_release(request & 1);
        const auto steady_cpp_allocations = g_cpp_allocations.load(std::memory_order_relaxed) - allocations_before_steady;
        const auto memory_after_steady = decoder.MemoryStats();
        Require(steady_cpp_allocations == 0, "zero steady-state FIELDPLUS C++ allocations");
        Require(memory_before_steady.cuda_allocation_calls == memory_after_steady.cuda_allocation_calls &&
            memory_before_steady.device_bytes == memory_after_steady.device_bytes &&
            memory_before_steady.pinned_host_bytes == memory_after_steady.pinned_host_bytes &&
            memory_before_steady.pageable_capacity_bytes == memory_after_steady.pageable_capacity_bytes,
            "zero steady-state FIELDPLUS CUDA/owned-memory allocations");
        const size_t per_slot_device_bytes = memory_before_steady.slot_count > 0
            ? (memory_before_steady.device_bytes - memory_before_steady.immutable_device_bytes) /
                size_t(memory_before_steady.slot_count)
            : 0;
        const size_t per_slot_pinned_bytes = memory_before_steady.slot_count > 0
            ? memory_before_steady.pinned_host_bytes / size_t(memory_before_steady.slot_count)
            : 0;
        std::cout << "FIELDPLUS memory max=1920x1080 slots=" << memory_before_steady.slot_count
            << " device_total_bytes=" << memory_before_steady.device_bytes
            << " immutable_device_bytes=" << memory_before_steady.immutable_device_bytes
            << " per_slot_device_bytes=" << per_slot_device_bytes
            << " pinned_total_bytes=" << memory_before_steady.pinned_host_bytes
            << " per_slot_pinned_bytes=" << per_slot_pinned_bytes
            << " pageable_capacity_total_bytes=" << memory_before_steady.pageable_capacity_bytes
            << " cuda_allocation_calls=" << memory_before_steady.cuda_allocation_calls << '\n';

        for (int frame : {0, 1}) {
            int reference_pitch = 0;
            const auto reference = DecodeUpstreamYuv(sample, frame, 64, 64, reference_pitch);
            const auto actual = DecodePlanar(decoder, sample, frame,
                &planar_coefficients, &planar_temporal_stages);
            const auto upstream_signed = ReconstructUpstreamSignedPlanes(
                planar_coefficients, planar_temporal_stages, frame);
            ComparePlanarWithUpstreamSigned(actual, upstream_signed, "canonical 64x64", frame);
            const std::array<int, 3> plane_for_channel{{0, 2, 1}};
            size_t mismatches = 0;
            for (int y = 0; y < 64; ++y) {
                const auto* packed = reinterpret_cast<const uint16_t*>(reference.data() + size_t(y) * reference_pitch);
                for (int x = 0; x < 64; x += 2) {
                    const size_t packed_index = size_t(x) * 2;
                    const int16_t y0 = actual.planes[0][size_t(y) * 64 + x];
                    const int16_t y1 = actual.planes[0][size_t(y) * 64 + x + 1];
                    const int16_t v = actual.planes[2][size_t(y) * 32 + x / 2];
                    const int16_t u = actual.planes[1][size_t(y) * 32 + x / 2];
                    const uint16_t expected_y0 = packed[packed_index];
                    const uint16_t expected_v = packed[packed_index + 1];
                    const uint16_t expected_y1 = packed[packed_index + 2];
                    const uint16_t expected_u = packed[packed_index + 3];
                    const auto to_yu64 = [](int16_t code) {
                        const int32_t limited = (std::max)(0, (std::min)(1023, int32_t(code)));
                        return static_cast<uint16_t>(limited * 64);
                    };
                    if (to_yu64(y0) != expected_y0 || to_yu64(v) != expected_v ||
                        to_yu64(y1) != expected_y1 || to_yu64(u) != expected_u) {
                        if (mismatches < 4)
                            std::cerr << "FIELDPLUS planar mismatch frame=" << frame << " x=" << x << " y=" << y
                                << " y=" << to_yu64(y0) << '/' << expected_y0 << " v=" << to_yu64(v) << '/' << expected_v
                                << " u=" << to_yu64(u) << '/' << expected_u << '\n';
                        ++mismatches;
                    }
                }
            }
            Require(mismatches == 0, "FIELDPLUS YU64 output matches upstream exactly");
        }

        const std::array<std::pair<int, HyperCine::DecodeOrientation>, 8> requests{{
            {0, HyperCine::DecodeOrientation::BottomUp},
            {1, HyperCine::DecodeOrientation::BottomUp},
            {1, HyperCine::DecodeOrientation::TopDown},
            {0, HyperCine::DecodeOrientation::TopDown},
            {0, HyperCine::DecodeOrientation::BottomUp},
            {1, HyperCine::DecodeOrientation::TopDown},
            {1, HyperCine::DecodeOrientation::BottomUp},
            {0, HyperCine::DecodeOrientation::TopDown}
        }};
        for (const auto& [frame, orientation] : requests) {
            const auto hash = DecodeFrame(decoder, sample, frame, orientation, bgra_coefficients, temporal_stages);
            const char* expected = frame == 0 ? "27fd4d3948775018" : "c06ad9438de588c8";
            if (hash != expected)
                throw std::runtime_error("FIELDPLUS frame " + std::to_string(frame) +
                    " BGRA hash mismatch: got " + hash + ", expected " + expected);
        }
        for (int frame : {0, 1}) {
            const char* expected = frame == 0 ? "27fd4d3948775018" : "c06ad9438de588c8";
            for (int repeat = 0; repeat < 200; ++repeat)
                Require(DecodeBgraHash(decoder, sample, frame, 64, 64,
                    HyperCine::DecodeOrientation::BottomUp) == expected,
                    "200-repeat canonical frame-index determinism");
        }

        struct FixtureOracle {
            const char* relative_path;
            size_t bytes;
            const char* sample_fnv;
            int width;
            int height;
            std::array<const char*, 2> bgra;
            std::array<std::array<const char*, 3>, 2> yu64_yuv;
        };
        const std::array<FixtureOracle, 4> additional_fixtures{{
            {"tests/fixtures/phase3b2r/temporal_asymmetric_64_progressive.cfhd", 12212,
                "1712cac09821aafc", 64, 64,
                {"33efa9b37256d867", "1b7d61cb2c78d5c2"},
                {{{"a7c823ef96962777", "16673fae27b8eee2", "8768f162f4d2d318"},
                  {"72edd541ee2cf7eb", "45ea8a45ede93c6f", "f77d4ff1219565a8"}}}},
            {"tests/fixtures/phase3b2r/motion_64_progressive.cfhd", 7380,
                "4267d3b3395d22cc", 64, 64,
                {"1adc1719767c4a0f", "bc81f05b2cb3c5ab"},
                {{{"6ffa7a7a10aea0f1", "e57117561ca36213", "3a752fe4b3f6879e"},
                  {"f0ad2e3fdb7222c2", "89dc1b557f883872", "7e2c7d4ba9678f05"}}}},
            {"tests/fixtures/phase3b2r/chroma_temporal_64_progressive.cfhd", 8568,
                "df0487f0a23169d8", 64, 64,
                {"87278ff765c7ffd1", "21f33d8701b527ac"},
                {{{"352c12b22e166ad7", "e245c5f1e66bcd03", "78bcb7265cda2f7a"},
                  {"352c12b22e166ad7", "039532240a81d2e0", "c61c588937e1454a"}}}},
            {"tests/fixtures/phase3b2a/motion_1080p_progressive.cfhd", 718396,
                "c59a1ca30aa101a9", 1920, 1080,
                {"7f633a5358642abd", "6d174c1993f61015"},
                {{{"f120bdbb80d1c1d5", "14725de67110f325", "7d2f403f9864f325"},
                  {"d1cbf5a9c101f405", "1ef077e6757cf325", "d7db648123560325"}}}}
        }};
        for (const auto& fixture : additional_fixtures) {
            const auto fixture_sample = ReadFile(root / fixture.relative_path);
            Require(fixture_sample.size() == fixture.bytes, "FIELDPLUS fixture byte length");
            std::ostringstream sample_hash;
            sample_hash << std::hex << std::setfill('0') << std::setw(16)
                << HashAdd(14695981039346656037ULL, fixture_sample.data(), fixture_sample.size());
            Require(sample_hash.str() == fixture.sample_fnv, "FIELDPLUS fixture source hash");
            UpstreamFieldPlusCapture fixture_coefficients;
            UpstreamFieldPlusCapture fixture_planar_coefficients;
            UpstreamTemporalStages fixture_stages;
            UpstreamTemporalStages fixture_planar_stages;
            fixture_planar_coefficients = CaptureUpstreamFieldPlusBands(fixture_sample, CFHD_PIXEL_FORMAT_YU64);
            fixture_planar_stages = CaptureUpstreamTemporalStages(fixture_planar_coefficients);
            if (fixture.width == 64) {
                fixture_coefficients = CaptureUpstreamFieldPlusBands(fixture_sample, CFHD_PIXEL_FORMAT_BGRA);
                fixture_stages = CaptureUpstreamTemporalStages(fixture_coefficients);
            }
            for (int frame = 0; frame < 2; ++frame) {
                const auto orientation = frame == 0 ? HyperCine::DecodeOrientation::BottomUp
                    : HyperCine::DecodeOrientation::TopDown;
                const auto fixture_bgra = fixture.width == 64
                    ? DecodeFrame(decoder, fixture_sample, frame, orientation, fixture_coefficients, fixture_stages)
                    : DecodeBgraHash(decoder, fixture_sample, frame, fixture.width, fixture.height, orientation);
                if (fixture_bgra != fixture.bgra[size_t(frame)])
                    throw std::runtime_error(std::string("FIELDPLUS BGRA hash mismatch for ") +
                        fixture.relative_path + " frame " + std::to_string(frame) + ": got " +
                        fixture_bgra + ", expected " + fixture.bgra[size_t(frame)]);
                const auto planar = DecodePlanar(decoder, fixture_sample, frame,
                    &fixture_planar_coefficients, &fixture_planar_stages);
                const auto upstream_signed = ReconstructUpstreamSignedPlanes(
                    fixture_planar_coefficients, fixture_planar_stages, frame);
                ComparePlanarWithUpstreamSigned(planar, upstream_signed,
                    fixture.relative_path, frame);
                int upstream_pitch = 0;
                const auto upstream_yu64 = DecodeUpstreamYuv(fixture_sample, frame,
                    fixture.width, fixture.height, upstream_pitch);
                const auto upstream_yu64_hashes = HashUpstreamYU64Planes(
                    upstream_yu64, upstream_pitch, fixture.width, fixture.height);
                for (int plane = 0; plane < 3; ++plane) {
                    if (upstream_yu64_hashes[size_t(plane)] != fixture.yu64_yuv[size_t(frame)][size_t(plane)])
                        throw std::runtime_error(std::string("FIELDPLUS YU64 plane hash mismatch for ") +
                            fixture.relative_path + " frame " + std::to_string(frame) + " plane " +
                            std::to_string(plane) + ": got " + upstream_yu64_hashes[size_t(plane)] + ", expected " +
                            fixture.yu64_yuv[size_t(frame)][size_t(plane)]);
                }
            }
            if (fixture.width == 1920) {
                for (int frame = 0; frame < 2; ++frame)
                    for (int repeat = 0; repeat < 100; ++repeat)
                        Require(DecodeBgraHash(decoder, fixture_sample, frame, fixture.width,
                            fixture.height, HyperCine::DecodeOrientation::BottomUp) == fixture.bgra[size_t(frame)],
                            "100-repeat 1080 frame-index determinism");
            }
        }

        HyperCine::DecodeRequest invalid{};
        invalid.frame_index = 2;
        HyperCine::DecodeError error = HyperCine::DecodeError::None;
        const auto rejected = decoder.QueueDecode(sample.data(), sample.size(), invalid, &error);
        Require(bool(rejected) && error == HyperCine::DecodeError::InvalidRequest,
            "FIELDPLUS frame index 2 returns an InvalidRequest ticket");
        Require(decoder.WaitForDecode(rejected).state == HyperCine::DecodeState::Failed,
            "invalid FIELDPLUS frame ticket is failed");
        Require(decoder.ReleaseOutput(rejected), "release invalid FIELDPLUS frame ticket");
        decoder.Release();
        std::cout << "progressive FIELDPLUS frame 0/1 BGRA hashes, 200/100-repeat determinism, "
            << "and zero-allocation 400-request alternation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
