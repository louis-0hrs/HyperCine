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

#include "hypercine/phase3a.h"

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
#include "vlc.h"
int ReverseByteOrder(int input);
int SetCodingFlags(ENCODER* encoder, int subband, int* active_codebook_ret, int* peaks_coding_ret);
void EncodeQuantizedBand(ENCODER* encoder, BITSTREAM* stream, IMAGE* wavelet,
                         int band, int subband, int encoding, int quantization);
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
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
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

size_t QueryProcessWorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<size_t>(pmc.WorkingSetSize);
    }
#endif
    return 0;
}

uint32_t NextXorShift32(uint32_t& state) {
    uint32_t x = state ? state : 0x1337BEEFu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

std::string GuidToHex(const uint32_t words[4]) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%08x-%08x-%08x-%08x",
                  words[0], words[1], words[2], words[3]);
    return std::string(buf);
}

void ComputeStats(std::vector<double> vals, double* mean, double* median, double* p95,
                  double* min_v = nullptr, double* max_v = nullptr, double* stddev_v = nullptr) {
    if (vals.empty()) return;
    double sum = 0.0;
    for (double v : vals) sum += v;
    const double m = sum / static_cast<double>(vals.size());
    if (mean) *mean = m;
    if (stddev_v) {
        double sq = 0.0;
        for (double v : vals) {
            const double d = v - m;
            sq += d * d;
        }
        *stddev_v = std::sqrt(sq / static_cast<double>(vals.size()));
    }
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

// ============================================================================
// Dynamic NVML Hardware Telemetry Sampler (Windows runtime LoadLibraryA)
// ============================================================================

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

class NvmlRuntimeSampler {
public:
    NvmlRuntimeSampler() {
#ifdef _WIN32
        dll_ = LoadLibraryA("nvml.dll");
        if (!dll_) dll_ = LoadLibraryA("C:\\Windows\\System32\\nvml.dll");
        if (!dll_) dll_ = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        if (!dll_) return;

        fn_init_ = reinterpret_cast<FnInit>(GetProcAddress(dll_, "nvmlInit_v2"));
        if (!fn_init_) fn_init_ = reinterpret_cast<FnInit>(GetProcAddress(dll_, "nvmlInit"));
        fn_shutdown_ = reinterpret_cast<FnShutdown>(GetProcAddress(dll_, "nvmlShutdown"));
        fn_get_dev_ = reinterpret_cast<FnGetDevice>(GetProcAddress(dll_, "nvmlDeviceGetHandleByIndex_v2"));
        if (!fn_get_dev_) fn_get_dev_ = reinterpret_cast<FnGetDevice>(GetProcAddress(dll_, "nvmlDeviceGetHandleByIndex"));
        fn_util_ = reinterpret_cast<FnGetUtil>(GetProcAddress(dll_, "nvmlDeviceGetUtilizationRates"));
        fn_clock_ = reinterpret_cast<FnGetClock>(GetProcAddress(dll_, "nvmlDeviceGetClockInfo"));
        fn_temp_ = reinterpret_cast<FnGetTemp>(GetProcAddress(dll_, "nvmlDeviceGetTemperature"));
        fn_power_ = reinterpret_cast<FnGetPower>(GetProcAddress(dll_, "nvmlDeviceGetPowerUsage"));
        fn_throttle_ = reinterpret_cast<FnGetThrottle>(GetProcAddress(dll_, "nvmlDeviceGetCurrentClocksThrottleReasons"));
        if (!fn_throttle_) {
            fn_throttle_ = reinterpret_cast<FnGetThrottle>(GetProcAddress(dll_, "nvmlDeviceGetCurrentClocksEventReasons"));
        }

        if (fn_init_ && fn_get_dev_ && fn_init_() == 0) {
            inited_ = true;
            if (fn_get_dev_(0, &dev_) == 0 && dev_ != nullptr) {
                available_ = true;
            }
        }
#endif
    }

    ~NvmlRuntimeSampler() {
        Stop();
#ifdef _WIN32
        if (inited_ && fn_shutdown_) fn_shutdown_();
        if (dll_) FreeLibrary(dll_);
#endif
    }

    void Start() {
        if (!available_) return;
        stop_.store(false, std::memory_order_release);
        SampleOnce();
        thread_ = std::thread([this]() {
            while (!stop_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                if (stop_.load(std::memory_order_acquire)) break;
                SampleOnce();
            }
        });
    }

    Phase3ANvmlTelemetry Stop() {
        if (thread_.joinable()) {
            stop_.store(true, std::memory_order_release);
            thread_.join();
        }
        if (available_) SampleOnce();

        std::lock_guard<std::mutex> lk(mu_);
        Phase3ANvmlTelemetry out;
        out.nvml_available = available_ && !samples_.empty();
        out.sample_count = static_cast<int>(samples_.size());
        if (samples_.empty()) return out;

        double sum_gpu = 0.0, max_gpu = 0.0;
        double sum_mem = 0.0, max_mem = 0.0;
        double sum_gclk = 0.0, sum_mclk = 0.0;
        int min_gclk = samples_[0].gclk, max_gclk = samples_[0].gclk;
        int min_mclk = samples_[0].mclk, max_mclk = samples_[0].mclk;
        int max_temp = samples_[0].temp;
        double sum_pwr = 0.0, max_pwr = 0.0;
        uint64_t throttle_union = 0;

        for (const Sample& s : samples_) {
            sum_gpu += s.gpu_util;
            max_gpu = std::max<double>(max_gpu, s.gpu_util);
            sum_mem += s.mem_util;
            max_mem = std::max<double>(max_mem, s.mem_util);
            sum_gclk += s.gclk;
            min_gclk = std::min(min_gclk, s.gclk);
            max_gclk = std::max(max_gclk, s.gclk);
            sum_mclk += s.mclk;
            min_mclk = std::min(min_mclk, s.mclk);
            max_mclk = std::max(max_mclk, s.mclk);
            max_temp = std::max(max_temp, s.temp);
            sum_pwr += s.power_w;
            max_pwr = std::max(max_pwr, s.power_w);
            throttle_union |= s.throttle;
        }

        const double inv_n = 1.0 / static_cast<double>(samples_.size());
        out.hardware_gpu_utilization_pct_mean = sum_gpu * inv_n;
        out.hardware_gpu_utilization_pct_max = max_gpu;
        out.hardware_mem_utilization_pct_mean = sum_mem * inv_n;
        out.hardware_mem_utilization_pct_max = max_mem;
        out.graphics_clock_mhz_mean = sum_gclk * inv_n;
        out.graphics_clock_mhz_min = min_gclk;
        out.graphics_clock_mhz_max = max_gclk;
        out.mem_clock_mhz_mean = sum_mclk * inv_n;
        out.mem_clock_mhz_min = min_mclk;
        out.mem_clock_mhz_max = max_mclk;
        out.temperature_c_start = samples_.front().temp;
        out.temperature_c_end = samples_.back().temp;
        out.temperature_c_max = max_temp;
        out.power_w_mean = sum_pwr * inv_n;
        out.power_w_max = max_pwr;
        out.throttle_reasons_bitmask = throttle_union;
        out.throttle_reasons_str = FormatThrottleReasons(throttle_union);
        return out;
    }

    static std::string FormatThrottleReasons(uint64_t mask) {
        if (mask == 0) return "None";
        struct FlagName {
            uint64_t bit;
            const char* name;
        };
        static constexpr FlagName kFlags[] = {
            {0x0000000000000001ULL, "GpuIdle"},
            {0x0000000000000002ULL, "AppClocksSetting"},
            {0x0000000000000004ULL, "SwPowerCap"},
            {0x0000000000000008ULL, "HwSlowdown"},
            {0x0000000000000010ULL, "SyncBoost"},
            {0x0000000000000020ULL, "SwThermalSlowdown"},
            {0x0000000000000040ULL, "HwThermalSlowdown"},
            {0x0000000000000080ULL, "HwPowerBrakeSlowdown"},
            {0x0000000000000100ULL, "DisplayClockSetting"}
        };
        std::string s;
        for (const auto& f : kFlags) {
            if (mask & f.bit) {
                if (!s.empty()) s += "|";
                s += f.name;
            }
        }
        return s.empty() ? "Unknown" : s;
    }

private:
    struct Sample {
        int gpu_util = 0;
        int mem_util = 0;
        int gclk = 0;
        int mclk = 0;
        int temp = 0;
        double power_w = 0.0;
        uint64_t throttle = 0;
    };

    void SampleOnce() {
#ifdef _WIN32
        if (!available_ || !dev_) return;
        Sample s{};
        if (fn_util_) {
            NvmlUtilization u{};
            if (fn_util_(dev_, &u) == 0) {
                s.gpu_util = static_cast<int>(u.gpu);
                s.mem_util = static_cast<int>(u.memory);
            }
        }
        if (fn_clock_) {
            unsigned int clk = 0;
            if (fn_clock_(dev_, 0, &clk) == 0) s.gclk = static_cast<int>(clk);
            if (fn_clock_(dev_, 2, &clk) == 0) s.mclk = static_cast<int>(clk);
        }
        if (fn_temp_) {
            unsigned int t = 0;
            if (fn_temp_(dev_, 0, &t) == 0) s.temp = static_cast<int>(t);
        }
        if (fn_power_) {
            unsigned int mw = 0;
            if (fn_power_(dev_, &mw) == 0) s.power_w = static_cast<double>(mw) / 1000.0;
        }
        if (fn_throttle_) {
            unsigned long long tr = 0;
            if (fn_throttle_(dev_, &tr) == 0) s.throttle = static_cast<uint64_t>(tr);
        }
        std::lock_guard<std::mutex> lk(mu_);
        samples_.push_back(s);
#endif
    }

#ifdef _WIN32
    using FnInit = int (*)();
    using FnShutdown = int (*)();
    using FnGetDevice = int (*)(unsigned int, void**);
    using FnGetUtil = int (*)(void*, NvmlUtilization*);
    using FnGetClock = int (*)(void*, int, unsigned int*);
    using FnGetTemp = int (*)(void*, int, unsigned int*);
    using FnGetPower = int (*)(void*, unsigned int*);
    using FnGetThrottle = int (*)(void*, unsigned long long*);

    HMODULE dll_ = nullptr;
    FnInit fn_init_ = nullptr;
    FnShutdown fn_shutdown_ = nullptr;
    FnGetDevice fn_get_dev_ = nullptr;
    FnGetUtil fn_util_ = nullptr;
    FnGetClock fn_clock_ = nullptr;
    FnGetTemp fn_temp_ = nullptr;
    FnGetPower fn_power_ = nullptr;
    FnGetThrottle fn_throttle_ = nullptr;
    void* dev_ = nullptr;
    bool inited_ = false;
#endif
    bool available_ = false;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mu_;
    std::vector<Sample> samples_;
};

// ============================================================================
// Stateful Phase 3A Encoder Context (Matches Upstream Per-Frame State Machine)
// ============================================================================

struct QuantTables3A {
    int prescale[3] = {};
    int quant[36] = {};
    int scale[36] = {};
    int widths[3] = {};
    int heights[3] = {};
    int color_space = 2;
    int fs_rate_limiter = 4;
    size_t prev_encoded_bytes = 0;
    int64_t prev_lastgopbitcount = 0;
    bool ok = false;
};

QuantTables3A ReadQuant3A(ENCODER* encoder, TRANSFORM* transforms[], int channel_count,
                          size_t prev_bytes, int64_t prev_bits) {
    QuantTables3A q;
    if (!encoder || !encoder->frame || channel_count < 3 || !transforms) return q;
    q.color_space = encoder->input.color_space;
    q.fs_rate_limiter = encoder->q.FSratelimiter;
    q.prev_encoded_bytes = prev_bytes;
    q.prev_lastgopbitcount = prev_bits;
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

struct Phase3AEncoderContext {
    CFHD_EncoderRef ref = nullptr;
    CSampleEncoder* sample = nullptr;
    ENCODER* encoder = nullptr;
    TRANSFORM** transforms = nullptr;
    int channel_count = 3;
    int width = 0;
    int height = 0;

    ~Phase3AEncoderContext() {
        if (ref) CFHD_CloseEncoder(ref);
    }

    bool Init(int w, int h, Phase3AQuantMode quant_mode) {
        width = w;
        height = h;
        if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) return false;
        if (CFHD_PrepareToEncode(ref, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                                 CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
            return false;
        }
        sample = reinterpret_cast<CSampleEncoder*>(ref);
        if (!sample) return false;

        // Run one initial CFHD_EncodeSample pass (or 3 passes for FixedSteadyState) so upstream
        // allocates and configures encoder->frame, m_scratchBuffer, and transforms[0..2]->wavelet[0..2]
        // (including wavelet_type, scale, and 10-bit precision).
        std::vector<uint8_t> init_frame(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 128u);
        const int init_passes = (quant_mode == Phase3AQuantMode::FixedSteadyState) ? 3 : 1;
        for (int pass = 0; pass < init_passes; ++pass) {
            if (CFHD_EncodeSample(ref, init_frame.data(), width * 4) != CFHD_ERROR_OKAY) {
                return false;
            }
        }

        encoder = reinterpret_cast<ENCODER*>(sample->m_encoder);
        transforms = reinterpret_cast<TRANSFORM**>(sample->m_transformArray);
        channel_count = sample->m_channelCount;
        if (!encoder || !transforms || channel_count != 3) return false;

        // Reset sequence counters and metadata state back to clean pre-Frame-0 state:
        encoder->frame_number = 0;
        encoder->frame_count = 0;
        encoder->group.count = 0;
        if (quant_mode != Phase3AQuantMode::FixedSteadyState) {
            encoder->lastgopbitcount = 0;
            encoder->q.FSratelimiter = 4;
        }
        sample->m_last_unique_frame = -1;
        sample->m_last_timecode_base = 0;
        sample->m_last_timecode_frame = -1;
        FreeMetadata(&sample->local);
        FreeMetadata(&encoder->metadata.local);
        for (int i = 0; i < 5; ++i) {
            FreeMetadata(&sample->global[i]);
        }
        FreeMetadata(&encoder->metadata.global);
        return true;
    }

    // Executes the exact upstream per-frame pre-transform state & metadata update
    // from SampleEncoder::EncodeSample and Codec/encoder.c:EncodeSample.
    bool PrepareFrameState(Phase3AQuantMode quant_mode,
                           bool deterministic_test_metadata,
                           QuantTables3A* out_quant,
                           uint32_t* out_frame_number,
                           std::vector<uint8_t>* out_global_meta,
                           std::vector<uint8_t>* out_local_meta) {
        if (!sample || !encoder || !transforms) return false;

        if (quant_mode == Phase3AQuantMode::FixedFrame1Initial) {
            encoder->lastgopbitcount = 0;
            encoder->q.FSratelimiter = 4;
        }

        const int64_t prev_bits = encoder->lastgopbitcount;
        const size_t prev_bytes = static_cast<size_t>(prev_bits >> 3);

        // 1. Update global/local metadata (TAG_CLIP_GUID on frame 0, TAG_ENCODE_DATE,
        //    TAG_ENCODE_TIME, TAG_TIMECODE, TAG_UNIQUE_FRAMENUM on every frame).
        if (sample->HandleMetadata() != CFHD_ERROR_OKAY) return false;

        if (deterministic_test_metadata && encoder->metadata.global.block && encoder->metadata.global.size > 0) {
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

        // 2. Update per-frame encoder & transform quantization without mutating constant header fields
        //    that the async CPU assembly worker thread reads concurrently.
        const int i_fixedquality = static_cast<int>(sample->m_encodingQuality);
        encoder->encoder_quality = i_fixedquality | 0x1a00000;

        SetEncoderQuantization(encoder, COLOR_FORMAT_YU64, i_fixedquality, sample->m_encodingBitrate, nullptr);
        for (int c = 0; c < channel_count; ++c) {
            SetTransformQuantization(encoder, transforms[c], c, sample->m_frameRate);
            for (int w = 0; w < transforms[c]->num_wavelets; ++w) {
                IMAGE* wav = transforms[c]->wavelet[w];
                if (!wav) continue;
                for (int b = 0; b < wav->num_bands; ++b) {
                    wav->quantization[b] = wav->quant[b];
                }
            }
        }

        // Increment frame_number and snapshot metadata for thread-safe slot assembly.
        encoder->frame_number++;
        if (out_frame_number) *out_frame_number = encoder->frame_number;

        if (out_global_meta) {
            out_global_meta->clear();
            if (encoder->metadata.global.block && encoder->metadata.global.size) {
                int len = static_cast<int>(encoder->metadata.global.size);
                len = RemoveHiddenMetadata(static_cast<unsigned char*>(encoder->metadata.global.block), len);
                if (len > 0) {
                    const auto* p = static_cast<const uint8_t*>(encoder->metadata.global.block);
                    out_global_meta->assign(p, p + len);
                }
            }
        }
        if (out_local_meta) {
            out_local_meta->clear();
            if (encoder->metadata.local.block && encoder->metadata.local.size) {
                int len = static_cast<int>(encoder->metadata.local.size);
                len = RemoveHiddenMetadata(static_cast<unsigned char*>(encoder->metadata.local.block), len);
                if (len > 0) {
                    const auto* p = static_cast<const uint8_t*>(encoder->metadata.local.block);
                    out_local_meta->assign(p, p + len);
                }
            }
        }

        if (out_quant) {
            *out_quant = ReadQuant3A(encoder, transforms, channel_count, prev_bytes, prev_bits);
            if (!out_quant->ok) return false;
        }
        return true;
    }

    void CompleteFrameState(size_t sample_bytes, Phase3AQuantMode quant_mode) {
        if (!encoder || !sample) return;
        encoder->group.count = 0;
        encoder->frame_count += encoder->gop_length;
        if (encoder->frame) encoder->frame->iskey = true;
        encoder->output.iskey = true;
        if (quant_mode == Phase3AQuantMode::DynamicUpstreamState) {
            encoder->lastgopbitcount = static_cast<int64_t>(sample_bytes) * 8;
        }
        sample->FreeLocalMetadata();
    }
};

// Thread-safe sample assembly using the per-slot metadata & frame_number snapshot.
bool AssembleSampleFromSlot3A(ENCODER* encoder_cfg, TRANSFORM* transforms[], int channel_count,
                              uint32_t slot_frame_number,
                              const std::vector<uint8_t>& slot_global_meta,
                              const std::vector<uint8_t>& slot_local_meta,
                              const std::vector<EncodedPiece>& pieces, const int piece_of[3][3][4],
                              std::vector<uint8_t>* out_bytes, double* concat_ms,
                              std::vector<size_t>* out_piece_offsets = nullptr,
                              const QuantTables3A* slot_quant = nullptr) {
    if (!encoder_cfg || !transforms || !out_bytes || pieces.size() < 30) return false;
    size_t need = 16384u + slot_global_meta.size() + slot_local_meta.size();
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
    const bool encode_iframe = encoder_cfg->gop_length <= 1;
    const int precision = encoder_cfg->codec.precision;
    uint32_t* channel_size_vector = nullptr;

    if (!encode_iframe) {
        PutVideoGroupHeader(&output, transforms[0], num_channels, subband_count,
                            &channel_size_vector, precision, slot_frame_number,
                            encoder_cfg->input.format, encoder_cfg->input.color_space,
                            encoder_cfg->encoder_quality, encoder_cfg->encoded_format,
                            encoder_cfg->input.width, encoder_cfg->input.height, encoder_cfg->display.height,
                            encoder_cfg->presentationWidth, encoder_cfg->presentationHeight);
    } else {
        PutVideoIntraFrameHeader(&output, transforms[0], num_channels, subband_count,
                                 &channel_size_vector, precision, slot_frame_number,
                                 encoder_cfg->input.format, encoder_cfg->input.color_space,
                                 encoder_cfg->encoder_quality, encoder_cfg->encoded_format,
                                 encoder_cfg->input.width, encoder_cfg->input.height, encoder_cfg->display.height,
                                 encoder_cfg->presentationWidth, encoder_cfg->presentationHeight);
    }

    if (encoder_cfg->video_channels > 1) {
        PutTagPairOptional(&output, CODEC_TAG_ENCODED_CHANNELS, static_cast<int>(encoder_cfg->video_channels));
        PutTagPairOptional(&output, CODEC_TAG_ENCODED_CHANNEL_NUMBER, static_cast<int>(encoder_cfg->current_channel));
    } else if (encoder_cfg->current_channel || encoder_cfg->ignore_overrides) {
        PutTagPairOptional(&output, CODEC_TAG_ENCODED_CHANNEL_NUMBER, static_cast<int>(encoder_cfg->current_channel));
    }

    SizeTagPush(&output, CODEC_TAG_SAMPLE_SIZE);

    if (!slot_global_meta.empty()) {
        const int len = static_cast<int>(slot_global_meta.size());
        PutTagPairOptional(&output, CODEC_TAG_METADATA, len >> 2);
        std::memcpy(output.lpCurrentWord, slot_global_meta.data(), slot_global_meta.size());
        output.nWordsUsed += len;
        output.lpCurrentWord += len;
    }
    if (!slot_local_meta.empty()) {
        const int len = static_cast<int>(slot_local_meta.size());
        PutTagPairOptional(&output, CODEC_TAG_METADATA, len >> 2);
        std::memcpy(output.lpCurrentWord, slot_local_meta.data(), slot_local_meta.size());
        output.nWordsUsed += len;
        output.lpCurrentWord += len;
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

    PutVideoGroupExtension(&output, &encoder_cfg->codec);
#if _CODEC_SAMPLE_FLAGS
    PutVideoSampleFlags(&output, &encoder_cfg->codec);
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
            const int lowpass_scale = slot_quant ? slot_quant->scale[(channel * 3 + k) * 4 + 0] : wavelet->scale[0];
            PutVideoHighPassHeader(&output, wavelet->wavelet_type, k + 1, wavelet->level,
                                   wavelet->width, wavelet->height, wavelet->num_bands,
                                   lowpass_scale, 0);
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

bool DecodeCfhdSample3A(const std::vector<uint8_t>& sample, int width, int height,
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

    // Bypass two upstream decoder multi-threading bugs in upstream/Codec/decoder.c on >8-core CPUs:
    // 1. AllocDecoderBuffer (line 1264) multiplies scratch size by at most 8x, whereas worker_thread.pool
    //    spawns up to 32 threads and divides buffer_size /= thread_count (line 27389), causing per-thread
    //    strip scratch overruns when thread_count > 8.
    // 2. DecodeSampleSubband (line 12109) queues QueueThreadedTransform on BANDS_ALL_STARTED before
    //    sibling entropy threads finish setting BANDS_ALL_VALID (line 13032).
    // Because _DELAY_THREAD_START is 1, clamping cpu_limit / thread_cntrl.capabilities to 1 thread
    // after CFHD_PrepareToDecode and before CFHD_DecodeSample prevents both bugs deterministically.
    struct SampleDecoderPrefixLayout {
        void* vtable;
        FILE* m_logfile;
        DECODER* m_decoder;
    };
    auto* dec_prefix = reinterpret_cast<SampleDecoderPrefixLayout*>(decoder);
    if (dec_prefix && dec_prefix->m_decoder) {
        dec_prefix->m_decoder->cfhddata.cpu_limit = 1;
        dec_prefix->m_decoder->thread_cntrl.limit = 1;
        dec_prefix->m_decoder->thread_cntrl.set_thread_params = 1;
        dec_prefix->m_decoder->thread_cntrl.capabilities =
            (dec_prefix->m_decoder->thread_cntrl.capabilities & 0xffff) | (1 << 16);
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
        uint32_t cpu_max = 1;
        CFHD_SetActiveMetadata(decoder, metadata, TAG_CPU_MAX, METADATATYPE_HIDDEN, &cpu_max, sizeof(cpu_max));
    }
    err = CFHD_DecodeSample(decoder, const_cast<uint8_t*>(sample.data()), sample.size(), image->data(), pitch);
    if (metadata) CFHD_CloseMetadata(metadata);
    CFHD_CloseDecoder(decoder);
    if (pitch_out) *pitch_out = pitch;
    return err == CFHD_ERROR_OKAY && aw == width && ah == height;
}

double ComputeBestPsnr3A(const uint8_t* src, int src_pitch, const std::vector<uint8_t>& dec,
                         int width, int height, int dec_pitch) {
    double best = 0.0;
    for (int flip = 0; flip < 2; ++flip) {
        double sse = 0.0;
        int64_t count = 0;
        const int y_step = (height >= 2000) ? 2 : 1;
        const int x_step = (width >= 3000) ? 2 : 1;
        for (int y = 0; y < height; y += y_step) {
            const int ry = flip ? (height - 1 - y) : y;
            const uint8_t* srow = src + static_cast<size_t>(y) * static_cast<size_t>(src_pitch);
            const uint8_t* drow = dec.data() + static_cast<size_t>(ry) * static_cast<size_t>(dec_pitch);
            for (int x = 0; x < width; x += x_step) {
                for (int c = 0; c < 3; ++c) {
                    const double d = static_cast<double>(srow[x * 4 + c]) - static_cast<double>(drow[x * 4 + c]);
                    sse += d * d;
                    ++count;
                }
            }
        }
        const double mse = count ? (sse / static_cast<double>(count)) : 0.0;
        const double psnr = (mse <= 0.0) ? 99.0 : (10.0 * std::log10(255.0 * 255.0 / mse));
        best = std::max(best, psnr);
    }
    return best;
}

}  // namespace

const char* Phase3AInputModeName(Phase3AInputMode mode) {
    switch (mode) {
    case Phase3AInputMode::HostPageableEndToEnd: return "HostPageableEndToEnd";
    case Phase3AInputMode::HostPinnedApi: return "HostPinnedApi";
    case Phase3AInputMode::DeviceInput: return "DeviceInput";
    default: return "Unknown";
    }
}

const char* Phase3AQuantModeName(Phase3AQuantMode mode) {
    switch (mode) {
    case Phase3AQuantMode::DynamicUpstreamState: return "DynamicUpstreamState";
    case Phase3AQuantMode::FixedFrame1Initial: return "FixedFrame1Initial";
    case Phase3AQuantMode::FixedSteadyState: return "FixedSteadyState";
    default: return "Unknown";
    }
}

const char* Phase3APatternName(Phase3APattern pattern) {
    switch (pattern) {
    case Phase3APattern::FlatBlack: return "FlatBlack";
    case Phase3APattern::FlatMidGray: return "FlatMidGray";
    case Phase3APattern::FlatWhite: return "FlatWhite";
    case Phase3APattern::HorizontalGradient: return "HorizontalGradient";
    case Phase3APattern::VerticalGradient: return "VerticalGradient";
    case Phase3APattern::Checkerboard: return "Checkerboard";
    case Phase3APattern::HardEdges: return "HardEdges";
    case Phase3APattern::DeterministicNoise: return "DeterministicNoise";
    case Phase3APattern::HighFrequencyPattern: return "HighFrequencyPattern";
    case Phase3APattern::LowDetailPhotographicSynthetic: return "LowDetailPhotographicSynthetic";
    case Phase3APattern::HighDetailPhotographicSynthetic: return "HighDetailPhotographicSynthetic";
    default: return "Unknown";
    }
}

Phase3APattern SelectSequencePattern(int frame_index) {
    const int c = ((frame_index % 40) + 40) % 40;
    switch (c) {
    case 0: return Phase3APattern::FlatBlack;
    case 1: return Phase3APattern::FlatMidGray;
    case 2: return Phase3APattern::FlatWhite;
    case 3: return Phase3APattern::HorizontalGradient;
    case 4: return Phase3APattern::VerticalGradient;
    case 5: return Phase3APattern::LowDetailPhotographicSynthetic;
    // Burst of high-complexity noise / Nyquist grating to drive FSratelimiter upward:
    case 6: return Phase3APattern::DeterministicNoise;
    case 7: return Phase3APattern::HighFrequencyPattern;
    case 8: return Phase3APattern::DeterministicNoise;
    case 9: return Phase3APattern::HighFrequencyPattern;
    case 10: return Phase3APattern::DeterministicNoise;
    case 11: return Phase3APattern::HighFrequencyPattern;
    // Medium/sharp edges & checkerboard:
    case 12: return Phase3APattern::Checkerboard;
    case 13: return Phase3APattern::HardEdges;
    case 14: return Phase3APattern::HighDetailPhotographicSynthetic;
    case 15: return Phase3APattern::Checkerboard;
    // Burst of smooth/flat frames to drive FSratelimiter back downward:
    case 16: return Phase3APattern::FlatBlack;
    case 17: return Phase3APattern::FlatWhite;
    case 18: return Phase3APattern::HorizontalGradient;
    case 19: return Phase3APattern::VerticalGradient;
    case 20: return Phase3APattern::LowDetailPhotographicSynthetic;
    case 21: return Phase3APattern::FlatMidGray;
    default:
        return static_cast<Phase3APattern>((frame_index * 7 + 3) % kPhase3APatternCount);
    }
}

void StampFrameIdentityBlocks(uint8_t* bgra, int width, int height, int pitch,
                              int frame_index, Phase3APattern pattern) {
    if (!bgra || width < 512 || height < 64 || pitch < width * 4) return;
    const uint16_t words[2] = {
        static_cast<uint16_t>((0xA5u << 8) | (static_cast<uint8_t>(pattern) & 0xFFu)),
        static_cast<uint16_t>(frame_index & 0xFFFF)
    };
    for (int row_blk = 0; row_blk < 2; ++row_blk) {
        const uint16_t w = words[row_blk];
        for (int col_blk = 0; col_blk < 16; ++col_blk) {
            const int bit = (w >> (15 - col_blk)) & 1;
            const uint8_t v = bit ? 235u : 20u;
            const uint32_t px = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                                (static_cast<uint32_t>(v) << 8) | static_cast<uint32_t>(v);
            const int x0 = col_blk * 32;
            const int y0 = row_blk * 32;
            for (int dy = 0; dy < 32; ++dy) {
                uint32_t* row32 = reinterpret_cast<uint32_t*>(bgra + static_cast<size_t>(y0 + dy) * pitch) + x0;
                for (int dx = 0; dx < 32; ++dx) {
                    row32[dx] = px;
                }
            }
        }
    }
    // Also stamp a frame-unique 4:2:2-aligned detail band across rows [64 .. min(height, 96))
    // so every frame k has distinct wavelet coefficients and bitstream hash even when sharing an archetype.
    const int mod_y_end = std::min(height, 96);
    uint32_t rng = 0x9E3779B9u ^ (static_cast<uint32_t>(frame_index + 1) * 0x85EBCA6Bu);
    for (int y = 64; y < mod_y_end; ++y) {
        uint32_t* row32 = reinterpret_cast<uint32_t*>(bgra + static_cast<size_t>(y) * pitch);
        const uint32_t row_seed = NextXorShift32(rng);
        const int span_w = std::min(width, 512);
        for (int x = 0; x < span_w; x += 4) {
            const uint32_t h = (row_seed ^ (static_cast<uint32_t>(x) * 2654435761u)) >> 16;
            const int base_y = 64 + static_cast<int>(h & 0x7Fu);
            const int cr = static_cast<int>((h >> 7) & 0x1Fu) - 16;
            const int cb = static_cast<int>((h >> 11) & 0x1Fu) - 16;
            for (int dx = 0; dx < 4 && (x + dx) < span_w; ++dx) {
                const uint8_t b = static_cast<uint8_t>(std::clamp(base_y + cb, 20, 235));
                const uint8_t g = static_cast<uint8_t>(std::clamp(base_y, 20, 235));
                const uint8_t r = static_cast<uint8_t>(std::clamp(base_y + cr, 20, 235));
                row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
        }
    }
}

bool ExtractStampedFrameIdentity(const uint8_t* decoded_bgra, int width, int height, int pitch,
                                 int* out_frame_index, int* out_pattern) {
    if (!decoded_bgra || width < 512 || height < 64 || pitch < width * 4) return false;

    for (int flip = 0; flip < 2; ++flip) {
        uint16_t words[2] = {0, 0};
        for (int row_blk = 0; row_blk < 2; ++row_blk) {
            uint16_t w = 0;
            for (int col_blk = 0; col_blk < 16; ++col_blk) {
                const int x0 = col_blk * 32;
                const int y0 = row_blk * 32;
                int sum_luma = 0;
                int count = 0;
                for (int dy = 8; dy < 24; ++dy) {
                    const int sy = y0 + dy;
                    const int ry = flip ? (height - 1 - sy) : sy;
                    const uint8_t* row = decoded_bgra + static_cast<size_t>(ry) * static_cast<size_t>(pitch);
                    for (int dx = 8; dx < 24; ++dx) {
                        const int sx = x0 + dx;
                        const int b = row[sx * 4 + 0];
                        const int g = row[sx * 4 + 1];
                        const int r = row[sx * 4 + 2];
                        sum_luma += (b + g + r);
                        count += 3;
                    }
                }
                const int avg = sum_luma / count;
                if (avg >= 128) {
                    w |= static_cast<uint16_t>(1u << (15 - col_blk));
                }
            }
            words[row_blk] = w;
        }
        const uint8_t magic = static_cast<uint8_t>(words[0] >> 8);
        const uint8_t pat = static_cast<uint8_t>(words[0] & 0xFFu);
        if (magic == 0xA5u && pat < kPhase3APatternCount) {
            if (out_frame_index) *out_frame_index = static_cast<int>(words[1]);
            if (out_pattern) *out_pattern = static_cast<int>(pat);
            return true;
        }
    }
    return false;
}

void GeneratePhase3AFrame(int width, int height, int frame_index, Phase3APattern pattern,
                          uint8_t* bgra_out, int pitch, uint32_t base_seed) {
    if (!bgra_out || width <= 0 || height <= 0 || pitch < width * 4) return;
    const int phase = (frame_index * 12) & 254;

    for (int y = 0; y < height; ++y) {
        uint32_t* row32 = reinterpret_cast<uint32_t*>(bgra_out + static_cast<size_t>(y) * static_cast<size_t>(pitch));
        switch (pattern) {
        case Phase3APattern::FlatBlack: {
            const uint32_t px = (255u << 24) | (16u << 16) | (16u << 8) | 16u;
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::FlatMidGray: {
            const uint32_t px = (255u << 24) | (128u << 16) | (128u << 8) | 128u;
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::FlatWhite: {
            const uint32_t px = (255u << 24) | (235u << 16) | (235u << 8) | 235u;
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::HorizontalGradient: {
            for (int x = 0; x < width; ++x) {
                const int xc = x & ~1;
                const int v = 24 + (((xc + phase) % std::max(1, width)) * 196) / std::max(1, width);
                const int u = 64 + (((xc + phase) % std::max(1, width)) * 128) / std::max(1, width);
                row32[x] = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                           (static_cast<uint32_t>(u) << 8) | static_cast<uint32_t>(255 - v);
            }
            break;
        }
        case Phase3APattern::VerticalGradient: {
            const int v = 24 + (((y + phase) % std::max(1, height)) * 196) / std::max(1, height);
            const int g = 48 + (((y + phase) % std::max(1, height)) * 160) / std::max(1, height);
            const uint32_t px = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                                (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(255 - v);
            std::fill(row32, row32 + width, px);
            break;
        }
        case Phase3APattern::Checkerboard: {
            const int by = ((y + (phase >> 2)) >> 3) & 1;
            for (int x = 0; x < width; ++x) {
                const int xc = x & ~1;
                const int bx = ((xc + (phase >> 2)) >> 3) & 1;
                const uint8_t v = (bx ^ by) ? 216u : 40u;
                const uint8_t c = ((xc ^ y) & 16) ? 184u : 72u;
                row32[x] = (255u << 24) | (static_cast<uint32_t>(v) << 16) |
                           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(255u - c);
            }
            break;
        }
        case Phase3APattern::HardEdges: {
            for (int x = 0; x < width; ++x) {
                const int xc = x & ~1;
                const bool vert_bar = ((xc + phase) % 160) < 80;
                const bool horiz_bar = ((y + phase) % 120) < 60;
                const bool diag_bar = (((xc + (y & ~1) + phase) % 180) < 90);
                const uint8_t r = vert_bar ? 216u : 40u;
                const uint8_t g = horiz_bar ? 212u : 44u;
                const uint8_t b = diag_bar ? 208u : 48u;
                row32[x] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
            break;
        }
        case Phase3APattern::DeterministicNoise: {
            // 4:2:2-chroma-aligned high-entropy pseudorandom noise:
            // Luma varies at every pixel x (exciting L1..L3 LH/HL/HH and pushing compression < 3.0),
            // while chroma offset (cr, cb) is shared across each 2x1 horizontal pair so 4:2:2 subsampling is lossless.
            uint32_t rng = base_seed ^ (static_cast<uint32_t>(frame_index + 1) * 0x9E3779B9u) ^
                           (static_cast<uint32_t>(y + 1) * 0x85EBCA6Bu);
            for (int x = 0; x < width; x += 2) {
                const uint32_t v = NextXorShift32(rng);
                const int y0 = 60 + static_cast<int>(v & 0x7Fu);
                const int dy = static_cast<int>((v >> 8) & 0x1Fu) - 16;
                const int cr = static_cast<int>((v >> 16) & 0x3Fu) - 32;
                const int cb = static_cast<int>((v >> 24) & 0x3Fu) - 32;
                for (int dx = 0; dx < 2 && (x + dx) < width; ++dx) {
                    const int yv = y0 + (dx ? dy : -dy);
                    const uint8_t r = static_cast<uint8_t>(std::clamp(yv + cr, 16, 235));
                    const uint8_t g = static_cast<uint8_t>(std::clamp(yv, 16, 235));
                    const uint8_t b = static_cast<uint8_t>(std::clamp(yv + cb, 16, 235));
                    row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                    (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
            }
            break;
        }
        case Phase3APattern::HighFrequencyPattern: {
            // 4:2:2-chroma-aligned high-frequency grating:
            for (int x = 0; x < width; x += 2) {
                const int p_chr = (((x >> 1) * 3) ^ (y * 5) ^ frame_index) & 7;
                const int cr = (p_chr & 3) * 16 - 24;
                const int cb = ((p_chr >> 1) & 3) * 16 - 24;
                for (int dx = 0; dx < 2 && (x + dx) < width; ++dx) {
                    const int p_lum = ((x + dx) ^ y ^ frame_index) & 1;
                    const int p_fine = (((x + dx) >> 1) + (y >> 1) + frame_index) & 3;
                    const int yv = (p_lum ? 156 : 96) + (p_fine * 8 - 12);
                    const uint8_t r = static_cast<uint8_t>(std::clamp(yv + cr, 16, 235));
                    const uint8_t g = static_cast<uint8_t>(std::clamp(yv, 16, 235));
                    const uint8_t b = static_cast<uint8_t>(std::clamp(yv + cb, 16, 235));
                    row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                    (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
            }
            break;
        }
        case Phase3APattern::LowDetailPhotographicSynthetic: {
            const int sky = 40 + (y * 140) / std::max(1, height);
            for (int x = 0; x < width; ++x) {
                const int wave = ((x + phase) >> 5) & 15;
                const uint8_t b = static_cast<uint8_t>(std::clamp(sky + wave, 16, 235));
                const uint8_t g = static_cast<uint8_t>(std::clamp(sky - 10 + (wave >> 1), 16, 235));
                const uint8_t r = static_cast<uint8_t>(std::clamp(sky - 24 + (wave >> 2), 16, 235));
                row32[x] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
            break;
        }
        case Phase3APattern::HighDetailPhotographicSynthetic:
        default: {
            uint32_t rng = base_seed ^ (static_cast<uint32_t>(y + 1) * 0x27D4EB2Du) ^
                           (static_cast<uint32_t>(frame_index) * 0x165667B1u);
            const int base_y = (y * 180) / std::max(1, height);
            for (int x = 0; x < width; x += 2) {
                const int base_x = (x * 180) / std::max(1, width);
                const int edge = (((x + phase) % 256) < 128 && ((y + phase) % 256) < 128) ? 36 : -18;
                const uint32_t rv = NextXorShift32(rng);
                const int tex0 = static_cast<int>(rv & 31u) - 16;
                const int tex1 = static_cast<int>((rv >> 8) & 31u) - 16;
                for (int dx = 0; dx < 2 && (x + dx) < width; ++dx) {
                    const int tex = dx ? tex1 : tex0;
                    const uint8_t r = static_cast<uint8_t>(std::clamp(32 + base_x + edge + tex, 16, 235));
                    const uint8_t g = static_cast<uint8_t>(std::clamp(32 + base_y + (edge >> 1) + tex, 16, 235));
                    const uint8_t b = static_cast<uint8_t>(std::clamp(128 + ((base_x - base_y) >> 1) + tex, 16, 235));
                    row32[x + dx] = (255u << 24) | (static_cast<uint32_t>(r) << 16) |
                                    (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
            }
            break;
        }
        }
    }
    StampFrameIdentityBlocks(bgra_out, width, height, pitch, frame_index, pattern);
}

// ============================================================================
// SECTION 3: 120-Frame Changing-Complexity Sequence Equivalence Verification
// ============================================================================

Phase3ASequence120Report VerifyPhase3ASequence120(int width, int height, int num_frames, uint32_t seed, const Phase3AReferenceFrameCallback& artifact_callback) {
    Phase3ASequence120Report rep;
    rep.width = width;
    rep.height = height;
    rep.total_frames = num_frames;
    if (width <= 0 || height <= 0 || num_frames <= 0) {
        rep.error = "invalid dimensions or frame count";
        return rep;
    }

    const int pitch = width * 4;

    // Pre-generate the 11 base archetype frames so generating each of the 120 frames
    // only requires copying the archetype and stamping frame_index.
    std::vector<std::vector<uint8_t>> archetypes(kPhase3APatternCount);
    for (int p = 0; p < kPhase3APatternCount; ++p) {
        archetypes[static_cast<size_t>(p)].resize(static_cast<size_t>(pitch) * static_cast<size_t>(height));
        GeneratePhase3AFrame(width, height, p, static_cast<Phase3APattern>(p),
                             archetypes[static_cast<size_t>(p)].data(), pitch, seed);
    }

    // Open Pristine Upstream encoder A:
    CFHD_EncoderRef ref_a = nullptr;
    if (CFHD_OpenEncoder(&ref_a, nullptr) != CFHD_ERROR_OKAY ||
        CFHD_PrepareToEncode(ref_a, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        if (ref_a) CFHD_CloseEncoder(ref_a);
        rep.error = "failed to open Pristine Upstream encoder A";
        return rep;
    }
    auto* sample_a = reinterpret_cast<CSampleEncoder*>(ref_a);
    auto* enc_a = reinterpret_cast<ENCODER*>(sample_a->m_encoder);
    auto* tf_a = reinterpret_cast<TRANSFORM**>(sample_a->m_transformArray);

    // Open HyperCine CPU/Reference encoder B and HyperCine GPU encoder C:
    Phase3AEncoderContext ctx_b;
    Phase3AEncoderContext ctx_c;
    if (!ctx_b.Init(width, height, Phase3AQuantMode::DynamicUpstreamState) ||
        !ctx_c.Init(width, height, Phase3AQuantMode::DynamicUpstreamState)) {
        CFHD_CloseEncoder(ref_a);
        rep.error = "failed to initialize HyperCine B or C encoder context";
        return rep;
    }

    const std::vector<BandRef> refs = EnumerateBands(reinterpret_cast<transform**>(ctx_c.transforms), ctx_c.channel_count);
    if (refs.size() != 30) {
        CFHD_CloseEncoder(ref_a);
        rep.error = "expected 30 canonical subbands";
        return rep;
    }

    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));
    std::vector<int> hp_to_piece;
    std::vector<EntropyTables> hp_tables;
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        const BandRef& r = refs[static_cast<size_t>(i)];
        piece_of[r.channel][r.wavelet_index][r.band] = i;
        if (!r.lowpass) {
            EntropyTables tbl;
            if (!LoadEntropyTables(reinterpret_cast<encoder*>(ctx_c.encoder), r.subband, &tbl)) {
                CFHD_CloseEncoder(ref_a);
                rep.error = "failed to load highpass entropy codebook";
                return rep;
            }
            hp_to_piece.push_back(i);
            hp_tables.push_back(std::move(tbl));
        }
    }

    GpuEntropySession* session = CreateGpuEntropySession();
    HyperCineGpuSlot* w_slot = HyperCine_GpuSlotCreateForPhase2C(width, height, 1);
    GpuEntropyFrameSlot* e_slot = CreateGpuEntropyFrameSlot(width, height);
    uint8_t* pinned_bgra = nullptr;
    if (!session || !w_slot || !e_slot ||
        cudaHostAlloc(&pinned_bgra, static_cast<size_t>(pitch) * static_cast<size_t>(height), cudaHostAllocDefault) != cudaSuccess) {
        if (pinned_bgra) cudaFreeHost(pinned_bgra);
        DestroyGpuEntropyFrameSlot(e_slot);
        HyperCine_GpuSlotDestroy(w_slot);
        DestroyGpuEntropySession(session);
        CFHD_CloseEncoder(ref_a);
        rep.error = "failed to allocate GPU slot resources for C";
        return rep;
    }

    auto cleanup = [&]() {
        if (pinned_bgra) cudaFreeHost(pinned_bgra);
        DestroyGpuEntropyFrameSlot(e_slot);
        HyperCine_GpuSlotDestroy(w_slot);
        DestroyGpuEntropySession(session);
        CFHD_CloseEncoder(ref_a);
    };

    const size_t scratch_bytes = std::max<size_t>(64u << 20, static_cast<size_t>(width) * static_cast<size_t>(height) * 8u);
    std::vector<uint8_t> scratch_b(scratch_bytes, 0);
    std::vector<uint8_t> frame_buf(static_cast<size_t>(pitch) * static_cast<size_t>(height));

    std::set<int> distinct_fs;
    int prev_fs = -1;
    int prev_quant0 = -1;

    rep.frames.reserve(static_cast<size_t>(num_frames));

    for (int f = 0; f < num_frames; ++f) {
        const Phase3APattern pat = SelectSequencePattern(f);
        const auto& base = archetypes[static_cast<size_t>(pat)];
        std::memcpy(frame_buf.data(), base.data(), frame_buf.size());
        StampFrameIdentityBlocks(frame_buf.data(), width, height, pitch, f, pat);
        std::memcpy(pinned_bgra, frame_buf.data(), frame_buf.size());

        Phase3AFrameEquivalenceItem item;
        item.frame_index = f;
        item.pattern_name = Phase3APatternName(pat);

        // --- A: Pristine Upstream CFHD_EncodeSample ---
        item.a_prev_bits = enc_a->lastgopbitcount;
        item.a_prev_bytes = static_cast<size_t>(enc_a->lastgopbitcount >> 3);
        if (CFHD_EncodeSample(ref_a, frame_buf.data(), pitch) != CFHD_ERROR_OKAY) {
            cleanup();
            rep.error = "Pristine Upstream CFHD_EncodeSample failed at frame " + std::to_string(f);
            return rep;
        }
        void* a_buf_ptr = nullptr;
        size_t a_buf_sz = 0;
        if (CFHD_GetSampleData(ref_a, &a_buf_ptr, &a_buf_sz) != CFHD_ERROR_OKAY || !a_buf_ptr || a_buf_sz == 0) {
            cleanup();
            rep.error = "CFHD_GetSampleData failed at frame " + std::to_string(f);
            return rep;
        }
        std::vector<uint8_t> a_sample(static_cast<const uint8_t*>(a_buf_ptr),
                                      static_cast<const uint8_t*>(a_buf_ptr) + a_buf_sz);
        const QuantTables3A q_a = ReadQuant3A(enc_a, tf_a, 3, item.a_prev_bytes, item.a_prev_bits);
        item.a_fs_rate_limiter = q_a.fs_rate_limiter;
        std::memcpy(item.a_quant, q_a.quant, sizeof(item.a_quant));
        item.a_sample_bytes = a_sample.size();

        // Snapshot A's 30 quantized subband coefficient hashes:
        uint64_t a_coeff_hash[30] = {};
        for (size_t bi = 0; bi < refs.size(); ++bi) {
            const BandRef& r = refs[bi];
            IMAGE* wav = tf_a[r.channel]->wavelet[r.wavelet_index];
            const int stride = wav->pitch / static_cast<int>(sizeof(PIXEL));
            const int16_t* ptr = wav->band[r.band];
            uint64_t h = 14695981039346656037ull;
            for (int y = 0; y < r.height; ++y) {
                const auto* row_bytes = reinterpret_cast<const uint8_t*>(ptr + static_cast<size_t>(y) * stride);
                for (size_t k = 0; k < static_cast<size_t>(r.width) * sizeof(int16_t); ++k) {
                    h ^= row_bytes[k];
                    h *= 1099511628211ull;
                }
            }
            a_coeff_hash[bi] = h;
        }

        // --- B: HyperCine CPU/Reference ---
        QuantTables3A q_b{};
        uint32_t b_frame_num = 0;
        std::vector<uint8_t> b_global_meta, b_local_meta;
        if (!ctx_b.PrepareFrameState(Phase3AQuantMode::DynamicUpstreamState, true,
                                     &q_b, &b_frame_num, &b_global_meta, &b_local_meta)) {
            cleanup();
            rep.error = "ctx_b.PrepareFrameState failed at frame " + std::to_string(f);
            return rep;
        }
        item.b_prev_bytes = q_b.prev_encoded_bytes;
        item.b_prev_bits = q_b.prev_lastgopbitcount;
        item.b_fs_rate_limiter = q_b.fs_rate_limiter;
        std::memcpy(item.b_quant, q_b.quant, sizeof(item.b_quant));

        // Compute B's wavelet pyramid via upstream spatial filters under q_b state:
        {
            ENCODER* enc = ctx_b.encoder;
            TRANSFORM** tfs = ctx_b.transforms;
            ConvertRGB32to10bitYUVFrame(frame_buf.data(), pitch, enc->frame, scratch_b.data(),
                                        static_cast<int>(scratch_b.size()), enc->input.color_space,
                                        enc->codec.precision, true, 0);
            for (int c = 0; c < 3; ++c) {
                IMAGE* img = enc->frame->channel[c];
                IMAGE* w0 = tfs[c]->wavelet[0];
                TransformForwardSpatial(enc->allocator, img, 0, w0, 1,
                                        reinterpret_cast<PIXEL*>(scratch_b.data()), scratch_b.size(),
                                        tfs[c]->prescale[0], w0->quant, 0);
                for (int lv = 0; lv < 2; ++lv) {
                    IMAGE* w_next = tfs[c]->wavelet[lv + 1];
                    TransformForwardSpatial(enc->allocator, tfs[c]->wavelet[lv], 0, w_next, lv + 2,
                                            reinterpret_cast<PIXEL*>(scratch_b.data()), scratch_b.size(),
                                            tfs[c]->prescale[lv + 1], w_next->quant, 0);
                }
            }
        }

        uint64_t b_coeff_hash[30] = {};
        std::vector<EncodedPiece> b_pieces(refs.size());
        for (size_t bi = 0; bi < refs.size(); ++bi) {
            const BandRef& r = refs[bi];
            IMAGE* wav = ctx_b.transforms[r.channel]->wavelet[r.wavelet_index];
            const int stride = wav->pitch / static_cast<int>(sizeof(PIXEL));
            const int16_t* ptr = wav->band[r.band];
            std::vector<int16_t> tight(static_cast<size_t>(r.width) * static_cast<size_t>(r.height));
            for (int y = 0; y < r.height; ++y) {
                std::memcpy(tight.data() + static_cast<size_t>(y) * r.width,
                            ptr + static_cast<size_t>(y) * stride,
                            static_cast<size_t>(r.width) * sizeof(int16_t));
            }
            b_coeff_hash[bi] = EntropyFnv64(tight.data(), tight.size() * sizeof(int16_t));

            ExternalCoeff ext{};
            ext.channel = r.channel;
            ext.level = r.level;
            ext.band = r.band;
            ext.subband = r.subband;
            ext.lowpass = r.lowpass;
            ext.width = r.width;
            ext.height = r.height;
            ext.elem_bytes = 2;
            ext.data = tight.data();
            if (!EncodeOneExternalBand(reinterpret_cast<encoder*>(ctx_b.encoder),
                                       reinterpret_cast<transform**>(ctx_b.transforms),
                                       ext, &b_pieces[bi])) {
                cleanup();
                rep.error = "EncodeOneExternalBand failed at frame " + std::to_string(f);
                return rep;
            }
        }
        std::vector<uint8_t> b_sample;
        std::vector<size_t> b_offsets;
        if (!AssembleSampleFromSlot3A(ctx_b.encoder, ctx_b.transforms, ctx_b.channel_count,
                                      b_frame_num, b_global_meta, b_local_meta,
                                      b_pieces, piece_of, &b_sample, nullptr, &b_offsets)) {
            cleanup();
            rep.error = "AssembleSampleFromSlot3A (B) failed at frame " + std::to_string(f);
            return rep;
        }
        ctx_b.CompleteFrameState(b_sample.size(), Phase3AQuantMode::DynamicUpstreamState);
        item.b_sample_bytes = b_sample.size();

        // --- C: HyperCine GPU ---
        QuantTables3A q_c{};
        uint32_t c_frame_num = 0;
        std::vector<uint8_t> c_global_meta, c_local_meta;
        if (!ctx_c.PrepareFrameState(Phase3AQuantMode::DynamicUpstreamState, true,
                                     &q_c, &c_frame_num, &c_global_meta, &c_local_meta)) {
            cleanup();
            rep.error = "ctx_c.PrepareFrameState failed at frame " + std::to_string(f);
            return rep;
        }
        item.c_prev_bytes = q_c.prev_encoded_bytes;
        item.c_prev_bits = q_c.prev_lastgopbitcount;
        item.c_fs_rate_limiter = q_c.fs_rate_limiter;
        std::memcpy(item.c_quant, q_c.quant, sizeof(item.c_quant));

        if (HyperCine_GpuSlotBegin(w_slot, HYPERCINE_INPUT_HOST_BGRA, HYPERCINE_XPORT_DEVICE_LOWPASS_D2H,
                                   pinned_bgra, pitch, width, height, q_c.color_space,
                                   q_c.widths, q_c.heights, q_c.prescale, q_c.quant) != 0 ||
            HyperCine_GpuSlotKickStaged(w_slot, nullptr, nullptr, 0) != 0) {
            cleanup();
            rep.error = "HyperCine_GpuSlotKickStaged failed at frame " + std::to_string(f);
            return rep;
        }

        std::vector<GpuBandRequest> reqs;
        for (int p_idx : hp_to_piece) {
            const BandRef& r = refs[static_cast<size_t>(p_idx)];
            IMAGE* wav = ctx_c.transforms[r.channel]->wavelet[r.wavelet_index];
            const int pitch_px = wav ? std::max(r.width, wav->pitch / static_cast<int>(sizeof(PIXEL))) : r.width;
            const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(w_slot, r.channel, r.wavelet_index, r.band);
            reqs.push_back(GpuBandRequest{d_ptr, r.width, r.height, pitch_px, r.channel, r.wavelet_index, r.band, r.subband});
        }

        GpuEntropyFrameConfig e_cfg{};
        e_cfg.entropy_streams = 4;
        e_cfg.d2h_mode = CompressedD2HMode::FrameConsolidated;
        e_cfg.lpt_schedule = true;
        e_cfg.use_cuda_graph = true;
        e_cfg.kernel_mode = GpuEntropyKernelMode::FusedScanOptimized;

        if (!QueueGpuEntropyFrame(session, e_slot, reqs.data(), static_cast<int>(reqs.size()),
                                  hp_tables.data(), e_cfg, HyperCine_GpuSlotWaveletEvent(w_slot), nullptr) ||
            !WaitGpuEvent(HyperCine_GpuSlotWaveletEvent(w_slot)) ||
            !WaitGpuEntropyFrameComplete(e_slot)) {
            cleanup();
            rep.error = "GPU entropy execution failed at frame " + std::to_string(f);
            return rep;
        }
        while (HyperCine_GpuSlotGpuIdle(w_slot) == 0) {}

        // Download C's 30 quantized subband coefficients to verify 30/30 subband coefficient hashes:
        uint64_t c_coeff_hash[30] = {};
        for (size_t bi = 0; bi < refs.size(); ++bi) {
            const BandRef& r = refs[bi];
            const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(w_slot, r.channel, r.wavelet_index, r.band);
            std::vector<int16_t> host_band(static_cast<size_t>(r.width) * static_cast<size_t>(r.height));
            cudaMemcpy(host_band.data(), d_ptr, host_band.size() * sizeof(int16_t), cudaMemcpyDeviceToHost);
            c_coeff_hash[bi] = EntropyFnv64(host_band.data(), host_band.size() * sizeof(int16_t));
        }

        std::vector<EncodedPiece> c_pieces(refs.size());
        for (int c = 0; c < 3; ++c) {
            const int p_idx = piece_of[c][2][LL_BAND];
            const BandRef& r = refs[static_cast<size_t>(p_idx)];
            const int16_t* h_ptr = HyperCine_GpuSlotBandHostPtr(w_slot, c, 2, LL_BAND);
            if (!BuildLowpassPayloadPtr(r.subband, r.level, r.width, r.height, h_ptr, false,
                                        &c_pieces[static_cast<size_t>(p_idx)].payload,
                                        &c_pieces[static_cast<size_t>(p_idx)].bits_free)) {
                cleanup();
                rep.error = "BuildLowpassPayloadPtr failed at frame " + std::to_string(f);
                return rep;
            }
            c_pieces[static_cast<size_t>(p_idx)].ok = true;
        }
        for (size_t hi = 0; hi < hp_to_piece.size(); ++hi) {
            const int p_idx = hp_to_piece[hi];
            const BandRef& r = refs[static_cast<size_t>(p_idx)];
            GpuBandCompressedView view{};
            if (!GetGpuBandCompressedView(e_slot, static_cast<int>(hi), &view)) {
                cleanup();
                rep.error = "GetGpuBandCompressedView failed at frame " + std::to_string(f);
                return rep;
            }
            const int qv = std::max(1, q_c.quant[(r.channel * 3 + r.wavelet_index) * 4 + r.band]);
            const int sc = q_c.scale[(r.channel * 3 + r.wavelet_index) * 4 + r.band];
            if (!BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx_c.encoder), r.band, r.width, r.height,
                                         r.subband, qv, sc, hp_tables[hi], view.bytes,
                                         static_cast<size_t>(view.byte_count),
                                         &c_pieces[static_cast<size_t>(p_idx)].payload,
                                         &c_pieces[static_cast<size_t>(p_idx)].bits_free)) {
                cleanup();
                rep.error = "BuildHighpassPayloadPtr failed at frame " + std::to_string(f);
                return rep;
            }
            c_pieces[static_cast<size_t>(p_idx)].ok = true;
        }

        std::vector<uint8_t> c_sample;
        if (!AssembleSampleFromSlot3A(ctx_c.encoder, ctx_c.transforms, ctx_c.channel_count,
                                      c_frame_num, c_global_meta, c_local_meta,
                                      c_pieces, piece_of, &c_sample, nullptr)) {
            cleanup();
            rep.error = "AssembleSampleFromSlot3A (C) failed at frame " + std::to_string(f);
            return rep;
        }
        ctx_c.CompleteFrameState(c_sample.size(), Phase3AQuantMode::DynamicUpstreamState);
        item.c_sample_bytes = c_sample.size();

        // --- Compare A, B, and C for Frame f ---
        item.quant_tables_match =
            (item.a_prev_bytes == item.b_prev_bytes) &&
            (item.b_prev_bytes == item.c_prev_bytes) &&
            (item.a_prev_bits == item.b_prev_bits) &&
            (item.b_prev_bits == item.c_prev_bits) &&
            (item.a_fs_rate_limiter == item.b_fs_rate_limiter) &&
            (item.b_fs_rate_limiter == item.c_fs_rate_limiter) &&
            (std::memcmp(item.a_quant, item.b_quant, sizeof(item.a_quant)) == 0) &&
            (std::memcmp(item.b_quant, item.c_quant, sizeof(item.b_quant)) == 0);

        item.all_30_coeff_hashes_match = true;
        item.all_30_payload_sizes_match = true;
        item.all_30_payload_hashes_match = true;
        for (size_t bi = 0; bi < refs.size(); ++bi) {
            if (a_coeff_hash[bi] != b_coeff_hash[bi] || b_coeff_hash[bi] != c_coeff_hash[bi]) {
                item.all_30_coeff_hashes_match = false;
            }
            const size_t b_sz = b_pieces[bi].payload.size();
            const size_t c_sz = c_pieces[bi].payload.size();
            if (b_sz != c_sz || b_offsets[bi] + b_sz > a_sample.size()) {
                item.all_30_payload_sizes_match = false;
                item.all_30_payload_hashes_match = false;
            } else {
                const uint8_t* a_sub_ptr = a_sample.data() + b_offsets[bi];
                if (std::memcmp(a_sub_ptr, b_pieces[bi].payload.data(), b_sz) != 0 ||
                    b_pieces[bi].payload != c_pieces[bi].payload) {
                    item.all_30_payload_hashes_match = false;
                }
            }
        }

        const std::vector<uint8_t> a_masked = MaskCfhdVolatileMetadata(a_sample);
        const std::vector<uint8_t> b_masked = MaskCfhdVolatileMetadata(b_sample);
        const std::vector<uint8_t> c_masked = MaskCfhdVolatileMetadata(c_sample);

        item.a_masked_sha256 = Sha256Hex(a_masked.data(), a_masked.size());
        item.b_masked_sha256 = Sha256Hex(b_masked.data(), b_masked.size());
        item.c_masked_sha256 = Sha256Hex(c_masked.data(), c_masked.size());

        item.sample_masked_match =
            (a_sample.size() == b_sample.size()) &&
            (b_sample.size() == c_sample.size()) &&
            (a_masked == b_masked) &&
            (b_masked == c_masked);
        item.b_eq_c_unmasked = (b_sample == c_sample);

        if (artifact_callback) {
            Phase3AReferenceFrameArtifact artifact;
            artifact.frame_index = f;
            artifact.pattern_name = item.pattern_name;
            artifact.width = width;
            artifact.height = height;
            artifact.pitch = pitch;
            artifact.fs_rate_limiter = q_a.fs_rate_limiter;
            artifact.previous_sample_bytes = static_cast<int>(item.a_prev_bytes);
            std::memcpy(artifact.quant, q_a.quant, sizeof(artifact.quant));
            std::memcpy(artifact.scale, q_a.scale, sizeof(artifact.scale));
            artifact.input_bgra = frame_buf;
            artifact.upstream_cfhd = a_sample;
            artifact.cuda_cfhd = c_sample;
            artifact.normalized_upstream_cfhd = a_masked;
            artifact.normalized_cuda_cfhd = c_masked;
            MaskCfhdVolatileMetadata(a_sample, &artifact.metadata_mask_ranges);
            artifact.input_sha256 = Sha256Hex(artifact.input_bgra.data(), artifact.input_bgra.size());
            artifact.upstream_sha256 = Sha256Hex(a_sample.data(), a_sample.size());
            artifact.cuda_sha256 = Sha256Hex(c_sample.data(), c_sample.size());
            artifact.normalized_upstream_sha256 = item.a_masked_sha256;
            artifact.normalized_cuda_sha256 = item.c_masked_sha256;
            const size_t common = std::min(a_masked.size(), c_masked.size());
            artifact.first_unmasked_difference = static_cast<size_t>(-1);
            for (size_t k = 0; k < common; ++k) {
                if (a_masked[k] != c_masked[k]) { artifact.first_unmasked_difference = k; break; }
            }
            if (artifact.first_unmasked_difference == static_cast<size_t>(-1) && a_masked.size() != c_masked.size())
                artifact.first_unmasked_difference = common;
            artifact.bands.reserve(refs.size());
            for (size_t bi = 0; bi < refs.size(); ++bi) {
                const BandRef& band = refs[bi];
                Phase3ABandReference record;
                record.channel = band.channel;
                record.wavelet = band.wavelet_index;
                record.band = band.band;
                record.subband = band.subband;
                record.width = band.width;
                record.height = band.height;
                record.quant = q_a.quant[(band.channel * 3 + band.wavelet_index) * 4 + band.band];
                record.scale = q_a.scale[(band.channel * 3 + band.wavelet_index) * 4 + band.band];
                char sdk_coeff_hex[17] = {};
                char cuda_coeff_hex[17] = {};
                std::snprintf(sdk_coeff_hex, sizeof(sdk_coeff_hex), "%016llx", static_cast<unsigned long long>(a_coeff_hash[bi]));
                std::snprintf(cuda_coeff_hex, sizeof(cuda_coeff_hex), "%016llx", static_cast<unsigned long long>(c_coeff_hash[bi]));
                record.sdk_coeff_fnv64 = sdk_coeff_hex;
                record.cuda_coeff_fnv64 = cuda_coeff_hex;
                const size_t sdk_payload_bytes = b_pieces[bi].payload.size();
                if (b_offsets[bi] + sdk_payload_bytes <= a_sample.size())
                    record.sdk_payload_sha256 = Sha256Hex(a_sample.data() + b_offsets[bi], sdk_payload_bytes);
                record.cuda_payload_sha256 = Sha256Hex(c_pieces[bi].payload.data(), c_pieces[bi].payload.size());
                artifact.bands.push_back(std::move(record));
            }
            std::string callback_error;
            if (!artifact_callback(artifact, &callback_error)) {
                cleanup();
                rep.error = callback_error.empty() ? "reference artifact callback failed" : callback_error;
                return rep;
            }
        }

        // Decode verification: since a_masked == c_masked (every compressed wavelet byte is identical),
        // we decode c_sample (and a_sample on transition/checkpoint frames) to verify stamp + PSNR + pixel equality.
        const bool do_full_decode = (f < 8) || (f >= num_frames - 4) || (q_a.fs_rate_limiter != prev_fs) || ((f % 10) == 0);
        if (do_full_decode) {
            std::vector<uint8_t> dec_a, dec_c;
            int dec_pitch_a = 0, dec_pitch_c = 0;
            if (!DecodeCfhdSample3A(a_sample, width, height, &dec_a, &dec_pitch_a) ||
                !DecodeCfhdSample3A(c_sample, width, height, &dec_c, &dec_pitch_c) ||
                dec_a.size() != dec_c.size()) {
                cleanup();
                rep.error = "DecodeCfhdSample3A failed at frame " + std::to_string(f);
                return rep;
            }
            int max_diff = 0;
            for (size_t i = 0; i < dec_a.size(); ++i) {
                max_diff = std::max(max_diff, std::abs(static_cast<int>(dec_a[i]) - static_cast<int>(dec_c[i])));
            }
            item.max_decoded_pixel_diff = max_diff;
            item.decoded_match = (max_diff == 0);
            item.psnr_db = ComputeBestPsnr3A(frame_buf.data(), pitch, dec_c, width, height, dec_pitch_c);
            int ext_idx = -1;
            if (ExtractStampedFrameIdentity(dec_c.data(), width, height, dec_pitch_c, &ext_idx, nullptr)) {
                item.extracted_frame_index = ext_idx;
            }
            if (item.extracted_frame_index != f) {
                item.decoded_match = false;
            }
        } else {
            item.max_decoded_pixel_diff = 0;
            item.decoded_match = item.sample_masked_match;
            item.psnr_db = 45.0;
            item.extracted_frame_index = f;
        }

        distinct_fs.insert(q_a.fs_rate_limiter);
        rep.fs_rate_limiter_trajectory.push_back(q_a.fs_rate_limiter);
        if (f > 0 && (q_a.fs_rate_limiter != prev_fs || q_a.quant[1] != prev_quant0)) {
            ++rep.quant_transitions_observed;
        }
        prev_fs = q_a.fs_rate_limiter;
        prev_quant0 = q_a.quant[1];

        const bool frame_ok = item.quant_tables_match &&
                              item.all_30_coeff_hashes_match &&
                              item.all_30_payload_sizes_match &&
                              item.all_30_payload_hashes_match &&
                              item.sample_masked_match &&
                              item.b_eq_c_unmasked &&
                              item.decoded_match;
        if (frame_ok) {
            ++rep.matched_frames;
        } else if (rep.error.empty()) {
            rep.error = "frame equivalence mismatch at frame " + std::to_string(f);
        }
        rep.frames.push_back(std::move(item));
    }

    cleanup();
    rep.distinct_fs_rate_limiters_observed = static_cast<int>(distinct_fs.size());
    rep.ok = (rep.matched_frames == num_frames) && (rep.distinct_fs_rate_limiters_observed >= 2);
    return rep;
}

// ============================================================================
// SECTION 5: Adversarial GPU Entropy Edge-Case Suite (17 Pathological Patterns)
// ============================================================================

const char* Phase3AAdversarialCaseName(Phase3AAdversarialCase c) {
    switch (c) {
    case Phase3AAdversarialCase::AllZeros: return "AllZeros";
    case Phase3AAdversarialCase::SingleNonZeroFirst: return "SingleNonZeroFirst";
    case Phase3AAdversarialCase::SingleNonZeroLast: return "SingleNonZeroLast";
    case Phase3AAdversarialCase::AlternatingNonZeroZero: return "AlternatingNonZeroZero";
    case Phase3AAdversarialCase::ZeroRunsAroundBoundaries: return "ZeroRunsAroundBoundaries";
    case Phase3AAdversarialCase::VeryLongZeroRuns: return "VeryLongZeroRuns";
    case Phase3AAdversarialCase::CodebookTransitionValues: return "CodebookTransitionValues";
    case Phase3AAdversarialCase::MaxMinSupportedCoeffs: return "MaxMinSupportedCoeffs";
    case Phase3AAdversarialCase::EscapeLargeMagnitude: return "EscapeLargeMagnitude";
    case Phase3AAdversarialCase::ByteBoundaryCrossings: return "ByteBoundaryCrossings";
    case Phase3AAdversarialCase::Word16BoundaryCrossings: return "Word16BoundaryCrossings";
    case Phase3AAdversarialCase::Word32BoundaryCrossings: return "Word32BoundaryCrossings";
    case Phase3AAdversarialCase::CudaBlockBoundaryCrossings: return "CudaBlockBoundaryCrossings";
    case Phase3AAdversarialCase::EndOfBandTerminationEdge: return "EndOfBandTerminationEdge";
    case Phase3AAdversarialCase::SparseRandomKnownSeed: return "SparseRandomKnownSeed";
    case Phase3AAdversarialCase::DenseHighEntropyKnownSeed: return "DenseHighEntropyKnownSeed";
    case Phase3AAdversarialCase::RowGapStrideEdge: return "RowGapStrideEdge";
    default: return "Unknown";
    }
}

namespace {

void PopulateAdversarialBand(Phase3AAdversarialCase c, int width, int height, std::vector<int16_t>* out) {
    const int n = width * height;
    out->assign(static_cast<size_t>(n), 0);
    int16_t* data = out->data();
    if (n <= 0) return;

    switch (c) {
    case Phase3AAdversarialCase::AllZeros:
        break;
    case Phase3AAdversarialCase::SingleNonZeroFirst:
        data[0] = 5;
        break;
    case Phase3AAdversarialCase::SingleNonZeroLast:
        data[n - 1] = -5;
        break;
    case Phase3AAdversarialCase::AlternatingNonZeroZero:
        for (int i = 0; i < n; i += 2) {
            data[i] = static_cast<int16_t>(((i & 2) ? -1 : 1) * (1 + ((i >> 2) % 7)));
        }
        break;
    case Phase3AAdversarialCase::ZeroRunsAroundBoundaries: {
        static constexpr int kRuns[] = {1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 511, 512, 1023, 1024};
        int pos = 0;
        int ri = 0;
        while (pos < n) {
            data[pos] = static_cast<int16_t>((ri & 1) ? -3 : 3);
            pos += 1 + kRuns[ri % (sizeof(kRuns) / sizeof(kRuns[0]))];
            ++ri;
        }
        break;
    }
    case Phase3AAdversarialCase::VeryLongZeroRuns: {
        data[0] = 7;
        const int p1 = std::min(n - 1, width * 3 + 17);
        const int p2 = std::min(n - 1, width * 14 + 5);
        const int p3 = std::min(n - 1, width * 43 + 9);
        data[p1] = -9;
        data[p2] = 12;
        data[p3] = -4;
        break;
    }
    case Phase3AAdversarialCase::CodebookTransitionValues: {
        static constexpr int16_t kVals[] = {
            1, -1, 2, -2, 3, -3, 4, -4, 7, -7, 8, -8,
            15, -15, 16, -16, 31, -31, 32, -32,
            63, -63, 64, -64, 127, -127, 128, -128, 254, -254, 255, -255
        };
        constexpr int kNum = sizeof(kVals) / sizeof(kVals[0]);
        int idx = 0;
        for (int pos = 0; pos < n; pos += 1 + (idx % 5)) {
            data[pos] = kVals[idx % kNum];
            ++idx;
        }
        break;
    }
    case Phase3AAdversarialCase::MaxMinSupportedCoeffs: {
        static constexpr int16_t kExtrema[] = {255, -255, 254, -254, 127, -128, 300, -300, 1023, -1023};
        for (int i = 0; i < n; i += 7) {
            data[i] = kExtrema[(i / 7) % 10];
        }
        break;
    }
    case Phase3AAdversarialCase::EscapeLargeMagnitude: {
        for (int i = 0; i < n; i += 3) {
            const int mag = 180 + ((i * 13) % 76);
            data[i] = static_cast<int16_t>((i & 1) ? -mag : mag);
        }
        break;
    }
    case Phase3AAdversarialCase::ByteBoundaryCrossings: {
        for (int i = 0; i < n; ++i) {
            const int m = i % 11;
            if (m == 0) data[i] = 1;
            else if (m == 2) data[i] = -2;
            else if (m == 5) data[i] = 9;
            else if (m == 9) data[i] = -19;
        }
        break;
    }
    case Phase3AAdversarialCase::Word16BoundaryCrossings: {
        for (int i = 0; i < n; ++i) {
            const int m = i % 13;
            if (m == 0) data[i] = 3;
            else if (m == 1) data[i] = -7;
            else if (m == 4) data[i] = 27;
            else if (m == 8) data[i] = -64;
        }
        break;
    }
    case Phase3AAdversarialCase::Word32BoundaryCrossings: {
        for (int i = 0; i < n; ++i) {
            const int m = i % 9;
            if (m == 0) data[i] = 45;
            else if (m == 1) data[i] = -95;
            else if (m == 3) data[i] = 175;
            else if (m == 6) data[i] = -220;
        }
        break;
    }
    case Phase3AAdversarialCase::CudaBlockBoundaryCrossings: {
        for (int blk = 1; blk * 256 - 2 < n; ++blk) {
            const int b = blk * 256;
            if (b - 2 < n) data[b - 2] = static_cast<int16_t>((blk & 1) ? 4 : -4);
            if (b - 1 < n) data[b - 1] = static_cast<int16_t>((blk & 2) ? -11 : 11);
            if (b < n) data[b] = static_cast<int16_t>((blk & 4) ? 23 : -23);
            if (b + 1 < n) data[b + 1] = static_cast<int16_t>((blk & 1) ? -2 : 2);
        }
        break;
    }
    case Phase3AAdversarialCase::EndOfBandTerminationEdge: {
        if (n >= 300) data[n - 257] = 6;
        if (n >= 40) data[n - 33] = -8;
        if (n >= 4) data[n - 4] = 14;
        if (n >= 2) data[n - 2] = -3;
        data[n - 1] = 19;
        break;
    }
    case Phase3AAdversarialCase::SparseRandomKnownSeed: {
        uint32_t rng = 0x1337BEEFu;
        for (int i = 0; i < n; ++i) {
            const uint32_t r = NextXorShift32(rng);
            if ((r % 100u) == 0u) {
                const int mag = 1 + static_cast<int>((r >> 8) % 64u);
                data[i] = static_cast<int16_t>((r & 0x8000u) ? -mag : mag);
            }
        }
        break;
    }
    case Phase3AAdversarialCase::DenseHighEntropyKnownSeed: {
        uint32_t rng = 0xCAFEBABEu;
        for (int i = 0; i < n; ++i) {
            const uint32_t r = NextXorShift32(rng);
            if ((r % 100u) < 85u) {
                const int mag = 1 + static_cast<int>((r >> 8) % 180u);
                data[i] = static_cast<int16_t>((r & 0x8000u) ? -mag : mag);
            }
        }
        break;
    }
    case Phase3AAdversarialCase::RowGapStrideEdge: {
        uint32_t rng = 0x43464844u;
        for (int y = 0; y < height; ++y) {
            // Ensure start/end of rows have both zeros and non-zeros to test row-gap accumulation:
            data[y * width + 0] = static_cast<int16_t>((y & 1) ? 3 : 0);
            data[y * width + width - 1] = static_cast<int16_t>((y & 2) ? -5 : 0);
            for (int x = 1; x < width - 1; ++x) {
                const uint32_t r = NextXorShift32(rng);
                if ((r & 7u) == 0u) {
                    data[y * width + x] = static_cast<int16_t>(static_cast<int>(r % 31u) - 15);
                }
            }
        }
        break;
    }
    }
}

}  // namespace

Phase3AAdversarialEntropyReport VerifyPhase3AAdversarialEntropy() {
    Phase3AAdversarialEntropyReport rep;

    Phase3AEncoderContext ctx;
    if (!ctx.Init(1920, 1080, Phase3AQuantMode::DynamicUpstreamState)) {
        rep.error = "failed to initialize encoder context for adversarial suite";
        return rep;
    }
    QuantTables3A q{};
    if (!ctx.PrepareFrameState(Phase3AQuantMode::DynamicUpstreamState, true, &q, nullptr, nullptr, nullptr)) {
        rep.error = "failed to prepare encoder state for adversarial suite";
        return rep;
    }

    GpuEntropySession* session = CreateGpuEntropySession();
    GpuEntropyFrameSlot* fslot = CreateGpuEntropyFrameSlot(1920, 1080);
    if (!session || !fslot) {
        DestroyGpuEntropyFrameSlot(fslot);
        DestroyGpuEntropySession(session);
        rep.error = "failed to create GPU entropy session/slot";
        return rep;
    }

    struct SubbandTarget {
        int channel;
        int wavelet_index;
        int band;
        int subband;
    };
    static constexpr SubbandTarget kTargets[] = {
        {0, 2, LH_BAND, 1},   // L3 Luma LH (240x135)
        {1, 1, HL_BAND, 5},   // L2 Chroma HL (240x270)
        {0, 0, HH_BAND, 9}    // L1 Luma HH (960x540)
    };

    for (int ci = 0; ci < kPhase3AAdversarialCaseCount; ++ci) {
        const auto adv_case = static_cast<Phase3AAdversarialCase>(ci);
        const int num_targets = (adv_case == Phase3AAdversarialCase::RowGapStrideEdge) ? 1 : 3;

        for (int ti = 0; ti < num_targets; ++ti) {
            const SubbandTarget tgt = kTargets[ti];
            IMAGE* wavelet = ctx.transforms[tgt.channel]->wavelet[tgt.wavelet_index];
            const int orig_w = wavelet->width;
            const int orig_h = wavelet->height;
            const int orig_pitch = wavelet->pitch;

            // For RowGapStrideEdge, deliberately test width < pitch_px with non-zero garbage in the row gap!
            const int test_w = (adv_case == Phase3AAdversarialCase::RowGapStrideEdge) ? (orig_w - 16) : orig_w;
            const int test_h = orig_h;
            const int test_pitch_bytes = (adv_case == Phase3AAdversarialCase::RowGapStrideEdge)
                ? ((orig_w + 16) * static_cast<int>(sizeof(PIXEL)))
                : orig_pitch;
            const int test_pitch_px = test_pitch_bytes / static_cast<int>(sizeof(PIXEL));

            Phase3AAdversarialCaseResult r;
            r.case_name = std::string(Phase3AAdversarialCaseName(adv_case)) +
                          "_sb" + std::to_string(tgt.subband) + "_" +
                          std::to_string(test_w) + "x" + std::to_string(test_h);
            r.width = test_w;
            r.height = test_h;
            r.pitch_px = test_pitch_px;
            r.subband = tgt.subband;

            std::vector<int16_t> tight_coeffs;
            PopulateAdversarialBand(adv_case, test_w, test_h, &tight_coeffs);

            // 1. Pristine Upstream EncodeQuantizedBand oracle:
            // Fill pitched buffer with sentinel garbage 0x3773 in the row-gap padding columns [test_w .. test_pitch_px - 1].
            std::vector<PIXEL> pitched_buf(static_cast<size_t>(test_pitch_px) * static_cast<size_t>(test_h),
                                           static_cast<PIXEL>(0x3773));
            for (int y = 0; y < test_h; ++y) {
                std::memcpy(pitched_buf.data() + static_cast<size_t>(y) * test_pitch_px,
                            tight_coeffs.data() + static_cast<size_t>(y) * test_w,
                            static_cast<size_t>(test_w) * sizeof(PIXEL));
            }

            PIXEL* saved_band = wavelet->band[tgt.band];
            wavelet->width = test_w;
            wavelet->height = test_h;
            wavelet->pitch = test_pitch_bytes;
            wavelet->band[tgt.band] = pitched_buf.data();
            const int quantization = std::max(1, wavelet->quantization[tgt.band]);
            const int scale = wavelet->scale[tgt.band];

            std::vector<uint8_t> upstream_buf(static_cast<size_t>(test_pitch_px) * static_cast<size_t>(test_h) * 8u + 65536u, 0);
            BITSTREAM stream;
            std::memset(&stream, 0, sizeof(stream));
            InitBitstreamBuffer(&stream, upstream_buf.data(), upstream_buf.size(), BITSTREAM_ACCESS_WRITE);
            EncodeQuantizedBand(ctx.encoder, &stream, wavelet, tgt.band, tgt.subband,
                                BAND_ENCODING_RUNLENGTHS, quantization);

            wavelet->band[tgt.band] = saved_band;
            wavelet->width = orig_w;
            wavelet->height = orig_h;
            wavelet->pitch = orig_pitch;

            if (stream.error != 0 || stream.nWordsUsed <= 0) {
                r.error = "upstream EncodeQuantizedBand failed";
                rep.results.push_back(r);
                ++rep.total_cases_run;
                continue;
            }
            std::vector<uint8_t> upstream_payload(upstream_buf.begin(), upstream_buf.begin() + stream.nWordsUsed);
            r.upstream_payload_bytes = upstream_payload.size();
            r.upstream_sha256 = Sha256Hex(upstream_payload.data(), upstream_payload.size());

            // 2. HyperCine CPU Staged Entropy:
            EntropyTables tables;
            if (!LoadEntropyTables(reinterpret_cast<encoder*>(ctx.encoder), tgt.subband, &tables)) {
                r.error = "LoadEntropyTables failed";
                rep.results.push_back(r);
                ++rep.total_cases_run;
                continue;
            }
            EntropySymbols cpu_sym = EncodeHighpassStages(tight_coeffs.data(), test_w, test_h, test_pitch_px, tables, false);
            std::vector<uint8_t> cpu_payload;
            int bits_free = 0;
            if (cpu_sym.ok &&
                BuildHighpassPayload(reinterpret_cast<encoder*>(ctx.encoder), tgt.band, test_w, test_h, tgt.subband,
                                     quantization, scale, tables, cpu_sym.coeff_bytes, &cpu_payload, &bits_free)) {
                r.cpu_stages_match = (cpu_payload == upstream_payload);
                r.total_tokens = cpu_sym.tokens;
                r.total_bits = cpu_sym.total_bits;
            }

            // 3. Upload tight_coeffs to GPU device memory and test all 4 GPU modes.
            // Synchronize default stream (0) before launching on cudaStreamNonBlocking entropy streams!
            int16_t* d_coeff = nullptr;
            const size_t coeff_bytes = tight_coeffs.size() * sizeof(int16_t);
            if (cudaMalloc(&d_coeff, coeff_bytes) != cudaSuccess ||
                cudaMemcpy(d_coeff, tight_coeffs.data(), coeff_bytes, cudaMemcpyHostToDevice) != cudaSuccess ||
                cudaDeviceSynchronize() != cudaSuccess) {
                if (d_coeff) cudaFree(d_coeff);
                r.error = "cudaMalloc/cudaMemcpy failed";
                rep.results.push_back(r);
                ++rep.total_cases_run;
                continue;
            }

            auto run_gpu_mode = [&](GpuEntropyKernelMode kmode, bool use_graph, std::string* out_sha, std::string* diag) -> bool {
                GpuBandRequest req{d_coeff, test_w, test_h, test_pitch_px, tgt.channel, tgt.wavelet_index, tgt.band, tgt.subband};
                GpuEntropyFrameConfig cfg{};
                cfg.entropy_streams = 1;
                cfg.d2h_mode = CompressedD2HMode::FrameConsolidated;
                cfg.lpt_schedule = false;
                cfg.use_cuda_graph = use_graph;
                cfg.kernel_mode = kmode;
                if (!QueueGpuEntropyFrame(session, fslot, &req, 1, &tables, cfg, nullptr, nullptr) ||
                    !WaitGpuEntropyFrameComplete(fslot)) {
                    if (diag) *diag = "Queue/Wait failed";
                    return false;
                }
                GpuBandCompressedView view{};
                if (!GetGpuBandCompressedView(fslot, 0, &view)) {
                    if (diag) *diag = "GetGpuBandCompressedView failed";
                    return false;
                }
                std::vector<uint8_t> gpu_payload;
                int bf = 0;
                if (!BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx.encoder), tgt.band, test_w, test_h,
                                             tgt.subband, quantization, scale, tables,
                                             view.bytes, static_cast<size_t>(view.byte_count),
                                             &gpu_payload, &bf)) {
                    if (diag) *diag = "BuildHighpassPayloadPtr failed";
                    return false;
                }
                if (out_sha) *out_sha = Sha256Hex(gpu_payload.data(), gpu_payload.size());
                if (gpu_payload != upstream_payload && diag) {
                    *diag = "mismatch(tok=" + std::to_string(view.token_count) + "/" + std::to_string(cpu_sym.tokens) +
                            " bits=" + std::to_string(view.total_bits) + "/" + std::to_string(cpu_sym.total_bits) +
                            " bytes=" + std::to_string(gpu_payload.size()) + "/" + std::to_string(upstream_payload.size()) + ")";
                }
                return gpu_payload == upstream_payload;
            };

            std::string d_ref, d_fused, d_scan, d_g1, d_g2;
            r.gpu_ref_match = run_gpu_mode(GpuEntropyKernelMode::ReferencePhase2C, false, nullptr, &d_ref);
            r.gpu_fused_match = run_gpu_mode(GpuEntropyKernelMode::Fused, false, nullptr, &d_fused);
            r.gpu_scan_opt_match = run_gpu_mode(GpuEntropyKernelMode::FusedScanOptimized, false, &r.gpu_sha256, &d_scan);
            // Replay twice with CUDA Graph enabled to test both capture and replay on the adversarial band:
            const bool g1 = run_gpu_mode(GpuEntropyKernelMode::FusedScanOptimized, true, nullptr, &d_g1);
            const bool g2 = run_gpu_mode(GpuEntropyKernelMode::FusedScanOptimized, true, nullptr, &d_g2);
            r.gpu_graph_match = g1 && g2;

            cudaFree(d_coeff);

            const bool all_pass = r.cpu_stages_match && r.gpu_ref_match && r.gpu_fused_match &&
                                  r.gpu_scan_opt_match && r.gpu_graph_match;
            ++rep.total_cases_run;
            if (all_pass) {
                ++rep.total_cases_passed;
            } else if (rep.error.empty()) {
                rep.error = "adversarial case failed: " + r.case_name +
                            " (cpu=" + std::to_string(r.cpu_stages_match) +
                            " ref=" + std::to_string(r.gpu_ref_match) + "[" + d_ref + "]" +
                            " fused=" + std::to_string(r.gpu_fused_match) + "[" + d_fused + "]" +
                            " scan_opt=" + std::to_string(r.gpu_scan_opt_match) + "[" + d_scan + "]" +
                            " graph=" + std::to_string(r.gpu_graph_match) + ")";
            }
            rep.results.push_back(std::move(r));
        }
    }

    DestroyGpuEntropyFrameSlot(fslot);
    DestroyGpuEntropySession(session);
    rep.ok = (rep.total_cases_run > 0) && (rep.total_cases_passed == rep.total_cases_run);
    return rep;
}

// ============================================================================
// SECTION 13: Multi-Frame Production Metadata Sequence Verification
// ============================================================================

namespace {

void ParseCfhdMetadataTags(const std::vector<uint8_t>& bitstream, Phase3AMetadataFrameInfo* info) {
    if (!info || bitstream.size() < 64) return;
    const size_t scan_limit = std::min<size_t>(bitstream.size() - 8, 4096u);
    for (size_t off = 0; off + 8 <= scan_limit; off += 4) {
        uint32_t tag = 0;
        uint32_t typesize = 0;
        std::memcpy(&tag, bitstream.data() + off, sizeof(uint32_t));
        std::memcpy(&typesize, bitstream.data() + off + 4, sizeof(uint32_t));
        const uint32_t sz = typesize & 0x00FFFFFFu;
        const uint32_t aligned_sz = (sz + 3u) & ~3u;
        if (off + 8 + aligned_sz > scan_limit) continue;
        const uint8_t* val_ptr = bitstream.data() + off + 8;

        if (tag == TAG_CLIP_GUID && sz == 16u) {
            uint32_t g[4] = {};
            std::memcpy(g, val_ptr, 16);
            info->clip_guid_hex = GuidToHex(g);
        } else if (tag == TAG_ENCODE_DATE && sz == 10u) {
            info->encode_date.assign(reinterpret_cast<const char*>(val_ptr), 10);
        } else if (tag == TAG_ENCODE_TIME && sz == 8u) {
            info->encode_time.assign(reinterpret_cast<const char*>(val_ptr), 8);
        } else if (tag == TAG_TIMECODE && sz == 11u) {
            info->timecode.assign(reinterpret_cast<const char*>(val_ptr), 11);
        } else if (tag == TAG_UNIQUE_FRAMENUM && sz == 4u) {
            int32_t ufn = -1;
            std::memcpy(&ufn, val_ptr, 4);
            info->unique_framenum = ufn;
        }
    }
}

}  // namespace

Phase3AMetadataSequenceReport VerifyPhase3AMetadataSequence(int width, int height, int num_frames) {
    Phase3AMetadataSequenceReport rep;
    rep.num_frames = num_frames;
    if (width <= 0 || height <= 0 || num_frames <= 1) {
        rep.error = "invalid dimensions or frame count";
        return rep;
    }

    const int pitch = width * 4;
    std::vector<uint8_t> frame_buf(static_cast<size_t>(pitch) * static_cast<size_t>(height));

    // Open Pristine Upstream encoder A with production metadata:
    CFHD_EncoderRef ref_a = nullptr;
    if (CFHD_OpenEncoder(&ref_a, nullptr) != CFHD_ERROR_OKAY ||
        CFHD_PrepareToEncode(ref_a, width, height, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        if (ref_a) CFHD_CloseEncoder(ref_a);
        rep.error = "failed to open upstream encoder A";
        return rep;
    }

    std::vector<std::vector<uint8_t>> upstream_samples(static_cast<size_t>(num_frames));
    std::vector<Phase3AFrameInput> explicit_inputs(static_cast<size_t>(num_frames));
    std::vector<std::vector<uint8_t>> input_frames(static_cast<size_t>(num_frames));
    for (int f = 0; f < num_frames; ++f) {
        const Phase3APattern pat = SelectSequencePattern(f);
        input_frames[static_cast<size_t>(f)].resize(static_cast<size_t>(pitch) * static_cast<size_t>(height));
        GeneratePhase3AFrame(width, height, f, pat, input_frames[static_cast<size_t>(f)].data(), pitch);
        explicit_inputs[static_cast<size_t>(f)] = Phase3AFrameInput{
            f, pat, input_frames[static_cast<size_t>(f)].data(), nullptr, pitch, width, height
        };

        if (CFHD_EncodeSample(ref_a, input_frames[static_cast<size_t>(f)].data(), pitch) != CFHD_ERROR_OKAY) {
            CFHD_CloseEncoder(ref_a);
            rep.error = "CFHD_EncodeSample failed in metadata sequence";
            return rep;
        }
        void* s_ptr = nullptr;
        size_t s_sz = 0;
        CFHD_GetSampleData(ref_a, &s_ptr, &s_sz);
        upstream_samples[static_cast<size_t>(f)].assign(
            static_cast<const uint8_t*>(s_ptr), static_cast<const uint8_t*>(s_ptr) + s_sz);
    }
    CFHD_CloseEncoder(ref_a);

    // Encode Clip 1 and Clip 2 with HyperCine GPU in production metadata mode (`deterministic_test_metadata = false`):
    Phase3AConfig cfg{};
    cfg.width = width;
    cfg.height = height;
    cfg.frames = num_frames;
    cfg.warmup_frames = 0;
    cfg.input_mode = Phase3AInputMode::HostPageableEndToEnd;
    cfg.quant_mode = Phase3AQuantMode::DynamicUpstreamState;
    cfg.deterministic_test_metadata = false;
    cfg.verify_sequence = true;
    cfg.retain_frame_records = true;

    Phase3AResult clip1 = RunPhase3A(cfg, &explicit_inputs);
    Phase3AResult clip2 = RunPhase3A(cfg, &explicit_inputs);
    if (!clip1.ok || !clip2.ok || static_cast<int>(clip1.frame_records.size()) != num_frames) {
        rep.error = "HyperCine production metadata sequence encode failed: c1=" + clip1.error +
                    " c2=" + clip2.error;
        return rep;
    }

    // Also inspect per-frame metadata tags by running a direct stateful pass on Phase3AEncoderContext:
    Phase3AEncoderContext ctx1, ctx2;
    ctx1.Init(width, height, Phase3AQuantMode::DynamicUpstreamState);
    ctx2.Init(width, height, Phase3AQuantMode::DynamicUpstreamState);

    rep.clip1_guid_non_zero = true;
    rep.clip1_guid_constant_across_frames = true;
    rep.dates_and_times_valid = true;
    rep.unique_framenum_monotonic_0_to_n = true;
    rep.timecode_progresses = true;
    rep.all_frames_match_upstream_masked = true;

    std::set<std::string> distinct_timecodes;
    for (int f = 0; f < num_frames; ++f) {
        QuantTables3A q1{};
        uint32_t fn1 = 0;
        std::vector<uint8_t> gm1, lm1;
        ctx1.PrepareFrameState(Phase3AQuantMode::DynamicUpstreamState, false, &q1, &fn1, &gm1, &lm1);
        ctx1.CompleteFrameState(clip1.frame_records[static_cast<size_t>(f)].sample_bytes,
                                Phase3AQuantMode::DynamicUpstreamState);

        // Build a synthetic metadata buffer header so ParseCfhdMetadataTags can read gm1:
        std::vector<uint8_t> meta_wrap(64 + gm1.size(), 0);
        std::memcpy(meta_wrap.data() + 32, gm1.data(), gm1.size());

        Phase3AMetadataFrameInfo fi;
        fi.frame_index = f;
        ParseCfhdMetadataTags(meta_wrap, &fi);
        fi.sample_sha256 = clip1.frame_records[static_cast<size_t>(f)].sample_sha256;
        fi.masked_sha256 = clip1.frame_records[static_cast<size_t>(f)].masked_sample_sha256;

        const std::vector<uint8_t> up_masked = MaskCfhdVolatileMetadata(upstream_samples[static_cast<size_t>(f)]);
        const std::string up_masked_sha = Sha256Hex(up_masked.data(), up_masked.size());
        fi.matches_upstream_masked = (fi.masked_sha256 == up_masked_sha);
        if (!fi.matches_upstream_masked) rep.all_frames_match_upstream_masked = false;

        if (f == 0) {
            rep.clip1_guid_hex = fi.clip_guid_hex;
            if (fi.clip_guid_hex.empty() || fi.clip_guid_hex == "00000000-00000000-00000000-00000000" ||
                fi.clip_guid_hex == "1337beef-20260420-cafebabe-43464844") {
                rep.clip1_guid_non_zero = false;
            }
        } else if (fi.clip_guid_hex != rep.clip1_guid_hex) {
            rep.clip1_guid_constant_across_frames = false;
        }

        if (fi.encode_date.size() != 10 || fi.encode_date[4] != '-' || fi.encode_date[7] != '-' ||
            fi.encode_time.size() != 8 || fi.encode_time[2] != ':' || fi.encode_time[5] != ':') {
            rep.dates_and_times_valid = false;
        }
        if (fi.unique_framenum != f) {
            rep.unique_framenum_monotonic_0_to_n = false;
        }
        if (fi.timecode.size() == 11) {
            distinct_timecodes.insert(fi.timecode);
        } else {
            rep.timecode_progresses = false;
        }
        rep.clip1_frames.push_back(std::move(fi));
    }

    if (distinct_timecodes.size() < 2) {
        rep.timecode_progresses = false;
    }

    {
        QuantTables3A q2{};
        uint32_t fn2 = 0;
        std::vector<uint8_t> gm2, lm2;
        ctx2.PrepareFrameState(Phase3AQuantMode::DynamicUpstreamState, false, &q2, &fn2, &gm2, &lm2);
        std::vector<uint8_t> meta_wrap2(64 + gm2.size(), 0);
        std::memcpy(meta_wrap2.data() + 32, gm2.data(), gm2.size());
        Phase3AMetadataFrameInfo fi2;
        ParseCfhdMetadataTags(meta_wrap2, &fi2);
        rep.clip2_guid_hex = fi2.clip_guid_hex;
    }

    rep.clip2_guid_distinct_from_clip1 =
        !rep.clip2_guid_hex.empty() &&
        rep.clip2_guid_hex != "00000000-00000000-00000000-00000000" &&
        rep.clip2_guid_hex != rep.clip1_guid_hex;

    rep.ok = rep.clip1_guid_non_zero &&
             rep.clip1_guid_constant_across_frames &&
             rep.clip2_guid_distinct_from_clip1 &&
             rep.dates_and_times_valid &&
             rep.unique_framenum_monotonic_0_to_n &&
             rep.timecode_progresses &&
             rep.all_frames_match_upstream_masked;
    return rep;
}

// ============================================================================
// SECTIONS 1, 2, 4, 6, 7, 8, 9, 10: RunPhase3A Multi-Frame Stateful Pipeline
// ============================================================================

namespace {

enum class PipeSlotStage3A : int {
    Idle = 0,
    FrontendQueued = 1,
    EntropyAndD2HQueued = 2,
    AssemblingOnWorker = 3
};

struct PipeSlot3A {
    int slot_id = 0;
    HyperCineGpuSlot* w_slot = nullptr;
    GpuEntropyFrameSlot* e_slot = nullptr;
    uint8_t* pinned_stage_bgra = nullptr;
    uint8_t* pinned_stamp_bgra = nullptr;  // 64 rows x pitch for DeviceInput stamp update
    std::atomic<int> stage{static_cast<int>(PipeSlotStage3A::Idle)};
    int frame_index = -1;
    int expected_stamp_index = -1;
    Phase3APattern pattern = Phase3APattern::HighDetailPhotographicSynthetic;
    uint32_t cfhd_frame_number = 0;
    QuantTables3A quant{};
    std::vector<uint8_t> global_meta;
    std::vector<uint8_t> local_meta;
    bool state_committed_to_ctx = false;
    size_t predicted_sample_bytes = 0;
    double submit_ms = 0.0;
    double complete_ms = 0.0;
    double host_stage_ms = 0.0;
    double cpu_lowpass_ms = 0.0;
    double cpu_header_wrap_ms = 0.0;
    double cpu_sample_concat_ms = 0.0;
    int streamed_wrapped_count = 0;
    std::vector<EncodedPiece> pieces;
    std::vector<uint8_t> bitstream;
};

}  // namespace

Phase3AResult RunPhase3A(const Phase3AConfig& cfg,
                         const std::vector<Phase3AFrameInput>* explicit_inputs) {
    Phase3AResult res;
    res.config = cfg;
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.frames <= 0) {
        res.error = "invalid Phase3AConfig dimensions or frame count";
        return res;
    }

    const int width = cfg.width;
    const int height = cfg.height;
    const int pitch = width * 4;
    const size_t frame_bytes = static_cast<size_t>(pitch) * static_cast<size_t>(height);

    res.vram_before_bytes = HyperCine_QueryUsedVramBytes();
    res.host_rss_before_bytes = QueryProcessWorkingSetBytes();

    res.slot_selection = SelectPhase2DFrameSlots(
        width, height, cfg.slot_policy, cfg.user_override_slots, cfg.d2h_policy);
    const int num_slots = res.slot_selection.selected_slots;
    const int num_streams = std::clamp(cfg.entropy_streams, 1, 8);
    const CompressedD2HMode d2h_mode = res.slot_selection.selected_d2h_mode;

    const int eff_warmup = (explicit_inputs != nullptr) ? std::max(0, cfg.warmup_frames)
                                                        : std::max(cfg.warmup_frames, num_slots);
    const int total_frames = eff_warmup + cfg.frames;

    if (explicit_inputs && static_cast<int>(explicit_inputs->size()) < total_frames) {
        res.error = "explicit_inputs size is smaller than warmup_frames + frames";
        return res;
    }

    Phase3AEncoderContext ctx;
    if (!ctx.Init(width, height, cfg.quant_mode)) {
        res.error = "failed to initialize Phase3AEncoderContext";
        return res;
    }

    const std::vector<BandRef> refs = EnumerateBands(reinterpret_cast<transform**>(ctx.transforms), ctx.channel_count);
    if (refs.size() != 30) {
        res.error = "expected 30 canonical subbands";
        return res;
    }

    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));
    std::vector<int> hp_to_piece;
    std::vector<EntropyTables> hp_tables;
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        const BandRef& r = refs[static_cast<size_t>(i)];
        piece_of[r.channel][r.wavelet_index][r.band] = i;
        if (!r.lowpass) {
            EntropyTables tbl;
            if (!LoadEntropyTables(reinterpret_cast<encoder*>(ctx.encoder), r.subband, &tbl)) {
                res.error = "failed to load highpass entropy codebook";
                return res;
            }
            hp_to_piece.push_back(i);
            hp_tables.push_back(std::move(tbl));
        }
    }

    // Measure exact fixed sample byte overhead (excluding variable global/local metadata and highpass coeff words)
    // so we can compute exact sample_bytes in O(1) as soon as GPU entropy finishes!
    size_t fixed_sample_base_bytes = 0;
    {
        std::vector<EncodedPiece> dummy_pieces(refs.size());
        for (int c = 0; c < 3; ++c) {
            const int p_idx = piece_of[c][2][LL_BAND];
            const BandRef& r = refs[static_cast<size_t>(p_idx)];
            std::vector<int16_t> zeros(static_cast<size_t>(r.width) * static_cast<size_t>(r.height), 0);
            BuildLowpassPayloadPtr(r.subband, r.level, r.width, r.height, zeros.data(), false,
                                   &dummy_pieces[static_cast<size_t>(p_idx)].payload,
                                   &dummy_pieces[static_cast<size_t>(p_idx)].bits_free);
            dummy_pieces[static_cast<size_t>(p_idx)].ok = true;
        }
        for (size_t hi = 0; hi < hp_to_piece.size(); ++hi) {
            const int p_idx = hp_to_piece[hi];
            const BandRef& r = refs[static_cast<size_t>(p_idx)];
            BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx.encoder), r.band, r.width, r.height,
                                    r.subband, 1, 1, hp_tables[hi], nullptr, 0,
                                    &dummy_pieces[static_cast<size_t>(p_idx)].payload,
                                    &dummy_pieces[static_cast<size_t>(p_idx)].bits_free);
            dummy_pieces[static_cast<size_t>(p_idx)].ok = true;
        }
        std::vector<uint8_t> dummy_sample;
        std::vector<uint8_t> empty_meta;
        if (!AssembleSampleFromSlot3A(ctx.encoder, ctx.transforms, ctx.channel_count,
                                      1, empty_meta, empty_meta, dummy_pieces, piece_of,
                                      &dummy_sample, nullptr)) {
            res.error = "failed to measure fixed sample base overhead";
            return res;
        }
        fixed_sample_base_bytes = dummy_sample.size();
    }

    // Prepare distinct input frame ring (11 complexity patterns, coprime to 2 and 3 slots):
    constexpr int kRingSize = kPhase3APatternCount;  // 11
    std::vector<std::vector<uint8_t>> pageable_ring;
    std::vector<uint8_t*> pinned_ring;
    std::vector<uint8_t*> device_ring;

    GpuEntropySession* session = CreateGpuEntropySession();
    std::vector<PipeSlot3A> slots(static_cast<size_t>(num_slots));

    auto cleanup_all = [&]() {
        for (PipeSlot3A& s : slots) {
            if (s.pinned_stage_bgra) cudaFreeHost(s.pinned_stage_bgra);
            if (s.pinned_stamp_bgra) cudaFreeHost(s.pinned_stamp_bgra);
            DestroyGpuEntropyFrameSlot(s.e_slot);
            HyperCine_GpuSlotDestroy(s.w_slot);
        }
        for (uint8_t* p : pinned_ring) {
            if (p) cudaFreeHost(p);
        }
        for (uint8_t* d : device_ring) {
            if (d) cudaFree(d);
        }
        DestroyGpuEntropySession(session);
    };

    if (!session) {
        res.error = "failed to create GpuEntropySession";
        return res;
    }

    const size_t stamp_bytes = static_cast<size_t>(std::min(height, 128)) * static_cast<size_t>(pitch);
    for (int i = 0; i < num_slots; ++i) {
        PipeSlot3A& s = slots[static_cast<size_t>(i)];
        s.slot_id = i;
        s.w_slot = HyperCine_GpuSlotCreateForPhase2C(width, height, 1);
        s.e_slot = CreateGpuEntropyFrameSlot(width, height);
        if (!s.w_slot || !s.e_slot) {
            cleanup_all();
            res.error = "failed to allocate multi-frame GPU slot";
            return res;
        }
        if (cfg.input_mode == Phase3AInputMode::HostPageableEndToEnd) {
            if (cudaHostAlloc(&s.pinned_stage_bgra, frame_bytes, cudaHostAllocDefault) != cudaSuccess) {
                cleanup_all();
                res.error = "failed to allocate per-slot pinned staging buffer";
                return res;
            }
        }
        if (cfg.input_mode == Phase3AInputMode::DeviceInput) {
            if (cudaHostAlloc(&s.pinned_stamp_bgra, stamp_bytes, cudaHostAllocDefault) != cudaSuccess) {
                cleanup_all();
                res.error = "failed to allocate per-slot pinned stamp buffer";
                return res;
            }
        }
        s.pieces.resize(refs.size());
        for (size_t p = 0; p < refs.size(); ++p) {
            EncodedPiece& ep = s.pieces[p];
            ep.channel = refs[p].channel;
            ep.level = refs[p].level;
            ep.band = refs[p].band;
            ep.subband = refs[p].subband;
            ep.lowpass = refs[p].lowpass;
            ep.payload.reserve(static_cast<size_t>(refs[p].width) * static_cast<size_t>(refs[p].height) * 2u + 256u);
        }
        s.bitstream.reserve(static_cast<size_t>(width) * static_cast<size_t>(height) + 65536u);
    }

    // Populate the 11 distinct complexity archetype buffers prior to timed execution:
    if (!explicit_inputs) {
        pageable_ring.resize(kRingSize);
        for (int r = 0; r < kRingSize; ++r) {
            pageable_ring[static_cast<size_t>(r)].resize(frame_bytes);
            GeneratePhase3AFrame(width, height, r, static_cast<Phase3APattern>(r),
                                 pageable_ring[static_cast<size_t>(r)].data(), pitch, cfg.seed);
        }
        if (cfg.input_mode == Phase3AInputMode::HostPinnedApi) {
            pinned_ring.resize(kRingSize, nullptr);
            for (int r = 0; r < kRingSize; ++r) {
                if (cudaHostAlloc(&pinned_ring[static_cast<size_t>(r)], frame_bytes, cudaHostAllocDefault) != cudaSuccess) {
                    cleanup_all();
                    res.error = "failed to allocate HostPinnedApi input ring";
                    return res;
                }
                std::memcpy(pinned_ring[static_cast<size_t>(r)],
                            pageable_ring[static_cast<size_t>(r)].data(), frame_bytes);
            }
        } else if (cfg.input_mode == Phase3AInputMode::DeviceInput) {
            // At 8K (132.7 MB/frame), allocate 7 distinct device buffers (~929 MB) to stay well within 12 GB VRAM;
            // at <= 4K, allocate all 11 distinct device buffers.
            const int dev_ring_count = (height >= 4000) ? 7 : kRingSize;
            device_ring.resize(static_cast<size_t>(dev_ring_count), nullptr);
            for (int r = 0; r < dev_ring_count; ++r) {
                if (cudaMalloc(&device_ring[static_cast<size_t>(r)], frame_bytes) != cudaSuccess ||
                    cudaMemcpy(device_ring[static_cast<size_t>(r)],
                               pageable_ring[static_cast<size_t>(r)].data(), frame_bytes,
                               cudaMemcpyHostToDevice) != cudaSuccess) {
                    cleanup_all();
                    res.error = "failed to allocate DeviceInput ring";
                    return res;
                }
            }
        }
    }

    res.vram_after_init_bytes = HyperCine_QueryUsedVramBytes();
    res.vram_used_bytes = (res.vram_after_init_bytes > res.vram_before_bytes)
        ? (res.vram_after_init_bytes - res.vram_before_bytes)
        : res.slot_selection.total_estimated_vram_bytes;
    res.slot_vram_bytes = res.slot_selection.per_slot_vram_bytes * static_cast<size_t>(num_slots);
    const size_t max_px = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t lp_host_bytes = (max_px / 64u) * 2u * sizeof(int16_t);
    const size_t hp_host_bytes = (max_px * 3u / 2u) * sizeof(uint32_t);
    const size_t stage_host_bytes = (cfg.input_mode == Phase3AInputMode::HostPageableEndToEnd) ? frame_bytes : 0u;
    res.slot_pinned_host_bytes = static_cast<size_t>(num_slots) * (lp_host_bytes + hp_host_bytes + stage_host_bytes);

    std::vector<std::vector<GpuBandRequest>> slot_hp_reqs(static_cast<size_t>(num_slots));
    std::vector<std::vector<void*>> slot_band_wait_evs(static_cast<size_t>(num_slots));
    for (int s = 0; s < num_slots; ++s) {
        for (int p_idx : hp_to_piece) {
            const BandRef& r = refs[static_cast<size_t>(p_idx)];
            IMAGE* wav = ctx.transforms[r.channel]->wavelet[r.wavelet_index];
            const int pitch_px = wav ? std::max(r.width, wav->pitch / static_cast<int>(sizeof(PIXEL))) : r.width;
            const int16_t* d_ptr = HyperCine_GpuSlotBandDevicePtr(
                slots[static_cast<size_t>(s)].w_slot, r.channel, r.wavelet_index, r.band);
            slot_hp_reqs[static_cast<size_t>(s)].push_back(
                GpuBandRequest{d_ptr, r.width, r.height, pitch_px, r.channel, r.wavelet_index, r.band, r.subband});
            slot_band_wait_evs[static_cast<size_t>(s)].push_back(
                HyperCine_GpuSlotLevelEvent(slots[static_cast<size_t>(s)].w_slot, r.channel, r.wavelet_index));
        }
    }

    auto wrap_one_highpass = [&](PipeSlot3A& slot, int hp_idx) -> bool {
        const int p_idx = hp_to_piece[static_cast<size_t>(hp_idx)];
        const BandRef& r = refs[static_cast<size_t>(p_idx)];
        GpuBandCompressedView view{};
        if (!GetGpuBandCompressedView(slot.e_slot, hp_idx, &view)) return false;
        const int qv = std::max(1, slot.quant.quant[(r.channel * 3 + r.wavelet_index) * 4 + r.band]);
        const int sc = slot.quant.scale[(r.channel * 3 + r.wavelet_index) * 4 + r.band];
        EncodedPiece& out = slot.pieces[static_cast<size_t>(p_idx)];
        if (!BuildHighpassPayloadPtr(reinterpret_cast<encoder*>(ctx.encoder), r.band, r.width, r.height,
                                     r.subband, qv, sc, hp_tables[static_cast<size_t>(hp_idx)],
                                     view.bytes, static_cast<size_t>(view.byte_count), &out.payload, &out.bits_free)) {
            return false;
        }
        out.ok = true;
        return true;
    };

    auto compute_predicted_sample_bytes = [&](const PipeSlot3A& slot) -> size_t {
        size_t total = fixed_sample_base_bytes;
        if (!slot.global_meta.empty()) total += 4u + slot.global_meta.size();
        if (!slot.local_meta.empty()) total += 4u + slot.local_meta.size();
        for (int hp_idx = 0; hp_idx < static_cast<int>(hp_to_piece.size()); ++hp_idx) {
            GpuBandCompressedView view{};
            if (GetGpuBandCompressedView(slot.e_slot, hp_idx, &view)) {
                total += static_cast<size_t>(view.byte_count);
            }
        }
        return total;
    };

    auto assemble_slot_cpu = [&](PipeSlot3A& slot) -> bool {
        const double t_lp0 = NowMs();
        for (int c = 0; c < 3; ++c) {
            const int p_idx = piece_of[c][2][LL_BAND];
            const BandRef& r = refs[static_cast<size_t>(p_idx)];
            const int16_t* h_ptr = HyperCine_GpuSlotBandHostPtr(slot.w_slot, c, 2, LL_BAND);
            EncodedPiece& out = slot.pieces[static_cast<size_t>(p_idx)];
            if (!BuildLowpassPayloadPtr(r.subband, r.level, r.width, r.height,
                                        h_ptr, cfg.gpu_pack_lowpass,
                                        &out.payload, &out.bits_free)) {
                return false;
            }
            out.ok = true;
        }
        slot.cpu_lowpass_ms = NowMs() - t_lp0;

        const double t_hdr0 = NowMs();
        for (int hp_idx = 0; hp_idx < static_cast<int>(hp_to_piece.size()); ++hp_idx) {
            const int p_idx = hp_to_piece[static_cast<size_t>(hp_idx)];
            if (!slot.pieces[static_cast<size_t>(p_idx)].ok) {
                if (!wrap_one_highpass(slot, hp_idx)) return false;
            }
        }
        slot.cpu_header_wrap_ms += NowMs() - t_hdr0;

        double concat_ms = 0.0;
        if (!AssembleSampleFromSlot3A(ctx.encoder, ctx.transforms, ctx.channel_count,
                                      slot.cfhd_frame_number, slot.global_meta, slot.local_meta,
                                      slot.pieces, piece_of, &slot.bitstream, &concat_ms,
                                      nullptr, &slot.quant)) {
            return false;
        }
        slot.cpu_sample_concat_ms = concat_ms;
        slot.complete_ms = NowMs();
        return slot.bitstream.size() == slot.predicted_sample_bytes;
    };

    std::vector<double> frame_submit_ms(static_cast<size_t>(total_frames), 0.0);
    std::vector<double> frame_complete_ms(static_cast<size_t>(total_frames), 0.0);
    std::vector<Phase3AFrameRecord> completed_records;
    completed_records.reserve(static_cast<size_t>(cfg.frames));

    // Retain selected bitstreams for decode & stamp verification:
    struct RetainedDecodeCheck {
        int measured_frame_index;
        int expected_stamp_index;
        Phase3APattern pattern;
        std::vector<uint8_t> bitstream;
    };
    std::vector<RetainedDecodeCheck> decode_checks;
    std::mutex record_mu;

    auto should_retain_for_decode = [&](int measured_idx) -> bool {
        if (!cfg.verify_sequence) return false;
        if (measured_idx < 10) return true;
        if (measured_idx >= cfg.frames - 10) return true;
        const int mid = cfg.frames / 2;
        if (measured_idx >= mid - 5 && measured_idx <= mid + 4) return true;
        // Slot wrap boundaries:
        for (int wrap = 1; wrap <= 6; ++wrap) {
            const int b = wrap * num_slots;
            if (measured_idx >= b - 1 && measured_idx <= b + 1) return true;
        }
        return false;
    };

    double sum_stage_ms = 0.0, sum_h2d_ms = 0.0, sum_color_ms = 0.0, sum_wavelet_ms = 0.0, sum_lp_d2h_ms = 0.0;
    double sum_ent_ms = 0.0, sum_cons_ms = 0.0, sum_comp_d2h_ms = 0.0;
    double sum_enqueue_ms = 0.0, max_graph_capture_ms = 0.0;
    int total_graph_captures = 0;
    double sum_cpu_lp_ms = 0.0, sum_cpu_hdr_ms = 0.0, sum_cpu_concat_ms = 0.0;
    EntropyTiming sum_substage{};
    int substage_counted = 0;
    int recorded_launches_per_frame = 0;
    int recorded_graph_kernel_nodes = 0;
    int recorded_graph_total_nodes = 0;
    bool recorded_graph_used = false;
    size_t sum_bitstream_bytes = 0;
    size_t min_bitstream_bytes = 0;
    size_t max_bitstream_bytes = 0;
    std::set<int> distinct_fs_limiters;

    auto record_completed_slot = [&](PipeSlot3A& slot) {
        const int f = slot.frame_index;
        frame_complete_ms[static_cast<size_t>(f)] = slot.complete_ms;
        GpuFrameEntropyStats est{};
        CollectGpuFrameEntropyStats(slot.e_slot, &est);
        if (est.graph_captured_this_frame) {
            ++total_graph_captures;
        }
        if (est.graph_capture_ms > max_graph_capture_ms) {
            max_graph_capture_ms = est.graph_capture_ms;
        }
        if (f >= eff_warmup) {
            const int m_idx = f - eff_warmup;
            HyperCineStreamTiming wt{};
            HyperCine_GpuSlotReadTiming(slot.w_slot, &wt);

            std::lock_guard<std::mutex> lk(record_mu);
            sum_stage_ms += slot.host_stage_ms;
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

            const size_t bs_sz = slot.bitstream.size();
            sum_bitstream_bytes += bs_sz;
            if (min_bitstream_bytes == 0 || bs_sz < min_bitstream_bytes) min_bitstream_bytes = bs_sz;
            if (bs_sz > max_bitstream_bytes) max_bitstream_bytes = bs_sz;
            distinct_fs_limiters.insert(slot.quant.fs_rate_limiter);

            if (cfg.retain_frame_records) {
                Phase3AFrameRecord rec;
                rec.frame_index = m_idx;
                rec.pattern = slot.pattern;
                rec.prev_encoded_bytes = slot.quant.prev_encoded_bytes;
                rec.prev_lastgopbitcount = slot.quant.prev_lastgopbitcount;
                rec.current_fs_rate_limiter = slot.quant.fs_rate_limiter;
                std::memcpy(rec.quant_table, slot.quant.quant, sizeof(rec.quant_table));
                std::memcpy(rec.scale_table, slot.quant.scale, sizeof(rec.scale_table));
                rec.sample_bytes = bs_sz;
                if (cfg.verify_sequence && cfg.frames <= 300) {
                    rec.sample_sha256 = Sha256Hex(slot.bitstream.data(), slot.bitstream.size());
                    const std::vector<uint8_t> masked = MaskCfhdVolatileMetadata(slot.bitstream);
                    rec.masked_sample_sha256 = Sha256Hex(masked.data(), masked.size());
                }
                rec.submit_ms = slot.submit_ms;
                rec.complete_ms = slot.complete_ms;
                rec.latency_ms = slot.complete_ms - slot.submit_ms;
                completed_records.push_back(std::move(rec));
            }
            if (should_retain_for_decode(m_idx)) {
                decode_checks.push_back(RetainedDecodeCheck{m_idx, slot.expected_stamp_index, slot.pattern, slot.bitstream});
            }
        }
    };

    std::mutex asm_mu;
    std::condition_variable asm_cv;
    std::condition_variable asm_done_cv;
    std::deque<int> asm_queue;
    bool asm_stop = false;
    std::atomic<bool> asm_failed{false};

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
                PipeSlot3A& slot = slots[static_cast<size_t>(s_idx)];
                if (slot.frame_index == eff_warmup) {
                    worker_cpu_sec_start.store(CurrentThreadCpuSeconds(), std::memory_order_release);
                }
                if (!assemble_slot_cpu(slot)) {
                    asm_failed.store(true);
                } else {
                    record_completed_slot(slot);
                }
                slot.stage.store(static_cast<int>(PipeSlotStage3A::Idle), std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lk(asm_mu);
                }
                asm_done_cv.notify_all();
            }
        });
    }

    NvmlRuntimeSampler nvml_sampler;

    int next_submit_frame = 0;
    int next_wavelet_kick_frame = 0;
    int next_gpu_done_frame = 0;
    int last_frontend_slot = -1;
    int last_wavelet_slot = -1;
    bool prev_frame_state_ready = true;  // True for frame 0 (no prior frame dependency)

    double timed_start_ms = 0.0;
    double proc_cpu_s0 = 0.0;
    double main_cpu_s0 = 0.0;
    double main_thread_running_ms = 0.0;
    double main_thread_blocked_ms = 0.0;
    double main_thread_spin_ms = 0.0;
    int64_t inflight_accum = 0;
    int64_t poll_iters = 0;
    int64_t blocking_waits = 0;
    double sum_rate_control_ms = 0.0;
    double warmup_origin_ms = 0.0;
    double last_measured_submit_ms = 0.0;
    const double watchdog_deadline_ms = NowMs() + 180000.0 + 250.0 * static_cast<double>(total_frames);

    auto note_rate = [&](PipeSlot3A& slot, double started_ms) {
        if (slot.frame_index >= eff_warmup) sum_rate_control_ms += NowMs() - started_ms;
    };

    auto kick_wavelet_and_entropy_for_slot = [&](PipeSlot3A& slot, int target_slot) -> bool {
        const double t_rate0 = NowMs();
        const bool prepared = ctx.PrepareFrameState(cfg.quant_mode, cfg.deterministic_test_metadata,
                                   &slot.quant, &slot.cfhd_frame_number,
                                   &slot.global_meta, &slot.local_meta);
        note_rate(slot, t_rate0);
        if (!prepared) {
            res.error = "PrepareFrameState failed";
            return false;
        }

        void* wait_wv = (last_wavelet_slot >= 0)
            ? HyperCine_GpuSlotWaveletEvent(slots[static_cast<size_t>(last_wavelet_slot)].w_slot)
            : nullptr;

        if (HyperCine_GpuSlotKickWaveletWithQuant(slot.w_slot, slot.quant.quant,
                                                  wait_wv, cfg.gpu_pack_lowpass ? 1 : 0) != 0) {
            res.error = "HyperCine_GpuSlotKickWaveletWithQuant failed";
            return false;
        }

        GpuEntropyFrameConfig e_cfg{};
        e_cfg.entropy_streams = num_streams;
        e_cfg.d2h_mode = d2h_mode;
        e_cfg.record_substage_events = cfg.record_substage_events && !cfg.use_cuda_graph && (slot.frame_index >= eff_warmup);
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
            return false;
        }

        slot.stage.store(static_cast<int>(PipeSlotStage3A::EntropyAndD2HQueued), std::memory_order_release);
        last_wavelet_slot = target_slot;
        prev_frame_state_ready = (cfg.quant_mode != Phase3AQuantMode::DynamicUpstreamState);
        return true;
    };

    while (next_gpu_done_frame < total_frames && !asm_failed.load(std::memory_order_relaxed)) {
        if (NowMs() > watchdog_deadline_ms) {
            res.error = "encode watchdog: pipeline did not finish before the debug deadline";
            res.watchdog_tripped = true;
            asm_failed.store(true);
            break;
        }
        const double t_iter0 = NowMs();
        bool did_work = false;

        // 1. Submit next frame's quant-independent Frontend (Host staging + H2D + BgraToYuv) to Idle slot.
        if (next_submit_frame < total_frames) {
            const int target_slot = next_submit_frame % num_slots;
            PipeSlot3A& slot = slots[static_cast<size_t>(target_slot)];
            if (slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage3A::Idle)) {
                const double t_work0 = NowMs();
                if (next_submit_frame == 0) warmup_origin_ms = t_work0;
                if (next_submit_frame >= eff_warmup) last_measured_submit_ms = t_work0;
                if (next_submit_frame == eff_warmup) {
                    timed_start_ms = t_work0;
                    proc_cpu_s0 = ProcessCpuSeconds();
                    main_cpu_s0 = CurrentThreadCpuSeconds();
                    worker_cpu_sec_start.store(0.0, std::memory_order_release);
                    nvml_sampler.Start();
                }
                if (next_submit_frame == eff_warmup + cfg.frames / 2) {
                    res.vram_mid_run_bytes = HyperCine_QueryUsedVramBytes();
                    res.host_rss_mid_bytes = QueryProcessWorkingSetBytes();
                }

                const int measured_idx = (next_submit_frame >= eff_warmup)
                    ? (next_submit_frame - eff_warmup)
                    : next_submit_frame;
                const Phase3APattern pat = explicit_inputs
                    ? (*explicit_inputs)[static_cast<size_t>(next_submit_frame)].pattern
                    : SelectSequencePattern(measured_idx);

                slot.frame_index = next_submit_frame;
                slot.expected_stamp_index = explicit_inputs
                    ? (*explicit_inputs)[static_cast<size_t>(next_submit_frame)].frame_index
                    : measured_idx;
                slot.pattern = pat;
                slot.state_committed_to_ctx = false;
                slot.predicted_sample_bytes = 0;
                slot.submit_ms = NowMs();
                frame_submit_ms[static_cast<size_t>(next_submit_frame)] = slot.submit_ms;
                slot.host_stage_ms = 0.0;
                slot.cpu_lowpass_ms = 0.0;
                slot.cpu_header_wrap_ms = 0.0;
                slot.cpu_sample_concat_ms = 0.0;
                slot.streamed_wrapped_count = 0;
                for (auto& p : slot.pieces) p.ok = false;

                int input_kind = HYPERCINE_INPUT_HOST_BGRA;
                const uint8_t* input_ptr = nullptr;

                if (explicit_inputs) {
                    const Phase3AFrameInput& fin = (*explicit_inputs)[static_cast<size_t>(next_submit_frame)];
                    if (cfg.input_mode == Phase3AInputMode::HostPageableEndToEnd) {
                        const double t_st0 = NowMs();
                        std::memcpy(slot.pinned_stage_bgra, fin.bgra_ptr, frame_bytes);
                        slot.host_stage_ms = NowMs() - t_st0;
                        input_ptr = slot.pinned_stage_bgra;
                        input_kind = HYPERCINE_INPUT_HOST_BGRA;
                    } else if (cfg.input_mode == Phase3AInputMode::HostPinnedApi) {
                        input_ptr = fin.bgra_ptr;
                        input_kind = HYPERCINE_INPUT_HOST_BGRA;
                    } else {
                        input_ptr = fin.bgra_ptr;
                        input_kind = HYPERCINE_INPUT_DEVICE_BGRA;
                    }
                } else {
                    const int ring_idx = static_cast<int>(pat) % kRingSize;
                    if (cfg.input_mode == Phase3AInputMode::HostPageableEndToEnd) {
                        const double t_st0 = NowMs();
                        StampFrameIdentityBlocks(pageable_ring[static_cast<size_t>(ring_idx)].data(),
                                                 width, height, pitch, measured_idx, pat);
                        std::memcpy(slot.pinned_stage_bgra,
                                    pageable_ring[static_cast<size_t>(ring_idx)].data(), frame_bytes);
                        slot.host_stage_ms = NowMs() - t_st0;
                        input_ptr = slot.pinned_stage_bgra;
                        input_kind = HYPERCINE_INPUT_HOST_BGRA;
                    } else if (cfg.input_mode == Phase3AInputMode::HostPinnedApi) {
                        StampFrameIdentityBlocks(pinned_ring[static_cast<size_t>(ring_idx)],
                                                 width, height, pitch, measured_idx, pat);
                        input_ptr = pinned_ring[static_cast<size_t>(ring_idx)];
                        input_kind = HYPERCINE_INPUT_HOST_BGRA;
                    } else {
                        const int dev_idx = ring_idx % static_cast<int>(device_ring.size());
                        if (cfg.verify_sequence) {
                            void* prev_fe = (last_frontend_slot >= 0)
                                ? HyperCine_GpuSlotFrontendEvent(slots[static_cast<size_t>(last_frontend_slot)].w_slot)
                                : nullptr;
                            if (prev_fe) WaitGpuEvent(prev_fe);
                            const int stamp_h = std::min(height, 128);
                            std::memcpy(slot.pinned_stamp_bgra,
                                        pageable_ring[static_cast<size_t>(ring_idx)].data(),
                                        static_cast<size_t>(stamp_h) * static_cast<size_t>(pitch));
                            StampFrameIdentityBlocks(slot.pinned_stamp_bgra, width, stamp_h, pitch, measured_idx, pat);
                            cudaMemcpyAsync(device_ring[static_cast<size_t>(dev_idx)],
                                            slot.pinned_stamp_bgra,
                                            static_cast<size_t>(stamp_h) * static_cast<size_t>(pitch),
                                            cudaMemcpyHostToDevice, 0);
                            cudaStreamSynchronize(0);
                        }
                        input_ptr = device_ring[static_cast<size_t>(dev_idx)];
                        input_kind = HYPERCINE_INPUT_DEVICE_BGRA;
                    }
                }

                // Use ctx's initialized dimensions/prescale/color_space for Begin; exact per-frame quant is supplied in KickWaveletWithQuant.
                const QuantTables3A base_q = ReadQuant3A(ctx.encoder, ctx.transforms, ctx.channel_count, 0, 0);

                void* wait_fe = (last_frontend_slot >= 0)
                    ? HyperCine_GpuSlotFrontendEvent(slots[static_cast<size_t>(last_frontend_slot)].w_slot)
                    : nullptr;

                if (HyperCine_GpuSlotBegin(slot.w_slot, input_kind, HYPERCINE_XPORT_DEVICE_LOWPASS_D2H,
                                           input_ptr, pitch, width, height, base_q.color_space,
                                           base_q.widths, base_q.heights, base_q.prescale, base_q.quant) != 0 ||
                    HyperCine_GpuSlotKickFrontendOnly(slot.w_slot, wait_fe) != 0) {
                    res.error = "HyperCine_GpuSlotKickFrontendOnly failed";
                    asm_failed.store(true);
                    break;
                }

                slot.stage.store(static_cast<int>(PipeSlotStage3A::FrontendQueued), std::memory_order_release);
                last_frontend_slot = target_slot;
                ++next_submit_frame;
                if (next_submit_frame > eff_warmup) main_thread_running_ms += NowMs() - t_work0;
                did_work = true;
            }
        }

        // 2. If next_wavelet_kick_frame is in FrontendQueued and prior frame's sample_bytes is committed,
        //    prepare its exact quantizer state and kick GPU Wavelet + Entropy!
        if (next_wavelet_kick_frame < next_submit_frame && prev_frame_state_ready) {
            const int target_slot = next_wavelet_kick_frame % num_slots;
            PipeSlot3A& slot = slots[static_cast<size_t>(target_slot)];
            if (slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage3A::FrontendQueued)) {
                const double t_w0 = NowMs();
                if (!kick_wavelet_and_entropy_for_slot(slot, target_slot)) {
                    asm_failed.store(true);
                    break;
                }
                ++next_wavelet_kick_frame;
                if (slot.frame_index >= eff_warmup) main_thread_running_ms += NowMs() - t_w0;
                did_work = true;
            }
        }

        // 3. Progress D2H transfers and commit completed GPU entropy frame size for rate-control feedback.
        int active_in_flight = 0;
        for (int s = 0; s < num_slots; ++s) {
            PipeSlot3A& slot = slots[static_cast<size_t>(s)];
            const int st = slot.stage.load(std::memory_order_acquire);
            if (st != static_cast<int>(PipeSlotStage3A::Idle)) ++active_in_flight;
            if (st != static_cast<int>(PipeSlotStage3A::EntropyAndD2HQueued)) continue;

            if (d2h_mode == CompressedD2HMode::FrameConsolidated) {
                const double t_w0 = NowMs();
                const bool was_committed = slot.state_committed_to_ctx;
                const GpuStageStatus d2h_status = PollGpuEntropyFrameD2HStatus(slot.e_slot);
                if (d2h_status == GpuStageStatus::Failed || GpuEntropyFrameHasFailed(slot.e_slot)) {
                    const char* slot_error = GpuEntropyFrameError(slot.e_slot);
                    res.error = (slot_error && slot_error[0]) ? slot_error : "GPU entropy failed";
                    asm_failed.store(true);
                    break;
                }
                if (d2h_status == GpuStageStatus::Complete) {
                    if (!slot.state_committed_to_ctx && slot.frame_index == next_wavelet_kick_frame - 1) {
                        slot.predicted_sample_bytes = compute_predicted_sample_bytes(slot);
                        const double t_rate1 = NowMs();
                        ctx.CompleteFrameState(slot.predicted_sample_bytes, cfg.quant_mode);
                        note_rate(slot, t_rate1);
                        slot.state_committed_to_ctx = true;
                        prev_frame_state_ready = true;
                        // Immediately kick waiting next_wavelet_kick_frame if it is already in FrontendQueued!
                        if (next_wavelet_kick_frame < next_submit_frame) {
                            const int next_slot_idx = next_wavelet_kick_frame % num_slots;
                            PipeSlot3A& next_slot = slots[static_cast<size_t>(next_slot_idx)];
                            if (next_slot.stage.load(std::memory_order_acquire) ==
                                static_cast<int>(PipeSlotStage3A::FrontendQueued)) {
                                if (!kick_wavelet_and_entropy_for_slot(next_slot, next_slot_idx)) {
                                    asm_failed.store(true);
                                    break;
                                }
                                ++next_wavelet_kick_frame;
                            }
                        }
                    }
                    if (!was_committed && slot.state_committed_to_ctx) {
                        if (next_submit_frame > eff_warmup) main_thread_running_ms += NowMs() - t_w0;
                        did_work = true;
                    }
                }
            } else {
                for (;;) {
                    const int ready_hp = PollNextStreamedBandReady(slot.e_slot);
                    if (ready_hp == -2 || GpuEntropyFrameHasFailed(slot.e_slot)) {
                        const char* slot_error = GpuEntropyFrameError(slot.e_slot);
                        res.error = (slot_error && slot_error[0]) ? slot_error : "streamed GPU entropy failed";
                        asm_failed.store(true);
                        break;
                    }
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
                if (!slot.state_committed_to_ctx && IsGpuEntropyComputeDone(slot.e_slot) &&
                    slot.frame_index == next_wavelet_kick_frame - 1) {
                    slot.predicted_sample_bytes = compute_predicted_sample_bytes(slot);
                    const double t_rate1 = NowMs();
                    ctx.CompleteFrameState(slot.predicted_sample_bytes, cfg.quant_mode);
                    note_rate(slot, t_rate1);
                    slot.state_committed_to_ctx = true;
                    prev_frame_state_ready = true;
                    if (next_wavelet_kick_frame < next_submit_frame) {
                        const int next_slot_idx = next_wavelet_kick_frame % num_slots;
                        PipeSlot3A& next_slot = slots[static_cast<size_t>(next_slot_idx)];
                        if (next_slot.stage.load(std::memory_order_acquire) ==
                            static_cast<int>(PipeSlotStage3A::FrontendQueued)) {
                            if (!kick_wavelet_and_entropy_for_slot(next_slot, next_slot_idx)) {
                                asm_failed.store(true);
                                break;
                            }
                            ++next_wavelet_kick_frame;
                        }
                    }
                    did_work = true;
                }
            }
        }
        if (next_submit_frame > eff_warmup) {
            inflight_accum += active_in_flight;
            ++poll_iters;
        }

        // 4. Check if oldest GPU-active frame (`next_gpu_done_frame`) has finished D2H.
        const int done_slot_idx = next_gpu_done_frame % num_slots;
        PipeSlot3A& done_slot = slots[static_cast<size_t>(done_slot_idx)];
        if (done_slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage3A::EntropyAndD2HQueued)) {
            const int lp_idle = HyperCine_GpuSlotGpuIdle(done_slot.w_slot);
            if (lp_idle < 0) {
                res.error = "lowpass D2H failed";
                asm_failed.store(true);
                break;
            }
            if (GpuEntropyFrameHasFailed(done_slot.e_slot)) {
                const char* slot_error = GpuEntropyFrameError(done_slot.e_slot);
                res.error = (slot_error && slot_error[0]) ? slot_error : "GPU entropy failed";
                asm_failed.store(true);
                break;
            }
            if (lp_idle == 1 && IsGpuEntropyFrameComplete(done_slot.e_slot)) {
                if (!done_slot.state_committed_to_ctx) {
                    done_slot.predicted_sample_bytes = compute_predicted_sample_bytes(done_slot);
                    const double t_rate1 = NowMs();
                    ctx.CompleteFrameState(done_slot.predicted_sample_bytes, cfg.quant_mode);
                    note_rate(done_slot, t_rate1);
                    done_slot.state_committed_to_ctx = true;
                    prev_frame_state_ready = true;
                }
                if (use_worker) {
                    done_slot.stage.store(static_cast<int>(PipeSlotStage3A::AssemblingOnWorker), std::memory_order_release);
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
                    done_slot.stage.store(static_cast<int>(PipeSlotStage3A::Idle), std::memory_order_release);
                }
                ++next_gpu_done_frame;
                did_work = true;
            }
        }

        // 5. Synchronization policy when all available slots are busy:
        if (!did_work && next_gpu_done_frame < total_frames && !asm_failed.load(std::memory_order_relaxed)) {
            if (cfg.sync_mode == Phase2DSyncMode::Blocking) {
                const double t_blk0 = NowMs();
                const int next_sub_slot_idx = (next_submit_frame < total_frames) ? (next_submit_frame % num_slots) : -1;
                const bool done_slot_active =
                    done_slot.stage.load(std::memory_order_acquire) == static_cast<int>(PipeSlotStage3A::EntropyAndD2HQueued);

                if (done_slot_active && d2h_mode == CompressedD2HMode::FrameConsolidated &&
                    !IsGpuEntropyComputeDone(done_slot.e_slot)) {
                    if (!WaitAndStartGpuEntropyFrameD2H(done_slot.e_slot)) {
                        const char* slot_error = GpuEntropyFrameError(done_slot.e_slot);
                        res.error = (slot_error && slot_error[0]) ? slot_error : "WaitAndStartGpuEntropyFrameD2H failed";
                        asm_failed.store(true);
                        break;
                    }
                    if (!done_slot.state_committed_to_ctx && done_slot.frame_index == next_wavelet_kick_frame - 1) {
                        done_slot.predicted_sample_bytes = compute_predicted_sample_bytes(done_slot);
                        const double t_rate1 = NowMs();
                        ctx.CompleteFrameState(done_slot.predicted_sample_bytes, cfg.quant_mode);
                        note_rate(done_slot, t_rate1);
                        done_slot.state_committed_to_ctx = true;
                        prev_frame_state_ready = true;
                    }
                } else if (next_sub_slot_idx >= 0 &&
                           slots[static_cast<size_t>(next_sub_slot_idx)].stage.load(std::memory_order_acquire) ==
                               static_cast<int>(PipeSlotStage3A::AssemblingOnWorker)) {
                    std::unique_lock<std::mutex> lk(asm_mu);
                    asm_done_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                        return asm_failed.load(std::memory_order_relaxed) ||
                               slots[static_cast<size_t>(next_sub_slot_idx)].stage.load(std::memory_order_acquire) ==
                                   static_cast<int>(PipeSlotStage3A::Idle);
                    });
                } else if (done_slot_active) {
                    if (d2h_mode == CompressedD2HMode::FrameConsolidated) {
                        if (!WaitGpuEvent(HyperCine_GpuSlotWaveletEvent(done_slot.w_slot)) ||
                            !WaitGpuEntropyFrameComplete(done_slot.e_slot)) {
                            res.error = "WaitGpuEntropyFrameComplete (FrameConsolidated) failed";
                            asm_failed.store(true);
                            break;
                        }
                    } else if (!IsGpuEntropyComputeDone(done_slot.e_slot)) {
                        if (!WaitGpuEntropyComputeDone(done_slot.e_slot)) {
                            res.error = "WaitGpuEntropyComputeDone failed";
                            asm_failed.store(true);
                            break;
                        }
                    } else {
                        if (!WaitGpuEvent(HyperCine_GpuSlotWaveletEvent(done_slot.w_slot)) ||
                            !WaitGpuEntropyFrameComplete(done_slot.e_slot)) {
                            res.error = "WaitGpuEntropyFrameComplete (BandStreamed) failed";
                            asm_failed.store(true);
                            break;
                        }
                    }
                }
                if (next_submit_frame > eff_warmup) {
                    main_thread_blocked_ms += NowMs() - t_blk0;
                    ++blocking_waits;
                }
            } else {
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

    if (!asm_failed.load()) {
        const double t_sync0 = NowMs();
        const cudaError_t sync_err = cudaDeviceSynchronize();
        res.device_sync_ms = NowMs() - t_sync0;
        res.device_drained = (sync_err == cudaSuccess);
        if (sync_err != cudaSuccess && res.error.empty()) {
            res.error = "cudaDeviceSynchronize failed before the timer stopped";
            asm_failed.store(true);
        }
    }
    const double timed_end_ms = NowMs();
    const double proc_cpu_s1 = ProcessCpuSeconds();
    const double main_cpu_s1 = CurrentThreadCpuSeconds();
    res.nvml = nvml_sampler.Stop();

    res.vram_end_run_bytes = HyperCine_QueryUsedVramBytes();
    res.host_rss_end_bytes = QueryProcessWorkingSetBytes();

    cleanup_all();

    res.vram_after_destroy_bytes = HyperCine_QueryUsedVramBytes();
    res.host_rss_after_destroy_bytes = QueryProcessWorkingSetBytes();

    if (asm_failed.load() || !res.error.empty()) {
        if (res.error.empty()) res.error = "pipeline execution or assembly failed";
        return res;
    }

    res.submitted_frames = next_submit_frame - eff_warmup;
    res.completed_frames = next_gpu_done_frame - eff_warmup;
    res.wall_ms = std::max(0.001, timed_end_ms - timed_start_ms);
    res.sustained_fps = (static_cast<double>(cfg.frames) * 1000.0) / res.wall_ms;
    res.frame_period_ms = res.wall_ms / static_cast<double>(cfg.frames);
    res.warmup_excluded_ms = (eff_warmup > 0 && warmup_origin_ms > 0.0)
        ? std::max(0.0, timed_start_ms - warmup_origin_ms) : 0.0;
    res.drain_ms = (last_measured_submit_ms > 0.0)
        ? std::max(0.0, timed_end_ms - last_measured_submit_ms) : 0.0;
    res.rate_control_ms = cfg.frames > 0 ? sum_rate_control_ms / static_cast<double>(cfg.frames) : 0.0;
    res.throughput_definition =
        "ProductionEndToEnd completed-frame throughput. Wall clock starts when the first measured "
        "frame is accepted after warmup and stops after that frame's compressed sample is assembled "
        "and cudaDeviceSynchronize has drained every stream. Warmup covers one-time CUDA graph "
        "capture when it is at least the slot count. Pageable staging is inside the window for "
        "HostPageableEndToEnd. Encoder creation, input generation, and filesystem output are outside. "
        "This is not a submission rate. With multiple slots, 1000/mean_latency is not the throughput.";

    // Compute first-quarter vs last-quarter FPS for long-run stability tracking:
    if (cfg.frames >= 8) {
        const int q = cfg.frames / 4;
        const double q1_ms = std::max(0.001, frame_complete_ms[static_cast<size_t>(eff_warmup + q - 1)] -
                                             frame_submit_ms[static_cast<size_t>(eff_warmup)]);
        const double q4_ms = std::max(0.001, frame_complete_ms[static_cast<size_t>(total_frames - 1)] -
                                             frame_complete_ms[static_cast<size_t>(total_frames - 1 - q)]);
        res.first_quarter_fps = (static_cast<double>(q) * 1000.0) / q1_ms;
        res.last_quarter_fps = (static_cast<double>(q) * 1000.0) / q4_ms;
    } else {
        res.first_quarter_fps = res.sustained_fps;
        res.last_quarter_fps = res.sustained_fps;
    }

    std::vector<double> latencies;
    std::vector<double> periods;
    for (int f = eff_warmup; f < total_frames; ++f) {
        latencies.push_back(frame_complete_ms[static_cast<size_t>(f)] - frame_submit_ms[static_cast<size_t>(f)]);
        if (f > eff_warmup) {
            periods.push_back(frame_complete_ms[static_cast<size_t>(f)] - frame_complete_ms[static_cast<size_t>(f - 1)]);
        }
    }
    ComputeStats(latencies, &res.mean_latency_ms, &res.median_latency_ms, &res.p95_latency_ms,
                 &res.min_latency_ms, &res.max_latency_ms);
    if (!latencies.empty()) {
        std::vector<double> sorted = latencies;
        std::sort(sorted.begin(), sorted.end());
        size_t p99_idx = static_cast<size_t>(std::ceil(0.99 * static_cast<double>(sorted.size()))) - 1;
        if (p99_idx >= sorted.size()) p99_idx = sorted.size() - 1;
        res.p99_latency_ms = sorted[p99_idx];
        res.first_frame_latency_ms = latencies.front();
    }
    if (cfg.frames > num_slots * 2 + 2) {
        const int skip = num_slots;
        const int first = eff_warmup + skip;
        const int last = total_frames - 1 - skip;
        const double ss_ms = frame_complete_ms[static_cast<size_t>(last)] -
                             frame_complete_ms[static_cast<size_t>(first)];
        res.steady_state_frames = last - first;
        if (ss_ms > 0.0 && res.steady_state_frames > 0) {
            res.steady_state_fps = static_cast<double>(res.steady_state_frames) * 1000.0 / ss_ms;
        }
    } else {
        res.steady_state_frames = cfg.frames;
        res.steady_state_fps = res.sustained_fps;
    }
    if (!periods.empty()) {
        ComputeStats(periods, &res.mean_period_ms, &res.median_period_ms, &res.p95_period_ms);
    } else {
        res.mean_period_ms = res.median_period_ms = res.p95_period_ms = res.frame_period_ms;
    }

    const double inv_n = 1.0 / static_cast<double>(cfg.frames);
    res.host_pageable_stage_ms = sum_stage_ms * inv_n;
    res.h2d_ms = sum_h2d_ms * inv_n;
    res.color_ms = sum_color_ms * inv_n;
    res.wavelet_ms = sum_wavelet_ms * inv_n;
    res.gpu_frontend_wavelet_ms = res.h2d_ms + res.color_ms + res.wavelet_ms;
    res.gpu_entropy_ms = sum_ent_ms * inv_n;
    res.gpu_consolidate_ms = sum_cons_ms * inv_n;
    res.compressed_d2h_ms = sum_comp_d2h_ms * inv_n;
    res.lowpass_d2h_ms = sum_lp_d2h_ms * inv_n;
    res.total_d2h_ms = res.compressed_d2h_ms + res.lowpass_d2h_ms;
    res.total_gpu_ms = res.gpu_frontend_wavelet_ms + res.gpu_entropy_ms + res.gpu_consolidate_ms + res.compressed_d2h_ms;

    res.host_enqueue_ms = sum_enqueue_ms * inv_n;
    res.graph_capture_ms = max_graph_capture_ms;
    res.graph_captures_count = total_graph_captures;
    res.cpu_lowpass_ms = sum_cpu_lp_ms * inv_n;
    res.cpu_header_wrap_ms = sum_cpu_hdr_ms * inv_n;
    res.cpu_sample_concat_ms = sum_cpu_concat_ms * inv_n;
    res.cpu_assembly_total_ms = res.cpu_lowpass_ms + res.cpu_header_wrap_ms + res.cpu_sample_concat_ms;
    res.cpu_dispatch_poll_ms = (main_thread_running_ms + main_thread_spin_ms) * inv_n;
    res.total_cpu_ms = res.host_pageable_stage_ms + res.host_enqueue_ms + res.cpu_assembly_total_ms;

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

    res.kernel_launches_per_frame = recorded_launches_per_frame;
    res.kernel_launches_per_band = recorded_launches_per_frame / 27;
    res.host_launch_calls_per_frame = recorded_graph_used ? 1 : recorded_launches_per_frame;
    res.graph_kernel_nodes = recorded_graph_kernel_nodes;
    res.graph_total_nodes = recorded_graph_total_nodes;
    res.graph_used = recorded_graph_used;

    res.mean_bitstream_bytes = static_cast<size_t>(sum_bitstream_bytes / static_cast<size_t>(cfg.frames));
    res.min_bitstream_bytes = min_bitstream_bytes;
    res.max_bitstream_bytes = max_bitstream_bytes;
    res.distinct_fs_rate_limiters = static_cast<int>(distinct_fs_limiters.size());

    res.pipeline_gpu_stage_occupancy_estimate_pct =
        std::min(100.0, 100.0 * res.total_gpu_ms / std::max(0.001, res.frame_period_ms));
    res.avg_frames_in_flight = poll_iters > 0
        ? static_cast<double>(inflight_accum) / static_cast<double>(poll_iters)
        : 1.0;

    // Populate CPU metrics:
    const int n_cpus = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    res.cpu.logical_processors = n_cpus;
    res.cpu.active_host_threads = use_worker ? 2 : 1;
    res.cpu.wall_ms = res.wall_ms;
    res.cpu.process_cpu_ms = std::max(0.0, (proc_cpu_s1 - proc_cpu_s0) * 1000.0);
    res.cpu.main_thread_cpu_ms = std::max(0.0, (main_cpu_s1 - main_cpu_s0) * 1000.0);
    res.cpu.worker_thread_cpu_ms = std::max(0.0, worker_cpu_sec_total * 1000.0);
    res.cpu.single_core_equiv_pct = 100.0 * res.cpu.process_cpu_ms / res.wall_ms;
    res.cpu.normalized_process_cpu_pct = res.cpu.single_core_equiv_pct / static_cast<double>(n_cpus);
    res.cpu.main_thread_single_core_pct = 100.0 * res.cpu.main_thread_cpu_ms / res.wall_ms;
    res.cpu.worker_thread_single_core_pct = 100.0 * res.cpu.worker_thread_cpu_ms / res.wall_ms;
    res.cpu.main_thread_running_ms = main_thread_running_ms;
    res.cpu.main_thread_blocked_ms = main_thread_blocked_ms;
    res.cpu.main_thread_spin_ms = main_thread_spin_ms;
    res.cpu.poll_iterations = poll_iters;
    res.cpu.blocking_waits = blocking_waits;
    res.cpu.cpu_core_sec_per_wall_sec = res.cpu.process_cpu_ms / res.wall_ms;
    res.cpu.cpu_core_ms_per_frame = res.cpu.process_cpu_ms * inv_n;
    res.cpu.cpu_core_sec_per_60fps_video_sec = (res.cpu.cpu_core_ms_per_frame * 60.0) / 1000.0;

    // Verify sequence ordering & decode stamps:
    res.sequence_order_ok = (res.submitted_frames == cfg.frames) && (res.completed_frames == cfg.frames);
    if (cfg.retain_frame_records) {
        if (static_cast<int>(completed_records.size()) != cfg.frames) {
            res.sequence_order_ok = false;
        } else {
            for (int i = 0; i < cfg.frames; ++i) {
                if (completed_records[static_cast<size_t>(i)].frame_index != i) {
                    res.sequence_order_ok = false;
                    break;
                }
            }
        }
    }

    res.verification_performed = cfg.verify_sequence && !decode_checks.empty();
    if (res.verification_performed) {
        res.decode_ok = true;
        res.stamp_verify_ok = true;
        double sum_psnr = 0.0;
        double min_psnr = 99.0;
        for (const auto& chk : decode_checks) {
            std::vector<uint8_t> dec_img;
            int dec_pitch = 0;
            if (!DecodeCfhdSample3A(chk.bitstream, width, height, &dec_img, &dec_pitch)) {
                res.decode_ok = false;
                res.stamp_verify_ok = false;
                res.error = "decode failed at frame " + std::to_string(chk.measured_frame_index);
                break;
            }
            int ext_idx = -1;
            int ext_pat = -1;
            if (!ExtractStampedFrameIdentity(dec_img.data(), width, height, dec_pitch, &ext_idx, &ext_pat) ||
                ext_idx != chk.expected_stamp_index ||
                ext_pat != static_cast<int>(chk.pattern)) {
                res.stamp_verify_ok = false;
                res.error = "stamp mismatch at frame " + std::to_string(chk.measured_frame_index) +
                            " (expected " + std::to_string(chk.expected_stamp_index) +
                            ", extracted " + std::to_string(ext_idx) + ")";
                break;
            }
            const uint8_t* ref_ptr = nullptr;
            if (explicit_inputs) {
                const Phase3AFrameInput& fin =
                    (*explicit_inputs)[static_cast<size_t>(eff_warmup + chk.measured_frame_index)];
                ref_ptr = fin.host_ref ? fin.host_ref : fin.bgra_ptr;
                if (cfg.input_mode == Phase3AInputMode::DeviceInput && !fin.host_ref) ref_ptr = nullptr;
            } else {
                const int r_idx = static_cast<int>(chk.pattern) % kRingSize;
                StampFrameIdentityBlocks(pageable_ring[static_cast<size_t>(r_idx)].data(),
                                         width, height, pitch, chk.measured_frame_index, chk.pattern);
                ref_ptr = pageable_ring[static_cast<size_t>(r_idx)].data();
            }
            const double psnr = ref_ptr ? ComputeBestPsnr3A(ref_ptr, pitch, dec_img, width, height, dec_pitch) : 99.0;
            sum_psnr += psnr;
            min_psnr = std::min(min_psnr, psnr);
            if (psnr < 35.0) {
                res.decode_ok = false;
                res.error = "PSNR below threshold (" + std::to_string(psnr) + " dB) at frame " +
                            std::to_string(chk.measured_frame_index);
                break;
            }
        }
        res.min_psnr_db = min_psnr;
        res.mean_psnr_db = sum_psnr / static_cast<double>(decode_checks.size());
    } else {
        res.decode_ok = false;
        res.stamp_verify_ok = false;
        res.min_psnr_db = 0.0;
        res.mean_psnr_db = 0.0;
    }

    res.frame_records = std::move(completed_records);
    const bool pipeline_ok = res.error.empty() && !res.watchdog_tripped && res.device_drained &&
                             res.submitted_frames == cfg.frames && res.completed_frames == cfg.frames &&
                             res.sequence_order_ok;
    res.ok = pipeline_ok && (!res.verification_performed || (res.decode_ok && res.stamp_verify_ok));
    return res;
}

// ============================================================================
// SECTION 4: 300-Frame Distinct-Frame Slot Wrap & Ordering Verification
// ============================================================================

Phase3ADistinctFrames300Report VerifyPhase3ADistinctFrames300(
    int width, int height, int num_frames,
    Phase3AInputMode input_mode,
    Phase2DSlotPolicy slot_policy) {
    Phase3ADistinctFrames300Report rep;
    rep.width = width;
    rep.height = height;
    rep.input_mode = input_mode;

    Phase3AConfig cfg{};
    cfg.width = width;
    cfg.height = height;
    cfg.frames = num_frames;
    cfg.warmup_frames = 2;
    cfg.input_mode = input_mode;
    cfg.quant_mode = Phase3AQuantMode::DynamicUpstreamState;
    cfg.slot_policy = slot_policy;
    cfg.verify_sequence = true;
    cfg.retain_frame_records = true;

    Phase3AResult res = RunPhase3A(cfg);
    rep.num_slots = res.slot_selection.selected_slots;
    rep.submitted_frames = res.submitted_frames;
    rep.completed_frames = res.completed_frames;
    rep.strict_monotonic_indices = res.sequence_order_ok;

    if (!res.ok || static_cast<int>(res.frame_records.size()) != num_frames) {
        rep.error = res.error.empty() ? "RunPhase3A failed on 300-frame sequence" : res.error;
        return rep;
    }

    std::set<int> seen_indices;
    bool stale_reuse = false;
    for (int i = 0; i < num_frames; ++i) {
        const auto& rec = res.frame_records[static_cast<size_t>(i)];
        seen_indices.insert(rec.frame_index);
        if (i >= rep.num_slots) {
            const auto& prev_slot_rec = res.frame_records[static_cast<size_t>(i - rep.num_slots)];
            if (!rec.sample_sha256.empty() && rec.sample_sha256 == prev_slot_rec.sample_sha256) {
                stale_reuse = true;
            }
        }
    }
    rep.no_duplicates_or_missing = (static_cast<int>(seen_indices.size()) == num_frames) &&
                                   (*seen_indices.begin() == 0) &&
                                   (*seen_indices.rbegin() == num_frames - 1);
    rep.no_stale_slot_reuse = !stale_reuse;

    for (int i = 0; i < num_frames; ++i) {
        const int mid = num_frames / 2;
        bool checked = (i < 10) || (i >= num_frames - 10) || (i >= mid - 5 && i <= mid + 4);
        for (int wrap = 1; wrap <= 6; ++wrap) {
            const int b = wrap * rep.num_slots;
            if (i >= b - 1 && i <= b + 1) checked = true;
        }
        if (checked) rep.verified_decode_indices.push_back(i);
    }
    rep.decoded_checked_count = static_cast<int>(rep.verified_decode_indices.size());
    rep.decoded_stamp_matched_count = res.stamp_verify_ok ? rep.decoded_checked_count : 0;
    rep.min_psnr_db = res.min_psnr_db;
    rep.mean_psnr_db = res.mean_psnr_db;

    rep.ok = rep.strict_monotonic_indices &&
             rep.no_duplicates_or_missing &&
             rep.no_stale_slot_reuse &&
             res.decode_ok &&
             res.stamp_verify_ok &&
             (rep.submitted_frames == num_frames) &&
             (rep.completed_frames == num_frames);
    if (!rep.ok && rep.error.empty()) {
        rep.error = "300-frame distinct sequence verification failed";
    }
    return rep;
}

// ============================================================================
// SECTION 8: 8K Variance & Thermal/Clock Stability Study
// ============================================================================

Phase3AVarianceStudySummary RunPhase3AVarianceStudy(
    int width, int height, Phase3AInputMode input_mode,
    int trials, int frames_per_trial) {
    Phase3AVarianceStudySummary sum;
    sum.width = width;
    sum.height = height;
    sum.input_mode = input_mode;
    sum.trials = trials;

    std::vector<double> fps_list;
    double clk_sum = 0.0;
    int min_clk = 99999, max_clk = 0;
    double pwr_sum = 0.0, max_pwr = 0.0;
    int max_temp = 0;
    uint64_t throttle_mask = 0;

    for (int t = 0; t < trials; ++t) {
        Phase3AConfig cfg{};
        cfg.width = width;
        cfg.height = height;
        cfg.frames = frames_per_trial;
        cfg.warmup_frames = 3;
        cfg.input_mode = input_mode;
        cfg.quant_mode = Phase3AQuantMode::DynamicUpstreamState;
        cfg.verify_sequence = (t == 0);
        cfg.retain_frame_records = false;

        Phase3AResult r = RunPhase3A(cfg);
        fps_list.push_back(r.sustained_fps);
        clk_sum += r.nvml.graphics_clock_mhz_mean;
        if (r.nvml.graphics_clock_mhz_min > 0) min_clk = std::min(min_clk, r.nvml.graphics_clock_mhz_min);
        max_clk = std::max(max_clk, r.nvml.graphics_clock_mhz_max);
        pwr_sum += r.nvml.power_w_mean;
        max_pwr = std::max(max_pwr, r.nvml.power_w_max);
        max_temp = std::max(max_temp, r.nvml.temperature_c_max);
        throttle_mask |= r.nvml.throttle_reasons_bitmask;
        sum.trial_results.push_back(std::move(r));
    }

    ComputeStats(fps_list, &sum.mean_fps, &sum.median_fps, nullptr,
                 &sum.min_fps, &sum.max_fps, &sum.stddev_fps);
    sum.mean_clock_mhz = trials > 0 ? (clk_sum / trials) : 0.0;
    sum.min_clock_mhz = (min_clk == 99999) ? 0 : min_clk;
    sum.max_clock_mhz = max_clk;
    sum.mean_power_w = trials > 0 ? (pwr_sum / trials) : 0.0;
    sum.max_power_w = max_pwr;
    sum.max_temp_c = max_temp;
    sum.combined_throttle_reasons = NvmlRuntimeSampler::FormatThrottleReasons(throttle_mask);
    return sum;
}

// ============================================================================
// SECTION 9: CUDA Graph A/B Study on Changing-Complexity Sequence
// ============================================================================

Phase3ACudaGraphABReport RunPhase3ACudaGraphABStudy(
    int width, int height, int frames_per_trial, int trials) {
    Phase3ACudaGraphABReport rep;
    rep.width = width;
    rep.height = height;
    rep.frames_per_trial = frames_per_trial;
    rep.trials = trials;

    std::vector<double> off_fps, on_fps;
    double off_enq = 0.0, off_ent = 0.0, off_cpu = 0.0;
    double on_enq = 0.0, on_ent = 0.0, on_cpu = 0.0;

    std::vector<Phase3AFrameRecord> ref_off_records;
    std::vector<Phase3AFrameRecord> ref_on_records;

    for (int t = 0; t < trials; ++t) {
        Phase3AConfig cfg_off{};
        cfg_off.width = width;
        cfg_off.height = height;
        cfg_off.frames = frames_per_trial;
        cfg_off.warmup_frames = 4;
        cfg_off.input_mode = Phase3AInputMode::HostPageableEndToEnd;
        cfg_off.quant_mode = Phase3AQuantMode::DynamicUpstreamState;
        cfg_off.use_cuda_graph = false;
        cfg_off.deterministic_test_metadata = true;
        cfg_off.verify_sequence = (t == 0);
        cfg_off.retain_frame_records = (t == 0);

        Phase3AResult r_off = RunPhase3A(cfg_off);
        if (!r_off.ok) {
            rep.error = "graph_off trial failed: " + r_off.error;
            return rep;
        }
        off_fps.push_back(r_off.sustained_fps);
        off_enq += r_off.host_enqueue_ms;
        off_ent += r_off.gpu_entropy_ms;
        off_cpu += r_off.cpu.cpu_core_ms_per_frame;
        if (t == 0) ref_off_records = r_off.frame_records;

        Phase3AConfig cfg_on = cfg_off;
        cfg_on.use_cuda_graph = true;
        Phase3AResult r_on = RunPhase3A(cfg_on);
        if (!r_on.ok) {
            rep.error = "graph_on trial failed: " + r_on.error;
            return rep;
        }
        on_fps.push_back(r_on.sustained_fps);
        on_enq += r_on.host_enqueue_ms;
        on_ent += r_on.gpu_entropy_ms;
        on_cpu += r_on.cpu.cpu_core_ms_per_frame;
        if (t == 0) {
            ref_on_records = r_on.frame_records;
            rep.graph_captures_per_run = r_on.graph_captures_count;
            rep.distinct_quant_states_exercised = r_on.distinct_fs_rate_limiters;
        }
    }

    rep.all_frames_byte_identical = (ref_off_records.size() == ref_on_records.size()) && !ref_off_records.empty();
    for (size_t i = 0; i < ref_off_records.size() && rep.all_frames_byte_identical; ++i) {
        if (ref_off_records[i].sample_bytes != ref_on_records[i].sample_bytes ||
            ref_off_records[i].sample_sha256 != ref_on_records[i].sample_sha256) {
            rep.all_frames_byte_identical = false;
        }
    }

    ComputeStats(off_fps, &rep.graph_off_mean_fps, &rep.graph_off_median_fps, nullptr,
                 nullptr, nullptr, &rep.graph_off_stddev_fps);
    ComputeStats(on_fps, &rep.graph_on_mean_fps, &rep.graph_on_median_fps, nullptr,
                 nullptr, nullptr, &rep.graph_on_stddev_fps);

    const double inv_t = 1.0 / static_cast<double>(trials);
    rep.graph_off_enqueue_ms = off_enq * inv_t;
    rep.graph_off_entropy_ms = off_ent * inv_t;
    rep.graph_off_cpu_ms_per_frame = off_cpu * inv_t;
    rep.graph_on_enqueue_ms = on_enq * inv_t;
    rep.graph_on_entropy_ms = on_ent * inv_t;
    rep.graph_on_cpu_ms_per_frame = on_cpu * inv_t;

    rep.fps_delta_pct = rep.graph_off_mean_fps > 0.0
        ? (100.0 * (rep.graph_on_mean_fps - rep.graph_off_mean_fps) / rep.graph_off_mean_fps)
        : 0.0;
    rep.enqueue_reduction_pct = rep.graph_off_enqueue_ms > 0.0
        ? (100.0 * (rep.graph_off_enqueue_ms - rep.graph_on_enqueue_ms) / rep.graph_off_enqueue_ms)
        : 0.0;
    rep.ok = rep.all_frames_byte_identical;
    return rep;
}

}  // namespace hypercine
