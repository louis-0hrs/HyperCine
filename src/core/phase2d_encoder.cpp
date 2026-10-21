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

#include "hypercine/phase2d.h"

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
#include <cstdio>
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
    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel, &user)) return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 1e7;
#else
    return 0.0;
#endif
}

double CurrentThreadCpuSeconds() {
#ifdef _WIN32
    FILETIME create_time, exit_time, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &create_time, &exit_time, &kernel, &user)) return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 1e7;
#else
    return 0.0;
#endif
}

std::string GuidToHex(const uint32_t words[4]) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%08x-%08x-%08x-%08x",
                  words[0], words[1], words[2], words[3]);
    return std::string(buf);
}

struct Phase2DEncoderContext {
    CFHD_EncoderRef ref = nullptr;
    CSampleEncoder* sample = nullptr;
    ENCODER* encoder = nullptr;
    TRANSFORM** transforms = nullptr;
    int channel_count = 3;

    ~Phase2DEncoderContext() {
        if (ref) CFHD_CloseEncoder(ref);
    }

    bool Init(int width, int height, const std::vector<uint8_t>& frame,
              Phase2DQuantStateMode state_mode,
              bool deterministic_test_metadata,
              EncodingQuality quality = EncodingQuality::FilmScan3) {
        if (!IsValidEncodingQuality(quality)) return false;
        if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) return false;
        if (CFHD_PrepareToEncode(ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                                 CFHD_ENCODING_FLAGS_NONE, static_cast<CFHD_EncodingQuality>(static_cast<int>(quality))) != CFHD_ERROR_OKAY) {
            return false;
        }
        sample = reinterpret_cast<CSampleEncoder*>(ref);
        if (!sample) return false;

        // Frame 1: q->FSratelimiter == 4 (lastgopbitcount == 0).
        // SteadyState (Frame 3): q->FSratelimiter adapts 4 -> 2 -> 1 and stabilizes.
        const int encode_passes = (state_mode == Phase2DQuantStateMode::SteadyState) ? 3 : 1;
        for (int pass = 0; pass < encode_passes; ++pass) {
            if (CFHD_EncodeSample(ref, const_cast<uint8_t*>(frame.data()), width * 4) != CFHD_ERROR_OKAY) {
                return false;
            }
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

struct QuantTables2D {
    int prescale[3] = {};
    int quant[36] = {};
    int scale[36] = {};
    int widths[3] = {};
    int heights[3] = {};
    int color_space = 2;
    int fs_rate_limiter = 0;
    int last_gop_bytes = 0;
    bool ok = false;
};

QuantTables2D ReadQuant2D(ENCODER* encoder, TRANSFORM* transforms[], int channel_count) {
    QuantTables2D q;
    if (!encoder || !encoder->frame || channel_count < 3 || !transforms) return q;
    q.color_space = encoder->input.color_space;
    q.fs_rate_limiter = encoder->q.FSratelimiter;
    q.last_gop_bytes = encoder->lastgopbitcount >> 3;
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
                q.quant[(c * 3 + level) * 4 + band] = wavelet->quant[band];
                q.scale[(c * 3 + level) * 4 + band] = wavelet->scale[band];
            }
        }
    }
    q.ok = true;
    return q;
}

const char* BandTag2D(int band) {
    switch (band) {
    case LL_BAND: return "LL";
    case LH_BAND: return "LH";
    case HL_BAND: return "HL";
    case HH_BAND: return "HH";
    default: return "?";
    }
}

const char* ChannelTag2D(int channel) {
    switch (channel) {
    case 0: return "Y";
    case 1: return "C1";
    case 2: return "C2";
    default: return "C";
    }
}

std::string BandName2D(int channel, int level, int band) {
    return std::string(ChannelTag2D(channel)) + std::to_string(level) + BandTag2D(band);
}

bool DecodeCfhdSample2D(const std::vector<uint8_t>& sample, int width, int height,
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

double ComputeBestPsnr2D(const std::vector<uint8_t>& src, const std::vector<uint8_t>& dec,
                         int width, int height, int pitch) {
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

bool AssembleSampleFast2D(ENCODER* encoder, TRANSFORM* transforms[], int channel_count,
                          const std::vector<EncodedPiece>& pieces, const int piece_of[3][3][4],
                          std::vector<uint8_t>* out_bytes, double* concat_ms,
                          std::vector<size_t>* out_piece_offsets = nullptr) {
    if (!encoder || !transforms || !out_bytes || pieces.size() < 30) return false;
    size_t need = 16384u;
    for (const EncodedPiece& p : pieces) {
        if (!p.ok || p.bits_free != BITSTREAM_BUFFER_SIZE) return false;
        need += p.payload.size() + 64u;
    }
    if (out_bytes->size() < need) out_bytes->resize(need);
    if (out_piece_offsets) out_piece_offsets->assign(pieces.size(), 0u);

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
        if (out_piece_offsets) {
            (*out_piece_offsets)[static_cast<size_t>(p_idx)] = static_cast<size_t>(output.nWordsUsed);
        }
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

void ComputeStats2D(std::vector<double> vals, double* mean, double* median, double* p95,
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

std::vector<uint8_t> MaskCfhdVolatileMetadata(
    const std::vector<uint8_t>& bitstream,
    std::vector<Phase2DMetadataDiffRange>* out_ranges) {
    std::vector<uint8_t> masked = bitstream;
    if (out_ranges) out_ranges->clear();
    if (masked.size() < 64) return masked;

    const size_t scan_limit = std::min<size_t>(masked.size() - 8, 4096u);
    for (size_t off = 0; off + 8 <= scan_limit; off += 4) {
        uint32_t tag = 0;
        uint32_t typesize = 0;
        std::memcpy(&tag, masked.data() + off, sizeof(uint32_t));
        std::memcpy(&typesize, masked.data() + off + 4, sizeof(uint32_t));
        const uint32_t sz = typesize & 0x00FFFFFFu;
        const uint32_t aligned_sz = (sz + 3u) & ~3u;

        const char* name = nullptr;
        bool match = false;
        if (tag == TAG_CLIP_GUID && sz == 16u) {
            name = "TAG_CLIP_GUID";
            match = true;
        } else if (tag == TAG_ENCODE_DATE && sz == 10u) {
            name = "TAG_ENCODE_DATE";
            match = true;
        } else if (tag == TAG_ENCODE_TIME && sz == 8u) {
            name = "TAG_ENCODE_TIME";
            match = true;
        } else if (tag == TAG_TIMECODE && sz == 11u) {
            name = "TAG_TIMECODE";
            match = true;
        }
        if (match && off + 8u + aligned_sz <= masked.size()) {
            std::memset(masked.data() + off + 8u, 0, aligned_sz);
            if (out_ranges) {
                Phase2DMetadataDiffRange r{};
                r.tag_fourcc = tag;
                r.tag_name = name;
                r.byte_offset = off + 8u;
                r.byte_length = aligned_sz;
                out_ranges->push_back(r);
            }
            off += 4u + aligned_sz;
        }
    }
    return masked;
}

Phase2DUpstreamComparisonReport VerifyPhase2DUpstreamComparison(
    int width, int height,
    Phase2DQuantStateMode state_mode,
    uint32_t seed,
    EncodingQuality quality) {
    (void)seed;
    Phase2DUpstreamComparisonReport rep;
    rep.width = width;
    rep.height = height;
    rep.quant_state_mode = state_mode;
    rep.encoding_quality = EncodingQualitySdkName(quality);

    const std::vector<uint8_t> frame = MakeFrame(width, height, CanonicalPattern::Canonical);

    // 1. Pristine Upstream Handle A (unmodified upstream CFHD_EncodeSample).
    Phase2DEncoderContext ctx_a;
    if (!ctx_a.Init(width, height, frame, state_mode, false, quality)) {
        rep.error = "failed to initialize pristine upstream encoder handle A";
        return rep;
    }
    void* a_data_ptr = nullptr;
    size_t a_data_size = 0;
    if (CFHD_GetSampleData(ctx_a.ref, &a_data_ptr, &a_data_size) != CFHD_ERROR_OKAY ||
        !a_data_ptr || a_data_size == 0) {
        rep.error = "CFHD_GetSampleData failed on handle A";
        return rep;
    }
    const std::vector<uint8_t> sample_a_indep(
        static_cast<const uint8_t*>(a_data_ptr),
        static_cast<const uint8_t*>(a_data_ptr) + a_data_size);

    // 2. HyperCine Reference/GPU Handle B/C initialized to the exact same quantizer state.
    Phase2DEncoderContext ctx_bc;
    if (!ctx_bc.Init(width, height, frame, state_mode, false, quality)) {
        rep.error = "failed to initialize encoder handle B/C";
        return rep;
    }
    void* a_same_ptr = nullptr;
    size_t a_same_size = 0;
    CFHD_GetSampleData(ctx_bc.ref, &a_same_ptr, &a_same_size);
    const std::vector<uint8_t> sample_a_same_handle(
        static_cast<const uint8_t*>(a_same_ptr),
        static_cast<const uint8_t*>(a_same_ptr) + a_same_size);

    const QuantTables2D quant = ReadQuant2D(ctx_bc.encoder, ctx_bc.transforms, ctx_bc.channel_count);
    const std::vector<BandRef> refs = EnumerateBands(
        reinterpret_cast<transform**>(ctx_bc.transforms), ctx_bc.channel_count);
    if (!quant.ok || refs.size() != 30) {
        rep.error = "invalid pyramid band count";
        return rep;
    }

    rep.fs_rate_limiter = quant.fs_rate_limiter;
    rep.last_gop_bytes = quant.last_gop_bytes;
    const double raw_yuv422_12bit_bytes = static_cast<double>(width) * static_cast<double>(height) * 3.0 * 12.0 / 8.0 / 1.5;
    rep.target_compression_ratio = a_data_size > 0 ? (raw_yuv422_12bit_bytes / static_cast<double>(a_data_size)) : 0.0;
    for (int i = 0; i < 3; ++i) rep.prescale[i] = quant.prescale[i];
    for (int i = 0; i < 36; ++i) {
        rep.quant_table[i] = quant.quant[i];
        rep.scale_table[i] = quant.scale[i];
    }

    // Refresh pyramid on ctx_bc using the exact quantizer state already established on ctx_bc.
    if (!RefreshUpstreamPyramid(reinterpret_cast<encoder*>(ctx_bc.encoder),
                                reinterpret_cast<transform**>(ctx_bc.transforms),
                                ctx_bc.channel_count, frame.data(), width * 4,
                                ctx_bc.sample->m_scratchBuffer, ctx_bc.sample->m_scratchBufferSize)) {
        rep.error = "RefreshUpstreamPyramid failed";
        return rep;
    }

    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));
    std::vector<EncodedPiece> b_pieces(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const BandRef& ref = refs[i];
        piece_of[ref.channel][ref.wavelet_index][ref.band] = static_cast<int>(i);
        IMAGE* wavelet = ctx_bc.transforms[ref.channel]->wavelet[ref.wavelet_index];
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        const PIXEL* src = wavelet->band[ref.band];
        std::vector<int16_t> coeffs(static_cast<size_t>(ref.width) * static_cast<size_t>(ref.height));
        for (int y = 0; y < ref.height; ++y) {
            std::memcpy(coeffs.data() + static_cast<size_t>(y) * ref.width,
                        src + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(ref.width) * sizeof(int16_t));
        }
        ExternalCoeff ext{};
        ext.channel = ref.channel;
        ext.level = ref.level;
        ext.band = ref.band;
        ext.subband = ref.subband;
        ext.lowpass = ref.lowpass;
        ext.data = coeffs.data();
        ext.elem_bytes = 2;
        ext.width = ref.width;
        ext.height = ref.height;
        if (!EncodeOneExternalBand(reinterpret_cast<encoder*>(ctx_bc.encoder),
                                   reinterpret_cast<transform**>(ctx_bc.transforms),
                                   ext, &b_pieces[i])) {
            rep.error = "EncodeOneExternalBand failed on B";
            return rep;
        }
    }

    // Rewind frame_number by 1 so AssembleSampleFast2D increments to the exact same frame_number as ctx_bc's last pass.
    const uint32_t saved_frame_num = ctx_bc.encoder->frame_number - 1;
    ctx_bc.encoder->frame_number = saved_frame_num;
    std::vector<uint8_t> sample_b;
    std::vector<size_t> b_piece_offsets;
    if (!AssembleSampleFast2D(ctx_bc.encoder, ctx_bc.transforms, ctx_bc.channel_count,
                              b_pieces, piece_of, &sample_b, nullptr, &b_piece_offsets)) {
        rep.error = "AssembleSampleFast2D failed for B";
        return rep;
    }

    // 3. Run C (HyperCine GPU) across ReferencePhase2C, Fused, FusedScanOptimized, and CUDA Graph.
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

    std::vector<GpuBandRequest> hp_reqs;
    std::vector<EntropyTables> hp_tables;
    for (const BandRef& ref : refs) {
        if (ref.lowpass) continue;
        const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
        EntropyTables tbl;
        LoadEntropyTables(reinterpret_cast<encoder*>(ctx_bc.encoder), ref.subband, &tbl);
        hp_reqs.push_back(GpuBandRequest{d_ptr, ref.width, ref.height, ref.width, ref.channel, ref.wavelet_index, ref.band, ref.subband});
        hp_tables.push_back(std::move(tbl));
    }

    auto encode_gpu_mode = [&](GpuEntropyKernelMode kmode, bool use_graph,
                               std::vector<EncodedPiece>* out_pieces,
                               std::vector<uint8_t>* out_sample) -> bool {
        GpuEntropyFrameConfig e_cfg;
        e_cfg.entropy_streams = 4;
        e_cfg.d2h_mode = CompressedD2HMode::FrameConsolidated;
        e_cfg.kernel_mode = kmode;
        e_cfg.use_cuda_graph = use_graph;
        if (!QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), e_cfg)) {
            return false;
        }
        if (!WaitGpuEntropyFrameComplete(e_slot)) return false;

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
                if (!BuildLowpassPayloadPtr(ref.subband, ref.level, ref.width, ref.height, h_ptr, false, &p.payload, &p.bits_free)) {
                    return false;
                }
                p.ok = true;
            } else {
                GpuBandCompressedView view{};
                GetGpuBandCompressedView(e_slot, hp_idx, &view);
                IMAGE* wavelet = ctx_bc.transforms[ref.channel]->wavelet[ref.wavelet_index];
                int qv = wavelet->quantization[ref.band];
                if (qv <= 0) qv = wavelet->quant[ref.band];
                if (qv <= 0) qv = 1;
                if (!BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx_bc.encoder), ref.band, ref.width, ref.height,
                                             ref.subband, qv, wavelet->scale[ref.band], hp_tables[static_cast<size_t>(hp_idx)],
                                             view.bytes, static_cast<size_t>(view.byte_count), &p.payload, &p.bits_free)) {
                    return false;
                }
                p.ok = true;
                ++hp_idx;
            }
        }
        ctx_bc.encoder->frame_number = saved_frame_num;
        return AssembleSampleFast2D(ctx_bc.encoder, ctx_bc.transforms, ctx_bc.channel_count,
                                    *out_pieces, piece_of, out_sample, nullptr);
    };

    std::vector<EncodedPiece> c_ref_pieces, c_fused_pieces, c_scan_pieces, c_graph_pieces;
    std::vector<uint8_t> sample_c_ref, sample_c_fused, sample_c_scan, sample_c_graph;
    if (!encode_gpu_mode(GpuEntropyKernelMode::ReferencePhase2C, false, &c_ref_pieces, &sample_c_ref) ||
        !encode_gpu_mode(GpuEntropyKernelMode::Fused, false, &c_fused_pieces, &sample_c_fused) ||
        !encode_gpu_mode(GpuEntropyKernelMode::FusedScanOptimized, false, &c_scan_pieces, &sample_c_scan) ||
        !encode_gpu_mode(GpuEntropyKernelMode::FusedScanOptimized, true, &c_graph_pieces, &sample_c_graph)) {
        rep.error = "GPU encode mode failed during upstream comparison";
        DestroyGpuEntropyFrameSlot(e_slot);
        DestroyGpuEntropySession(session);
        HyperCine_GpuSlotDestroy(w_slot);
        return rep;
    }

    rep.a_upstream_sample_bytes = sample_a_indep.size();
    rep.b_cpu_ref_sample_bytes = sample_b.size();
    rep.c_gpu_ref_sample_bytes = sample_c_ref.size();
    rep.c_gpu_fused_sample_bytes = sample_c_fused.size();
    rep.c_gpu_scan_opt_sample_bytes = sample_c_scan.size();
    rep.c_gpu_graph_sample_bytes = sample_c_graph.size();

    rep.a_upstream_sha256 = Sha256Hex(sample_a_indep.data(), sample_a_indep.size());
    rep.b_cpu_ref_sha256 = Sha256Hex(sample_b.data(), sample_b.size());
    rep.c_gpu_ref_sha256 = Sha256Hex(sample_c_ref.data(), sample_c_ref.size());
    rep.c_gpu_fused_sha256 = Sha256Hex(sample_c_fused.data(), sample_c_fused.size());
    rep.c_gpu_scan_opt_sha256 = Sha256Hex(sample_c_scan.data(), sample_c_scan.size());
    rep.c_gpu_graph_sha256 = Sha256Hex(sample_c_graph.data(), sample_c_graph.size());

    rep.b_eq_c_unmasked = (sample_b == sample_c_ref);
    rep.c_all_modes_byte_identical =
        (sample_c_ref == sample_c_fused) &&
        (sample_c_ref == sample_c_scan) &&
        (sample_c_ref == sample_c_graph);
    rep.a_eq_b_same_handle_unmasked = (sample_a_same_handle == sample_b);

    // Compare independent handle A vs B/C with and without volatile metadata masking.
    if (sample_a_indep.size() == sample_b.size()) {
        size_t raw_diffs = 0;
        for (size_t i = 0; i < sample_a_indep.size(); ++i) {
            if (sample_a_indep[i] != sample_b[i]) ++raw_diffs;
        }
        rep.unmasked_independent_diff_bytes = raw_diffs;
    }

    const std::vector<uint8_t> masked_a = MaskCfhdVolatileMetadata(sample_a_indep, &rep.volatile_metadata_ranges);
    const std::vector<uint8_t> masked_b = MaskCfhdVolatileMetadata(sample_b);
    const std::vector<uint8_t> masked_c = MaskCfhdVolatileMetadata(sample_c_graph);
    rep.a_masked_sha256 = Sha256Hex(masked_a.data(), masked_a.size());
    rep.b_masked_sha256 = Sha256Hex(masked_b.data(), masked_b.size());
    rep.c_masked_sha256 = Sha256Hex(masked_c.data(), masked_c.size());

    if (masked_a.size() == masked_b.size()) {
        size_t m_diffs = 0;
        for (size_t i = 0; i < masked_a.size(); ++i) {
            if (masked_a[i] != masked_b[i]) ++m_diffs;
        }
        rep.masked_independent_diff_bytes = m_diffs;
    }
    rep.a_eq_b_independent_handle_masked = (masked_a == masked_b);
    rep.a_eq_c_independent_handle_masked = (masked_a == masked_c);

    // Compare all 30 subbands across A (sliced from pristine CFHD_EncodeSample bitstream), B, and C.
    rep.bands.resize(refs.size());
    rep.all_30_subbands_a_eq_b_eq_c = true;
    for (size_t i = 0; i < refs.size(); ++i) {
        const BandRef& ref = refs[i];
        Phase2DSubbandComparison& sb = rep.bands[i];
        sb.index = static_cast<int>(i);
        sb.channel = ref.channel;
        sb.level = ref.level;
        sb.band = ref.band;
        sb.subband = ref.subband;
        sb.name = BandName2D(ref.channel, ref.level, ref.band);
        sb.lowpass = ref.lowpass;
        sb.width = ref.width;
        sb.height = ref.height;
        IMAGE* wavelet = ctx_bc.transforms[ref.channel]->wavelet[ref.wavelet_index];
        sb.quant = wavelet->quant[ref.band];
        sb.scale = wavelet->scale[ref.band];

        const size_t off = b_piece_offsets[i];
        const size_t len = b_pieces[i].payload.size();
        sb.b_cpu_ref_bytes = len;
        sb.c_gpu_bytes = c_graph_pieces[i].payload.size();
        sb.b_sha256 = Sha256Hex(b_pieces[i].payload.data(), b_pieces[i].payload.size());
        sb.c_sha256 = Sha256Hex(c_graph_pieces[i].payload.data(), c_graph_pieces[i].payload.size());
        sb.b_eq_c = (b_pieces[i].payload == c_graph_pieces[i].payload);

        if (off + len <= sample_a_indep.size()) {
            sb.a_upstream_bytes = len;
            sb.a_sha256 = Sha256Hex(sample_a_indep.data() + off, len);
            sb.a_eq_b = (std::memcmp(sample_a_indep.data() + off, b_pieces[i].payload.data(), len) == 0);
            sb.a_eq_c = (std::memcmp(sample_a_indep.data() + off, c_graph_pieces[i].payload.data(), len) == 0);
        }
        if (!sb.a_eq_b || !sb.b_eq_c || !sb.a_eq_c) {
            rep.all_30_subbands_a_eq_b_eq_c = false;
        }
    }

    // Decode A, B, and C via upstream CFHD_DecodeSample and compare pixels.
    std::vector<uint8_t> dec_a, dec_b, dec_c;
    int pitch_a = 0, pitch_b = 0, pitch_c = 0;
    if (DecodeCfhdSample2D(sample_a_indep, width, height, &dec_a, &pitch_a) &&
        DecodeCfhdSample2D(sample_b, width, height, &dec_b, &pitch_b) &&
        DecodeCfhdSample2D(sample_c_graph, width, height, &dec_c, &pitch_c)) {
        rep.decoded_a_eq_b = (dec_a == dec_b);
        rep.decoded_b_eq_c = (dec_b == dec_c);
        int max_diff = 0;
        for (size_t i = 0; i < dec_a.size() && i < dec_c.size(); ++i) {
            max_diff = std::max(max_diff, std::abs(static_cast<int>(dec_a[i]) - static_cast<int>(dec_c[i])));
        }
        rep.max_abs_pixel_diff = max_diff;
        rep.decoded_pixels_sha256 = Sha256Hex(dec_c.data(), dec_c.size());
        rep.psnr_db = ComputeBestPsnr2D(frame, dec_c, width, height, pitch_c);
    }

    rep.ok = rep.b_eq_c_unmasked &&
             rep.c_all_modes_byte_identical &&
             rep.a_eq_b_same_handle_unmasked &&
             rep.a_eq_b_independent_handle_masked &&
             rep.a_eq_c_independent_handle_masked &&
             rep.all_30_subbands_a_eq_b_eq_c &&
             rep.decoded_a_eq_b &&
             rep.decoded_b_eq_c &&
             rep.max_abs_pixel_diff == 0 &&
             rep.psnr_db > 35.0;

    DestroyGpuEntropyFrameSlot(e_slot);
    DestroyGpuEntropySession(session);
    HyperCine_GpuSlotDestroy(w_slot);
    return rep;
}

Phase2DMetadataSafetyReport VerifyPhase2DMetadataSafety(int width, int height) {
    Phase2DMetadataSafetyReport rep;
    const std::vector<uint8_t> frame = MakeFrame(width, height, CanonicalPattern::Canonical);

    auto encode_one_clip = [&](bool det_meta, std::vector<uint8_t>* out_sample,
                               std::string* out_guid, std::string* out_date, std::string* out_time) -> bool {
        Phase2DConfig cfg;
        cfg.width = width;
        cfg.height = height;
        cfg.warmup_frames = 0;
        cfg.frames = 1;
        cfg.slot_policy = Phase2DSlotPolicy::LowLatency;
        cfg.deterministic_test_metadata = det_meta;
        cfg.verify_bitstream = true;
        Phase2DResult r = RunPhase2D(cfg);
        // Metadata safety depends only on the strict encoder/bitstream outcome.
        // Phase2DResult.ok also folds in the original-SDK PSNR, which is a
        // non-reproducible reference signal and must not fail this check.
        if (!r.bitstream_byte_match || !r.decode_ok || r.last_bitstream.empty()) return false;
        *out_sample = std::move(r.last_bitstream);

        // Extract GUID, date, and time from sample metadata header.
        const size_t scan_limit = std::min<size_t>(out_sample->size() - 8, 4096u);
        for (size_t off = 0; off + 8 <= scan_limit; off += 4) {
            uint32_t tag = 0, typesize = 0;
            std::memcpy(&tag, out_sample->data() + off, 4);
            std::memcpy(&typesize, out_sample->data() + off + 4, 4);
            const uint32_t sz = typesize & 0x00FFFFFFu;
            if (tag == TAG_CLIP_GUID && sz == 16u && off + 24u <= out_sample->size() && out_guid) {
                uint32_t g[4] = {};
                std::memcpy(g, out_sample->data() + off + 8u, 16);
                *out_guid = GuidToHex(g);
            } else if (tag == TAG_ENCODE_DATE && sz == 10u && off + 18u <= out_sample->size() && out_date) {
                out_date->assign(reinterpret_cast<const char*>(out_sample->data() + off + 8u), 10);
            } else if (tag == TAG_ENCODE_TIME && sz == 8u && off + 16u <= out_sample->size() && out_time) {
                out_time->assign(reinterpret_cast<const char*>(out_sample->data() + off + 8u), 8);
            }
        }
        return true;
    };

    std::vector<uint8_t> clip1, clip2, det1, det2;
    std::string date2, time2;
    if (!encode_one_clip(false, &clip1, &rep.clip1_guid_hex, &rep.clip1_date, &rep.clip1_time) ||
        !encode_one_clip(false, &clip2, &rep.clip2_guid_hex, &date2, &time2) ||
        !encode_one_clip(true, &det1, nullptr, nullptr, nullptr) ||
        !encode_one_clip(true, &det2, nullptr, nullptr, nullptr)) {
        rep.error = "failed to encode test clips for metadata safety verification";
        return rep;
    }

    rep.clip1_sha256 = Sha256Hex(clip1.data(), clip1.size());
    rep.clip2_sha256 = Sha256Hex(clip2.data(), clip2.size());
    const std::vector<uint8_t> m1 = MaskCfhdVolatileMetadata(clip1);
    const std::vector<uint8_t> m2 = MaskCfhdVolatileMetadata(clip2);
    rep.clip1_masked_sha256 = Sha256Hex(m1.data(), m1.size());
    rep.clip2_masked_sha256 = Sha256Hex(m2.data(), m2.size());
    rep.det1_sha256 = Sha256Hex(det1.data(), det1.size());
    rep.det2_sha256 = Sha256Hex(det2.data(), det2.size());

    const std::string fake_guid = "1337beef-20260420-cafebabe-43464844";
    rep.normal_mode_guids_distinct =
        !rep.clip1_guid_hex.empty() &&
        !rep.clip2_guid_hex.empty() &&
        (rep.clip1_guid_hex != rep.clip2_guid_hex) &&
        (rep.clip1_guid_hex != fake_guid) &&
        (rep.clip2_guid_hex != fake_guid);
    rep.normal_mode_unmasked_distinct = (clip1 != clip2);
    rep.normal_mode_masked_identical = (m1 == m2);
    rep.deterministic_mode_unmasked_identical = (det1 == det2);

    rep.ok = rep.normal_mode_guids_distinct &&
             rep.normal_mode_unmasked_distinct &&
             rep.normal_mode_masked_identical &&
             rep.deterministic_mode_unmasked_identical;
    if (!rep.ok) {
        rep.error = "metadata safety check failed (guid1=" + rep.clip1_guid_hex +
                    ", guid2=" + rep.clip2_guid_hex + ")";
    }
    return rep;
}

Phase2DSlotSelection SelectPhase2DFrameSlots(
    int width, int height,
    Phase2DSlotPolicy slot_policy,
    int user_override_slots,
    Phase2DD2HPolicy d2h_policy,
    size_t override_available_vram_bytes) {
    Phase2DSlotSelection sel{};
    sel.width = width;
    sel.height = height;
    sel.policy = (user_override_slots > 0) ? Phase2DSlotPolicy::UserOverride : slot_policy;
    sel.requested_override_slots = user_override_slots;

    const size_t px = static_cast<size_t>(std::max(1, width)) * static_cast<size_t>(std::max(1, height));
    const size_t w_slot_vram = px * 4u + px * 10u;
    const size_t e_slot_vram = (px + 32u * 32768u) * 4u + (px / 2u + 131072u) * 4u;
    sel.per_slot_vram_bytes = w_slot_vram + e_slot_vram;
    // Session workspaces (4 streams * max highpass band buffers ~ 0.5 * px * 24 bytes).
    sel.session_workspace_vram_bytes = px * 12u + (32ull << 20);

    size_t free_vram = override_available_vram_bytes;
    if (free_vram == 0) {
        size_t total_vram = 0;
        if (cudaMemGetInfo(&free_vram, &total_vram) != cudaSuccess || free_vram == 0) {
            free_vram = 4ull << 30;  // Fallback 4 GB if query unavailable
        }
    }
    sel.available_vram_bytes = free_vram;

    // Reserve at least 256 MB + session workspaces, using up to 75% of remaining free VRAM for slots.
    const size_t reserve_bytes = sel.session_workspace_vram_bytes + (256ull << 20);
    const size_t usable_vram = (free_vram > reserve_bytes)
        ? ((free_vram - reserve_bytes) * 3u / 4u)
        : sel.per_slot_vram_bytes;
    const int vram_bound = static_cast<int>(std::clamp<size_t>(
        usable_vram / std::max<size_t>(1u, sel.per_slot_vram_bytes), 1u, 8u));
    sel.vram_max_slots = vram_bound;

    int target = 2;
    if (sel.policy == Phase2DSlotPolicy::UserOverride && user_override_slots > 0) {
        target = std::clamp(user_override_slots, 1, 8);
    } else if (sel.policy == Phase2DSlotPolicy::LowLatency) {
        target = 1;
    } else if (sel.policy == Phase2DSlotPolicy::MaxThroughput) {
        if (px <= 1920ull * 1080ull) {
            target = 2;  // 1080p peaks at 2 slots (271 fps vs 257 fps at 3 slots)
        } else {
            target = 3;  // 4K and 8K max throughput at 3 slots
        }
    } else {
        // Phase2DSlotPolicy::DefaultAuto
        if (px <= 1920ull * 1080ull) {
            target = 2;  // 1080p -> 2 slots (higher FPS, 33% less VRAM, 36% lower latency)
        } else if (px <= 4096ull * 2160ull) {
            target = 3;  // 4K -> 3 slots (peak sustained FPS)
        } else {
            target = 2;  // 8K -> 2 slots (saves ~239 MB VRAM & 32% latency within ~1% of 3-slot FPS)
        }
    }
    sel.policy_target_slots = target;
    sel.selected_slots = std::clamp(std::min(target, vram_bound), 1, 8);
    sel.total_estimated_vram_bytes =
        sel.session_workspace_vram_bytes + static_cast<size_t>(sel.selected_slots) * sel.per_slot_vram_bytes;

    // D2H strategy selection (Part 5).
    if (d2h_policy == Phase2DD2HPolicy::ForceFrameConsolidated) {
        sel.selected_d2h_mode = CompressedD2HMode::FrameConsolidated;
    } else if (d2h_policy == Phase2DD2HPolicy::ForceBandStreamed) {
        sel.selected_d2h_mode = CompressedD2HMode::BandStreamed;
    } else if (d2h_policy == Phase2DD2HPolicy::LowLatency) {
        // BandStreamed for 1080p/4K low-latency single-frame turnarounds; FrameConsolidated for 8K.
        sel.selected_d2h_mode = (px <= 4096ull * 2160ull)
            ? CompressedD2HMode::BandStreamed
            : CompressedD2HMode::FrameConsolidated;
    } else {
        // Throughput default: FrameConsolidated across all resolutions.
        sel.selected_d2h_mode = CompressedD2HMode::FrameConsolidated;
    }
    return sel;
}

namespace {

enum class PipeSlotStage2D {
    Idle = 0,
    EntropyAndD2HQueued = 2,
    AssemblingOnWorker = 3
};

struct PipeSlot2D {
    int slot_id = 0;
    HyperCineGpuSlot* w_slot = nullptr;
    GpuEntropyFrameSlot* e_slot = nullptr;
    std::atomic<int> stage{static_cast<int>(PipeSlotStage2D::Idle)};
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

Phase2DResult RunPhase2D(const Phase2DConfig& cfg) {
    Phase2DResult res;
    res.config = cfg;
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.frames <= 0) {
        res.error = "invalid Phase2DConfig dimensions or frame count";
        return res;
    }

    const int width = cfg.width;
    const int height = cfg.height;
    const int pitch = width * 4;
    res.slot_selection = SelectPhase2DFrameSlots(
        width, height, cfg.slot_policy, cfg.user_override_slots, cfg.d2h_policy);
    const int num_slots = res.slot_selection.selected_slots;
    const int num_streams = std::clamp(cfg.entropy_streams, 1, 8);
    const CompressedD2HMode d2h_mode = res.slot_selection.selected_d2h_mode;

    const std::vector<uint8_t> canonical_frame = MakeFrame(width, height, CanonicalPattern::Canonical);
    Phase2DEncoderContext ctx;
    if (!ctx.Init(width, height, canonical_frame, cfg.quant_state_mode, cfg.deterministic_test_metadata)) {
        res.error = "failed to initialize upstream encoder";
        return res;
    }

    const QuantTables2D quant = ReadQuant2D(ctx.encoder, ctx.transforms, ctx.channel_count);
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

    // Optional reference CPU bitstream for verification under identical quantizer state.
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

    std::vector<PipeSlot2D> slots(static_cast<size_t>(num_slots));
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
        for (PipeSlot2D& s : slots) {
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
            // Copy before cleanup so the diagnostic cannot be invalidated by
            // teardown CUDA calls.
            const std::string alloc_error = GpuEntropySlotCreationError();
            cleanup_all();
            res.error = "failed to allocate multi-frame GPU slot";
            if (!alloc_error.empty()) res.error += ": " + alloc_error;
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

    auto wrap_one_highpass = [&](PipeSlot2D& slot, int hp_idx) -> bool {
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

    auto assemble_slot_cpu = [&](PipeSlot2D& slot) -> bool {
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

        if (d2h_mode == CompressedD2HMode::FrameConsolidated) {
            const double t_hdr0 = NowMs();
            for (int hp_idx = 0; hp_idx < static_cast<int>(hp_to_piece.size()); ++hp_idx) {
                if (!wrap_one_highpass(slot, hp_idx)) return false;
            }
            slot.cpu_header_wrap_ms = NowMs() - t_hdr0;
        }

        double concat_ms = 0.0;
        if (!AssembleSampleFast2D(ctx.encoder, ctx.transforms, ctx.channel_count,
                                  slot.pieces, piece_of, &slot.bitstream, &concat_ms)) {
            return false;
        }
        slot.cpu_sample_concat_ms = concat_ms;
        slot.complete_ms = NowMs();
        return true;
    };

    const int eff_warmup = std::max(cfg.warmup_frames, std::max(num_slots * 2, height < 2000 ? 6 : 4));
    const int total_frames = eff_warmup + cfg.frames;
    std::vector<double> frame_submit_ms(static_cast<size_t>(total_frames), 0.0);
    std::vector<double> frame_complete_ms(static_cast<size_t>(total_frames), 0.0);

    std::mutex asm_mu;
    std::condition_variable asm_cv;
    std::condition_variable asm_done_cv;
    std::deque<int> asm_queue;
    bool asm_stop = false;
    std::atomic<bool> asm_failed{false};

    double sum_h2d_ms = 0.0, sum_color_ms = 0.0, sum_wavelet_ms = 0.0, sum_lp_d2h_ms = 0.0;
    double sum_ent_ms = 0.0, sum_cons_ms = 0.0, sum_comp_d2h_ms = 0.0;
    double sum_enqueue_ms = 0.0, max_graph_capture_ms = 0.0;
    double sum_cpu_lp_ms = 0.0, sum_cpu_hdr_ms = 0.0, sum_cpu_concat_ms = 0.0;
    EntropyTiming sum_substage{};
    int substage_counted = 0;
    int recorded_launches_per_frame = 0;
    int recorded_graph_kernel_nodes = 0;
    int recorded_graph_total_nodes = 0;
    bool recorded_graph_used = false;

    auto record_completed_slot = [&](PipeSlot2D& slot) {
        const int f = slot.frame_index;
        frame_complete_ms[static_cast<size_t>(f)] = slot.complete_ms;
        GpuFrameEntropyStats est{};
        CollectGpuFrameEntropyStats(slot.e_slot, &est);
        if (est.graph_capture_ms > max_graph_capture_ms) {
            max_graph_capture_ms = est.graph_capture_ms;
        }
        if (f >= eff_warmup) {
            HyperCineStreamTiming wt{};
            HyperCine_GpuSlotReadTiming(slot.w_slot, &wt);

            sum_h2d_ms += wt.h2d_ms;
            sum_color_ms += wt.color_ms;
            sum_wavelet_ms += wt.compute_ms;
            sum_lp_d2h_ms += wt.d2h_ms;
            sum_ent_ms += est.gpu_entropy_wall_ms;
            sum_cons_ms += est.gpu_consolidate_ms;
            sum_comp_d2h_ms += est.compressed_d2h_ms;
            sum_enqueue_ms += est.host_enqueue_ms;

            sum_cpu_lp_ms += slot.cpu_lowpass_ms;
            sum_cpu_hdr_ms += slot.cpu_header_wrap_ms;
            sum_cpu_concat_ms += slot.cpu_sample_concat_ms;

            recorded_launches_per_frame = est.total_kernel_launches;
            recorded_graph_kernel_nodes = est.graph_kernel_nodes;
            recorded_graph_total_nodes = est.graph_total_nodes;
            recorded_graph_used = est.graph_used;

            if (slot.e_slot && cfg.record_substage_events && !cfg.use_cuda_graph) {
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
    std::atomic<double> worker_cpu_sec_start{0.0};
    double worker_cpu_sec_total = 0.0;
    std::thread asm_thread;
    if (use_worker) {
        asm_thread = std::thread([&]() {
            for (;;) {
                int s_idx = -1;
                {
                    std::unique_lock<std::mutex> lk(asm_mu);
                    asm_cv.wait(lk, [&] { return asm_stop || !asm_queue.empty(); });
                    if (asm_stop && asm_queue.empty()) {
                        worker_cpu_sec_total = std::max(0.0, CurrentThreadCpuSeconds() - worker_cpu_sec_start.load(std::memory_order_acquire));
                        return;
                    }
                    s_idx = asm_queue.front();
                    asm_queue.pop_front();
                }
                PipeSlot2D& slot = slots[static_cast<size_t>(s_idx)];
                if (slot.frame_index == eff_warmup) {
                    worker_cpu_sec_start.store(CurrentThreadCpuSeconds(), std::memory_order_release);
                }
                if (!assemble_slot_cpu(slot)) {
                    asm_failed.store(true);
                } else {
                    record_completed_slot(slot);
                }
                slot.stage.store(static_cast<int>(PipeSlotStage2D::Idle), std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lk(asm_mu);
                }
                asm_done_cv.notify_all();
            }
        });
    }

    int next_submit_frame = 0;
    int next_gpu_done_frame = 0;
    int last_submitted_slot = -1;
    double timed_start_ms = 0.0;
    double proc_cpu_s0 = 0.0;
    double main_cpu_s0 = 0.0;
    double main_thread_running_ms = 0.0;
    double main_thread_blocked_ms = 0.0;
    double main_thread_spin_ms = 0.0;
    int64_t inflight_accum = 0;
    int64_t poll_iters = 0;
    int64_t blocking_waits = 0;

    while (next_gpu_done_frame < total_frames && !asm_failed.load(std::memory_order_relaxed)) {
        const double t_iter0 = NowMs();
        bool did_work = false;

        // 1. Submit at most one new frame to the next Idle slot in FIFO order.
        if (next_submit_frame < total_frames) {
            const int target_slot = next_submit_frame % num_slots;
            PipeSlot2D& slot = slots[static_cast<size_t>(target_slot)];
            if (slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage2D::Idle)) {
                const double t_work0 = NowMs();
                if (next_submit_frame == eff_warmup) {
                    timed_start_ms = t_work0;
                    proc_cpu_s0 = ProcessCpuSeconds();
                    main_cpu_s0 = CurrentThreadCpuSeconds();
                    worker_cpu_sec_start.store(0.0, std::memory_order_release);
                }

                void* wait_fe = (last_submitted_slot >= 0)
                    ? HyperCine_GpuSlotFrontendEvent(slots[static_cast<size_t>(last_submitted_slot)].w_slot)
                    : nullptr;
                void* wait_wv = (last_submitted_slot >= 0)
                    ? HyperCine_GpuSlotWaveletEvent(slots[static_cast<size_t>(last_submitted_slot)].w_slot)
                    : nullptr;
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
                e_cfg.d2h_mode = d2h_mode;
                e_cfg.record_substage_events = cfg.record_substage_events && !cfg.use_cuda_graph && (next_submit_frame >= eff_warmup);
                e_cfg.lpt_schedule = true;
                e_cfg.use_cuda_graph = cfg.use_cuda_graph;
                e_cfg.kernel_mode = cfg.kernel_mode;

                void* wv_done_ev = HyperCine_GpuSlotWaveletEvent(slot.w_slot);
                void* const* band_evs = (!cfg.use_cuda_graph && d2h_mode == CompressedD2HMode::BandStreamed)
                    ? slot_band_wait_evs[static_cast<size_t>(target_slot)].data()
                    : nullptr;

                if (!QueueGpuEntropyFrame(session, slot.e_slot,
                                          slot_hp_reqs[static_cast<size_t>(target_slot)].data(),
                                          static_cast<int>(slot_hp_reqs[static_cast<size_t>(target_slot)].size()),
                                          hp_tables.data(), e_cfg,
                                          (cfg.use_cuda_graph || d2h_mode == CompressedD2HMode::FrameConsolidated) ? wv_done_ev : nullptr,
                                          band_evs)) {
                    const char* slot_error = GpuEntropyFrameError(slot.e_slot);
                    res.error = (slot_error && slot_error[0]) ? slot_error : "QueueGpuEntropyFrame failed";
                    asm_failed.store(true);
                    break;
                }

                slot.stage.store(static_cast<int>(PipeSlotStage2D::EntropyAndD2HQueued), std::memory_order_release);
                last_submitted_slot = target_slot;
                ++next_submit_frame;
                if (next_submit_frame > eff_warmup) main_thread_running_ms += NowMs() - t_work0;
                did_work = true;
            }
        }

        // 2. Progress D2H transfers across all active slots in flight.
        int active_in_flight = 0;
        for (int s = 0; s < num_slots; ++s) {
            PipeSlot2D& slot = slots[static_cast<size_t>(s)];
            const int st = slot.stage.load(std::memory_order_acquire);
            if (st != static_cast<int>(PipeSlotStage2D::Idle)) ++active_in_flight;
            if (st != static_cast<int>(PipeSlotStage2D::EntropyAndD2HQueued)) continue;

            if (d2h_mode == CompressedD2HMode::FrameConsolidated) {
                const double t_w0 = NowMs();
                if (PollGpuEntropyFrameD2H(slot.e_slot)) {
                    if (next_submit_frame > eff_warmup) main_thread_running_ms += NowMs() - t_w0;
                    did_work = true;
                }
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
                    if (slot.frame_index >= eff_warmup) main_thread_running_ms += dt;
                    ++slot.streamed_wrapped_count;
                    did_work = true;
                }
            }
        }
        if (next_submit_frame > eff_warmup) {
            inflight_accum += active_in_flight;
            ++poll_iters;
        }

        // 3. Check if the oldest GPU-active frame (`next_gpu_done_frame`) has finished D2H.
        const int done_slot_idx = next_gpu_done_frame % num_slots;
        PipeSlot2D& done_slot = slots[static_cast<size_t>(done_slot_idx)];
        if (done_slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage2D::EntropyAndD2HQueued)) {
            const int lp_idle = HyperCine_GpuSlotGpuIdle(done_slot.w_slot);
            if (lp_idle < 0) {
                res.error = "lowpass D2H failed";
                asm_failed.store(true);
                break;
            }
            if (lp_idle == 1 && IsGpuEntropyFrameComplete(done_slot.e_slot)) {
                if (use_worker) {
                    done_slot.stage.store(static_cast<int>(PipeSlotStage2D::AssemblingOnWorker), std::memory_order_release);
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
                    if (done_slot.frame_index >= eff_warmup) main_thread_running_ms += NowMs() - t_asm0;
                    done_slot.stage.store(static_cast<int>(PipeSlotStage2D::Idle), std::memory_order_release);
                }
                ++next_gpu_done_frame;
                did_work = true;
            }
        }

        // 4. Synchronization policy when all available slots are busy:
        //    - Blocking mode: sleep on OS-backed CUDA completion event or worker condition variable.
        //    - Polling mode: spin in tight loop (measuring spin time).
        if (!did_work && next_gpu_done_frame < total_frames && !asm_failed.load(std::memory_order_relaxed)) {
            if (cfg.sync_mode == Phase2DSyncMode::Blocking) {
                const double t_blk0 = NowMs();
                const int next_sub_slot_idx = (next_submit_frame < total_frames) ? (next_submit_frame % num_slots) : -1;
                const bool done_slot_active =
                    done_slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage2D::EntropyAndD2HQueued);

                if (done_slot_active && d2h_mode == CompressedD2HMode::FrameConsolidated &&
                    !IsGpuEntropyComputeDone(done_slot.e_slot)) {
                    // Priority 1: Block until oldest active GPU frame finishes entropy so we can immediately kick its D2H DMA.
                    WaitAndStartGpuEntropyFrameD2H(done_slot.e_slot);
                    for (int s = 0; s < num_slots; ++s) {
                        if (slots[static_cast<size_t>(s)].stage.load(std::memory_order_acquire) ==
                            static_cast<int>(PipeSlotStage2D::EntropyAndD2HQueued)) {
                            PollGpuEntropyFrameD2H(slots[static_cast<size_t>(s)].e_slot);
                        }
                    }
                } else if (next_sub_slot_idx >= 0 &&
                           slots[static_cast<size_t>(next_sub_slot_idx)].stage.load(std::memory_order_acquire) ==
                               static_cast<int>(PipeSlotStage2D::AssemblingOnWorker)) {
                    // Priority 2: Wait for worker thread to free the next submission slot.
                    std::unique_lock<std::mutex> lk(asm_mu);
                    asm_done_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                        return asm_failed.load(std::memory_order_relaxed) ||
                               slots[static_cast<size_t>(next_sub_slot_idx)].stage.load(std::memory_order_acquire) ==
                                   static_cast<int>(PipeSlotStage2D::Idle);
                    });
                } else if (done_slot_active) {
                    // Priority 3: Block until D2H (or BandStreamed compute) completes on done_slot.
                    if (d2h_mode == CompressedD2HMode::FrameConsolidated) {
                        WaitGpuEvent(HyperCine_GpuSlotWaveletEvent(done_slot.w_slot));
                        WaitGpuEntropyFrameComplete(done_slot.e_slot);
                    } else {
                        WaitGpuEntropyComputeDone(done_slot.e_slot);
                    }
                }
                if (next_submit_frame > eff_warmup) {
                    main_thread_blocked_ms += NowMs() - t_blk0;
                    ++blocking_waits;
                }
            } else {
                // Polling mode: record non-productive spin time.
                if (next_submit_frame > eff_warmup) {
                    main_thread_spin_ms += NowMs() - t_iter0;
                }
            }
        }
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
    const double proc_cpu_sec = std::max(0.0, ProcessCpuSeconds() - proc_cpu_s0);
    const double main_cpu_sec = std::max(0.0, CurrentThreadCpuSeconds() - main_cpu_s0);
    const size_t vram_after = HyperCine_QueryUsedVramBytes();
    res.vram_used_bytes = vram_after > vram_before ? (vram_after - vram_before) : vram_after;

    const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    res.slot_vram_bytes = static_cast<size_t>(num_slots) * res.slot_selection.per_slot_vram_bytes;
    res.slot_pinned_host_bytes = static_cast<size_t>(num_slots) *
        ((px / 16u) * 2u + (px + 32u * 32768u) * 4u + (px / 2u + 131072u) * 4u);

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
    for (int f = eff_warmup; f < total_frames; ++f) {
        latencies.push_back(frame_complete_ms[static_cast<size_t>(f)] - frame_submit_ms[static_cast<size_t>(f)]);
        if (f > eff_warmup) {
            periods.push_back(frame_complete_ms[static_cast<size_t>(f)] - frame_complete_ms[static_cast<size_t>(f - 1)]);
        }
    }
    ComputeStats2D(latencies, &res.mean_latency_ms, &res.median_latency_ms, &res.p95_latency_ms,
                   &res.min_latency_ms, &res.max_latency_ms);
    if (!periods.empty()) {
        ComputeStats2D(periods, &res.mean_period_ms, &res.median_period_ms, &res.p95_period_ms);
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

    res.host_enqueue_ms = sum_enqueue_ms * inv_f;
    res.graph_capture_ms = max_graph_capture_ms;
    res.cpu_lowpass_ms = sum_cpu_lp_ms * inv_f;
    res.cpu_header_wrap_ms = sum_cpu_hdr_ms * inv_f;
    res.cpu_sample_concat_ms = sum_cpu_concat_ms * inv_f;
    res.cpu_assembly_total_ms = res.cpu_lowpass_ms + res.cpu_header_wrap_ms + res.cpu_sample_concat_ms;
    res.cpu_dispatch_poll_ms = (main_thread_running_ms + main_thread_spin_ms) * inv_f;
    res.total_cpu_ms = res.cpu_assembly_total_ms + res.cpu_dispatch_poll_ms;

    res.kernel_launches_per_frame = recorded_launches_per_frame;
    res.kernel_launches_per_band = (recorded_launches_per_frame >= 2) ? ((recorded_launches_per_frame - 2) / 27) : 0;
    res.host_launch_calls_per_frame = recorded_graph_used ? 1 : recorded_launches_per_frame;
    res.graph_kernel_nodes = recorded_graph_kernel_nodes;
    res.graph_total_nodes = recorded_graph_total_nodes;
    res.graph_used = recorded_graph_used;

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
    res.gpu_utilization_pct = std::min(100.0, 100.0 * gpu_compute_busy_ms / res.wall_ms);

    // Part 3: Normalized & Per-Thread CPU Utilization Metrics.
    const int logical_cpus = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    res.cpu.logical_processors = logical_cpus;
    res.cpu.active_host_threads = use_worker ? 2 : 1;
    res.cpu.wall_ms = res.wall_ms;
    res.cpu.process_cpu_ms = proc_cpu_sec * 1000.0;
    res.cpu.main_thread_cpu_ms = main_cpu_sec * 1000.0;
    res.cpu.worker_thread_cpu_ms = worker_cpu_sec_total * 1000.0;

    res.cpu.single_core_equiv_pct = 100.0 * res.cpu.process_cpu_ms / res.wall_ms;
    res.cpu.normalized_process_cpu_pct = std::min(100.0, res.cpu.single_core_equiv_pct / static_cast<double>(logical_cpus));
    res.cpu.main_thread_single_core_pct = 100.0 * res.cpu.main_thread_cpu_ms / res.wall_ms;
    res.cpu.worker_thread_single_core_pct = 100.0 * res.cpu.worker_thread_cpu_ms / res.wall_ms;

    res.cpu.main_thread_running_ms = main_thread_running_ms;
    res.cpu.main_thread_blocked_ms = main_thread_blocked_ms;
    res.cpu.main_thread_spin_ms = main_thread_spin_ms;
    res.cpu.poll_iterations = poll_iters;
    res.cpu.blocking_waits = blocking_waits;

    res.cpu.cpu_core_sec_per_wall_sec = proc_cpu_sec / (res.wall_ms / 1000.0);
    res.cpu.cpu_core_ms_per_frame = res.cpu.process_cpu_ms * inv_f;
    res.cpu.cpu_core_sec_per_60fps_video_sec = 60.0 * proc_cpu_sec * inv_f;

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
                res.bitstream_byte_match = (diffs <= 2);
            }
            std::vector<uint8_t> dec;
            int dec_pitch = 0;
            res.decode_ok = DecodeCfhdSample2D(res.last_bitstream, width, height, &dec, &dec_pitch);
            if (res.decode_ok) {
                res.psnr_db = ComputeBestPsnr2D(canonical_frame, dec, width, height, dec_pitch);
            }
            if (!res.bitstream_byte_match) {
                res.error = "bitstream mismatch (got_size=" + std::to_string(res.last_bitstream.size()) +
                            ", ref_size=" + std::to_string(ref_cpu_bitstream.size()) +
                            ", diffs=" + std::to_string(diffs) + ")";
            } else if (!res.decode_ok) {
                res.error = "DecodeCfhdSample2D failed";
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

std::vector<Phase2DKernelStudyPoint> BenchmarkPhase2DKernelEvolution(
    int width, int height, int iterations,
    Phase2DQuantStateMode quant_state,
    uint32_t seed) {
    (void)seed;
    std::vector<Phase2DKernelStudyPoint> points;
    if (iterations < 1) iterations = 1;

    const std::vector<uint8_t> frame = MakeFrame(width, height, CanonicalPattern::Canonical);
    Phase2DEncoderContext ctx;
    if (!ctx.Init(width, height, frame, quant_state, true)) return points;

    const QuantTables2D quant = ReadQuant2D(ctx.encoder, ctx.transforms, ctx.channel_count);
    const std::vector<BandRef> refs = EnumerateBands(reinterpret_cast<transform**>(ctx.transforms), ctx.channel_count);
    if (!quant.ok || refs.size() != 30) return points;

    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));
    for (size_t i = 0; i < refs.size(); ++i) {
        piece_of[refs[i].channel][refs[i].wavelet_index][refs[i].band] = static_cast<int>(i);
    }

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
    for (const BandRef& ref : refs) {
        if (ref.lowpass) continue;
        const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
        EntropyTables tbl;
        LoadEntropyTables(reinterpret_cast<encoder*>(ctx.encoder), ref.subband, &tbl);
        hp_reqs.push_back(GpuBandRequest{d_ptr, ref.width, ref.height, ref.width, ref.channel, ref.wavelet_index, ref.band, ref.subband});
        hp_tables.push_back(std::move(tbl));
    }

    const uint32_t base_frame_num = ctx.encoder->frame_number;

    auto assemble_current_slot = [&](std::vector<uint8_t>* out_sample) -> bool {
        std::vector<EncodedPiece> pieces(refs.size());
        int hp_idx = 0;
        for (size_t i = 0; i < refs.size(); ++i) {
            const BandRef& ref = refs[i];
            EncodedPiece& p = pieces[i];
            p.channel = ref.channel;
            p.level = ref.level;
            p.band = ref.band;
            p.subband = ref.subband;
            p.lowpass = ref.lowpass;
            if (ref.lowpass) {
                const int16_t* h_ptr = HyperCine_GpuSlotBandHostPtr(w_slot, ref.channel, ref.wavelet_index, ref.band);
                if (!BuildLowpassPayloadPtr(ref.subband, ref.level, ref.width, ref.height, h_ptr, false, &p.payload, &p.bits_free)) {
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
        return AssembleSampleFast2D(ctx.encoder, ctx.transforms, ctx.channel_count, pieces, piece_of, out_sample, nullptr);
    };

    struct Spec {
        const char* label;
        bool use_graph;
        GpuEntropyKernelMode kmode;
    };
    const Spec specs[] = {
        {"PHASE2C_REFERENCE_NO_GRAPH", false, GpuEntropyKernelMode::ReferencePhase2C},
        {"PHASE2D_A_GRAPH_REFERENCE", true, GpuEntropyKernelMode::ReferencePhase2C},
        {"PHASE2D_B_GRAPH_FUSED", true, GpuEntropyKernelMode::Fused},
        {"PHASE2D_C_GRAPH_FUSED_SCAN_OPT", true, GpuEntropyKernelMode::FusedScanOptimized},
    };

    std::vector<uint8_t> ref_sample;
    double ref_no_graph_ms = 0.0;

    for (const auto& sp : specs) {
        // 1. Substage timing pass (runs without graph so per-band events can be recorded).
        GpuEntropyFrameConfig prof_cfg;
        prof_cfg.entropy_streams = 4;
        prof_cfg.d2h_mode = CompressedD2HMode::FrameConsolidated;
        prof_cfg.record_substage_events = true;
        prof_cfg.lpt_schedule = true;
        prof_cfg.use_cuda_graph = false;
        prof_cfg.kernel_mode = sp.kmode;
        QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), prof_cfg);
        WaitGpuEntropyFrameComplete(e_slot);
        GpuFrameEntropyStats prof_st{};
        CollectGpuFrameEntropyStats(e_slot, &prof_st);

        // 2. Warmup pass in actual target graph/kernel configuration.
        GpuEntropyFrameConfig cfg;
        cfg.entropy_streams = 4;
        cfg.d2h_mode = CompressedD2HMode::FrameConsolidated;
        cfg.record_substage_events = false;
        cfg.lpt_schedule = true;
        cfg.use_cuda_graph = sp.use_graph;
        cfg.kernel_mode = sp.kmode;
        QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), cfg);
        WaitGpuEntropyFrameComplete(e_slot);
        GpuFrameEntropyStats warm_st{};
        CollectGpuFrameEntropyStats(e_slot, &warm_st);

        std::vector<uint8_t> cur_sample;
        assemble_current_slot(&cur_sample);
        if (ref_sample.empty()) ref_sample = cur_sample;

        // 3. Timed iterations.
        std::vector<double> wall_samples;
        double sum_ent_ms = 0.0, sum_cons_ms = 0.0, sum_d2h_ms = 0.0, sum_enq_ms = 0.0;
        for (int it = 0; it < iterations; ++it) {
            const double t0 = NowMs();
            QueueGpuEntropyFrame(session, e_slot, hp_reqs.data(), static_cast<int>(hp_reqs.size()), hp_tables.data(), cfg);
            WaitGpuEntropyFrameComplete(e_slot);
            wall_samples.push_back(NowMs() - t0);
            GpuFrameEntropyStats st{};
            CollectGpuFrameEntropyStats(e_slot, &st);
            sum_ent_ms += st.gpu_entropy_wall_ms;
            sum_cons_ms += st.gpu_consolidate_ms;
            sum_d2h_ms += st.compressed_d2h_ms;
            sum_enq_ms += st.host_enqueue_ms;
        }

        Phase2DKernelStudyPoint pt;
        pt.label = sp.label;
        pt.use_cuda_graph = sp.use_graph;
        pt.kernel_mode = sp.kmode;
        pt.streams = 4;
        pt.kernel_launches_per_frame = warm_st.total_kernel_launches;
        pt.kernel_launches_per_band = (warm_st.total_kernel_launches >= 2) ? ((warm_st.total_kernel_launches - 2) / 27) : 0;
        pt.host_launch_calls_per_frame = sp.use_graph ? 1 : warm_st.total_kernel_launches;
        pt.graph_kernel_nodes = warm_st.graph_kernel_nodes;
        pt.graph_total_nodes = warm_st.graph_total_nodes;
        pt.graph_capture_ms = warm_st.graph_capture_ms;
        pt.host_enqueue_ms = sum_enq_ms / iterations;
        ComputeStats2D(wall_samples, &pt.mean_wall_ms, &pt.median_wall_ms, &pt.p95_wall_ms);
        pt.gpu_entropy_ms = sum_ent_ms / iterations;
        pt.gpu_consolidate_ms = sum_cons_ms / iterations;
        pt.compressed_d2h_ms = sum_d2h_ms / iterations;
        pt.substage_sum = prof_st.substage_sum;
        pt.sample_bytes = cur_sample.size();
        pt.sample_sha256 = Sha256Hex(cur_sample.data(), cur_sample.size());
        pt.byte_match_reference = (cur_sample == ref_sample);

        if (!sp.use_graph && sp.kmode == GpuEntropyKernelMode::ReferencePhase2C) {
            ref_no_graph_ms = pt.gpu_entropy_ms;
        }
        points.push_back(pt);
    }

    if (ref_no_graph_ms > 0.0) {
        for (auto& pt : points) {
            pt.speedup_vs_ref_no_graph = pt.gpu_entropy_ms > 0.0 ? (ref_no_graph_ms / pt.gpu_entropy_ms) : 1.0;
        }
    }

    DestroyGpuEntropyFrameSlot(e_slot);
    DestroyGpuEntropySession(session);
    HyperCine_GpuSlotDestroy(w_slot);
    return points;
}

}  // namespace hypercine
