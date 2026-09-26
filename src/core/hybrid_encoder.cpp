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

#include "hypercine/hybrid_encoder.h"
#include "hypercine/entropy_parallel.h"
#include "hypercine/gpu_pyramid.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <numeric>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "frame.h"
#include "image.h"
}

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

void PackPlane(const IMAGE* image, int16_t* dst) {
    const int stride = image->pitch / static_cast<int>(sizeof(PIXEL));
    const PIXEL* src = image->band[0];
    for (int y = 0; y < image->height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(image->width),
                    src + static_cast<size_t>(y) * static_cast<size_t>(stride),
                    static_cast<size_t>(image->width) * sizeof(int16_t));
    }
}

int InstallAndCompare(HyperCineGpuSlot* slot, TRANSFORM* transforms[], int channel_count, bool compare, int* compared) {
    const int count = HyperCine_GpuSlotBandCount(slot);
    int mismatches = 0;
    int seen = 0;
    for (int i = 0; i < count; ++i) {
        int channel = 0, level = 0, band = 0, width = 0, height = 0;
        const int16_t* data = nullptr;
        if (HyperCine_GpuSlotBand(slot, i, &channel, &level, &band, &width, &height, &data) != 0) continue;
        if (channel >= channel_count || !transforms[channel]) continue;
        if (level < 0 || level >= transforms[channel]->num_wavelets) continue;
        IMAGE* wavelet = transforms[channel]->wavelet[level];
        if (!wavelet || !wavelet->band[band]) continue;
        const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
        PIXEL* dst = wavelet->band[band];
        for (int y = 0; y < height; ++y) {
            PIXEL* row = dst + static_cast<size_t>(y) * static_cast<size_t>(stride);
            const int16_t* src = data + static_cast<size_t>(y) * static_cast<size_t>(width);
            if (compare) {
                for (int x = 0; x < width; ++x) {
                    ++seen;
                    if (row[x] != src[x]) ++mismatches;
                }
            }
            std::memcpy(row, src, static_cast<size_t>(width) * sizeof(int16_t));
        }
    }
    if (compared) *compared = seen;
    return mismatches;
}

struct PreparedQuant {
    int prescale[3] = {};
    int quant[36] = {};
    int widths[3] = {};
    int heights[3] = {};
    bool ok = false;
};

PreparedQuant ReadQuant(ENCODER* encoder, TRANSFORM* transforms[], int channel_count) {
    PreparedQuant q;
    if (!encoder->frame || channel_count < 3) return q;
    for (int level = 0; level < 3; ++level) q.prescale[level] = transforms[0]->prescale[level];
    for (int c = 0; c < 3; ++c) {
        IMAGE* image = encoder->frame->channel[c];
        if (!image) return q;
        q.widths[c] = image->width;
        q.heights[c] = image->height;
        for (int level = 0; level < 3; ++level) {
            IMAGE* wavelet = transforms[c]->wavelet[level];
            if (!wavelet) return q;
            for (int band = 0; band < 4; ++band) {
                q.quant[(c * 3 + level) * 4 + band] = wavelet->quant[band];
            }
        }
    }
    q.ok = true;
    return q;
}

bool ConvertAndPack(ENCODER* encoder, const uint8_t* bgra, int pitch, void* scratch, size_t scratch_bytes,
                    HyperCineGpuSlot* slot, const PreparedQuant& q, double* color_ms, double* pack_ms) {
    const double c0 = NowMs();
    ConvertRGB32to10bitYUVFrame(const_cast<uint8_t*>(bgra), pitch, encoder->frame,
                                static_cast<uint8_t*>(scratch), static_cast<int>(scratch_bytes),
                                encoder->input.color_space, encoder->codec.precision, 1, 0);
    if (color_ms) *color_ms = NowMs() - c0;
    const double p0 = NowMs();
    for (int c = 0; c < 3; ++c) {
        int cap = 0;
        int16_t* dst = HyperCine_GpuSlotHostPlane(slot, c, &cap);
        IMAGE* image = encoder->frame->channel[c];
        if (!dst || !image || cap < image->width * image->height) return false;
        PackPlane(image, dst);
    }
    if (pack_ms) *pack_ms = NowMs() - p0;
    return true;
}

const int16_t* PlanePtr(HyperCineGpuSlot* slot, int channel) {
    return HyperCine_GpuSlotHostPlane(slot, channel, nullptr);
}

}  // namespace

HybridRun RunHybrid(encoder* enc_opaque,
                    transform* transforms_opaque[],
                    int channel_count,
                    const uint8_t* bgra,
                    int pitch,
                    void* scratch,
                    size_t scratch_bytes,
                    int frame_count,
                    int entropy_workers,
                    bool pipelined) {
    HybridRun run;
    run.pipelined = pipelined;
    run.entropy_workers = entropy_workers;
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || !bgra || frame_count <= 0) {
        run.error = "missing hybrid input";
        return run;
    }
    PreparedQuant quant = ReadQuant(encoder, transforms, channel_count);
    if (!quant.ok) {
        run.error = "encoder pyramid is not a 3-level 3-channel spatial transform";
        return run;
    }

    const int slots_n = pipelined ? 3 : 1;
    HyperCineGpuSlot* slots[3] = {};
    for (int i = 0; i < slots_n; ++i) {
        slots[i] = HyperCine_GpuSlotCreate(quant.widths[0], quant.heights[0]);
        if (!slots[i]) {
            run.error = "GPU slot allocation failed";
            for (int j = 0; j < i; ++j) HyperCine_GpuSlotDestroy(slots[j]);
            return run;
        }
    }

    auto launch_slot = [&](HyperCineGpuSlot* slot, int async_launch) {
        const int16_t* planes[3] = {PlanePtr(slot, 0), PlanePtr(slot, 1), PlanePtr(slot, 2)};
        return HyperCine_GpuSlotLaunch(slot, planes, quant.widths, quant.heights, quant.prescale, quant.quant, async_launch);
    };

    const double cpu0 = ProcessCpuSeconds();
    const double wall0 = NowMs();
    int encoded_frames = 0;

    if (!pipelined) {
        for (int frame = 0; frame < frame_count; ++frame) {
            HybridFrame sample;
            const double start = NowMs();
            if (!ConvertAndPack(encoder, bgra, pitch, scratch, scratch_bytes, slots[0], quant,
                                &sample.color_ms, &sample.pack_ms)) {
                run.error = "color conversion failed";
                break;
            }
            if (launch_slot(slots[0], 0) != 0) {
                run.error = "GPU pyramid launch failed";
                break;
            }
            HyperCineGpuTiming timing{};
            if (HyperCine_GpuSlotSync(slots[0], &timing) != 0) {
                run.error = "GPU sync failed";
                break;
            }
            sample.h2d_ms = timing.h2d_ms;
            sample.gpu_ms = timing.compute_ms;
            sample.d2h_ms = timing.d2h_ms;
            sample.h2d_bytes = timing.h2d_bytes;
            sample.d2h_bytes = timing.d2h_bytes;
            InstallAndCompare(slots[0], transforms, channel_count, false, nullptr);
            EntropyOutput entropy = EncodeEntropy(enc_opaque, transforms_opaque, channel_count,
                                                  entropy_workers, EntropySchedule::SubbandParallel, false);
            sample.entropy_ms = entropy.entropy_wall_ms;
            sample.assembly_ms = entropy.assembly_ms;
            sample.encoded_bytes = entropy.bitstream_bytes;
            sample.latency_ms = NowMs() - start;
            if (!entropy.success) {
                run.error = entropy.error.empty() ? "entropy failed" : entropy.error;
                break;
            }
            run.bitstream = std::move(entropy.bitstream);
            run.frame_times.push_back(sample);
            ++encoded_frames;
        }
        run.wall_ms = NowMs() - wall0;
        run.startup_ms = run.frame_times.empty() ? 0 : run.frame_times.front().latency_ms;
        run.process_cpu_seconds = ProcessCpuSeconds() - cpu0;
    } else {
        struct Inflight {
            HybridFrame sample;
            double submit_ms = 0;
            bool launched = false;
        };
        std::vector<Inflight> inflight(static_cast<size_t>(frame_count));
        EntropyOutput finished;
        std::thread worker;
        int entropy_frame = -1;
        double first_ready = -1.0;
        for (int step = 0; step < frame_count + 2 && run.error.empty(); ++step) {
            const int launch_frame = step;
            const int sync_frame = step - 1;
            const int retire_frame = step - 2;
            if (entropy_frame >= 0 && retire_frame == entropy_frame) {
                worker.join();
                if (!finished.success) {
                    run.error = finished.error.empty() ? "entropy failed" : finished.error;
                    entropy_frame = -1;
                    break;
                }
                Inflight& done = inflight[static_cast<size_t>(retire_frame)];
                done.sample.entropy_ms = finished.entropy_wall_ms;
                done.sample.assembly_ms = finished.assembly_ms;
                done.sample.encoded_bytes = finished.bitstream_bytes;
                done.sample.latency_ms = NowMs() - done.submit_ms;
                if (first_ready < 0.0) first_ready = NowMs();
                run.bitstream = finished.bitstream;
                ++encoded_frames;
                entropy_frame = -1;
            }
            if (sync_frame >= 0 && sync_frame < frame_count && run.error.empty()) {
                HyperCineGpuTiming timing{};
                if (HyperCine_GpuSlotSync(slots[sync_frame % slots_n], &timing) != 0) {
                    run.error = "GPU sync failed";
                    break;
                }
                HybridFrame& sample = inflight[static_cast<size_t>(sync_frame)].sample;
                sample.h2d_ms = timing.h2d_ms;
                sample.gpu_ms = timing.compute_ms;
                sample.d2h_ms = timing.d2h_ms;
                sample.h2d_bytes = timing.h2d_bytes;
                sample.d2h_bytes = timing.d2h_bytes;
                InstallAndCompare(slots[sync_frame % slots_n], transforms, channel_count, false, nullptr);
                worker = std::thread([&]() {
                    finished = EncodeEntropy(enc_opaque, transforms_opaque, channel_count,
                                             entropy_workers, EntropySchedule::SubbandParallel, false);
                });
                entropy_frame = sync_frame;
            }
            if (launch_frame >= 0 && launch_frame < frame_count && run.error.empty()) {
                Inflight& item = inflight[static_cast<size_t>(launch_frame)];
                item.submit_ms = NowMs();
                if (!ConvertAndPack(encoder, bgra, pitch, scratch, scratch_bytes, slots[launch_frame % slots_n], quant,
                                    &item.sample.color_ms, &item.sample.pack_ms)) {
                    run.error = "color conversion failed";
                    break;
                }
                if (launch_slot(slots[launch_frame % slots_n], 1) != 0) {
                    run.error = "GPU pyramid launch failed";
                    break;
                }
                item.launched = true;
            }
        }
        if (entropy_frame >= 0) {
            worker.join();
            if (finished.success && run.error.empty()) {
                Inflight& done = inflight[static_cast<size_t>(entropy_frame)];
                done.sample.entropy_ms = finished.entropy_wall_ms;
                done.sample.assembly_ms = finished.assembly_ms;
                done.sample.encoded_bytes = finished.bitstream_bytes;
                done.sample.latency_ms = NowMs() - done.submit_ms;
                if (first_ready < 0.0) first_ready = NowMs();
                run.bitstream = finished.bitstream;
                ++encoded_frames;
            } else if (run.error.empty()) {
                run.error = finished.error.empty() ? "entropy failed" : finished.error;
            }
            entropy_frame = -1;
        }
        run.wall_ms = NowMs() - wall0;
        run.startup_ms = (first_ready < 0.0) ? run.wall_ms : (first_ready - wall0);
        run.process_cpu_seconds = ProcessCpuSeconds() - cpu0;
        for (const Inflight& item : inflight) {
            if (item.launched) run.frame_times.push_back(item.sample);
        }
    }

    if (run.error.empty() && encoded_frames == frame_count) {
        // One untimed upstream-vs-GPU coefficient check on the same converted planes.
        RefreshUpstreamPyramid(enc_opaque, transforms_opaque, channel_count, bgra, pitch, scratch, scratch_bytes);
        if (ConvertAndPack(encoder, bgra, pitch, scratch, scratch_bytes, slots[0], quant, nullptr, nullptr) &&
            launch_slot(slots[0], 0) == 0) {
            int compared = 0;
            run.coeff_mismatches = InstallAndCompare(slots[0], transforms, channel_count, true, &compared);
            run.coeff_compared = compared;
            // InstallAndCompare writes GPU values after comparing, so the wavelet now holds GPU
            // coefficients. Byte equality is only meaningful when every coefficient matched,
            // in which case the GPU write did not change the upstream coefficients.
            if (run.coeff_mismatches == 0) {
                EntropyOutput check = EncodeEntropy(enc_opaque, transforms_opaque, channel_count,
                                                    entropy_workers, EntropySchedule::SubbandParallel, true);
                run.byte_match = check.byte_match;
                if (check.success) run.bitstream = check.bitstream;
            }
        }
        run.success = true;
    }

    if (!run.frame_times.empty()) {
        double lat = 0, color = 0, pack = 0, h2d = 0, d2h = 0, gpu = 0, ent = 0, assembly = 0;
        for (const HybridFrame& frame : run.frame_times) {
            lat += frame.latency_ms;
            color += frame.color_ms;
            pack += frame.pack_ms;
            h2d += frame.h2d_ms;
            d2h += frame.d2h_ms;
            gpu += frame.gpu_ms;
            ent += frame.entropy_ms;
            assembly += frame.assembly_ms;
            run.h2d_bytes = frame.h2d_bytes;
            run.d2h_bytes = frame.d2h_bytes;
            run.encoded_bytes = frame.encoded_bytes;
        }
        const double n = static_cast<double>(run.frame_times.size());
        run.mean_latency_ms = lat / n;
        run.mean_color_ms = color / n;
        run.mean_pack_ms = pack / n;
        run.mean_h2d_ms = h2d / n;
        run.mean_d2h_ms = d2h / n;
        run.mean_gpu_ms = gpu / n;
        run.mean_entropy_ms = ent / n;
        run.mean_assembly_ms = assembly / n;
        run.frames = static_cast<int>(run.frame_times.size());
        if (run.wall_ms > 0) {
            run.sustained_fps = 1000.0 * n / run.wall_ms;
            run.gpu_compute_duty = (gpu / run.wall_ms);
        }
    }

    for (int i = 0; i < slots_n; ++i) HyperCine_GpuSlotDestroy(slots[i]);
    return run;
}

}  // namespace hypercine
