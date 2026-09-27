// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_ENTROPY_STAGE_H
#define HYPERCINE_ENTROPY_STAGE_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

struct encoder;
struct transform;

namespace hypercine {

struct ExternalCoeff;

// Host copy of one CineForm run/value codebook. Run index i is the code emitted
// for a zero run of length i when that code's `count` covers it; longer runs
// repeat the last entry the way EncodeQuantLongRuns does.
struct EntropyTables {
    int codebook = 1;
    int peaks = 0;
    int codingflags = 0;
    int run_length = 0;
    int value_length = 0;
    std::vector<uint32_t> run_bits;
    std::vector<int16_t> run_size;
    std::vector<int16_t> run_count;
    std::vector<uint32_t> value_entry;
    uint32_t band_end_code = 0;
    int band_end_size = 0;
};

struct EntropyTiming {
    double scan_ms = 0;
    double block_scan_ms = 0;
    double merge_carry_ms = 0;
    double gather_ms = 0;
    double token_ms = 0;
    double codebook_ms = 0;
    double prefix_ms = 0;
    double pack_ms = 0;
    double d2h_ms = 0;
    double header_ms = 0;
    double lowpass_ms = 0;
    double total_ms = 0;
};

// tokens: bit 31 clear = run-table index, bit 31 set = value-table index.
// codeword/bit_length/bit_offset include the band-end code as the last element.
// coeff_bytes is the coefficient bitstream after the band-end code and the
// 32-bit zero pad. payload is the full band (header + coeff_bytes + trailer).
struct EntropySymbols {
    bool ok = false;
    bool byte_match = false;
    int tokens = 0;
    int total_bits = 0;
    int payload_bytes = 0;
    uint64_t coeff_hash = 0;
    uint64_t payload_hash = 0;
    EntropyTiming timing;
    std::vector<uint32_t> tokens_packed;
    std::vector<uint32_t> codeword;
    std::vector<uint16_t> bit_length;
    std::vector<uint32_t> bit_offset;
    std::vector<uint8_t> coeff_bytes;
    std::vector<uint8_t> payload;
    std::string error;
};

bool LoadEntropyTables(encoder* encoder, int subband, EntropyTables* tables);

// Upper bound on packed uint32 words for one highpass band of N virtual
// coefficients (N = height * pitch_px). Includes the band-end codeword and the
// final partial word. It does not include the CPU band header or trailer;
// those are assembled in a separate buffer.
//
// Assumptions, checked by ValidateEntropyTablesForWordBound:
//   * every selectable run code covers at least one zero and is 1..31 bits
//   * every value codeword is 0..31 bits (VLE stores the size in 5 bits)
//   * the band-end code is 1..31 bits
// Under those rules the encoder emits at most one token per virtual coefficient
// plus one band-end codeword, so bits <= 31*(N+1) and words = ceil(bits/32) <= N+1.
inline int EntropyPackedWordUpperBound(int virtual_coefficients) {
    if (virtual_coefficients <= 0) return 1;
    const long long bits = 31ll * (static_cast<long long>(virtual_coefficients) + 1ll);
    return static_cast<int>((bits + 31ll) / 32ll);
}

bool ValidateEntropyTablesForWordBound(const EntropyTables& tables, std::string* error);

// pitch_px is in samples. Samples past `width` on each row are implicit zeros
// and are not read. The active FilmScan path uses pitch_px == width.
EntropySymbols EncodeHighpassStages(const int16_t* coeff, int width, int height, int pitch_px,
                                    const EntropyTables& tables, bool keep_symbols);

// Build the band payload around an already packed coefficient bitstream and
// compare it with EncodeOneExternalBand. `coeff` must be tight (pitch == width).
bool MatchUpstreamHighpass(encoder* encoder, transform* transforms[], const ExternalCoeff& coeff,
                           EntropySymbols* symbols);

// Header and trailer around coefficient bytes that already include the band-end
// code and the 32-bit pad. bits_free is 32 when the band is byte aligned.
bool BuildHighpassPayload(encoder* encoder, int band, int width, int height, int subband,
                          int quantization, int scale, const EntropyTables& tables,
                          const std::vector<uint8_t>& coeff_bytes, std::vector<uint8_t>* payload,
                          int* bits_free);

bool BuildHighpassPayloadPtr(encoder* encoder, int band, int width, int height, int subband,
                             int quantization, int scale, const EntropyTables& tables,
                             const uint8_t* coeff_bytes, size_t coeff_byte_count,
                             std::vector<uint8_t>* payload, int* bits_free);

bool BuildLowpassPayloadPtr(int subband, int level, int width, int height,
                            const int16_t* coeff, bool already_big_endian,
                            std::vector<uint8_t>* payload, int* bits_free);

uint64_t EntropyFnv64(const void* data, size_t size);

// Codebook, exclusive prefix, and PutBits packing for a token list. Bit 31 marks
// a value-table index. The band-end code is appended here. coeff_bytes includes
// that code and the 32-bit pad.
bool FinishFromTokens(const EntropyTables& tables, const uint32_t* tokens, int token_count, EntropySymbols* symbols);

// Exclusive prefix and PutBits packing. `code_count` includes the band-end code.
bool FinishFromCodewords(const uint32_t* codeword, const uint16_t* bit_length, int code_count, EntropySymbols* symbols);

}  // namespace hypercine

#endif
