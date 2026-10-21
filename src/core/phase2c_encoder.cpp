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

#include "hypercine/phase2c.h"

#include "hypercine/canonical.h"
#include "hypercine/entropy_parallel.h"
#include "hypercine/entropy_stage.h"

#include "CFHDEncoder.h"
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "wavelet.h"
#include "frame.h"
#include "bitstream.h"
#include "image.h"
int ReverseByteOrder(int input);
}

#include "VideoBuffers.h"
#include "SampleMetadata.h"

#define private public
#define protected public
#include "SampleEncoder.h"
#undef private
#undef protected

#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hypercine {
namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto origin = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - origin).count();
}

double ProcessCpuSeconds() {
#ifdef _WIN32
    FILETIME create_time, exit_time, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel, &user)) return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 1e7;
#else
    return 0;
#endif
}

uint32_t RotR(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

void Sha256Transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

std::string Sha256Hex(const void* data, size_t size) {
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset + 64 <= size) {
        Sha256Transform(state, bytes + offset);
        offset += 64;
    }
    uint8_t tail[128] = {};
    const size_t rem = size - offset;
    if (rem > 0 && bytes) std::memcpy(tail, bytes + offset, rem);
    tail[rem] = 0x80u;
    const size_t pad_blocks = (rem >= 56) ? 2u : 1u;
    const uint64_t bit_len = static_cast<uint64_t>(size) * 8ull;
    for (int i = 0; i < 8; ++i) {
        tail[pad_blocks * 64 - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu);
    }
    Sha256Transform(state, tail);
    if (pad_blocks == 2u) Sha256Transform(state, tail + 64);

    static const char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) {
            const uint8_t v = static_cast<uint8_t>((state[i] >> ((3 - b) * 8)) & 0xFFu);
            out[static_cast<size_t>(i * 8 + b * 2)] = kHex[v >> 4];
            out[static_cast<size_t>(i * 8 + b * 2 + 1)] = kHex[v & 0x0Fu];
        }
    }
    return out;
}

namespace {

struct EncoderContext {
    CFHD_EncoderRef ref = nullptr;
    CSampleEncoder* sample = nullptr;
    ENCODER* encoder = nullptr;
    TRANSFORM** transforms = nullptr;
    int channel_count = 3;

    ~EncoderContext() {
        if (ref) CFHD_CloseEncoder(ref);
    }

    bool Init(int width, int height, const std::vector<uint8_t>& frame, bool deterministic_test_metadata = false) {
        if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) return false;
        if (CFHD_PrepareToEncode(ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                                 CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
            return false;
        }
        sample = reinterpret_cast<CSampleEncoder*>(ref);
        if (!sample) return false;
        if (CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), width * 4) != CFHD_ERROR_OKAY) {
            return false;
        }
        encoder = reinterpret_cast<ENCODER*>(sample->m_encoder);
        transforms = reinterpret_cast<TRANSFORM**>(sample->m_transformArray);
        channel_count = sample->m_channelCount;
        if (deterministic_test_metadata && encoder && encoder->metadata.global.block && encoder->metadata.global.size > 0) {
            const uint32_t fixed_guid[4] = {0x1337BEEFu, 0x20260420u, 0xCAFEBABEu, 0x43464844u};
            const char fixed_date[16] = "2026-01-01";
            const char fixed_time[16] = "00:00:00";
            const char fixed_tc[16] = "00:00:00:00";
            AddMetadata(&encoder->metadata.global, TAG_CLIP_GUID, static_cast<int>('G'), 16,
                        const_cast<uint32_t*>(fixed_guid));
            AddMetadata(&encoder->metadata.global, TAG_ENCODE_DATE, static_cast<int>('c'), 10,
                        reinterpret_cast<uint32_t*>(const_cast<char*>(fixed_date)));
            AddMetadata(&encoder->metadata.global, TAG_ENCODE_TIME, static_cast<int>('c'), 8,
                        reinterpret_cast<uint32_t*>(const_cast<char*>(fixed_time)));
            AddMetadata(&encoder->metadata.global, TAG_TIMECODE, static_cast<int>('c'), 11,
                        reinterpret_cast<uint32_t*>(const_cast<char*>(fixed_tc)));
        }
        return encoder != nullptr && transforms != nullptr && channel_count == 3;
    }
};

struct QuantTables {
    int prescale[3] = {};
    int quant[36] = {};
    int scale[36] = {};
    int widths[3] = {};
    int heights[3] = {};
    int color_space = 2;
    bool ok = false;
};

QuantTables ReadQuant(ENCODER* encoder, TRANSFORM* transforms[], int channel_count) {
    QuantTables q;
    if (!encoder || !encoder->frame || channel_count < 3 || !transforms) return q;
    q.color_space = encoder->input.color_space;
    for (int level = 0; level < 3; ++level) {
        if (!transforms[0]) return q;
        q.prescale[level] = transforms[0]->prescale[level];
    }
    for (int c = 0; c < 3; ++c) {
        IMAGE* image = encoder->frame->channel[c];
        if (!image || !transforms[c]) return q;
        q.widths[c] = image->width;
        q.heights[c] = image->height;
        for (int level = 0; level < 3; ++level) {
            IMAGE* wavelet = transforms[c]->wavelet[level];
            if (!wavelet) return q;
            for (int band = 0; band < 4; ++band) {
                int qv = wavelet->quantization[band];
                if (qv <= 0) qv = wavelet->quant[band];
                if (qv <= 0) qv = 1;
                q.quant[(c * 3 + level) * 4 + band] = wavelet->quant[band];
                q.scale[(c * 3 + level) * 4 + band] = wavelet->scale[band];
            }
        }
    }
    q.ok = true;
    return q;
}

const char* BandTag(int band) {
    switch (band) {
    case LL_BAND: return "LL";
    case LH_BAND: return "LH";
    case HL_BAND: return "HL";
    case HH_BAND: return "HH";
    default: return "?";
    }
}

const char* ChannelTag(int channel) {
    switch (channel) {
    case 0: return "Y";
    case 1: return "C1";
    case 2: return "C2";
    default: return "C";
    }
}

std::string BandName(int channel, int level, int band) {
    return std::string(ChannelTag(channel)) + std::to_string(level) + BandTag(band);
}

bool DecodeCfhdSample(const std::vector<uint8_t>& sample, int width, int height,
                      std::vector<uint8_t>* image, int* pitch_out) {
    CFHD_DecoderRef decoder = nullptr;
    if (CFHD_OpenDecoder(&decoder, nullptr) != CFHD_ERROR_OKAY) return false;
    int aw = 0, ah = 0;
    CFHD_PixelFormat fmt = CFHD_PIXEL_FORMAT_UNKNOWN;
    CFHD_Error err = CFHD_PrepareToDecode(decoder, 0, 0, CFHD_PIXEL_FORMAT_BGRA, CFHD_DECODED_RESOLUTION_FULL,
                                          CFHD_DECODING_FLAGS_NONE, const_cast<uint8_t*>(sample.data()), sample.size(),
                                          &aw, &ah, &fmt);
    if (err != CFHD_ERROR_OKAY) {
        CFHD_CloseDecoder(decoder);
        return false;
    }
    int32_t pitch = 0;
    CFHD_GetImagePitch(aw > 0 ? aw : width, fmt, &pitch);
    if (pitch <= 0) pitch = width * 4;
    image->assign(static_cast<size_t>(pitch) * static_cast<size_t>(ah > 0 ? ah : height), 0);
    CFHD_MetadataRef metadata = nullptr;
    CFHD_OpenMetadata(&metadata);
    if (metadata) {
        CFHD_InitSampleMetadata(metadata, METADATATYPE_ORIGINAL, const_cast<uint8_t*>(sample.data()),
                                static_cast<uint32_t>(sample.size()));
        unsigned int processflags = PROCESSING_ALL_ON;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_PROCESS_PATH, METADATATYPE_UINT32, &processflags, sizeof(processflags));
    }
    err = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(), image->data(), pitch);
    if (metadata) CFHD_CloseMetadata(metadata);
    CFHD_CloseDecoder(decoder);
    if (pitch_out) *pitch_out = pitch;
    return err == CFHD_ERROR_OKAY && aw == width && ah == height;
}

double ComputeBestPsnr(const std::vector<uint8_t>& src, const std::vector<uint8_t>& dec, int width, int height, int pitch) {
    double best = 0;
    for (int flip = 0; flip < 2; ++flip) {
        double sse = 0;
        int64_t count = 0;
        for (int y = 0; y < height; ++y) {
            const int ry = flip ? (height - 1 - y) : y;
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int a = src[(static_cast<size_t>(y) * width + x) * 4 + c];
                    const int b = dec[static_cast<size_t>(ry) * pitch + static_cast<size_t>(x) * 4 + c];
                    const double d = static_cast<double>(a - b);
                    sse += d * d;
                    ++count;
                }
            }
        }
        const double mse = count ? sse / static_cast<double>(count) : 0;
        const double psnr = mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
        best = std::max(best, psnr);
    }
    return best;
}

// Fast zero-allocation sample concatenation directly from `pieces` in canonical order.
bool AssembleSampleFast(ENCODER* encoder, TRANSFORM* transforms[], int channel_count,
                        const std::vector<EncodedPiece>& pieces, const int piece_of[3][3][4],
                        std::vector<uint8_t>* out_bytes, double* concat_ms) {
    if (!encoder || !transforms || !out_bytes || pieces.size() < 30) return false;
    size_t need = 16384u;
    for (const EncodedPiece& p : pieces) {
        if (!p.ok || p.bits_free != BITSTREAM_BUFFER_SIZE) return false;
        need += p.payload.size() + 64u;
    }
    if (out_bytes->size() < need) out_bytes->resize(need);

    BITSTREAM output;
    std::memset(&output, 0, sizeof(output));
    InitBitstreamBuffer(&output, out_bytes->data(), out_bytes->size(), BITSTREAM_ACCESS_WRITE);

    const double t0 = NowMs();
    const int num_channels = channel_count;
    const int subband_count = SubbandCount(transforms[0]);
    const bool encode_iframe = encoder->gop_length <= 1;
    encoder->frame_number++;
    const uint32_t frame_number = encoder->frame_number;
    const int precision = encoder->codec.precision;
    uint32_t* channel_size_vector = nullptr;

    if (!encode_iframe) {
        PutVideoGroupHeader(&output, transforms[0], num_channels, subband_count,
                            &channel_size_vector, precision, frame_number,
                            encoder->input.format, encoder->input.color_space,
                            encoder->encoder_quality, encoder->encoded_format,
                            encoder->input.width, encoder->input.height, encoder->display.height,
                            encoder->presentationWidth, encoder->presentationHeight);
    } else {
        PutVideoIntraFrameHeader(&output, transforms[0], num_channels, subband_count,
                                 &channel_size_vector, precision, frame_number,
                                 encoder->input.format, encoder->input.color_space,
                                 encoder->encoder_quality, encoder->encoded_format,
                                 encoder->input.width, encoder->input.height, encoder->display.height,
                                 encoder->presentationWidth, encoder->presentationHeight);
    }

    if (encoder->video_channels > 1) {
        PutTagPairOptional(&output, CODEC_TAG_ENCODED_CHANNELS, static_cast<int>(encoder->video_channels));
        PutTagPairOptional(&output, CODEC_TAG_ENCODED_CHANNEL_NUMBER, static_cast<int>(encoder->current_channel));
    } else if (encoder->current_channel || encoder->ignore_overrides) {
        PutTagPairOptional(&output, CODEC_TAG_ENCODED_CHANNEL_NUMBER, static_cast<int>(encoder->current_channel));
    }

    SizeTagPush(&output, CODEC_TAG_SAMPLE_SIZE);

    if (encoder->metadata.global.block && encoder->metadata.global.size) {
        int len = static_cast<int>(encoder->metadata.global.size);
        len = RemoveHiddenMetadata(static_cast<unsigned char*>(encoder->metadata.global.block), len);
        if (len) {
            PutTagPairOptional(&output, CODEC_TAG_METADATA, len >> 2);
            std::memcpy(output.lpCurrentWord, encoder->metadata.global.block, static_cast<size_t>(len));
            output.nWordsUsed += len;
            output.lpCurrentWord += len;
        }
    }
    if (encoder->metadata.local.block && encoder->metadata.local.size) {
        int len = static_cast<int>(encoder->metadata.local.size);
        len = RemoveHiddenMetadata(static_cast<unsigned char*>(encoder->metadata.local.block), len);
        if (len) {
            PutTagPairOptional(&output, CODEC_TAG_METADATA, len >> 2);
            std::memcpy(output.lpCurrentWord, encoder->metadata.local.block, static_cast<size_t>(len));
            output.nWordsUsed += len;
            output.lpCurrentWord += len;
        }
    }

    constexpr int kFreeMeta = 512;
    {
        PutTagPairOptional(&output, CODEC_TAG_METADATA, kFreeMeta >> 2);
        uint32_t* ptr = reinterpret_cast<uint32_t*>(output.lpCurrentWord);
        *ptr++ = TAG_FREESPACE;
        *ptr++ = kFreeMeta - 8;
        std::memset(ptr, 0, kFreeMeta - 8);
        output.nWordsUsed += kFreeMeta;
        output.lpCurrentWord += kFreeMeta;
    }

    PutVideoGroupExtension(&output, &encoder->codec);
#if _CODEC_SAMPLE_FLAGS
    PutVideoSampleFlags(&output, &encoder->codec);
#endif

    if (!channel_size_vector) return false;

    constexpr int kHighpassOrder[3] = {LH_BAND, HL_BAND, HH_BAND};
    auto append_piece = [&](int p_idx) {
        if (p_idx < 0) return;
        const auto& bytes = pieces[static_cast<size_t>(p_idx)].payload;
        if (bytes.empty()) return;
        std::memcpy(output.lpCurrentWord, bytes.data(), bytes.size());
        output.nWordsUsed += static_cast<int>(bytes.size());
        output.lpCurrentWord += bytes.size();
    };

    for (int channel = 0; channel < num_channels; ++channel) {
        PadBits(&output);
        if (channel > 0) PutVideoChannelHeader(&output, channel);
        const int channel_begin = BitstreamSize(&output);

        TRANSFORM* tf = transforms[channel];
        const int top = tf->num_wavelets - 1;
        append_piece(piece_of[channel][top][LL_BAND]);

        for (int k = top; k >= 0; --k) {
            IMAGE* wavelet = tf->wavelet[k];
            if (!wavelet) continue;
            PutVideoHighPassHeader(&output, wavelet->wavelet_type, k + 1, wavelet->level,
                                   wavelet->width, wavelet->height, wavelet->num_bands,
                                   wavelet->scale[0], 0);
            for (int i = 0; i < 3; ++i) {
                append_piece(piece_of[channel][k][kHighpassOrder[i]]);
            }
            PutVideoHighPassTrailer(&output, 0, 0, 0, 0, 0);
        }

        PadBits(&output);
        const int channel_bytes = BitstreamSize(&output) - channel_begin;
        channel_size_vector[channel] = static_cast<uint32_t>(ReverseByteOrder(channel_bytes));
    }

    if (encode_iframe) PutVideoIntraFrameTrailer(&output);
    else PutVideoGroupTrailer(&output);
    SizeTagPop(&output);

    if (concat_ms) *concat_ms = NowMs() - t0;
    if (output.error != 0 || output.nWordsUsed <= 0) return false;
    out_bytes->resize(static_cast<size_t>(output.nWordsUsed));
    return true;
}

void ComputeStats(std::vector<double> vals, double* mean, double* median, double* p95,
                  double* min_v = nullptr, double* max_v = nullptr) {
    if (vals.empty()) return;
    double sum = 0.0;
    for (double v : vals) sum += v;
    if (mean) *mean = sum / static_cast<double>(vals.size());
    std::sort(vals.begin(), vals.end());
    if (min_v) *min_v = vals.front();
    if (max_v) *max_v = vals.back();
    const size_t n = vals.size();
    if (median) {
        *median = (n & 1u) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
    }
    if (p95) {
        size_t idx = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(n))) - 1;
        if (idx >= n) idx = n - 1;
        *p95 = vals[idx];
    }
}

}  // namespace

Phase2BCoverageAudit AuditPhase2BCoverage(int width, int height, uint32_t seed) {
    (void)seed;
    Phase2BCoverageAudit audit;
    audit.width = width;
    audit.height = height;

    const std::vector<uint8_t> frame = MakeFrame(width, height, CanonicalPattern::Canonical);
    EncoderContext ctx;
    if (!ctx.Init(width, height, frame)) {
        audit.error = "failed to initialize upstream encoder";
        return audit;
    }

    const QuantTables quant = ReadQuant(ctx.encoder, ctx.transforms, ctx.channel_count);
    const std::vector<BandRef> refs = EnumerateBands(reinterpret_cast<transform**>(ctx.transforms), ctx.channel_count);
    if (!quant.ok || refs.size() != 30) {
        audit.error = "expected 30 canonical subbands";
        return audit;
    }

    HyperCineGpuSlot* w_slot = HyperCine_GpuSlotCreateForPhase2C(width, height, 1);
    GpuEntropySession* session = CreateGpuEntropySession();
    GpuEntropyFrameSlot* e_slot = CreateGpuEntropyFrameSlot(width, height);
    if (!w_slot || !session || !e_slot) {
        audit.error = "failed to allocate GPU resources for coverage audit";
        DestroyGpuEntropyFrameSlot(e_slot);
        DestroyGpuEntropySession(session);
        HyperCine_GpuSlotDestroy(w_slot);
        return audit;
    }

    const void* device_bgra = nullptr;
    HyperCine_GpuSlotUploadBgra(w_slot, frame.data(), width * 4, width, height, &device_bgra);

    const double t_frame0 = NowMs();
    HyperCine_GpuSlotBegin(w_slot, HYPERCINE_INPUT_DEVICE_BGRA, HYPERCINE_XPORT_DEVICE_LOWPASS_D2H,
                           static_cast<const uint8_t*>(device_bgra), width * 4, width, height, quant.color_space,
                           quant.widths, quant.heights, quant.prescale, quant.quant);
    HyperCine_GpuSlotKick(w_slot);
    while (HyperCine_GpuSlotGpuIdle(w_slot) == 0) {}
    HyperCineStreamTiming w_timing{};
    HyperCine_GpuSlotReadTiming(w_slot, &w_timing);
    audit.gpu_wavelet_ms = w_timing.color_ms + w_timing.compute_ms;

    std::vector<GpuBandRequest> hp_reqs;
    std::vector<EntropyTables> hp_tables;
    std::vector<int> hp_ref_idx;
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        const BandRef& ref = refs[static_cast<size_t>(i)];
        if (ref.lowpass) continue;
        const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
        EntropyTables tbl;
        LoadEntropyTables(reinterpret_cast<encoder*>(ctx.encoder), ref.subband, &tbl);
        hp_reqs.push_back(GpuBandRequest{d_ptr, ref.width, ref.height, ref.width, ref.channel, ref.wavelet_index, ref.band, ref.subband});
        hp_tables.push_back(std::move(tbl));
        hp_ref_idx.push_back(i);
    }

    GpuEntropyFrameConfig e_cfg;
    e_cfg.entropy_streams = 4;
    e_cfg.d2h_mode = CompressedD2HMode::FrameConsolidated;
    e_cfg.record_substage_events = true;
    QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), e_cfg);
    while (!IsGpuEntropyFrameComplete(e_slot)) {}
    GpuFrameEntropyStats e_stats{};
    CollectGpuFrameEntropyStats(e_slot, &e_stats);
    audit.gpu_entropy_ms = e_stats.gpu_entropy_wall_ms;
    audit.d2h_ms = static_cast<double>(w_timing.d2h_ms) + e_stats.compressed_d2h_ms;

    audit.total_bands = static_cast<int>(refs.size());
    audit.bands.resize(refs.size());
    std::vector<EncodedPiece> pieces(refs.size());
    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));

    int hp_cursor = 0;
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        const BandRef& ref = refs[static_cast<size_t>(i)];
        piece_of[ref.channel][ref.wavelet_index][ref.band] = i;
        Phase2BSubbandAuditEntry& entry = audit.bands[static_cast<size_t>(i)];
        entry.index = i;
        entry.channel = ref.channel;
        entry.level = ref.level;
        entry.band = ref.band;
        entry.subband = ref.subband;
        entry.name = BandName(ref.channel, ref.level, ref.band);
        entry.lowpass = ref.lowpass;
        entry.width = ref.width;
        entry.height = ref.height;
        entry.coeff_count = ref.width * ref.height;

        EncodedPiece& out = pieces[static_cast<size_t>(i)];
        out.channel = ref.channel;
        out.level = ref.level;
        out.band = ref.band;
        out.subband = ref.subband;
        out.lowpass = ref.lowpass;

        if (ref.lowpass) {
            entry.gpu_entropy = false;
            entry.cpu_entropy = true;
            entry.raw_coeff_d2h_bytes = static_cast<size_t>(entry.coeff_count) * sizeof(int16_t);
            entry.compressed_payload_d2h_bytes = 0;
            const int16_t* h_ptr = HyperCine_GpuSlotBandHostPtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
            const double t0 = NowMs();
            int bits_free = 0;
            BuildLowpassPayloadPtr(ref.subband, ref.level, ref.width, ref.height,
                                   h_ptr, false, &out.payload, &bits_free);
            entry.cpu_encode_or_wrap_ms = NowMs() - t0;
            out.ok = true;
            out.bits_free = bits_free;
            entry.final_band_stream_bytes = out.payload.size();

            audit.cpu_lowpass_bands++;
            audit.lowpass_total_coeffs += entry.coeff_count;
            audit.lowpass_raw_d2h_bytes += entry.raw_coeff_d2h_bytes;
            audit.lowpass_encoded_bytes += entry.final_band_stream_bytes;
            audit.lowpass_cpu_ms += entry.cpu_encode_or_wrap_ms;
        } else {
            entry.gpu_entropy = true;
            entry.cpu_entropy = false;
            entry.raw_coeff_d2h_bytes = 0;
            GpuBandCompressedView view{};
            GetGpuBandCompressedView(e_slot, hp_cursor, &view);
            entry.compressed_payload_d2h_bytes = static_cast<size_t>(view.byte_count);
            entry.gpu_entropy_ms = e_stats.band_timings[hp_cursor].total_ms;

            IMAGE* wavelet = ctx.transforms[ref.channel]->wavelet[ref.wavelet_index];
            int qv = wavelet->quantization[ref.band];
            if (qv <= 0) qv = wavelet->quant[ref.band];
            if (qv <= 0) qv = 1;
            int bits_free = 0;
            const double t0 = NowMs();
            BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx.encoder), ref.band, ref.width, ref.height,
                                    ref.subband, qv, wavelet->scale[ref.band], hp_tables[static_cast<size_t>(hp_cursor)],
                                    view.bytes, static_cast<size_t>(view.byte_count), &out.payload, &bits_free);
            entry.cpu_encode_or_wrap_ms = NowMs() - t0;
            out.ok = true;
            out.bits_free = bits_free;
            entry.final_band_stream_bytes = out.payload.size();

            audit.gpu_entropy_bands++;
            audit.highpass_total_coeffs += entry.coeff_count;
            audit.highpass_raw_equiv_bytes += static_cast<size_t>(entry.coeff_count) * sizeof(int16_t);
            audit.highpass_compressed_d2h_bytes += entry.compressed_payload_d2h_bytes;
            audit.highpass_encoded_bytes += entry.final_band_stream_bytes;
            audit.highpass_header_cpu_ms += entry.cpu_encode_or_wrap_ms;
            ++hp_cursor;
        }
    }

    std::vector<uint8_t> sample_bytes;
    AssembleSampleFast(ctx.encoder, ctx.transforms, ctx.channel_count, pieces, piece_of, &sample_bytes, &audit.assembly_cpu_ms);
    audit.total_frame_ms = NowMs() - t_frame0;
    audit.total_d2h_bytes = audit.lowpass_raw_d2h_bytes + audit.highpass_compressed_d2h_bytes;
    audit.lowpass_pct_of_frame = audit.total_frame_ms > 0.0 ? (100.0 * audit.lowpass_cpu_ms / audit.total_frame_ms) : 0.0;
    audit.ok = true;

    DestroyGpuEntropyFrameSlot(e_slot);
    DestroyGpuEntropySession(session);
    HyperCine_GpuSlotDestroy(w_slot);
    return audit;
}

Phase2CIdentityReport VerifyPhase2CIdentity(int width, int height, uint32_t seed) {
    (void)seed;
    Phase2CIdentityReport rep;
    rep.width = width;
    rep.height = height;

    const std::vector<uint8_t> frame = MakeFrame(width, height, CanonicalPattern::Canonical);
    EncoderContext ctx;
    if (!ctx.Init(width, height, frame, true)) {
        rep.error = "failed to initialize upstream encoder";
        return rep;
    }

    if (!RefreshUpstreamPyramid(reinterpret_cast<encoder*>(ctx.encoder),
                                reinterpret_cast<transform**>(ctx.transforms),
                                ctx.channel_count, frame.data(), width * 4,
                                ctx.sample->m_scratchBuffer, ctx.sample->m_scratchBufferSize)) {
        rep.error = "RefreshUpstreamPyramid failed";
        return rep;
    }

    const QuantTables quant = ReadQuant(ctx.encoder, ctx.transforms, ctx.channel_count);
    const std::vector<BandRef> refs = EnumerateBands(reinterpret_cast<transform**>(ctx.transforms), ctx.channel_count);
    if (!quant.ok || refs.size() != 30) {
        rep.error = "invalid pyramid band count";
        return rep;
    }

    // 1. Extract tight reference coefficients and CPU external band payloads for all 30 bands
    // immediately after RefreshUpstreamPyramid (before EncodeEntropy's EncodeQuantizedBand packs bands to int8 in-place).
    std::vector<std::vector<int16_t>> cpu_coeffs(refs.size());
    std::vector<EncodedPiece> cpu_pieces(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const BandRef& ref = refs[i];
        IMAGE* wavelet = ctx.transforms[ref.channel]->wavelet[ref.wavelet_index];
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        const PIXEL* src = wavelet->band[ref.band];
        cpu_coeffs[i].resize(static_cast<size_t>(ref.width) * static_cast<size_t>(ref.height));
        for (int y = 0; y < ref.height; ++y) {
            std::memcpy(cpu_coeffs[i].data() + static_cast<size_t>(y) * ref.width,
                        src + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(ref.width) * sizeof(int16_t));
        }
        ExternalCoeff ext{};
        ext.channel = ref.channel;
        ext.level = ref.level;
        ext.band = ref.band;
        ext.subband = ref.subband;
        ext.lowpass = ref.lowpass;
        ext.data = cpu_coeffs[i].data();
        ext.elem_bytes = 2;
        ext.width = ref.width;
        ext.height = ref.height;
        if (!EncodeOneExternalBand(reinterpret_cast<encoder*>(ctx.encoder),
                                   reinterpret_cast<transform**>(ctx.transforms),
                                   ext, &cpu_pieces[i])) {
            rep.error = "CPU EncodeOneExternalBand failed";
            return rep;
        }
    }

    const uint32_t base_frame_num = ctx.encoder->frame_number;
    EntropyOutput cpu_ref = EncodeEntropy(reinterpret_cast<encoder*>(ctx.encoder),
                                          reinterpret_cast<transform**>(ctx.transforms),
                                          ctx.channel_count, 1, EntropySchedule::Serial, true);
    if (!cpu_ref.success || !cpu_ref.byte_match) {
        rep.error = "CPU reference EncodeEntropy failed: " + cpu_ref.error;
        return rep;
    }
    rep.cpu_sample_sha256 = Sha256Hex(cpu_ref.bitstream.data(), cpu_ref.bitstream.size());
    rep.sample_bytes = cpu_ref.bitstream.size();

    // 2. Run GPU pyramid + GPU entropy (Consolidated, Streamed, Multi-stream 1/2/4/8, and GPU lowpass pack).
    HyperCineGpuSlot* w_slot = HyperCine_GpuSlotCreateForPhase2C(width, height, 1);
    GpuEntropySession* session = CreateGpuEntropySession();
    GpuEntropyFrameSlot* e_slot = CreateGpuEntropyFrameSlot(width, height);
    if (!w_slot || !session || !e_slot) {
        rep.error = "GPU slot allocation failed";
        DestroyGpuEntropyFrameSlot(e_slot);
        DestroyGpuEntropySession(session);
        HyperCine_GpuSlotDestroy(w_slot);
        return rep;
    }

    const void* device_bgra = nullptr;
    HyperCine_GpuSlotUploadBgra(w_slot, frame.data(), width * 4, width, height, &device_bgra);
    HyperCine_GpuSlotBegin(w_slot, HYPERCINE_INPUT_DEVICE_BGRA, HYPERCINE_XPORT_DEVICE_LOWPASS_D2H,
                           static_cast<const uint8_t*>(device_bgra), width * 4, width, height, quant.color_space,
                           quant.widths, quant.heights, quant.prescale, quant.quant);
    HyperCine_GpuSlotKick(w_slot);
    while (HyperCine_GpuSlotGpuIdle(w_slot) == 0) {}

    // Check 1: Pre-entropy coefficient equality on all 30 bands.
    rep.pre_entropy_coeff_equal = true;
    rep.bands.resize(refs.size());
    std::vector<GpuBandRequest> hp_reqs;
    std::vector<EntropyTables> hp_tables;
    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));

    for (size_t i = 0; i < refs.size(); ++i) {
        const BandRef& ref = refs[i];
        piece_of[ref.channel][ref.wavelet_index][ref.band] = static_cast<int>(i);
        Phase2CBandIdentity& b_id = rep.bands[i];
        b_id.index = static_cast<int>(i);
        b_id.channel = ref.channel;
        b_id.level = ref.level;
        b_id.band = ref.band;
        b_id.subband = ref.subband;
        b_id.name = BandName(ref.channel, ref.level, ref.band);
        b_id.lowpass = ref.lowpass;
        b_id.width = ref.width;
        b_id.height = ref.height;

        const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
        std::vector<int16_t> gpu_coeff(static_cast<size_t>(ref.width) * static_cast<size_t>(ref.height));
        cudaMemcpy(gpu_coeff.data(), d_ptr, gpu_coeff.size() * sizeof(int16_t), cudaMemcpyDeviceToHost);
        b_id.coeff_equal = (gpu_coeff == cpu_coeffs[i]);
        if (!b_id.coeff_equal) {
            rep.pre_entropy_coeff_equal = false;
            if (rep.error.empty()) {
                for (size_t k = 0; k < gpu_coeff.size(); ++k) {
                    if (gpu_coeff[k] != cpu_coeffs[i][k]) {
                        rep.error = "coeff mismatch in band " + b_id.name + " (wavelet_index=" +
                                    std::to_string(ref.wavelet_index) + ", level=" + std::to_string(ref.level) +
                                    ") at k=" + std::to_string(k) + " cpu=" + std::to_string(cpu_coeffs[i][k]) +
                                    " gpu=" + std::to_string(gpu_coeff[k]);
                        break;
                    }
                }
            }
        }
        b_id.coeff_sha256 = Sha256Hex(gpu_coeff.data(), gpu_coeff.size() * sizeof(int16_t));

        if (!ref.lowpass) {
            EntropyTables tbl;
            LoadEntropyTables(reinterpret_cast<encoder*>(ctx.encoder), ref.subband, &tbl);
            hp_reqs.push_back(GpuBandRequest{d_ptr, ref.width, ref.height, ref.width, ref.channel, ref.wavelet_index, ref.band, ref.subband});
            hp_tables.push_back(std::move(tbl));
        }
    }

    auto run_and_assemble = [&](int streams, CompressedD2HMode d2h_mode, bool gpu_lp,
                                std::vector<EncodedPiece>* out_pieces, std::vector<uint8_t>* out_sample) -> bool {
        HyperCine_GpuSlotBegin(w_slot, HYPERCINE_INPUT_DEVICE_BGRA, HYPERCINE_XPORT_DEVICE_LOWPASS_D2H,
                               static_cast<const uint8_t*>(device_bgra), width * 4, width, height, quant.color_space,
                               quant.widths, quant.heights, quant.prescale, quant.quant);
        HyperCine_GpuSlotKickStaged(w_slot, nullptr, nullptr, gpu_lp ? 1 : 0);
        while (HyperCine_GpuSlotGpuIdle(w_slot) == 0) {}
        GpuEntropyFrameConfig e_cfg;
        e_cfg.entropy_streams = streams;
        e_cfg.d2h_mode = d2h_mode;
        e_cfg.record_substage_events = false;
        if (!QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), e_cfg)) {
            return false;
        }
        if (d2h_mode == CompressedD2HMode::BandStreamed) {
            while (!IsGpuEntropyFrameComplete(e_slot)) {
                PollNextStreamedBandReady(e_slot);
            }
        } else {
            while (!IsGpuEntropyFrameComplete(e_slot)) {}
        }

        out_pieces->resize(refs.size());
        int hp_idx = 0;
        for (size_t i = 0; i < refs.size(); ++i) {
            const BandRef& ref = refs[i];
            EncodedPiece& p = (*out_pieces)[i];
            p.channel = ref.channel;
            p.level = ref.level;
            p.band = ref.band;
            p.subband = ref.subband;
            p.lowpass = ref.lowpass;
            if (ref.lowpass) {
                const int16_t* h_ptr = HyperCine_GpuSlotBandHostPtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
                if (!BuildLowpassPayloadPtr(ref.subband, ref.level, ref.width, ref.height,
                                            h_ptr, gpu_lp, &p.payload, &p.bits_free)) {
                    return false;
                }
                p.ok = true;
            } else {
                GpuBandCompressedView view{};
                GetGpuBandCompressedView(e_slot, hp_idx, &view);
                IMAGE* wavelet = ctx.transforms[ref.channel]->wavelet[ref.wavelet_index];
                int qv = wavelet->quantization[ref.band];
                if (qv <= 0) qv = wavelet->quant[ref.band];
                if (qv <= 0) qv = 1;
                if (!BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx.encoder), ref.band, ref.width, ref.height,
                                             ref.subband, qv, wavelet->scale[ref.band], hp_tables[static_cast<size_t>(hp_idx)],
                                             view.bytes, static_cast<size_t>(view.byte_count), &p.payload, &p.bits_free)) {
                    return false;
                }
                p.ok = true;
                ++hp_idx;
            }
        }
        ctx.encoder->frame_number = base_frame_num;
        return AssembleSampleFast(ctx.encoder, ctx.transforms, ctx.channel_count, *out_pieces, piece_of, out_sample, nullptr);
    };

    std::vector<EncodedPiece> gpu_pieces;
    std::vector<uint8_t> gpu_consolidated_sample;
    if (!run_and_assemble(4, CompressedD2HMode::FrameConsolidated, false, &gpu_pieces, &gpu_consolidated_sample)) {
        rep.error = "GPU consolidated sample encode failed";
        DestroyGpuEntropyFrameSlot(e_slot);
        DestroyGpuEntropySession(session);
        HyperCine_GpuSlotDestroy(w_slot);
        return rep;
    }

    // Checks 2 & 3: Per-band payload length and byte equality + SHA-256.
    rep.per_band_payload_len_equal = true;
    rep.per_band_payload_byte_equal = true;
    for (size_t i = 0; i < refs.size(); ++i) {
        Phase2CBandIdentity& b_id = rep.bands[i];
        b_id.cpu_payload_bytes = cpu_pieces[i].payload.size();
        b_id.gpu_payload_bytes = gpu_pieces[i].payload.size();
        b_id.payload_len_equal = (b_id.cpu_payload_bytes == b_id.gpu_payload_bytes);
        b_id.payload_byte_equal = (cpu_pieces[i].payload == gpu_pieces[i].payload);
        if (!b_id.payload_len_equal) rep.per_band_payload_len_equal = false;
        if (!b_id.payload_byte_equal) rep.per_band_payload_byte_equal = false;
        b_id.cpu_payload_sha256 = Sha256Hex(cpu_pieces[i].payload.data(), cpu_pieces[i].payload.size());
        b_id.gpu_payload_sha256 = Sha256Hex(gpu_pieces[i].payload.data(), gpu_pieces[i].payload.size());
    }

    // Check 4: Complete frame/sample byte equality across Consolidated, Streamed, 1/2/4/8 streams, and GPU lowpass.
    rep.gpu_consolidated_sample_sha256 = Sha256Hex(gpu_consolidated_sample.data(), gpu_consolidated_sample.size());
    rep.complete_sample_byte_equal = (gpu_consolidated_sample == cpu_ref.bitstream);
    if (!rep.complete_sample_byte_equal) {
        const size_t n = std::min(gpu_consolidated_sample.size(), cpu_ref.bitstream.size());
        for (size_t i = 0; i < n; ++i) {
            if (gpu_consolidated_sample[i] != cpu_ref.bitstream[i]) {
                rep.mismatch_offset = static_cast<int>(i);
                break;
            }
        }
        if (rep.mismatch_offset < 0) rep.mismatch_offset = static_cast<int>(n);
    }

    std::vector<EncodedPiece> tmp_pieces;
    std::vector<uint8_t> gpu_streamed_sample;
    if (run_and_assemble(4, CompressedD2HMode::BandStreamed, false, &tmp_pieces, &gpu_streamed_sample)) {
        rep.gpu_streamed_sample_sha256 = Sha256Hex(gpu_streamed_sample.data(), gpu_streamed_sample.size());
        rep.streamed_d2h_sample_byte_equal = (gpu_streamed_sample == cpu_ref.bitstream);
    }

    rep.multistream_sample_byte_equal = true;
    for (int s_cnt : {1, 2, 4, 8}) {
        std::vector<uint8_t> s_sample;
        if (!run_and_assemble(s_cnt, CompressedD2HMode::FrameConsolidated, false, &tmp_pieces, &s_sample) ||
            s_sample != cpu_ref.bitstream) {
            rep.multistream_sample_byte_equal = false;
        }
    }

    std::vector<uint8_t> gpu_lp_sample;
    if (run_and_assemble(4, CompressedD2HMode::FrameConsolidated, true, &tmp_pieces, &gpu_lp_sample)) {
        rep.gpu_lowpass_sample_sha256 = Sha256Hex(gpu_lp_sample.data(), gpu_lp_sample.size());
        rep.gpu_lowpass_sample_byte_equal = (gpu_lp_sample == cpu_ref.bitstream);
    }

    // Check 5: Upstream decoder pixel equality.
    std::vector<uint8_t> cpu_dec, gpu_dec;
    int cpu_pitch = 0, gpu_pitch = 0;
    if (DecodeCfhdSample(cpu_ref.bitstream, width, height, &cpu_dec, &cpu_pitch) &&
        DecodeCfhdSample(gpu_consolidated_sample, width, height, &gpu_dec, &gpu_pitch) &&
        cpu_dec.size() == gpu_dec.size()) {
        int max_diff = 0;
        for (size_t i = 0; i < cpu_dec.size(); ++i) {
            max_diff = std::max(max_diff, std::abs(static_cast<int>(cpu_dec[i]) - static_cast<int>(gpu_dec[i])));
        }
        rep.decoded_max_abs_diff = max_diff;
        rep.upstream_decoder_pixel_equal = (max_diff == 0);
        rep.cpu_decoded_pixels_sha256 = Sha256Hex(cpu_dec.data(), cpu_dec.size());
        rep.gpu_decoded_pixels_sha256 = Sha256Hex(gpu_dec.data(), gpu_dec.size());
        rep.psnr_vs_source_db = ComputeBestPsnr(frame, gpu_dec, width, height, gpu_pitch);
    }

    rep.ok = rep.pre_entropy_coeff_equal &&
             rep.per_band_payload_len_equal &&
             rep.per_band_payload_byte_equal &&
             rep.complete_sample_byte_equal &&
             rep.streamed_d2h_sample_byte_equal &&
             rep.multistream_sample_byte_equal &&
             rep.gpu_lowpass_sample_byte_equal &&
             rep.upstream_decoder_pixel_equal &&
             rep.psnr_vs_source_db > 35.0;

    DestroyGpuEntropyFrameSlot(e_slot);
    DestroyGpuEntropySession(session);
    HyperCine_GpuSlotDestroy(w_slot);
    return rep;
}

std::vector<Phase2CConcurrencyPoint> BenchmarkPhase2CEntropyConcurrency(int width, int height,
                                                                        int iterations,
                                                                        uint32_t seed) {
    (void)seed;
    std::vector<Phase2CConcurrencyPoint> points;
    if (iterations < 1) iterations = 1;

    const std::vector<uint8_t> frame = MakeFrame(width, height, CanonicalPattern::Canonical);
    EncoderContext ctx;
    if (!ctx.Init(width, height, frame)) return points;

    const QuantTables quant = ReadQuant(ctx.encoder, ctx.transforms, ctx.channel_count);
    const std::vector<BandRef> refs = EnumerateBands(reinterpret_cast<transform**>(ctx.transforms), ctx.channel_count);
    if (!quant.ok || refs.size() != 30) return points;

    HyperCineGpuSlot* w_slot = HyperCine_GpuSlotCreateForPhase2C(width, height, 1);
    GpuEntropySession* session = CreateGpuEntropySession();
    GpuEntropyFrameSlot* e_slot = CreateGpuEntropyFrameSlot(width, height);
    if (!w_slot || !session || !e_slot) {
        DestroyGpuEntropyFrameSlot(e_slot);
        DestroyGpuEntropySession(session);
        HyperCine_GpuSlotDestroy(w_slot);
        return points;
    }

    const void* device_bgra = nullptr;
    HyperCine_GpuSlotUploadBgra(w_slot, frame.data(), width * 4, width, height, &device_bgra);
    HyperCine_GpuSlotBegin(w_slot, HYPERCINE_INPUT_DEVICE_BGRA, HYPERCINE_XPORT_DEVICE_LOWPASS_D2H,
                           static_cast<const uint8_t*>(device_bgra), width * 4, width, height, quant.color_space,
                           quant.widths, quant.heights, quant.prescale, quant.quant);
    HyperCine_GpuSlotKick(w_slot);
    while (HyperCine_GpuSlotGpuIdle(w_slot) == 0) {}

    std::vector<GpuBandRequest> hp_reqs;
    std::vector<EntropyTables> hp_tables;
    size_t total_hp_coeff_bytes = 0;
    for (const BandRef& ref : refs) {
        if (ref.lowpass) continue;
        const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
        EntropyTables tbl;
        LoadEntropyTables(reinterpret_cast<encoder*>(ctx.encoder), ref.subband, &tbl);
        hp_reqs.push_back(GpuBandRequest{d_ptr, ref.width, ref.height, ref.width, ref.channel, ref.wavelet_index, ref.band, ref.subband});
        hp_tables.push_back(std::move(tbl));
        total_hp_coeff_bytes += static_cast<size_t>(ref.width) * static_cast<size_t>(ref.height) * sizeof(int16_t);
    }

    // Reference payloads from 1-stream EncodeHighpassBatch.
    std::vector<GpuBandResult> ref_results(hp_reqs.size());
    EntropyTiming ref_wall{};
    EncodeHighpassBatch(session, hp_reqs.data(), ref_results.data(), static_cast<int>(hp_reqs.size()),
                        hp_tables.data(), GpuEntropyDispatch::OneStream, 1, &ref_wall);

    // 1. Legacy Phase 2B dispatch modes (OneStream, Batched, MultiStream 4).
    struct LegacySpec {
        const char* label;
        GpuEntropyDispatch dispatch;
        int streams;
    };
    const LegacySpec legacy_specs[] = {
        {"Phase2B_OneStream_SyncPerBand", GpuEntropyDispatch::OneStream, 1},
        {"Phase2B_Batched_WaveSync", GpuEntropyDispatch::Batched, 4},
        {"Phase2B_MultiStream4_SyncPerBand", GpuEntropyDispatch::MultiStream, 4},
    };
    for (const auto& spec : legacy_specs) {
        std::vector<double> wall_samples;
        std::vector<GpuBandResult> res(hp_reqs.size());
        EntropyTiming wall{};
        // Warmup
        EncodeHighpassBatch(session, hp_reqs.data(), res.data(), static_cast<int>(hp_reqs.size()),
                            hp_tables.data(), spec.dispatch, spec.streams, &wall);
        bool match = true;
        for (size_t i = 0; i < hp_reqs.size(); ++i) {
            if (res[i].coeff_payload != ref_results[i].coeff_payload) match = false;
        }
        EntropyTiming sum_timing{};
        for (int it = 0; it < iterations; ++it) {
            const double t0 = NowMs();
            EncodeHighpassBatch(session, hp_reqs.data(), res.data(), static_cast<int>(hp_reqs.size()),
                                hp_tables.data(), spec.dispatch, spec.streams, &wall);
            wall_samples.push_back(NowMs() - t0);
            sum_timing.scan_ms += wall.scan_ms;
            sum_timing.block_scan_ms += wall.block_scan_ms;
            sum_timing.merge_carry_ms += wall.merge_carry_ms;
            sum_timing.token_ms += wall.token_ms;
            sum_timing.codebook_ms += wall.codebook_ms;
            sum_timing.prefix_ms += wall.prefix_ms;
            sum_timing.pack_ms += wall.pack_ms;
        }
        Phase2CConcurrencyPoint pt;
        pt.label = spec.label;
        pt.streams = spec.streams;
        pt.d2h_mode = CompressedD2HMode::BandStreamed;
        ComputeStats(wall_samples, &pt.mean_wall_ms, &pt.median_wall_ms, &pt.p95_wall_ms);
        pt.gpu_entropy_ms = pt.mean_wall_ms;
        pt.max_active_blocks_per_sm = GpuEntropyScanOccupancy();
        pt.substage_sum.scan_ms = sum_timing.scan_ms / iterations;
        pt.substage_sum.block_scan_ms = sum_timing.block_scan_ms / iterations;
        pt.substage_sum.merge_carry_ms = sum_timing.merge_carry_ms / iterations;
        pt.substage_sum.token_ms = sum_timing.token_ms / iterations;
        pt.substage_sum.codebook_ms = sum_timing.codebook_ms / iterations;
        pt.substage_sum.prefix_ms = sum_timing.prefix_ms / iterations;
        pt.substage_sum.pack_ms = sum_timing.pack_ms / iterations;
        pt.byte_match = match;
        pt.effective_gbps = pt.mean_wall_ms > 0.0
            ? (static_cast<double>(total_hp_coeff_bytes) / 1e9) / (pt.mean_wall_ms / 1e3)
            : 0.0;
        points.push_back(pt);
    }

    // 2. Phase 2C non-blocking multi-stream configurations (1, 2, 3, 4, 6, 8 streams) + BandStreamed.
    struct P2CSpec {
        const char* label;
        int streams;
        CompressedD2HMode mode;
    };
    const P2CSpec p2c_specs[] = {
        {"Phase2C_Consolidated_1s", 1, CompressedD2HMode::FrameConsolidated},
        {"Phase2C_Consolidated_2s", 2, CompressedD2HMode::FrameConsolidated},
        {"Phase2C_Consolidated_3s", 3, CompressedD2HMode::FrameConsolidated},
        {"Phase2C_Consolidated_4s", 4, CompressedD2HMode::FrameConsolidated},
        {"Phase2C_Consolidated_6s", 6, CompressedD2HMode::FrameConsolidated},
        {"Phase2C_Consolidated_8s", 8, CompressedD2HMode::FrameConsolidated},
        {"Phase2C_BandStreamed_1s", 1, CompressedD2HMode::BandStreamed},
        {"Phase2C_BandStreamed_4s", 4, CompressedD2HMode::BandStreamed},
    };

    double baseline_1s_ms = 0.0;
    for (const auto& spec : p2c_specs) {
        GpuEntropyFrameConfig cfg;
        cfg.entropy_streams = spec.streams;
        cfg.d2h_mode = spec.mode;
        cfg.record_substage_events = true;
        cfg.lpt_schedule = true;

        // Warmup + sub-stage event profiling pass
        QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), cfg);
        while (!IsGpuEntropyFrameComplete(e_slot)) {
            if (spec.mode == CompressedD2HMode::BandStreamed) PollNextStreamedBandReady(e_slot);
        }
        GpuFrameEntropyStats profile_stats{};
        CollectGpuFrameEntropyStats(e_slot, &profile_stats);

        bool match = true;
        for (size_t i = 0; i < hp_reqs.size(); ++i) {
            GpuBandCompressedView view{};
            GetGpuBandCompressedView(e_slot, static_cast<int>(i), &view);
            if (static_cast<size_t>(view.byte_count) != ref_results[i].coeff_payload.size() ||
                std::memcmp(view.bytes, ref_results[i].coeff_payload.data(), static_cast<size_t>(view.byte_count)) != 0) {
                match = false;
            }
        }

        // Timed iterations without per-band 216-event overhead
        cfg.record_substage_events = false;
        std::vector<double> wall_samples;
        double sum_ent_ms = 0.0, sum_cons_ms = 0.0, sum_d2h_ms = 0.0;
        for (int it = 0; it < iterations; ++it) {
            const double t0 = NowMs();
            QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), cfg);
            while (!IsGpuEntropyFrameComplete(e_slot)) {
                if (spec.mode == CompressedD2HMode::BandStreamed) PollNextStreamedBandReady(e_slot);
            }
            wall_samples.push_back(NowMs() - t0);
            GpuFrameEntropyStats st{};
            CollectGpuFrameEntropyStats(e_slot, &st);
            sum_ent_ms += st.gpu_entropy_wall_ms;
            sum_cons_ms += st.gpu_consolidate_ms;
            sum_d2h_ms += st.compressed_d2h_ms;
        }

        Phase2CConcurrencyPoint pt;
        pt.label = spec.label;
        pt.streams = spec.streams;
        pt.d2h_mode = spec.mode;
        ComputeStats(wall_samples, &pt.mean_wall_ms, &pt.median_wall_ms, &pt.p95_wall_ms);
        pt.gpu_entropy_ms = sum_ent_ms / iterations;
        pt.gpu_consolidate_ms = sum_cons_ms / iterations;
        pt.compressed_d2h_ms = sum_d2h_ms / iterations;
        pt.max_active_blocks_per_sm = profile_stats.max_active_blocks_per_sm;
        pt.substage_sum = profile_stats.substage_sum;
        pt.byte_match = match;
        pt.effective_gbps = pt.gpu_entropy_ms > 0.0
            ? (static_cast<double>(total_hp_coeff_bytes + profile_stats.compressed_d2h_bytes) / 1e9) / (pt.gpu_entropy_ms / 1e3)
            : 0.0;
        if (spec.streams == 1 && spec.mode == CompressedD2HMode::FrameConsolidated) {
            baseline_1s_ms = pt.mean_wall_ms;
        }
        points.push_back(pt);
    }

    if (baseline_1s_ms > 0.0) {
        for (auto& pt : points) {
            pt.speedup_vs_1stream = baseline_1s_ms / pt.mean_wall_ms;
        }
    }

    DestroyGpuEntropyFrameSlot(e_slot);
    DestroyGpuEntropySession(session);
    HyperCine_GpuSlotDestroy(w_slot);
    return points;
}

namespace {

enum class PipeSlotStage {
    Idle = 0,
    WaveletQueued = 1,
    EntropyAndD2HQueued = 2,
    AssemblingOnWorker = 3
};

struct PipeSlot {
    int slot_id = 0;
    HyperCineGpuSlot* w_slot = nullptr;
    GpuEntropyFrameSlot* e_slot = nullptr;
    std::atomic<int> stage{static_cast<int>(PipeSlotStage::Idle)};
    int frame_index = -1;
    double submit_ms = 0.0;
    double complete_ms = 0.0;
    double cpu_lowpass_ms = 0.0;
    double cpu_header_wrap_ms = 0.0;
    double cpu_sample_concat_ms = 0.0;
    int streamed_wrapped_count = 0;
    std::vector<EncodedPiece> pieces;
    std::vector<uint8_t> bitstream;
};

}  // namespace

Phase2CResult RunPhase2C(const Phase2CConfig& cfg) {
    Phase2CResult res;
    res.config = cfg;
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.frames <= 0) {
        res.error = "invalid Phase2CConfig dimensions or frame count";
        return res;
    }

    const int width = cfg.width;
    const int height = cfg.height;
    const int pitch = width * 4;
    const int num_slots = std::clamp(cfg.frame_slots, 1, 8);
    const int num_streams = std::clamp(cfg.entropy_streams, 1, 8);

    const std::vector<uint8_t> canonical_frame = MakeFrame(width, height, CanonicalPattern::Canonical);
    EncoderContext ctx;
    if (!ctx.Init(width, height, canonical_frame, cfg.deterministic_test_metadata)) {
        res.error = "failed to initialize upstream encoder";
        return res;
    }

    const QuantTables quant = ReadQuant(ctx.encoder, ctx.transforms, ctx.channel_count);
    const std::vector<BandRef> refs = EnumerateBands(reinterpret_cast<transform**>(ctx.transforms), ctx.channel_count);
    if (!quant.ok || refs.size() != 30) {
        res.error = "expected 30 canonical subbands";
        return res;
    }

    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));
    std::vector<int> hp_to_piece;
    std::vector<EntropyTables> hp_tables;
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        const BandRef& ref = refs[static_cast<size_t>(i)];
        piece_of[ref.channel][ref.wavelet_index][ref.band] = i;
        if (!ref.lowpass) {
            EntropyTables tbl;
            if (!LoadEntropyTables(reinterpret_cast<encoder*>(ctx.encoder), ref.subband, &tbl)) {
                res.error = "failed to load highpass entropy codebook";
                return res;
            }
            hp_to_piece.push_back(i);
            hp_tables.push_back(std::move(tbl));
        }
    }

    // Optional reference CPU bitstream for verification.
    std::vector<uint8_t> ref_cpu_bitstream;
    if (cfg.verify_bitstream) {
        const size_t scratch_bytes = std::max<size_t>(64u << 20, static_cast<size_t>(width) * static_cast<size_t>(height) * 8u);
        std::vector<uint8_t> scratch(scratch_bytes, 0);
        RefreshUpstreamPyramid(reinterpret_cast<encoder*>(ctx.encoder),
                               reinterpret_cast<transform**>(ctx.transforms),
                               ctx.channel_count, canonical_frame.data(), pitch,
                               scratch.data(), scratch.size());
        EntropyOutput cpu_out = EncodeEntropy(reinterpret_cast<encoder*>(ctx.encoder),
                                              reinterpret_cast<transform**>(ctx.transforms),
                                              ctx.channel_count, 1, EntropySchedule::Serial, true);
        if (!cpu_out.success || !cpu_out.byte_match) {
            res.error = "reference CPU bitstream generation failed";
            return res;
        }
        ref_cpu_bitstream = std::move(cpu_out.bitstream);
    }

    const size_t vram_before = HyperCine_QueryUsedVramBytes();
    GpuEntropySession* session = CreateGpuEntropySession();
    if (!session) {
        res.error = "failed to create GpuEntropySession";
        return res;
    }

    std::vector<PipeSlot> slots(static_cast<size_t>(num_slots));
    uint8_t* pinned_host_bgra = nullptr;
    if (cfg.input_mode == Phase2CInputMode::HostInput) {
        if (cudaHostAlloc(&pinned_host_bgra, canonical_frame.size(), cudaHostAllocDefault) != cudaSuccess) {
            DestroyGpuEntropySession(session);
            res.error = "failed to allocate pinned host BGRA input";
            return res;
        }
        std::memcpy(pinned_host_bgra, canonical_frame.data(), canonical_frame.size());
    }

    auto cleanup_all = [&]() {
        for (PipeSlot& s : slots) {
            DestroyGpuEntropyFrameSlot(s.e_slot);
            HyperCine_GpuSlotDestroy(s.w_slot);
        }
        if (pinned_host_bgra) cudaFreeHost(pinned_host_bgra);
        DestroyGpuEntropySession(session);
    };

    const void* shared_device_bgra = nullptr;
    for (int i = 0; i < num_slots; ++i) {
        slots[static_cast<size_t>(i)].slot_id = i;
        slots[static_cast<size_t>(i)].w_slot = HyperCine_GpuSlotCreateForPhase2C(width, height, 1);
        slots[static_cast<size_t>(i)].e_slot = CreateGpuEntropyFrameSlot(width, height);
        if (!slots[static_cast<size_t>(i)].w_slot || !slots[static_cast<size_t>(i)].e_slot) {
            cleanup_all();
            const char* alloc_error = GpuEntropySlotCreationError();
            res.error = "failed to allocate multi-frame GPU slot";
            if (alloc_error && alloc_error[0]) res.error += std::string(": ") + alloc_error;
            return res;
        }
        if (i == 0 && cfg.input_mode == Phase2CInputMode::DeviceInput) {
            if (HyperCine_GpuSlotUploadBgra(slots[0].w_slot, canonical_frame.data(), pitch, width, height, &shared_device_bgra) != 0) {
                cleanup_all();
                res.error = "failed to upload device BGRA input";
                return res;
            }
        }
        slots[static_cast<size_t>(i)].pieces.resize(refs.size());
        for (size_t p = 0; p < refs.size(); ++p) {
            EncodedPiece& ep = slots[static_cast<size_t>(i)].pieces[p];
            ep.channel = refs[p].channel;
            ep.level = refs[p].level;
            ep.band = refs[p].band;
            ep.subband = refs[p].subband;
            ep.lowpass = refs[p].lowpass;
            ep.payload.reserve(static_cast<size_t>(refs[p].width) * static_cast<size_t>(refs[p].height) * 2u + 256u);
        }
        slots[static_cast<size_t>(i)].bitstream.reserve(static_cast<size_t>(width) * static_cast<size_t>(height) + 65536u);
    }

    const int input_kind = (cfg.input_mode == Phase2CInputMode::DeviceInput)
        ? HYPERCINE_INPUT_DEVICE_BGRA
        : HYPERCINE_INPUT_HOST_BGRA;
    const uint8_t* input_source = (cfg.input_mode == Phase2CInputMode::DeviceInput)
        ? static_cast<const uint8_t*>(shared_device_bgra)
        : pinned_host_bgra;

    // Pre-build per-slot GpuBandRequest arrays (device pointers are fixed per w_slot).
    std::vector<std::vector<GpuBandRequest>> slot_hp_reqs(static_cast<size_t>(num_slots));
    std::vector<std::vector<void*>> slot_band_wait_evs(static_cast<size_t>(num_slots));
    for (int s = 0; s < num_slots; ++s) {
        for (int p_idx : hp_to_piece) {
            const BandRef& ref = refs[static_cast<size_t>(p_idx)];
            const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(
                slots[static_cast<size_t>(s)].w_slot, ref.channel, ref.wavelet_index, ref.band);
            slot_hp_reqs[static_cast<size_t>(s)].push_back(
                GpuBandRequest{d_ptr, ref.width, ref.height, ref.width, ref.channel, ref.wavelet_index, ref.band, ref.subband});
            slot_band_wait_evs[static_cast<size_t>(s)].push_back(
                HyperCine_GpuSlotLevelEvent(slots[static_cast<size_t>(s)].w_slot, ref.channel, ref.wavelet_index));
        }
    }

    auto wrap_one_highpass = [&](PipeSlot& slot, int hp_idx) -> bool {
        const int p_idx = hp_to_piece[static_cast<size_t>(hp_idx)];
        const BandRef& ref = refs[static_cast<size_t>(p_idx)];
        GpuBandCompressedView view{};
        if (!GetGpuBandCompressedView(slot.e_slot, hp_idx, &view)) return false;
        IMAGE* wavelet = ctx.transforms[ref.channel]->wavelet[ref.wavelet_index];
        int qv = wavelet->quantization[ref.band];
        if (qv <= 0) qv = wavelet->quant[ref.band];
        if (qv <= 0) qv = 1;
        EncodedPiece& out = slot.pieces[static_cast<size_t>(p_idx)];
        if (!BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx.encoder), ref.band, ref.width, ref.height,
                                     ref.subband, qv, wavelet->scale[ref.band], hp_tables[static_cast<size_t>(hp_idx)],
                                     view.bytes, static_cast<size_t>(view.byte_count), &out.payload, &out.bits_free)) {
            return false;
        }
        out.ok = true;
        return true;
    };

    auto assemble_slot_cpu = [&](PipeSlot& slot) -> bool {
        const double t_lp0 = NowMs();
        for (int c = 0; c < 3; ++c) {
            const int p_idx = piece_of[c][2][LL_BAND];
            const BandRef& ref = refs[static_cast<size_t>(p_idx)];
            const int16_t* h_ptr = HyperCine_GpuSlotBandHostPtr(slot.w_slot, c, 2, LL_BAND);
            EncodedPiece& out = slot.pieces[static_cast<size_t>(p_idx)];
            if (!BuildLowpassPayloadPtr(ref.subband, ref.level, ref.width, ref.height,
                                        h_ptr, cfg.gpu_pack_lowpass,
                                        &out.payload, &out.bits_free)) {
                return false;
            }
            out.ok = true;
        }
        slot.cpu_lowpass_ms = NowMs() - t_lp0;

        if (cfg.d2h_mode == CompressedD2HMode::FrameConsolidated) {
            const double t_hdr0 = NowMs();
            for (int hp_idx = 0; hp_idx < static_cast<int>(hp_to_piece.size()); ++hp_idx) {
                if (!wrap_one_highpass(slot, hp_idx)) return false;
            }
            slot.cpu_header_wrap_ms = NowMs() - t_hdr0;
        }

        double concat_ms = 0.0;
        if (!AssembleSampleFast(ctx.encoder, ctx.transforms, ctx.channel_count,
                                slot.pieces, piece_of, &slot.bitstream, &concat_ms)) {
            return false;
        }
        slot.cpu_sample_concat_ms = concat_ms;
        slot.complete_ms = NowMs();
        return true;
    };

    const int total_frames = cfg.warmup_frames + cfg.frames;
    std::vector<double> frame_submit_ms(static_cast<size_t>(total_frames), 0.0);
    std::vector<double> frame_complete_ms(static_cast<size_t>(total_frames), 0.0);

    // Stage 4 background CPU assembly worker for multi-frame pipelining.
    std::mutex asm_mu;
    std::condition_variable asm_cv;
    std::deque<int> asm_queue;
    bool asm_stop = false;
    std::atomic<bool> asm_failed{false};

    double sum_h2d_ms = 0.0, sum_color_ms = 0.0, sum_wavelet_ms = 0.0, sum_lp_d2h_ms = 0.0;
    double sum_ent_ms = 0.0, sum_cons_ms = 0.0, sum_comp_d2h_ms = 0.0;
    double sum_cpu_lp_ms = 0.0, sum_cpu_hdr_ms = 0.0, sum_cpu_concat_ms = 0.0;
    EntropyTiming sum_substage{};
    int substage_counted = 0;

    auto record_completed_slot = [&](PipeSlot& slot) {
        const int f = slot.frame_index;
        frame_complete_ms[static_cast<size_t>(f)] = slot.complete_ms;
        if (f >= cfg.warmup_frames) {
            HyperCineStreamTiming wt{};
            HyperCine_GpuSlotReadTiming(slot.w_slot, &wt);
            GpuFrameEntropyStats est{};
            CollectGpuFrameEntropyStats(slot.e_slot, &est);

            sum_h2d_ms += wt.h2d_ms;
            sum_color_ms += wt.color_ms;
            sum_wavelet_ms += wt.compute_ms;
            sum_lp_d2h_ms += wt.d2h_ms;
            sum_ent_ms += est.gpu_entropy_wall_ms;
            sum_cons_ms += est.gpu_consolidate_ms;
            sum_comp_d2h_ms += est.compressed_d2h_ms;

            sum_cpu_lp_ms += slot.cpu_lowpass_ms;
            sum_cpu_hdr_ms += slot.cpu_header_wrap_ms;
            sum_cpu_concat_ms += slot.cpu_sample_concat_ms;

            if (slot.e_slot && cfg.record_substage_events) {
                sum_substage.block_scan_ms += est.substage_sum.block_scan_ms;
                sum_substage.merge_carry_ms += est.substage_sum.merge_carry_ms;
                sum_substage.gather_ms += est.substage_sum.gather_ms;
                sum_substage.scan_ms += est.substage_sum.scan_ms;
                sum_substage.token_ms += est.substage_sum.token_ms;
                sum_substage.codebook_ms += est.substage_sum.codebook_ms;
                sum_substage.prefix_ms += est.substage_sum.prefix_ms;
                sum_substage.pack_ms += est.substage_sum.pack_ms;
                sum_substage.total_ms += est.substage_sum.total_ms;
                ++substage_counted;
            }

            res.h2d_bytes_per_frame = wt.h2d_bytes;
            res.lowpass_d2h_bytes_per_frame = wt.d2h_bytes;
            res.compressed_highpass_d2h_bytes_per_frame = est.compressed_d2h_bytes;
            res.total_d2h_bytes_per_frame = wt.d2h_bytes + est.compressed_d2h_bytes;
            res.bitstream_bytes = slot.bitstream.size();
            if (f == total_frames - 1) {
                res.last_bitstream = slot.bitstream;
            }
        }
    };

    const bool use_worker = cfg.async_cpu_assembly && num_slots >= 2;
    std::thread asm_thread;
    if (use_worker) {
        asm_thread = std::thread([&]() {
            for (;;) {
                int s_idx = -1;
                {
                    std::unique_lock<std::mutex> lk(asm_mu);
                    asm_cv.wait(lk, [&] { return asm_stop || !asm_queue.empty(); });
                    if (asm_stop && asm_queue.empty()) return;
                    s_idx = asm_queue.front();
                    asm_queue.pop_front();
                }
                PipeSlot& slot = slots[static_cast<size_t>(s_idx)];
                if (!assemble_slot_cpu(slot)) {
                    asm_failed.store(true);
                } else {
                    record_completed_slot(slot);
                }
                slot.stage.store(static_cast<int>(PipeSlotStage::Idle), std::memory_order_release);
            }
        });
    }

    int next_submit_frame = 0;
    int next_gpu_done_frame = 0;
    int last_submitted_slot = -1;
    double timed_start_ms = 0.0;
    double cpu_s0 = 0.0;
    double main_thread_busy_ms = 0.0;
    int64_t inflight_accum = 0;
    int64_t poll_iters = 0;

    // Warmup + timed multi-frame execution loop.
    // Submitting at most one new frame per poll iteration prevents CPU launch bursts from
    // delaying D2H polling while keeping the WDDM hardware queue continuously fed.
    while (next_gpu_done_frame < total_frames && !asm_failed.load(std::memory_order_relaxed)) {
        bool did_work = false;

        // 1. Submit at most one new frame to the next Idle slot in FIFO order.
        if (next_submit_frame < total_frames) {
            const int target_slot = next_submit_frame % num_slots;
            PipeSlot& slot = slots[static_cast<size_t>(target_slot)];
            if (slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage::Idle)) {
                const double t_work0 = NowMs();
                if (next_submit_frame == cfg.warmup_frames) {
                    timed_start_ms = t_work0;
                    cpu_s0 = ProcessCpuSeconds();
                }

                void* wait_fe = (last_submitted_slot >= 0)
                    ? HyperCine_GpuSlotFrontendEvent(slots[static_cast<size_t>(last_submitted_slot)].w_slot)
                    : nullptr;
                void* wait_wv = (last_submitted_slot >= 0)
                    ? HyperCine_GpuSlotWaveletEvent(slots[static_cast<size_t>(last_submitted_slot)].w_slot)
                    : nullptr;
                // When >= 3 slots are active, also pace Frame N's wavelet to start after Frame N-2's
                // GPU entropy finishes so Frame N's wavelet overlaps with Frame N-1's entropy rather
                // than piling up multiple wavelets during Frame N-2's entropy.
                if (num_slots >= 3 && next_submit_frame >= 2) {
                    const int prev2_slot = (next_submit_frame - 2) % num_slots;
                    wait_wv = GetGpuEntropyComputeDoneEvent(slots[static_cast<size_t>(prev2_slot)].e_slot);
                }

                slot.frame_index = next_submit_frame;
                slot.submit_ms = NowMs();
                frame_submit_ms[static_cast<size_t>(next_submit_frame)] = slot.submit_ms;
                slot.cpu_lowpass_ms = 0.0;
                slot.cpu_header_wrap_ms = 0.0;
                slot.cpu_sample_concat_ms = 0.0;
                slot.streamed_wrapped_count = 0;

                if (HyperCine_GpuSlotBegin(slot.w_slot, input_kind, HYPERCINE_XPORT_DEVICE_LOWPASS_D2H,
                                           input_source, pitch, width, height, quant.color_space,
                                           quant.widths, quant.heights, quant.prescale, quant.quant) != 0 ||
                    HyperCine_GpuSlotKickStaged(slot.w_slot, wait_fe, wait_wv, cfg.gpu_pack_lowpass ? 1 : 0) != 0) {
                    res.error = "HyperCine_GpuSlotKickStaged failed";
                    asm_failed.store(true);
                    break;
                }

                GpuEntropyFrameConfig e_cfg;
                e_cfg.entropy_streams = num_streams;
                e_cfg.d2h_mode = cfg.d2h_mode;
                e_cfg.record_substage_events = cfg.record_substage_events && (next_submit_frame >= cfg.warmup_frames);
                e_cfg.lpt_schedule = true;

                void* wv_done_ev = HyperCine_GpuSlotWaveletEvent(slot.w_slot);
                void* const* band_evs = (cfg.d2h_mode == CompressedD2HMode::BandStreamed)
                    ? slot_band_wait_evs[static_cast<size_t>(target_slot)].data()
                    : nullptr;

                if (!QueueGpuEntropyFrame(session, slot.e_slot,
                                          slot_hp_reqs[static_cast<size_t>(target_slot)].data(),
                                          static_cast<int>(slot_hp_reqs[static_cast<size_t>(target_slot)].size()),
                                          hp_tables.data(), e_cfg,
                                          (cfg.d2h_mode == CompressedD2HMode::FrameConsolidated) ? wv_done_ev : nullptr,
                                          band_evs)) {
                    const char* slot_error = GpuEntropyFrameError(slot.e_slot);
                    res.error = (slot_error && slot_error[0]) ? slot_error : "QueueGpuEntropyFrame failed";
                    asm_failed.store(true);
                    break;
                }

                slot.stage.store(static_cast<int>(PipeSlotStage::EntropyAndD2HQueued), std::memory_order_release);
                last_submitted_slot = target_slot;
                ++next_submit_frame;
                if (next_submit_frame > cfg.warmup_frames) main_thread_busy_ms += NowMs() - t_work0;
                did_work = true;
            }
        }

        // 2. Progress D2H transfers across all active slots in flight.
        int active_in_flight = 0;
        for (int s = 0; s < num_slots; ++s) {
            PipeSlot& slot = slots[static_cast<size_t>(s)];
            const int st = slot.stage.load(std::memory_order_acquire);
            if (st != static_cast<int>(PipeSlotStage::Idle)) ++active_in_flight;
            if (st != static_cast<int>(PipeSlotStage::EntropyAndD2HQueued)) continue;

            if (cfg.d2h_mode == CompressedD2HMode::FrameConsolidated) {
                if (PollGpuEntropyFrameD2H(slot.e_slot)) did_work = true;
            } else {
                for (;;) {
                    const int ready_hp = PollNextStreamedBandReady(slot.e_slot);
                    if (ready_hp < 0) break;
                    const double t_hdr0 = NowMs();
                    if (!wrap_one_highpass(slot, ready_hp)) {
                        res.error = "streamed highpass header wrap failed";
                        asm_failed.store(true);
                        break;
                    }
                    const double dt = NowMs() - t_hdr0;
                    slot.cpu_header_wrap_ms += dt;
                    if (slot.frame_index >= cfg.warmup_frames) main_thread_busy_ms += dt;
                    ++slot.streamed_wrapped_count;
                    did_work = true;
                }
            }
        }
        if (next_submit_frame > cfg.warmup_frames) {
            inflight_accum += active_in_flight;
            ++poll_iters;
        }

        // 3. Check if the oldest GPU-active frame (`next_gpu_done_frame`) has finished D2H.
        const int done_slot_idx = next_gpu_done_frame % num_slots;
        PipeSlot& done_slot = slots[static_cast<size_t>(done_slot_idx)];
        if (done_slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage::EntropyAndD2HQueued)) {
            const int lp_idle = HyperCine_GpuSlotGpuIdle(done_slot.w_slot);
            if (lp_idle < 0) {
                res.error = "lowpass D2H failed";
                asm_failed.store(true);
                break;
            }
            if (lp_idle == 1 && IsGpuEntropyFrameComplete(done_slot.e_slot)) {
                if (use_worker) {
                    done_slot.stage.store(static_cast<int>(PipeSlotStage::AssemblingOnWorker), std::memory_order_release);
                    {
                        std::lock_guard<std::mutex> lk(asm_mu);
                        asm_queue.push_back(done_slot_idx);
                    }
                    asm_cv.notify_one();
                } else {
                    const double t_asm0 = NowMs();
                    if (!assemble_slot_cpu(done_slot)) {
                        res.error = "CPU sample assembly failed";
                        asm_failed.store(true);
                        break;
                    }
                    record_completed_slot(done_slot);
                    if (done_slot.frame_index >= cfg.warmup_frames) main_thread_busy_ms += NowMs() - t_asm0;
                    done_slot.stage.store(static_cast<int>(PipeSlotStage::Idle), std::memory_order_release);
                }
                ++next_gpu_done_frame;
                did_work = true;
            }
        }
        (void)did_work;
    }

    if (use_worker) {
        {
            std::lock_guard<std::mutex> lk(asm_mu);
            asm_stop = true;
        }
        asm_cv.notify_all();
        if (asm_thread.joinable()) asm_thread.join();
    }

    const double timed_end_ms = NowMs();
    const double cpu_elapsed_sec = ProcessCpuSeconds() - cpu_s0;
    const size_t vram_after = HyperCine_QueryUsedVramBytes();
    res.vram_used_bytes = vram_after > vram_before ? (vram_after - vram_before) : vram_after;

    // Analytical per-slot VRAM and pinned host footprint accounting.
    const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t w_slot_vram = px * 4u + px * 10u; // device BGRA + pyramid device bands
    const size_t e_slot_vram = (px + 32u * 32768u) * 4u + (px / 2u + 131072u) * 4u;
    res.slot_vram_bytes = static_cast<size_t>(num_slots) * (w_slot_vram + e_slot_vram);
    res.slot_pinned_host_bytes = static_cast<size_t>(num_slots) * ((px / 16u) * 2u + (px + 32u * 32768u) * 4u + (px / 2u + 131072u) * 4u);

    if (asm_failed.load()) {
        if (res.error.empty()) res.error = "pipeline execution failed";
        cleanup_all();
        return res;
    }

    res.encoded_frames = cfg.frames;
    res.wall_ms = std::max(0.001, timed_end_ms - timed_start_ms);
    res.sustained_fps = 1000.0 * static_cast<double>(cfg.frames) / res.wall_ms;
    res.frame_period_ms = res.wall_ms / static_cast<double>(cfg.frames);

    std::vector<double> latencies;
    std::vector<double> periods;
    for (int f = cfg.warmup_frames; f < total_frames; ++f) {
        latencies.push_back(frame_complete_ms[static_cast<size_t>(f)] - frame_submit_ms[static_cast<size_t>(f)]);
        if (f > cfg.warmup_frames) {
            periods.push_back(frame_complete_ms[static_cast<size_t>(f)] - frame_complete_ms[static_cast<size_t>(f - 1)]);
        }
    }
    ComputeStats(latencies, &res.mean_latency_ms, &res.median_latency_ms, &res.p95_latency_ms,
                 &res.min_latency_ms, &res.max_latency_ms);
    if (!periods.empty()) {
        ComputeStats(periods, &res.mean_period_ms, &res.median_period_ms, &res.p95_period_ms);
    } else {
        res.mean_period_ms = res.median_period_ms = res.p95_period_ms = res.frame_period_ms;
    }

    const double inv_f = 1.0 / static_cast<double>(cfg.frames);
    res.h2d_ms = sum_h2d_ms * inv_f;
    res.color_ms = sum_color_ms * inv_f;
    res.wavelet_ms = sum_wavelet_ms * inv_f;
    res.gpu_frontend_wavelet_ms = (sum_h2d_ms + sum_color_ms + sum_wavelet_ms) * inv_f;
    res.gpu_entropy_ms = sum_ent_ms * inv_f;
    res.gpu_consolidate_ms = sum_cons_ms * inv_f;
    res.compressed_d2h_ms = sum_comp_d2h_ms * inv_f;
    res.lowpass_d2h_ms = sum_lp_d2h_ms * inv_f;
    res.total_d2h_ms = res.compressed_d2h_ms + res.lowpass_d2h_ms;
    res.total_gpu_ms = res.gpu_frontend_wavelet_ms + res.gpu_entropy_ms + res.compressed_d2h_ms;

    res.cpu_lowpass_ms = sum_cpu_lp_ms * inv_f;
    res.cpu_header_wrap_ms = sum_cpu_hdr_ms * inv_f;
    res.cpu_sample_concat_ms = sum_cpu_concat_ms * inv_f;
    res.cpu_assembly_total_ms = res.cpu_lowpass_ms + res.cpu_header_wrap_ms + res.cpu_sample_concat_ms;
    res.cpu_dispatch_poll_ms = main_thread_busy_ms * inv_f;
    res.total_cpu_ms = res.cpu_assembly_total_ms + res.cpu_dispatch_poll_ms;

    if (substage_counted > 0) {
        const double inv_s = 1.0 / static_cast<double>(substage_counted);
        res.entropy_substage_sum.block_scan_ms = sum_substage.block_scan_ms * inv_s;
        res.entropy_substage_sum.merge_carry_ms = sum_substage.merge_carry_ms * inv_s;
        res.entropy_substage_sum.gather_ms = sum_substage.gather_ms * inv_s;
        res.entropy_substage_sum.scan_ms = sum_substage.scan_ms * inv_s;
        res.entropy_substage_sum.token_ms = sum_substage.token_ms * inv_s;
        res.entropy_substage_sum.codebook_ms = sum_substage.codebook_ms * inv_s;
        res.entropy_substage_sum.prefix_ms = sum_substage.prefix_ms * inv_s;
        res.entropy_substage_sum.pack_ms = sum_substage.pack_ms * inv_s;
        res.entropy_substage_sum.total_ms = sum_substage.total_ms * inv_s;
    }

    const double gpu_compute_busy_ms = sum_h2d_ms + sum_color_ms + sum_wavelet_ms + sum_ent_ms;
    const double logical_cpus = static_cast<double>(std::max(1u, std::thread::hardware_concurrency()));
    res.gpu_utilization_pct = std::min(100.0, 100.0 * gpu_compute_busy_ms / res.wall_ms);
    res.cpu_utilization_pct = std::min(100.0, 100.0 * (cpu_elapsed_sec * 1000.0) / (res.wall_ms * logical_cpus));
    res.avg_frames_in_flight = poll_iters > 0
        ? static_cast<double>(inflight_accum) / static_cast<double>(poll_iters)
        : 1.0;

    if (!res.last_bitstream.empty()) {
        res.sample_sha256 = Sha256Hex(res.last_bitstream.data(), res.last_bitstream.size());
        if (cfg.verify_bitstream) {
            res.bitstream_byte_match = (res.last_bitstream.size() == ref_cpu_bitstream.size());
            int diffs = 0;
            if (res.bitstream_byte_match) {
                for (size_t i = 0; i < res.last_bitstream.size(); ++i) {
                    if (res.last_bitstream[i] != ref_cpu_bitstream[i]) ++diffs;
                }
                // Only the 16-bit/32-bit CODEC_TAG_FRAME_NUMBER value differs across frame numbers.
                res.bitstream_byte_match = (diffs <= 2);
            }
            std::vector<uint8_t> dec;
            int dec_pitch = 0;
            res.decode_ok = DecodeCfhdSample(res.last_bitstream, width, height, &dec, &dec_pitch);
            if (res.decode_ok) {
                res.psnr_db = ComputeBestPsnr(canonical_frame, dec, width, height, dec_pitch);
            }
            if (!res.bitstream_byte_match) {
                res.error = "bitstream mismatch (got_size=" + std::to_string(res.last_bitstream.size()) +
                            ", ref_size=" + std::to_string(ref_cpu_bitstream.size()) +
                            ", diffs=" + std::to_string(diffs) + ")";
            } else if (!res.decode_ok) {
                res.error = "DecodeCfhdSample failed";
            } else if (res.psnr_db <= 35.0) {
                res.error = "PSNR too low: " + std::to_string(res.psnr_db);
            }
        } else {
            res.bitstream_byte_match = true;
            res.decode_ok = true;
        }
    } else if (cfg.verify_bitstream) {
        res.error = "last_bitstream is empty";
    }

    res.ok = !cfg.verify_bitstream || (res.bitstream_byte_match && res.decode_ok && res.psnr_db > 35.0);
    cleanup_all();
    return res;
}

}  // namespace hypercine
