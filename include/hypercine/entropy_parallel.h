// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_ENTROPY_PARALLEL_H
#define HYPERCINE_ENTROPY_PARALLEL_H

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>

struct encoder;
struct transform;

namespace hypercine {

enum class EntropySchedule {
    Serial = 0,
    ChannelParallel = 1,
    SubbandParallel = 2
};

struct BandInfo {
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    bool lowpass = false;
    int width = 0;
    int height = 0;
    int pixel_type = 0;
    int pitch_bytes = 0;
    int coeff_count = 0;
    int element_bytes = 0;
    int allocated_bytes = 0;
    int entropy_input_bytes = 0;
    int value_min = 0;
    int value_max = 0;
    int fits_in_8bit = 0;
    int encoded_bytes = 0;
    double encode_ms = 0.0;
    std::string storage_type;
    std::string name;
};

struct ZeroRunStats {
    int64_t coeff_count = 0;
    int64_t zero_count = 0;
    int64_t nonzero_count = 0;
    int64_t run_count = 0;
    int64_t run_hist[8] = {};
    double mean_run = 0.0;
    double encoded_bits_per_coeff = 0.0;
};

struct EntropyOutput {
    bool success = false;
    bool byte_match = false;
    int mismatch_offset = -1;
    int worker_count = 0;
    int workers_used = 0;
    EntropySchedule schedule = EntropySchedule::Serial;
    int band_count = 0;
    double entropy_wall_ms = 0.0;
    double assembly_ms = 0.0;
    double sum_band_ms = 0.0;
    double max_worker_ms = 0.0;
    double min_worker_ms = 0.0;
    double scheduling_overhead_ms = 0.0;
    std::vector<double> worker_busy_ms;
    size_t bitstream_bytes = 0;
    std::vector<uint8_t> bitstream;
    std::vector<BandInfo> bands;
    std::string error;
};

// Rebuild the production spatial pyramid from a BGRA frame into the encoder
// wavelets without running entropy. Requires a prepared encoder that has
// already encoded at least one sample (buffers, quantizers, codebooks).
bool RefreshUpstreamPyramid(encoder* encoder,
                            transform* transforms[],
                            int channel_count,
                            const uint8_t* bgra,
                            int pitch,
                            void* scratch,
                            size_t scratch_bytes);

std::vector<BandInfo> DescribeBands(transform* transforms[], int channel_count);

ZeroRunStats ScanZeroRuns(transform* transforms[], int channel_count, size_t encoded_bytes);

// Encode the current wavelet bands. When compare_serial is set, the output is
// checked against EncodeQuantizedGroup on a restored copy of the same coefficients.
// Peaks coding may overlay one highpass band during the encode.
EntropyOutput EncodeEntropy(encoder* encoder,
                            transform* transforms[],
                            int channel_count,
                            int worker_count,
                            EntropySchedule schedule,
                            bool compare_serial);

// One entropy job whose coefficients live outside the encoder wavelet.
// elem_bytes is 2 (int16) or 1 (int8). int8 is legal only when every value is
// in [-128, 127]. The encoder never saturates a coefficient to make it fit.
struct ExternalCoeff {
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    bool lowpass = false;
    const void* data = nullptr;
    int elem_bytes = 2;
    int width = 0;
    int height = 0;
    int value_min = 0;
    int value_max = 0;
};

struct EncodedPiece {
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    bool lowpass = false;
    bool ok = false;
    bool used_int8 = false;
    bool peaks_fallback = false;
    int bits_free = 0;
    double encode_ms = 0;
    std::vector<uint8_t> payload;
};

struct BandRef {
    int channel = 0;
    int wavelet_index = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    bool lowpass = false;
    int width = 0;
    int height = 0;
};

std::vector<BandRef> EnumerateBands(transform* transforms[], int channel_count);

// Encode one band from an external buffer. Highpass int8 is read directly.
// The bitstream fragment matches EncodeQuantizedBand / EncodeLowPassBand.
bool EncodeOneExternalBand(encoder* encoder,
                           transform* transforms[],
                           const ExternalCoeff& coeff,
                           EncodedPiece* piece);

// Concatenate pieces in canonical order. compare_serial runs EncodeQuantizedGroup
// on the coefficients currently stored in the wavelets.
EntropyOutput AssembleEncodedPieces(encoder* encoder,
                                    transform* transforms[],
                                    int channel_count,
                                    std::vector<EncodedPiece>& pieces,
                                    bool compare_serial);

}  // namespace hypercine

#endif
