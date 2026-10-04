// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>
#if defined(HYPERCINE_CUDA_AVAILABLE) || defined(__CUDACC__)
#include <cuda_runtime.h>
#else
typedef struct CUstream_st* cudaStream_t;
typedef struct CUevent_st* cudaEvent_t;
typedef int cudaError_t;
constexpr cudaError_t cudaSuccess = 0;
constexpr cudaError_t cudaErrorNotSupported = 801;
#endif

namespace HyperCine {

enum class GpuBandDecodeStatus : uint32_t {
    Complete = 0,
    InvalidDescriptor = 1,
    InputTruncated = 2,
    InvalidCodebook = 3,
    MissingEndMarker = 4,
    CoefficientOverflow = 5,
    UnsupportedBandMethod = 6,
    CUDAFailure = 7
};

enum class BandEncodingMethod : uint32_t {
    RunLengthVLC = 0,   // Standard CineForm VLC run-lengths
    LowPass16S = 1,     // Deepest LL 16-bit signed words
    Lossless = 2,       // Unsupported in Phase 2B (fallback)
    Raw16 = 3           // Unsupported in Phase 2B (fallback)
};

enum class CompandingPolicy : uint32_t {
    None = 0,           // cs9 (no companding)
    Cubic = 1,          // cs17 (cubic companding: mag += mag^3 * 768 / 256^3)
    PiecewiseLinear = 2 // cs18 (piecewise linear companding)
};

// 8-byte immutable FSM lookup table entry (4 x 16-bit words)
struct GpuFsmEntry {
    int16_t value0 = 0;          // Base signed magnitude (after companding, before quantization)
    int16_t value1 = 0;          // Second base signed magnitude
    uint16_t pre_post_skip = 0;  // pre_skip (bits 0..8) | (post_skip << 12)
    uint16_t next_state = 0;     // Target FSM state index
};

// Immutable GPU Entropy Band Descriptor containing all state to decode one band
struct GpuEntropyBandDescriptor {
    // Compressed bitstream location inside uploaded sample
    size_t payload_offset = 0;      // Byte offset from start of compressed frame buffer
    uint32_t payload_bytes = 0;     // Bounded compressed payload byte length
    uint32_t safe_upper_bound = 0;  // Maximum bytes allowed to read
    uint32_t trailer_word = 0x00003800; // Expected framing trailer (0x00003800 for highpass)

    // Wavelet and geometry hierarchy
    int channel = 0;                // 0: Y, 1: V, 2: U
    int level = 0;                  // 2 (deepest, e.g. 240x135), 1, 0 (largest)
    int band = 0;                   // 0: LL, 1: LH, 2: HL, 3: HH
    int subband = 0;                // 0..9 per channel
    int width = 0;                  // Band width in samples
    int height = 0;                 // Band height in samples
    size_t pitch_bytes = 0;         // Row stride in bytes (16-byte aligned)
    size_t pitch_samples = 0;       // Row stride in int16_t samples
    size_t coefficient_count = 0;   // Total sample capacity (height * pitch_samples)

    // Codec and quantization parameters
    BandEncodingMethod method = BandEncodingMethod::RunLengthVLC;
    int codebook_id = 0;            // 0: cs9, 1: cs17, 2: cs18
    uint16_t initial_fsm_state = 0; // Initial FSM state (0)
    int quantization = 1;           // Band quantization multiplier
    CompandingPolicy companding = CompandingPolicy::None;
    int precision = 10;             // Source precision (10-bit nominal)
    int prescale = 0;               // Wavelet prescale (0 or 2)
    int channel_offset = 0;         // LL band channel offset (24 for BGRA, 4 for YU64)

    // Prediction and peak tables
    bool difference_coding = false; // Haar row differencing flag
    bool has_peaks = false;         // Out-of-band peak table present
    size_t peak_offset = 0;         // Offset to peak table in bitstream
    int peak_level = 0;             // Peak magnitude threshold
    int peak_count = 0;             // Number of peak entries

    // Destination memory pointer in GPU VRAM
    int16_t* d_destination = nullptr;
};

// Execution diagnostics reported by GPU entropy kernel for each band
struct GpuBandDecodeResult {
    GpuBandDecodeStatus status = GpuBandDecodeStatus::Complete;
    uint32_t consumed_bytes = 0;
    uint16_t final_fsm_state = 0;
    uint32_t final_coeff_pos = 0;
    uint32_t nonzero_count = 0;
    uint64_t duration_cycles = 0;   // GPU clock cycles measured inside block
};

// Diagnostic token recorded for low-level symbol validation (Rule 18)
struct DecodedSymbolDiagnostic {
    uint16_t nibble_index = 0;
    uint16_t state_before = 0;
    uint16_t state_after = 0;
    uint16_t pre_skip = 0;
    uint16_t post_skip = 0;
    int16_t raw_val0 = 0;
    int16_t raw_val1 = 0;
    uint32_t out_pos = 0;
};

// Parse compressed CineForm sample tags on CPU and construct immutable band descriptors.
// Returns true if sample is a supported intra progressive YUV422 sample with bounded payloads.
// If unsupported, returns false with an explanatory reason in out_error.
bool ParseCineFormSample(
    const uint8_t* compressed_data,
    size_t compressed_size,
    int pixel_format_channeloffset, // 24 for BGRA, 4 for YU64
    std::vector<GpuEntropyBandDescriptor>& out_bands,
    std::string* out_error = nullptr
);

// Global GPU FSM base tables manager
class GpuFsmTableManager {
public:
    GpuFsmTableManager();
    ~GpuFsmTableManager();

    GpuFsmTableManager(const GpuFsmTableManager&) = delete;
    GpuFsmTableManager& operator=(const GpuFsmTableManager&) = delete;

    bool Initialize(int cuda_device = 0);
    void Release();
    bool IsInitialized() const;

    const GpuFsmEntry* GetDeviceTable() const;
    size_t GetCodebookOffset(int codebook_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Launch GPU LL decode kernel for deepest lowpass bands (subband 0 of each channel)
cudaError_t LaunchDecodeLowpassGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_descriptors,
    int num_ll_bands,
    cudaStream_t stream
);

// Launch GPU HighPass VLC decode kernel (27 bands, 1 block per band)
cudaError_t LaunchDecodeEntropyGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_descriptors,
    int num_vlc_bands,
    const GpuFsmEntry* d_fsm_tables,
    GpuBandDecodeResult* d_results,
    cudaStream_t stream
);

// Diagnostic launch for symbol-level verification (Rule 18)
cudaError_t LaunchDecodeEntropyDiagnosticGpu(
    const uint8_t* d_compressed_sample,
    size_t compressed_size,
    const GpuEntropyBandDescriptor* d_desc,
    const GpuFsmEntry* d_fsm_tables,
    DecodedSymbolDiagnostic* d_symbols,
    int max_symbols,
    int* d_actual_symbols,
    GpuBandDecodeResult* d_result,
    cudaStream_t stream
);

} // namespace HyperCine
