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

#include "hypercine/entropy_parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <numeric>

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

void EncodeQuantizedBand(ENCODER* encoder, BITSTREAM* stream, IMAGE* wavelet,
                         int band, int subband, int encoding, int quantization);
void EncodeQuantizedGroup(ENCODER* encoder, TRANSFORM* transform[], int num_transforms, BITSTREAM* output);
int ReverseByteOrder(int input);
int SetCodingFlags(ENCODER* encoder, int subband, int* active_codebook_ret, int* peaks_coding_ret);
}

namespace hypercine {
namespace {

constexpr int kLevels = 3;
constexpr int kHighpassOrder[] = {LH_BAND, HL_BAND, HH_BAND};

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
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

int ElementBytes(int pixel_type) {
    if (pixel_type == PIXEL_TYPE_8S || pixel_type == PIXEL_TYPE_8U) return 1;
    return static_cast<int>(sizeof(PIXEL));
}

struct Job {
    int channel = 0;
    int level = 0;
    int band = 0;
    int subband = 0;
    bool lowpass = false;
    int coeff_count = 0;
    std::vector<uint8_t> payload;
    double encode_ms = 0.0;
    int bits_free = 0;
    bool ok = false;
};

struct BandSnap {
    int channel;
    int level;
    int band;
    int width;
    int height;
    int pitch_bytes;
    std::vector<uint8_t> bytes;
};

std::vector<BandRef> EnumerateTransform(TRANSFORM* transforms[], int channel_count) {
    std::vector<BandRef> refs;
    for (int c = 0; c < channel_count; ++c) {
        TRANSFORM* tf = transforms[c];
        if (!tf || tf->num_wavelets <= 0) continue;
        const int top = tf->num_wavelets - 1;
        IMAGE* low = tf->wavelet[top];
        if (!low) continue;
        BandRef lowpass;
        lowpass.channel = c;
        lowpass.wavelet_index = top;
        lowpass.level = low->level;
        lowpass.band = LL_BAND;
        lowpass.subband = 0;
        lowpass.lowpass = true;
        lowpass.width = low->width;
        lowpass.height = low->height;
        refs.push_back(lowpass);

        int subband = 1;
        for (int k = top; k >= 0; --k) {
            IMAGE* wavelet = tf->wavelet[k];
            if (!wavelet) continue;
            const int high_count = std::max(0, wavelet->num_bands - 1);
            for (int i = 0; i < high_count && i < 3; ++i) {
                BandRef ref;
                ref.channel = c;
                ref.wavelet_index = k;
                ref.level = wavelet->level;
                ref.band = kHighpassOrder[i];
                ref.subband = subband++;
                ref.lowpass = false;
                ref.width = wavelet->width;
                ref.height = wavelet->height;
                refs.push_back(ref);
            }
        }
    }
    return refs;
}

std::vector<Job> BuildJobs(TRANSFORM* transforms[], int channel_count) {
    std::vector<Job> jobs;
    for (const BandRef& ref : EnumerateTransform(transforms, channel_count)) {
        Job job;
        job.channel = ref.channel;
        job.level = ref.level;
        job.band = ref.band;
        job.subband = ref.subband;
        job.lowpass = ref.lowpass;
        job.coeff_count = ref.width * ref.height;
        jobs.push_back(std::move(job));
    }
    return jobs;
}

void EncodeJob(ENCODER* encoder, TRANSFORM* transforms[], Job& job) {
    TRANSFORM* tf = transforms[job.channel];
    IMAGE* wavelet = nullptr;
    if (job.lowpass) {
        wavelet = tf->wavelet[tf->num_wavelets - 1];
    } else {
        for (int k = 0; k < tf->num_wavelets; ++k) {
            if (tf->wavelet[k] && tf->wavelet[k]->level == job.level) {
                wavelet = tf->wavelet[k];
                break;
            }
        }
    }
    if (!wavelet || !wavelet->band[job.band]) {
        job.ok = false;
        return;
    }

    const size_t cap = static_cast<size_t>(job.coeff_count) * 8u + 65536u;
    job.payload.assign(cap, 0);
    BITSTREAM stream;
    std::memset(&stream, 0, sizeof(stream));
    InitBitstreamBuffer(&stream, job.payload.data(), job.payload.size(), BITSTREAM_ACCESS_WRITE);

    const double t0 = NowMs();
    if (job.lowpass) {
        EncodeLowPassBand(encoder, &stream, wavelet, job.channel, job.subband);
    } else {
        int quantization = wavelet->quantization[job.band];
        if (quantization <= 0) quantization = wavelet->quant[job.band];
        if (quantization <= 0) quantization = 1;
        EncodeQuantizedBand(encoder, &stream, wavelet, job.band, job.subband,
                            BAND_ENCODING_RUNLENGTHS, quantization);
    }
    job.encode_ms = NowMs() - t0;
    job.bits_free = stream.nBitsFree;
    if (stream.error != 0 || stream.nWordsUsed < 0 ||
        static_cast<size_t>(stream.nWordsUsed) > job.payload.size()) {
        job.ok = false;
        return;
    }
    job.payload.resize(static_cast<size_t>(stream.nWordsUsed));
    job.ok = true;
}

std::vector<BandSnap> SnapshotBands(TRANSFORM* transforms[], int channel_count) {
    std::vector<BandSnap> snaps;
    for (int c = 0; c < channel_count; ++c) {
        TRANSFORM* tf = transforms[c];
        if (!tf) continue;
        for (int k = 0; k < tf->num_wavelets; ++k) {
            IMAGE* wavelet = tf->wavelet[k];
            if (!wavelet) continue;
            for (int b = 0; b < wavelet->num_bands && b < IMAGE_NUM_BANDS; ++b) {
                if (!wavelet->band[b]) continue;
                BandSnap snap;
                snap.channel = c;
                snap.level = wavelet->level;
                snap.band = b;
                snap.width = wavelet->width;
                snap.height = wavelet->height;
                snap.pitch_bytes = wavelet->pitch;
                const size_t bytes = static_cast<size_t>(wavelet->pitch) * static_cast<size_t>(wavelet->height);
                snap.bytes.resize(bytes);
                std::memcpy(snap.bytes.data(), wavelet->band[b], bytes);
                snaps.push_back(std::move(snap));
            }
        }
    }
    return snaps;
}

void RestoreBands(TRANSFORM* transforms[], const std::vector<BandSnap>& snaps) {
    for (const BandSnap& snap : snaps) {
        TRANSFORM* tf = transforms[snap.channel];
        if (!tf) continue;
        for (int k = 0; k < tf->num_wavelets; ++k) {
            IMAGE* wavelet = tf->wavelet[k];
            if (!wavelet || wavelet->level != snap.level) continue;
            if (!wavelet->band[snap.band]) continue;
            std::memcpy(wavelet->band[snap.band], snap.bytes.data(), snap.bytes.size());
        }
    }
}

void AppendBytes(BITSTREAM* output, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return;
    std::memcpy(output->lpCurrentWord, bytes.data(), bytes.size());
    output->nWordsUsed += static_cast<int>(bytes.size());
    output->lpCurrentWord += bytes.size();
}

bool Assemble(ENCODER* encoder, TRANSFORM* transforms[], int channel_count,
              const std::vector<Job>& jobs, std::vector<uint8_t>& out_bytes, double* assembly_ms) {
    const size_t cap = std::max<size_t>(out_bytes.capacity(), 1024u * 1024u);
    size_t need = 1024u * 1024u;
    for (const Job& job : jobs) need += job.payload.size() + 256u;
    if (out_bytes.size() < need) out_bytes.assign(need, 0);

    BITSTREAM output;
    std::memset(&output, 0, sizeof(output));
    InitBitstreamBuffer(&output, out_bytes.data(), out_bytes.size(), BITSTREAM_ACCESS_WRITE);

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

    if (!channel_size_vector) {
        return false;
    }

    for (int channel = 0; channel < num_channels; ++channel) {
        PadBits(&output);
        if (channel > 0) PutVideoChannelHeader(&output, channel);
        const int channel_begin = BitstreamSize(&output);

        for (const Job& job : jobs) {
            if (job.channel != channel || !job.lowpass) continue;
            AppendBytes(&output, job.payload);
        }

        TRANSFORM* tf = transforms[channel];
        for (int k = tf->num_wavelets - 1; k >= 0; --k) {
            IMAGE* wavelet = tf->wavelet[k];
            if (!wavelet) continue;
            const int divisor = 0;
            PutVideoHighPassHeader(&output, wavelet->wavelet_type, k + 1, wavelet->level,
                                   wavelet->width, wavelet->height, wavelet->num_bands,
                                   wavelet->scale[0], divisor);
            for (int i = 0; i < 3; ++i) {
                const int band = kHighpassOrder[i];
                for (const Job& job : jobs) {
                    if (job.channel == channel && !job.lowpass && job.level == wavelet->level && job.band == band) {
                        AppendBytes(&output, job.payload);
                    }
                }
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

    if (assembly_ms) *assembly_ms = NowMs() - t0;
    if (output.error != 0 || output.nWordsUsed <= 0) return false;
    out_bytes.resize(static_cast<size_t>(output.nWordsUsed));
    return true;
}

int CompareBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return static_cast<int>(i);
    }
    if (a.size() != b.size()) return static_cast<int>(n);
    return -1;
}

void FillBandInfo(TRANSFORM* transforms[], const Job& job, BandInfo* info) {
    TRANSFORM* tf = transforms[job.channel];
    IMAGE* wavelet = nullptr;
    for (int k = 0; k < tf->num_wavelets; ++k) {
        if (tf->wavelet[k] && tf->wavelet[k]->level == job.level) wavelet = tf->wavelet[k];
    }
    info->channel = job.channel;
    info->level = job.level;
    info->band = job.band;
    info->subband = job.subband;
    info->lowpass = job.lowpass;
    info->encode_ms = job.encode_ms;
    info->encoded_bytes = static_cast<int>(job.payload.size());
    info->name = std::string(ChannelTag(job.channel)) + std::to_string(job.level) + BandTag(job.band);
    if (!wavelet) return;
    info->width = wavelet->width;
    info->height = wavelet->height;
    info->pixel_type = wavelet->pixel_type[job.band];
    info->pitch_bytes = wavelet->pitch;
    info->coeff_count = wavelet->width * wavelet->height;
    info->element_bytes = ElementBytes(info->pixel_type);
    info->allocated_bytes = wavelet->pitch * wavelet->height;
    info->entropy_input_bytes = info->coeff_count * info->element_bytes;
    if (info->pixel_type == PIXEL_TYPE_8S) info->storage_type = "int8";
    else if (info->pixel_type == PIXEL_TYPE_16S) info->storage_type = "int16";
    else info->storage_type = "other";

    const PIXEL* row = wavelet->band[job.band];
    if (!row || info->pixel_type != PIXEL_TYPE_16S) return;
    const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
    int vmin = row[0];
    int vmax = row[0];
    for (int y = 0; y < wavelet->height; ++y) {
        const PIXEL* p = row + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < wavelet->width; ++x) {
            vmin = std::min(vmin, static_cast<int>(p[x]));
            vmax = std::max(vmax, static_cast<int>(p[x]));
        }
    }
    info->value_min = vmin;
    info->value_max = vmax;
    info->fits_in_8bit = (vmin >= -128 && vmax <= 127) ? 1 : 0;
}

}  // namespace

bool RefreshUpstreamPyramid(encoder* enc_opaque,
                            transform* transforms_opaque[],
                            int channel_count,
                            const uint8_t* bgra,
                            int pitch,
                            void* scratch,
                            size_t scratch_bytes) {
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || !encoder->frame || !bgra || !scratch) return false;

    ConvertRGB32to10bitYUVFrame(const_cast<uint8_t*>(bgra), pitch, encoder->frame,
                                static_cast<uint8_t*>(scratch), static_cast<int>(scratch_bytes),
                                encoder->input.color_space, encoder->codec.precision, 1, 0);

    for (int c = 0; c < channel_count; ++c) {
        IMAGE* image = encoder->frame->channel[c];
        IMAGE* wavelet = transforms[c] ? transforms[c]->wavelet[0] : nullptr;
        if (!image || !wavelet) return false;
        PIXEL* buffer = static_cast<PIXEL*>(scratch);
        TransformForwardSpatial(encoder->allocator, image, 0, wavelet, 1,
                                buffer, scratch_bytes, 0, wavelet->quant, 0);
    }
    ComputeGroupTransformQuant(encoder, transforms, channel_count);
    return true;
}

std::vector<BandInfo> DescribeBands(transform* transforms_opaque[], int channel_count) {
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    std::vector<Job> jobs = BuildJobs(transforms, channel_count);
    std::vector<BandInfo> infos;
    infos.reserve(jobs.size());
    for (const Job& job : jobs) {
        BandInfo info;
        FillBandInfo(transforms, job, &info);
        infos.push_back(std::move(info));
    }
    return infos;
}

ZeroRunStats ScanZeroRuns(transform* transforms_opaque[], int channel_count, size_t encoded_bytes) {
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    ZeroRunStats stats;
    std::vector<Job> jobs = BuildJobs(transforms, channel_count);
    for (const Job& job : jobs) {
        TRANSFORM* tf = transforms[job.channel];
        IMAGE* wavelet = nullptr;
        for (int k = 0; k < tf->num_wavelets; ++k) {
            if (tf->wavelet[k] && tf->wavelet[k]->level == job.level) wavelet = tf->wavelet[k];
        }
        if (!wavelet || wavelet->pixel_type[job.band] != PIXEL_TYPE_16S || !wavelet->band[job.band]) continue;
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        const PIXEL* base = wavelet->band[job.band];
        int count = 0;
        for (int y = 0; y < wavelet->height; ++y) {
            const PIXEL* row = base + static_cast<size_t>(y) * static_cast<size_t>(stride);
            for (int x = 0; x < wavelet->width; ++x) {
                ++stats.coeff_count;
                if (row[x] == 0) {
                    ++stats.zero_count;
                    ++count;
                } else {
                    ++stats.nonzero_count;
                    if (count > 0) {
                        ++stats.run_count;
                        int bucket = 7;
                        if (count <= 1) bucket = 0;
                        else if (count <= 2) bucket = 1;
                        else if (count <= 4) bucket = 2;
                        else if (count <= 8) bucket = 3;
                        else if (count <= 16) bucket = 4;
                        else if (count <= 32) bucket = 5;
                        else if (count <= 128) bucket = 6;
                        ++stats.run_hist[bucket];
                        count = 0;
                    }
                }
            }
        }
        if (count > 0) {
            ++stats.run_count;
            int bucket = 7;
            if (count <= 1) bucket = 0;
            else if (count <= 2) bucket = 1;
            else if (count <= 4) bucket = 2;
            else if (count <= 8) bucket = 3;
            else if (count <= 16) bucket = 4;
            else if (count <= 32) bucket = 5;
            else if (count <= 128) bucket = 6;
            ++stats.run_hist[bucket];
        }
    }
    if (stats.run_count > 0) stats.mean_run = static_cast<double>(stats.zero_count) / static_cast<double>(stats.run_count);
    if (stats.coeff_count > 0) {
        stats.encoded_bits_per_coeff = (static_cast<double>(encoded_bytes) * 8.0) / static_cast<double>(stats.coeff_count);
    }
    return stats;
}

EntropyOutput EncodeEntropy(encoder* enc_opaque,
                            transform* transforms_opaque[],
                            int channel_count,
                            int worker_count,
                            EntropySchedule schedule,
                            bool compare_serial) {
    EntropyOutput result;
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || channel_count <= 0) {
        result.error = "missing encoder";
        return result;
    }
    if (transforms[0]->type != TRANSFORM_TYPE_SPATIAL) {
        result.error = "entropy scheduler covers intra spatial pyramids";
        return result;
    }

    std::vector<Job> jobs = BuildJobs(transforms, channel_count);
    if (jobs.empty()) {
        result.error = "no subbands";
        return result;
    }
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) {
        return a.coeff_count > b.coeff_count;
    });

    std::vector<uint8_t> serial_bytes;
    std::vector<BandSnap> snaps;
    uint32_t saved_frame = encoder->frame_number;
    std::vector<uint8_t> global_meta;
    std::vector<uint8_t> local_meta;
    if (compare_serial) {
        if (encoder->metadata.global.block && encoder->metadata.global.size) {
            global_meta.assign(static_cast<uint8_t*>(encoder->metadata.global.block),
                               static_cast<uint8_t*>(encoder->metadata.global.block) + encoder->metadata.global.size);
        }
        if (encoder->metadata.local.block && encoder->metadata.local.size) {
            local_meta.assign(static_cast<uint8_t*>(encoder->metadata.local.block),
                              static_cast<uint8_t*>(encoder->metadata.local.block) + encoder->metadata.local.size);
        }
        snaps = SnapshotBands(transforms, channel_count);
        BITSTREAM serial;
        std::vector<uint8_t> buffer(std::max<size_t>(8u << 20, static_cast<size_t>(encoder->input.width) * encoder->input.height), 0);
        std::memset(&serial, 0, sizeof(serial));
        InitBitstreamBuffer(&serial, buffer.data(), buffer.size(), BITSTREAM_ACCESS_WRITE);
        EncodeQuantizedGroup(encoder, transforms, channel_count, &serial);
        if (serial.error != 0 || serial.nWordsUsed <= 0) {
            result.error = "serial EncodeQuantizedGroup failed";
            RestoreBands(transforms, snaps);
            encoder->frame_number = saved_frame;
            return result;
        }
        serial_bytes.assign(buffer.begin(), buffer.begin() + serial.nWordsUsed);
        RestoreBands(transforms, snaps);
        encoder->frame_number = saved_frame;
        if (!global_meta.empty() && encoder->metadata.global.block) {
            std::memcpy(encoder->metadata.global.block, global_meta.data(), global_meta.size());
            encoder->metadata.global.size = global_meta.size();
        }
        if (!local_meta.empty() && encoder->metadata.local.block) {
            std::memcpy(encoder->metadata.local.block, local_meta.data(), local_meta.size());
            encoder->metadata.local.size = local_meta.size();
        }
    }

    int workers = std::max(1, worker_count);
    if (schedule == EntropySchedule::Serial) workers = 1;
    if (schedule == EntropySchedule::ChannelParallel) workers = std::min(workers, channel_count);
    result.worker_count = workers;
    result.schedule = schedule;
    result.worker_busy_ms.assign(static_cast<size_t>(workers), 0.0);

    std::vector<std::vector<int>> assigned(static_cast<size_t>(workers));
    if (schedule == EntropySchedule::SubbandParallel) {
        // Dynamic claim. assigned stays empty and workers pull from `next`.
    } else {
        for (int i = 0; i < static_cast<int>(jobs.size()); ++i) {
            const int owner = (schedule == EntropySchedule::ChannelParallel)
                ? (jobs[static_cast<size_t>(i)].channel % workers)
                : 0;
            assigned[static_cast<size_t>(owner)].push_back(i);
        }
    }

    std::atomic<int> next{0};
    const double wall0 = NowMs();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers));
    for (int w = 0; w < workers; ++w) {
        pool.emplace_back([&, w]() {
            const double busy0 = NowMs();
            if (schedule == EntropySchedule::SubbandParallel) {
                for (;;) {
                    const int index = next.fetch_add(1);
                    if (index >= static_cast<int>(jobs.size())) break;
                    EncodeJob(encoder, transforms, jobs[static_cast<size_t>(index)]);
                }
            } else {
                for (int index : assigned[static_cast<size_t>(w)]) {
                    EncodeJob(encoder, transforms, jobs[static_cast<size_t>(index)]);
                }
            }
            result.worker_busy_ms[static_cast<size_t>(w)] = NowMs() - busy0;
        });
    }
    for (std::thread& thread : pool) thread.join();
    const double wall1 = NowMs();
    result.entropy_wall_ms = wall1 - wall0;

    bool jobs_ok = true;
    for (const Job& job : jobs) {
        if (!job.ok || job.bits_free != BITSTREAM_BUFFER_SIZE) jobs_ok = false;
        result.sum_band_ms += job.encode_ms;
    }
    result.max_worker_ms = *std::max_element(result.worker_busy_ms.begin(), result.worker_busy_ms.end());
    result.min_worker_ms = *std::min_element(result.worker_busy_ms.begin(), result.worker_busy_ms.end());
    result.workers_used = 0;
    for (double busy : result.worker_busy_ms) {
        if (busy > 0.01) ++result.workers_used;
    }
    result.scheduling_overhead_ms = std::max(0.0, result.entropy_wall_ms - result.max_worker_ms);

    // Restore canonical channel/level order for assembly. Payloads stay with the jobs.
    std::vector<Job> ordered = jobs;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Job& a, const Job& b) {
        if (a.channel != b.channel) return a.channel < b.channel;
        if (a.lowpass != b.lowpass) return a.lowpass > b.lowpass;
        if (a.level != b.level) return a.level > b.level;
        return a.band < b.band;
    });

    double assembly_ms = 0.0;
    std::vector<uint8_t> assembled;
    const bool assembled_ok = Assemble(encoder, transforms, channel_count, ordered, assembled, &assembly_ms);
    result.assembly_ms = assembly_ms;
    result.bitstream = std::move(assembled);
    result.bitstream_bytes = result.bitstream.size();
    result.success = jobs_ok && assembled_ok;
    if (!jobs_ok) result.error = "a subband encode did not finish byte-aligned";
    if (!assembled_ok) result.error = "bitstream assembly failed";

    result.bands.reserve(ordered.size());
    for (const Job& job : ordered) {
        BandInfo info;
        FillBandInfo(transforms, job, &info);
        info.encode_ms = job.encode_ms;
        info.encoded_bytes = static_cast<int>(job.payload.size());
        result.bands.push_back(std::move(info));
    }
    result.band_count = static_cast<int>(result.bands.size());

    if (compare_serial && result.success) {
        const int mismatch = CompareBytes(serial_bytes, result.bitstream);
        result.byte_match = mismatch < 0;
        result.mismatch_offset = mismatch;
        if (!result.byte_match && result.error.empty()) {
            result.error = "parallel assembly differs from EncodeQuantizedGroup";
        }
    }
    return result;
}

namespace {

// Same VLC index limit EncodeQuantLongRuns applies. This is the codebook,
// not a transport conversion: values outside the table are indexed the way
// the upstream routine indexes them, and int8 transport never reaches it.
template <typename Sample>
void EncodeQuantLongRunsOf(ENCODER* encoder, BITSTREAM* stream, const Sample* image,
                           int width, int height, int virtual_pitch_elems, int active_codebook) {
    RLCBOOK* runsbook = encoder->codebook_runbook[active_codebook];
    VALBOOK* valuebook = encoder->valuebook[active_codebook];
    const Sample* rowptr = image;
    int count = 0;
    const int gap = std::max(0, virtual_pitch_elems - width);

    for (int row = 0; row < height; ++row) {
        int index = 0;
        const int runsbook_length = runsbook->length;
        RLC* rlc = reinterpret_cast<RLC*>(reinterpret_cast<char*>(runsbook) + sizeof(RLCBOOK));
        VLE* table = reinterpret_cast<VLE*>(reinterpret_cast<char*>(valuebook) + sizeof(VALBOOK));
        uint32_t wBuffer = stream->wBuffer;
        int nBitsFree = stream->nBitsFree;
        const int nWordsPerLong = static_cast<int>(sizeof(uint32_t) / sizeof(uint8_t));
        uint32_t* lpCurrentWord = reinterpret_cast<uint32_t*>(stream->lpCurrentWord);
        int nWordsUsed = stream->nWordsUsed;

        while (index < width) {
            for (; index < width; ++index) {
                if (rowptr[index] == 0) ++count;
                else break;
            }
            if (index < width) {
                int value = static_cast<int>(rowptr[index]);
                if (count > 0) {
                    while (count > 0) {
                        const int indx = (count < runsbook_length) ? count : runsbook_length - 1;
                        int nBits = rlc[indx].size;
                        const int wBits = static_cast<int>(rlc[indx].bits);
                        if (nBits <= nBitsFree) {
                            wBuffer <<= nBits;
                            wBuffer |= static_cast<uint32_t>(wBits) & BITMASK(nBits);
                            nBitsFree -= nBits;
                        } else {
                            wBuffer <<= nBitsFree;
                            nBits -= nBitsFree;
                            wBuffer |= (static_cast<uint32_t>(wBits) >> nBits) & BITMASK(nBitsFree);
                            nWordsUsed += nWordsPerLong;
                            *(lpCurrentWord++) = SwapInt32NtoB(wBuffer);
                            wBuffer = static_cast<uint32_t>(wBits) & BITMASK(nBits);
                            nBitsFree = BITSTREAM_LONG_SIZE - nBits;
                        }
                        count -= rlc[indx].count;
                    }
                    count = 0;
                }

                int indx;
                if (value < 0) {
                    if (value <= -(VALUE_TABLE_LENGTH >> 1)) value = -((VALUE_TABLE_LENGTH >> 1) - 1);
                    indx = VALUE_TABLE_LENGTH + value;
                } else {
                    if (value >= (VALUE_TABLE_LENGTH >> 1)) value = ((VALUE_TABLE_LENGTH >> 1) - 1);
                    indx = value;
                }
                const uint32_t codeword = table[indx].entry & VLE_CODEWORD_MASK;
                const uint32_t codesize = table[indx].entry >> VLE_CODESIZE_SHIFT;
                int nBits = static_cast<int>(codesize);
                const int wBits = static_cast<int>(codeword);
                if (nBits <= nBitsFree) {
                    wBuffer <<= nBits;
                    wBuffer |= static_cast<uint32_t>(wBits) & BITMASK(nBits);
                    nBitsFree -= nBits;
                } else {
                    wBuffer <<= nBitsFree;
                    nBits -= nBitsFree;
                    wBuffer |= (static_cast<uint32_t>(wBits) >> nBits) & BITMASK(nBitsFree);
                    nWordsUsed += nWordsPerLong;
                    *(lpCurrentWord++) = SwapInt32NtoB(wBuffer);
                    wBuffer = static_cast<uint32_t>(wBits) & BITMASK(nBits);
                    nBitsFree = BITSTREAM_LONG_SIZE - nBits;
                }
                ++index;
            }
            if (index == width) count += gap;
        }

        stream->wBuffer = wBuffer;
        stream->nBitsFree = nBitsFree;
        stream->lpCurrentWord = reinterpret_cast<uint8_t*>(lpCurrentWord);
        stream->nWordsUsed = nWordsUsed;
        rowptr += width;
    }
    if (count > 0) PutZeroRun(stream, count, runsbook);
}

IMAGE* FindWavelet(TRANSFORM* tf, int level) {
    if (!tf) return nullptr;
    for (int k = 0; k < tf->num_wavelets; ++k) {
        if (tf->wavelet[k] && tf->wavelet[k]->level == level) return tf->wavelet[k];
    }
    return nullptr;
}

void CopyTightToPitch(const int16_t* src, int width, int height, PIXEL* dst, int dst_stride) {
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride),
                    src + static_cast<size_t>(y) * static_cast<size_t>(width),
                    static_cast<size_t>(width) * sizeof(PIXEL));
    }
}

bool EncodeHighpassView(ENCODER* encoder, IMAGE* wavelet, const ExternalCoeff& coeff, EncodedPiece* piece) {
    int active_codebook = 0;
    int peaks_coding = 0;
    const int codingflags = SetCodingFlags(encoder, coeff.subband, &active_codebook, &peaks_coding);
    const int scale = wavelet->scale[coeff.band];
    int quantization = wavelet->quantization[coeff.band];
    if (quantization <= 0) quantization = wavelet->quant[coeff.band];
    if (quantization <= 0) quantization = 1;

    const size_t cap = static_cast<size_t>(coeff.width) * static_cast<size_t>(coeff.height) * 8u + 65536u;
    piece->payload.assign(cap, 0);
    BITSTREAM stream;
    std::memset(&stream, 0, sizeof(stream));
    InitBitstreamBuffer(&stream, piece->payload.data(), piece->payload.size(), BITSTREAM_ACCESS_WRITE);

    const bool as_int8 = coeff.elem_bytes == 1 && !peaks_coding;
    piece->used_int8 = as_int8;
    piece->peaks_fallback = peaks_coding != 0;
    const int virtual_pitch_elems = std::max(coeff.width, wavelet->pitch / static_cast<int>(sizeof(PIXEL)));

    if (as_int8) {
        PutVideoBandHeader(&stream, coeff.band, coeff.width, coeff.height, coeff.subband,
                           BAND_ENCODING_RUNLENGTHS, quantization, scale, 0, nullptr, codingflags, peaks_coding);
        EncodeQuantLongRunsOf<int8_t>(encoder, &stream, static_cast<const int8_t*>(coeff.data),
                                      coeff.width, coeff.height, virtual_pitch_elems, active_codebook);
        FinishEncodeBand(&stream, encoder->band_end_code[active_codebook], encoder->band_end_size[active_codebook]);
        PutVideoBandTrailer(&stream);
    } else if (!peaks_coding && coeff.elem_bytes == 2) {
        PutVideoBandHeader(&stream, coeff.band, coeff.width, coeff.height, coeff.subband,
                           BAND_ENCODING_RUNLENGTHS, quantization, scale, 0, nullptr, codingflags, peaks_coding);
        EncodeQuantLongRunsOf<int16_t>(encoder, &stream, static_cast<const int16_t*>(coeff.data),
                                       coeff.width, coeff.height, virtual_pitch_elems, active_codebook);
        FinishEncodeBand(&stream, encoder->band_end_code[active_codebook], encoder->band_end_size[active_codebook]);
        PutVideoBandTrailer(&stream);
    } else {
        static std::mutex legacy_band;
        std::lock_guard<std::mutex> lock(legacy_band);
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        std::vector<PIXEL> owned(static_cast<size_t>(stride) * static_cast<size_t>(coeff.height));
        if (coeff.elem_bytes == 1) {
            const auto* src = static_cast<const int8_t*>(coeff.data);
            for (int i = 0; i < coeff.width * coeff.height; ++i) {
                const int value = static_cast<int>(src[i]);
                if (value < -128 || value > 127) return false;
                const int y = i / coeff.width;
                const int x = i - y * coeff.width;
                owned[static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)] =
                    static_cast<PIXEL>(value);
            }
        } else {
            CopyTightToPitch(static_cast<const int16_t*>(coeff.data), coeff.width, coeff.height, owned.data(), stride);
        }
        PIXEL* saved = wavelet->band[coeff.band];
        wavelet->band[coeff.band] = owned.data();
        EncodeQuantizedBand(encoder, &stream, wavelet, coeff.band, coeff.subband, BAND_ENCODING_RUNLENGTHS, quantization);
        wavelet->band[coeff.band] = saved;
    }

    if (stream.error != 0 || stream.nWordsUsed <= 0 ||
        static_cast<size_t>(stream.nWordsUsed) > piece->payload.size()) {
        return false;
    }
    piece->bits_free = stream.nBitsFree;
    piece->payload.resize(static_cast<size_t>(stream.nWordsUsed));
    return piece->bits_free == BITSTREAM_BUFFER_SIZE;
}

bool EncodeLowpassView(ENCODER* encoder, IMAGE* wavelet, const ExternalCoeff& coeff, EncodedPiece* piece) {
    if (coeff.elem_bytes != static_cast<int>(sizeof(PIXEL))) return false;
    const size_t cap = static_cast<size_t>(coeff.width) * static_cast<size_t>(coeff.height) * 4u + 65536u;
    piece->payload.assign(cap, 0);
    BITSTREAM stream;
    std::memset(&stream, 0, sizeof(stream));
    InitBitstreamBuffer(&stream, piece->payload.data(), piece->payload.size(), BITSTREAM_ACCESS_WRITE);

    static std::mutex legacy_lowpass;
    std::lock_guard<std::mutex> lock(legacy_lowpass);
    const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
    PIXEL* saved = wavelet->band[LL_BAND];
    std::vector<PIXEL> owned;
    if (stride == coeff.width) {
        wavelet->band[LL_BAND] = const_cast<PIXEL*>(static_cast<const PIXEL*>(coeff.data));
    } else {
        owned.resize(static_cast<size_t>(stride) * static_cast<size_t>(coeff.height));
        CopyTightToPitch(static_cast<const int16_t*>(coeff.data), coeff.width, coeff.height, owned.data(), stride);
        wavelet->band[LL_BAND] = owned.data();
    }
    EncodeLowPassBand(encoder, &stream, wavelet, coeff.channel, coeff.subband);
    wavelet->band[LL_BAND] = saved;

    if (stream.error != 0 || stream.nWordsUsed <= 0 ||
        static_cast<size_t>(stream.nWordsUsed) > piece->payload.size()) {
        return false;
    }
    piece->bits_free = stream.nBitsFree;
    piece->payload.resize(static_cast<size_t>(stream.nWordsUsed));
    piece->used_int8 = false;
    return piece->bits_free == BITSTREAM_BUFFER_SIZE;
}

}  // namespace

std::vector<BandRef> EnumerateBands(transform* transforms_opaque[], int channel_count) {
    return EnumerateTransform(reinterpret_cast<TRANSFORM**>(transforms_opaque), channel_count);
}

bool EncodeOneExternalBand(encoder* enc_opaque, transform* transforms_opaque[],
                           const ExternalCoeff& coeff, EncodedPiece* piece) {
    if (!piece) return false;
    *piece = EncodedPiece{};
    piece->channel = coeff.channel;
    piece->level = coeff.level;
    piece->band = coeff.band;
    piece->subband = coeff.subband;
    piece->lowpass = coeff.lowpass;
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || !coeff.data || coeff.width <= 0 || coeff.height <= 0) return false;
    if (coeff.elem_bytes == 1 && (coeff.value_min < -128 || coeff.value_max > 127 || coeff.lowpass)) return false;
    if (coeff.elem_bytes != 1 && coeff.elem_bytes != 2) return false;
    TRANSFORM* tf = transforms[coeff.channel];
    IMAGE* wavelet = FindWavelet(tf, coeff.level);
    if (!wavelet || coeff.band < 0 || coeff.band >= wavelet->num_bands) return false;
    if (wavelet->width != coeff.width || wavelet->height != coeff.height) return false;

    const double t0 = NowMs();
    const bool ok = coeff.lowpass ? EncodeLowpassView(encoder, wavelet, coeff, piece)
                                  : EncodeHighpassView(encoder, wavelet, coeff, piece);
    piece->encode_ms = NowMs() - t0;
    piece->ok = ok;
    return ok;
}

EntropyOutput AssembleEncodedPieces(encoder* enc_opaque, transform* transforms_opaque[],
                                    int channel_count, std::vector<EncodedPiece>& pieces,
                                    bool compare_serial) {
    EntropyOutput result;
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || pieces.empty()) {
        result.error = "missing pieces";
        return result;
    }
    std::vector<Job> jobs;
    jobs.reserve(pieces.size());
    bool jobs_ok = true;
    for (EncodedPiece& piece : pieces) {
        if (!piece.ok || piece.bits_free != BITSTREAM_BUFFER_SIZE) jobs_ok = false;
        Job job;
        job.channel = piece.channel;
        job.level = piece.level;
        job.band = piece.band;
        job.subband = piece.subband;
        job.lowpass = piece.lowpass;
        job.payload = piece.payload;
        job.encode_ms = piece.encode_ms;
        job.bits_free = piece.bits_free;
        job.ok = piece.ok;
        job.coeff_count = 0;
        result.sum_band_ms += piece.encode_ms;
        jobs.push_back(std::move(job));
    }

    std::vector<uint8_t> serial_bytes;
    uint32_t saved_frame = encoder->frame_number;
    std::vector<uint8_t> global_meta;
    std::vector<uint8_t> local_meta;
    std::vector<BandSnap> snaps;
    if (compare_serial) {
        if (encoder->metadata.global.block && encoder->metadata.global.size) {
            global_meta.assign(static_cast<uint8_t*>(encoder->metadata.global.block),
                               static_cast<uint8_t*>(encoder->metadata.global.block) + encoder->metadata.global.size);
        }
        if (encoder->metadata.local.block && encoder->metadata.local.size) {
            local_meta.assign(static_cast<uint8_t*>(encoder->metadata.local.block),
                              static_cast<uint8_t*>(encoder->metadata.local.block) + encoder->metadata.local.size);
        }
        snaps = SnapshotBands(transforms, channel_count);
        BITSTREAM serial;
        std::vector<uint8_t> buffer(std::max<size_t>(8u << 20, static_cast<size_t>(encoder->input.width) * encoder->input.height), 0);
        std::memset(&serial, 0, sizeof(serial));
        InitBitstreamBuffer(&serial, buffer.data(), buffer.size(), BITSTREAM_ACCESS_WRITE);
        EncodeQuantizedGroup(encoder, transforms, channel_count, &serial);
        if (serial.error != 0 || serial.nWordsUsed <= 0) {
            result.error = "serial EncodeQuantizedGroup failed";
            RestoreBands(transforms, snaps);
            encoder->frame_number = saved_frame;
            return result;
        }
        serial_bytes.assign(buffer.begin(), buffer.begin() + serial.nWordsUsed);
        RestoreBands(transforms, snaps);
        encoder->frame_number = saved_frame;
        if (!global_meta.empty() && encoder->metadata.global.block) {
            std::memcpy(encoder->metadata.global.block, global_meta.data(), global_meta.size());
            encoder->metadata.global.size = global_meta.size();
        }
        if (!local_meta.empty() && encoder->metadata.local.block) {
            std::memcpy(encoder->metadata.local.block, local_meta.data(), local_meta.size());
            encoder->metadata.local.size = local_meta.size();
        }
    }

    double assembly_ms = 0;
    std::vector<uint8_t> assembled;
    const bool assembled_ok = Assemble(encoder, transforms, channel_count, jobs, assembled, &assembly_ms);
    result.assembly_ms = assembly_ms;
    result.bitstream = std::move(assembled);
    result.bitstream_bytes = result.bitstream.size();
    result.success = jobs_ok && assembled_ok;
    result.band_count = static_cast<int>(pieces.size());
    if (!jobs_ok) result.error = "a subband encode did not finish byte-aligned";
    if (!assembled_ok && result.error.empty()) result.error = "bitstream assembly failed";
    if (compare_serial && result.success) {
        const int mismatch = CompareBytes(serial_bytes, result.bitstream);
        result.byte_match = mismatch < 0;
        result.mismatch_offset = mismatch;
        if (!result.byte_match) result.error = "external assembly differs from EncodeQuantizedGroup";
    }
    return result;
}

}  // namespace hypercine
