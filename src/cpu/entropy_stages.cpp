// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#ifndef _ALLOCATOR
#define _ALLOCATOR 1
#endif

#include "hypercine/entropy_stage.h"
#include "hypercine/entropy_parallel.h"

#include <chrono>
#include <cstring>
#include <vector>

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "wavelet.h"
#include "frame.h"
#include "bitstream.h"
#include "image.h"
#include "vlc.h"
#include "swap.h"

int SetCodingFlags(ENCODER* encoder, int subband, int* active_codebook_ret, int* peaks_coding_ret);
}

namespace hypercine {
namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

uint32_t BitMask(int n) {
    if (n <= 0) return 0;
    if (n >= 32) return 0xFFFFFFFFu;
    return (1u << n) - 1u;
}

int ValueIndex(int value) {
    constexpr int kHalf = VALUE_TABLE_LENGTH >> 1;
    if (value < 0) {
        if (value <= -kHalf) value = -(kHalf - 1);
        return VALUE_TABLE_LENGTH + value;
    }
    if (value >= kHalf) value = kHalf - 1;
    return value;
}

struct BitWriter {
    uint32_t buffer = 0;
    int free_bits = 32;
    std::vector<uint32_t> words;

    void Put(uint32_t bits, int n) {
        if (n <= 0) return;
        if (n <= free_bits) {
            buffer = (buffer << n) | (bits & BitMask(n));
            free_bits -= n;
            return;
        }
        const int kept = free_bits;
        const int remain = n - kept;
        buffer = (buffer << kept) | ((bits >> remain) & BitMask(kept));
        words.push_back(SwapInt32(buffer));
        buffer = bits & BitMask(remain);
        free_bits = 32 - remain;
    }

    void Finish() {
        if (free_bits == 0) {
            words.push_back(SwapInt32(buffer));
            buffer = 0;
            free_bits = 32;
            return;
        }
        if (free_bits < 32) {
            Put(0, free_bits);
            words.push_back(SwapInt32(buffer));
            buffer = 0;
            free_bits = 32;
        }
    }
};

void EmitRun(const EntropyTables& tables, int count, std::vector<uint32_t>* tokens) {
    while (count > 0) {
        const int index = count < tables.run_length ? count : tables.run_length - 1;
        const int covered = tables.run_count[static_cast<size_t>(index)];
        if (covered <= 0) return;
        tokens->push_back(static_cast<uint32_t>(index));
        count -= covered;
    }
}

}  // namespace

uint64_t EntropyFnv64(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool FinishFromCodewords(const uint32_t* codeword, const uint16_t* bit_length, int code_count, EntropySymbols* symbols) {
    if (!symbols || code_count <= 0 || !codeword || !bit_length) return false;
    const double t_prefix = NowMs();
    symbols->bit_offset.resize(static_cast<size_t>(code_count));
    uint32_t sum = 0;
    for (int i = 0; i < code_count; ++i) {
        const int n = bit_length[i];
        if (n <= 0 || n >= 32) {
            symbols->error = "codeword length is outside 1..31";
            symbols->ok = false;
            return false;
        }
        symbols->bit_offset[static_cast<size_t>(i)] = sum;
        sum += static_cast<uint32_t>(n);
    }
    symbols->total_bits = static_cast<int>(sum);
    symbols->timing.prefix_ms = NowMs() - t_prefix;

    const double t_pack = NowMs();
    BitWriter writer;
    for (int i = 0; i < code_count; ++i) writer.Put(codeword[i], bit_length[i]);
    writer.Finish();
    symbols->coeff_bytes.resize(writer.words.size() * sizeof(uint32_t));
    if (!writer.words.empty()) std::memcpy(symbols->coeff_bytes.data(), writer.words.data(), symbols->coeff_bytes.size());
    symbols->timing.pack_ms = NowMs() - t_pack;
    symbols->ok = true;
    return true;
}

bool FinishFromTokens(const EntropyTables& tables, const uint32_t* tokens, int token_count, EntropySymbols* symbols) {
    if (!symbols || token_count < 0 || (token_count > 0 && !tokens)) return false;
    if (tables.run_length <= 1 || tables.value_length <= 0 || tables.band_end_size <= 0) {
        symbols->error = "codebook is incomplete";
        symbols->ok = false;
        return false;
    }
    const double t_book = NowMs();
    const int codes = token_count + 1;
    symbols->codeword.resize(static_cast<size_t>(codes));
    symbols->bit_length.resize(static_cast<size_t>(codes));
    symbols->tokens = token_count;
    for (int i = 0; i < token_count; ++i) {
        const uint32_t packed = tokens[i];
        if (packed & 0x80000000u) {
            const uint32_t index = packed & 0x7FFFFFFFu;
            if (index >= static_cast<uint32_t>(tables.value_length)) {
                symbols->error = "value index outside the codebook";
                symbols->ok = false;
                return false;
            }
            const uint32_t entry = tables.value_entry[index];
            symbols->codeword[static_cast<size_t>(i)] = entry & VLE_CODEWORD_MASK;
            symbols->bit_length[static_cast<size_t>(i)] = static_cast<uint16_t>(entry >> VLE_CODESIZE_SHIFT);
        } else {
            if (packed >= static_cast<uint32_t>(tables.run_length)) {
                symbols->error = "run index outside the codebook";
                symbols->ok = false;
                return false;
            }
            symbols->codeword[static_cast<size_t>(i)] = tables.run_bits[packed];
            symbols->bit_length[static_cast<size_t>(i)] = static_cast<uint16_t>(tables.run_size[packed]);
        }
    }
    symbols->codeword.back() = tables.band_end_code;
    symbols->bit_length.back() = static_cast<uint16_t>(tables.band_end_size);
    symbols->timing.codebook_ms = NowMs() - t_book;
    return FinishFromCodewords(symbols->codeword.data(), symbols->bit_length.data(), codes, symbols);
}

bool LoadEntropyTables(encoder* enc_opaque, int subband, EntropyTables* tables) {
    if (!enc_opaque || !tables) return false;
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    int codebook = 0;
    int peaks = 0;
    const int flags = SetCodingFlags(encoder, subband, &codebook, &peaks);
    if (codebook < 0 || codebook >= CODEC_NUM_CODESETS) return false;
    RLCBOOK* runs = encoder->codebook_runbook[codebook];
    VALBOOK* values = encoder->valuebook[codebook];
    if (!runs || !values || runs->length <= 1 || values->length <= 0) return false;
    tables->codebook = codebook;
    tables->peaks = peaks;
    tables->codingflags = flags;
    tables->run_length = runs->length;
    tables->value_length = values->length;
    tables->band_end_code = encoder->band_end_code[codebook];
    tables->band_end_size = encoder->band_end_size[codebook];
    const RLC* rlc = reinterpret_cast<const RLC*>(reinterpret_cast<const char*>(runs) + sizeof(RLCBOOK));
    tables->run_bits.resize(static_cast<size_t>(runs->length));
    tables->run_size.resize(static_cast<size_t>(runs->length));
    tables->run_count.resize(static_cast<size_t>(runs->length));
    for (int i = 0; i < runs->length; ++i) {
        tables->run_bits[static_cast<size_t>(i)] = rlc[i].bits;
        tables->run_size[static_cast<size_t>(i)] = rlc[i].size;
        tables->run_count[static_cast<size_t>(i)] = rlc[i].count;
    }
    const VLE* vle = reinterpret_cast<const VLE*>(reinterpret_cast<const char*>(values) + sizeof(VALBOOK));
    tables->value_entry.resize(static_cast<size_t>(values->length));
    for (int i = 0; i < values->length; ++i) tables->value_entry[static_cast<size_t>(i)] = vle[i].entry;
    return tables->band_end_size > 0 && tables->band_end_size < 32;
}

EntropySymbols EncodeHighpassStages(const int16_t* coeff, int width, int height, int pitch_px,
                                    const EntropyTables& tables, bool keep_symbols) {
    EntropySymbols out;
    if (!coeff || width <= 0 || height <= 0 || pitch_px < width || tables.run_length <= 1 ||
        tables.value_length <= 0 || tables.band_end_size <= 0) {
        out.error = "bad highpass stage input";
        return out;
    }
    const double t_all = NowMs();
    std::vector<int> positions;
    std::vector<int> values;
    positions.reserve(static_cast<size_t>(width) * static_cast<size_t>(height) / 4u + 8u);
    const double t_scan = NowMs();
    for (int y = 0; y < height; ++y) {
        const int16_t* row = coeff + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) {
            if (row[x] == 0) continue;
            positions.push_back(y * pitch_px + x);
            values.push_back(row[x]);
        }
    }
    const int virtual_len = height * pitch_px;
    out.timing.scan_ms = NowMs() - t_scan;

    const double t_tok = NowMs();
    std::vector<uint32_t> tokens;
    tokens.reserve(positions.size() * 2u + 8u);
    int previous = -1;
    for (size_t i = 0; i < positions.size(); ++i) {
        const int run = positions[i] - previous - 1;
        if (run > 0) EmitRun(tables, run, &tokens);
        const int index = ValueIndex(values[i]);
        if (index < 0 || index >= tables.value_length) {
            out.error = "value index outside the codebook";
            return out;
        }
        tokens.push_back(0x80000000u | static_cast<uint32_t>(index));
        previous = positions[i];
    }
    const int tail = virtual_len - previous - 1;
    if (tail > 0) EmitRun(tables, tail, &tokens);
    out.timing.token_ms = NowMs() - t_tok;
    out.tokens = static_cast<int>(tokens.size());
    if (!FinishFromTokens(tables, tokens.empty() ? nullptr : tokens.data(), out.tokens, &out)) return out;
    if (!keep_symbols) {
        out.tokens_packed.clear();
        out.codeword.clear();
        out.bit_length.clear();
        out.bit_offset.clear();
    } else {
        out.tokens_packed = std::move(tokens);
    }
    out.timing.total_ms = NowMs() - t_all;
    out.ok = true;
    return out;
}

bool BuildHighpassPayloadPtr(encoder* enc_opaque, int band, int width, int height, int subband,
                             int quantization, int scale, const EntropyTables& tables,
                             const uint8_t* coeff_bytes, size_t coeff_byte_count,
                             std::vector<uint8_t>* payload, int* bits_free) {
    if (!enc_opaque || !payload || tables.peaks) return false;
    payload->resize(coeff_byte_count + 256u);
    BITSTREAM stream;
    std::memset(&stream, 0, sizeof(stream));
    InitBitstreamBuffer(&stream, payload->data(), payload->size(), BITSTREAM_ACCESS_WRITE);
    PutVideoBandHeader(&stream, band, width, height, subband, BAND_ENCODING_RUNLENGTHS,
                       quantization, scale, 0, nullptr, tables.codingflags, 0);
    if (stream.nBitsFree != BITSTREAM_LONG_SIZE || stream.error != 0) return false;
    if (coeff_byte_count > 0 && coeff_bytes) {
        if (stream.nWordsUsed + static_cast<int>(coeff_byte_count) > static_cast<int>(payload->size())) return false;
        std::memcpy(stream.lpCurrentWord, coeff_bytes, coeff_byte_count);
        stream.nWordsUsed += static_cast<int>(coeff_byte_count);
        stream.lpCurrentWord += coeff_byte_count;
    }
    PutVideoBandTrailer(&stream);
    if (stream.error != 0 || stream.nWordsUsed <= 0) return false;
    payload->resize(static_cast<size_t>(stream.nWordsUsed));
    if (bits_free) *bits_free = stream.nBitsFree;
    return stream.nBitsFree == BITSTREAM_BUFFER_SIZE;
}

bool BuildHighpassPayload(encoder* enc_opaque, int band, int width, int height, int subband,
                          int quantization, int scale, const EntropyTables& tables,
                          const std::vector<uint8_t>& coeff_bytes, std::vector<uint8_t>* payload,
                          int* bits_free) {
    return BuildHighpassPayloadPtr(enc_opaque, band, width, height, subband, quantization, scale,
                                   tables, coeff_bytes.empty() ? nullptr : coeff_bytes.data(),
                                   coeff_bytes.size(), payload, bits_free);
}

bool BuildLowpassPayloadPtr(int subband, int level, int width, int height,
                            const int16_t* coeff, bool already_big_endian,
                            std::vector<uint8_t>* payload, int* bits_free) {
    if (!coeff || width <= 0 || height <= 0 || (width & 1) != 0 || !payload) return false;
    const size_t coeff_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint16_t);
    payload->resize(coeff_bytes + 256u);
    BITSTREAM stream;
    std::memset(&stream, 0, sizeof(stream));
    InitBitstreamBuffer(&stream, payload->data(), payload->size(), BITSTREAM_ACCESS_WRITE);
    PutVideoLowPassHeader(&stream, subband, level, width, height, 0, 0, 0, 0, 0, 1, 16);
#if (_CODEC_TAGS && _CODEC_MARKERS)
    PutVideoLowPassMarker(&stream);
#endif
    if (stream.nBitsFree != BITSTREAM_LONG_SIZE || stream.error != 0) return false;
    if (already_big_endian) {
        std::memcpy(stream.lpCurrentWord, coeff, coeff_bytes);
        stream.nWordsUsed += static_cast<int>(coeff_bytes);
        stream.lpCurrentWord += coeff_bytes;
    } else {
        const auto* pixelptr = reinterpret_cast<const uint16_t*>(coeff);
        const int count = width * height;
        uint32_t* out32 = reinterpret_cast<uint32_t*>(stream.lpCurrentWord);
        for (int i = 0; i < count; i += 2) {
            uint32_t val = static_cast<uint32_t>(pixelptr[i]) << 16;
            val |= static_cast<uint32_t>(pixelptr[i + 1]) & 0xFFFFu;
            *out32++ = SwapInt32NtoB(val);
        }
        stream.nWordsUsed += static_cast<int>(coeff_bytes);
        stream.lpCurrentWord += coeff_bytes;
    }
#if _CODEC_TAGS
    PadBitsTag(&stream);
#endif
    PutVideoLowPassTrailer(&stream);
    if (stream.error != 0 || stream.nWordsUsed <= 0) return false;
    payload->resize(static_cast<size_t>(stream.nWordsUsed));
    if (bits_free) *bits_free = stream.nBitsFree;
    return stream.nBitsFree == BITSTREAM_BUFFER_SIZE;
}

bool MatchUpstreamHighpass(encoder* enc_opaque, transform* transforms_opaque[], const ExternalCoeff& coeff,
                           EntropySymbols* symbols) {
    if (!symbols) return false;
    *symbols = EntropySymbols{};
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || !coeff.data || coeff.elem_bytes != 2 || coeff.lowpass) {
        symbols->error = "upstream match needs a tight int16 highpass band";
        return false;
    }
    EntropyTables tables;
    if (!LoadEntropyTables(enc_opaque, coeff.subband, &tables)) {
        symbols->error = "codebook load failed";
        return false;
    }
    if (tables.peaks) {
        symbols->error = "peaks coding is not in the GPU entropy path";
        return false;
    }
    *symbols = EncodeHighpassStages(static_cast<const int16_t*>(coeff.data), coeff.width, coeff.height,
                                    coeff.width, tables, true);
    if (!symbols->ok) return false;

    IMAGE* wavelet = nullptr;
    for (int k = 0; transforms[coeff.channel] && k < transforms[coeff.channel]->num_wavelets; ++k) {
        IMAGE* image = transforms[coeff.channel]->wavelet[k];
        if (image && image->level == coeff.level) wavelet = image;
    }
    if (!wavelet) {
        symbols->error = "wavelet for this band was not found";
        symbols->ok = false;
        return false;
    }
    int quantization = wavelet->quantization[coeff.band];
    if (quantization <= 0) quantization = wavelet->quant[coeff.band];
    if (quantization <= 0) quantization = 1;
    const int scale = wavelet->scale[coeff.band];

    const double t_header = NowMs();
    int bits_free = 0;
    if (!BuildHighpassPayload(enc_opaque, coeff.band, coeff.width, coeff.height, coeff.subband, quantization, scale,
                             tables, symbols->coeff_bytes, &symbols->payload, &bits_free)) {
        symbols->error = "band header or trailer failed";
        symbols->ok = false;
        return false;
    }
    symbols->timing.header_ms = NowMs() - t_header;
    (void)bits_free;
    symbols->payload_bytes = static_cast<int>(symbols->payload.size());
    symbols->coeff_hash = EntropyFnv64(coeff.data, static_cast<size_t>(coeff.width) * static_cast<size_t>(coeff.height) * sizeof(int16_t));
    symbols->payload_hash = EntropyFnv64(symbols->payload.data(), symbols->payload.size());

    EncodedPiece oracle;
    const double t_oracle = NowMs();
    const bool encoded = EncodeOneExternalBand(enc_opaque, transforms_opaque, coeff, &oracle);
    (void)t_oracle;
    if (!encoded || !oracle.ok) {
        symbols->error = "EncodeOneExternalBand failed";
        symbols->ok = false;
        return false;
    }
    symbols->byte_match = oracle.payload == symbols->payload;
    if (!symbols->byte_match) {
        symbols->error = "staged payload differs from EncodeQuantizedBand";
        symbols->ok = false;
        return false;
    }
    symbols->timing.total_ms += symbols->timing.header_ms;
    return true;
}

}  // namespace hypercine
