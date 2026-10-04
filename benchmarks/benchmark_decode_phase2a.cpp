// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cuda_runtime.h>
#include "../tests/decode_phase0_support.h"
#include "../tests/decode_phase1_support.h"
#include "hypercine/decode_phase2a.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using namespace DecodePhase0;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;
using Clock = std::chrono::steady_clock;

double Percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double position = q * double(v.size() - 1);
    const auto low = size_t(position), high = (std::min)(low + 1, v.size() - 1);
    return v[low] + (v[high] - v[low]) * (position - double(low));
}

size_t PeakMemory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pm{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm))) return pm.PeakWorkingSetSize;
#endif
    return 0;
}

size_t PeakGpuMemory() {
    size_t free_bytes = 0, total_bytes = 0;
    if (cudaMemGetInfo(&free_bytes, &total_bytes) == cudaSuccess) {
        return total_bytes - free_bytes;
    }
    return 0;
}

void ConvertPlanarToBgra8CpuReference(
    const int16_t* y_plane, size_t y_pitch,
    const int16_t* v_plane, size_t v_pitch,
    const int16_t* u_plane, size_t u_pitch,
    uint8_t* bgra_out, size_t bgra_pitch,
    int width, int height)
{
    const int16_t ymult = 19072;
    const int16_t r_vmult = 230;
    const int16_t g_vmult = 137;
    const int16_t g_umult = 55;
    const int16_t b_umult = 135;
    const int16_t y_offset = 16;
    const int16_t chroma_offset = 128;
    const int16_t limiterRGB = 0x7F00;

    auto clamp255_simd = [&](int16_t v) -> int16_t {
        int32_t add = int32_t(v) + limiterRGB;
        if (add > 32767) add = 32767;
        if (add < -32768) add = -32768;
        uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(add));
        if (u < static_cast<uint16_t>(limiterRGB)) return 0;
        return static_cast<int16_t>(u - static_cast<uint16_t>(limiterRGB));
    };

    auto sat_add16 = [](int16_t a, int16_t b) -> int16_t {
        int32_t sum = int32_t(a) + int32_t(b);
        if (sum > 32767) return 32767;
        if (sum < -32768) return -32768;
        return static_cast<int16_t>(sum);
    };

    auto sat_sub16 = [](int16_t a, int16_t b) -> int16_t {
        int32_t diff = int32_t(a) - int32_t(b);
        if (diff > 32767) return 32767;
        if (diff < -32768) return -32768;
        return static_cast<int16_t>(diff);
    };

    auto packus8 = [](int32_t v) -> uint8_t {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };

    int lowpass_width = width / 2;
    int post_column_lowpass = lowpass_width - (lowpass_width % 16);
    while (post_column_lowpass > (lowpass_width - 2 - 2)) {
        post_column_lowpass -= 16;
    }
    int post_column_pixels = post_column_lowpass * 2;

    for (int y = 0; y < height; ++y) {
        int out_y = height - 1 - y; // BottomUp
        const int16_t* y_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(y_plane) + y * y_pitch);
        const int16_t* v_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(v_plane) + y * v_pitch);
        const int16_t* u_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(u_plane) + y * u_pitch);
        uint8_t* out_row = bgra_out + out_y * bgra_pitch;

        for (int x = 0; x < width; ++x) {
            int16_t raw_y = y_row[x];
            int16_t raw_v = v_row[x / 2];
            int16_t raw_u = u_row[x / 2];

            int16_t y8 = static_cast<int16_t>(static_cast<uint16_t>(raw_y) >> 2);
            int16_t v8 = static_cast<int16_t>(static_cast<uint16_t>(raw_v) >> 2);
            int16_t u8 = static_cast<int16_t>(static_cast<uint16_t>(raw_u) >> 2);

            uint8_t b_out = 0, g_out = 0, r_out = 0;

            if (x < post_column_pixels) {
                int16_t yy = clamp255_simd(sat_sub16(y8, y_offset));
                int16_t uu = sat_sub16(clamp255_simd(u8), chroma_offset);
                int16_t vv = sat_sub16(clamp255_simd(v8), chroma_offset);

                int16_t yy_slli = static_cast<int16_t>(yy << 7);
                int16_t yy_hi = static_cast<int16_t>((int32_t(yy_slli) * int32_t(ymult)) >> 16);
                int16_t yy_sse = static_cast<int16_t>(yy_hi << 1);

                int16_t t_r = static_cast<int16_t>((int16_t(vv * r_vmult)) >> 1);
                int16_t r1 = sat_add16(yy_sse, t_r);
                r1 = sat_add16(r1, 32);
                r1 = static_cast<int16_t>(r1 >> 6);

                int16_t t_gv = static_cast<int16_t>((int16_t(vv * g_vmult)) >> 2);
                int16_t g1 = sat_sub16(yy_sse, t_gv);
                int16_t t_gu = static_cast<int16_t>((int16_t(uu * g_umult)) >> 2);
                g1 = sat_sub16(g1, t_gu);
                g1 = sat_add16(g1, 32);
                g1 = static_cast<int16_t>(g1 >> 6);

                int16_t t_bu = static_cast<int16_t>(uu * b_umult);
                int16_t b1 = sat_add16(yy_sse, t_bu);
                b1 = sat_add16(b1, 32);
                b1 = static_cast<int16_t>(b1 >> 6);

                b_out = packus8(b1);
                g_out = packus8(g1);
                r_out = packus8(r1);
            } else {
                int32_t yy = y8 - y_offset;
                int32_t uu = u8 - chroma_offset;
                int32_t vv = v8 - chroma_offset;

                int32_t yy_scaled = (yy * ymult) >> 7;
                int32_t r1 = (yy_scaled                          + r_vmult * vv + 64) >> 7;
                int32_t g1 = (yy_scaled * 2 - g_umult * uu - g_vmult * vv + 128) >> 8;
                int32_t b1 = (yy_scaled     + 2 * b_umult * uu                  + 64) >> 7;

                b_out = packus8(b1);
                g_out = packus8(g1);
                r_out = packus8(r1);
            }

            out_row[4 * x + 0] = b_out;
            out_row[4 * x + 1] = g_out;
            out_row[4 * x + 2] = r_out;
            out_row[4 * x + 3] = 255;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        int trials = 5, cycles = 3;
        std::string resolution = "all";
        std::string output = "benchmarks/results/decode_phase2a_windows_msvc.json";

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (arg == "--trials") trials = std::stoi(value);
            else if (arg == "--cycles") cycles = std::stoi(value);
            else if (arg == "--resolution") resolution = value;
            else if (arg == "--output") output = value;
            else throw std::runtime_error("unknown option: " + arg);
        }

        if (trials < 1 || cycles < 1 || trials > 100 || cycles > 1000)
            throw std::runtime_error("trials/cycles out of range");
        if (resolution != "all" && resolution != "1080" && resolution != "4k" && resolution != "8k")
            throw std::runtime_error("resolution must be all/1080/4k/8k");

        std::ofstream json_file;
        if (output != "-") {
            json_file.open(output);
            if (!json_file.is_open()) {
                throw std::runtime_error("cannot open output file: " + output + " (" + strerror(errno) + ")");
            }
        }
        std::ostream& json = output == "-" ? std::cout : json_file;

        std::cout << "=== HyperCine Phase 2A Decoder Benchmark ===" << std::endl;
        std::cout << "Configuration: trials=" << trials << ", cycles=" << cycles
                  << ", resolution=" << resolution << std::endl;

        json << std::fixed << std::setprecision(6)
             << "{\n  \"schema\": \"hypercine_decode_phase2a_v1\",\n"
             << "  \"contract\": \"CUDA Color Conversion & Native DeviceOutput\",\n"
             << "  \"input\": \"YUV422 FilmScan3\", \"seed\": 322420463,\n"
             << "  \"trials\": " << trials << ", \"cycles_per_trial\": " << cycles << ",\n"
             << "  \"resolutions\": [\n";

        const int widths[] = {1920, 3840, 7680};
        const int heights[] = {1080, 2160, 4320};
        const char* names[] = {"1080", "4k", "8k"};

        bool first_res = true;

        for (int r = 0; r < 3; ++r) {
            if (resolution != "all" && resolution != names[r]) continue;

            int w = widths[r];
            int h = heights[r];
            int num_patterns = (r == 2) ? 1 : 5;

            std::cout << "\n==========================================" << std::endl;
            std::cout << "--- Running " << names[r] << " (" << w << "x" << h << ") ---" << std::endl;
            std::cout << "==========================================" << std::endl;

            auto samples = Generate(w, h, num_patterns);

            CudaDecoderPipelinePhase2A pipeline;
            if (!pipeline.Initialize(w, h, 3, 10)) {
                throw std::runtime_error("Failed to initialize CUDA pipeline for " + std::string(names[r]));
            }

            int frames_per_trial = num_patterns * cycles;
            double megapixels = (double(w) * h) / 1e6;

            // Warmup
            std::cout << "Warming up pipeline..." << std::endl;
            for (auto& s : samples) {
                ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                ref.Decode(s);
                auto* d = ref.GetNative();
                auto bands = ExtractBandsFromNative(d);
                uint64_t t = pipeline.QueueDecode(bands, DeviceSurfaceType::BGRA8);
                pipeline.WaitForDecode(t);
                pipeline.ReleaseOutput(t);
            }

            // ----------------------------------------------------
            // 1. ReferenceCpu (Pristine CPU decode to BGRA8)
            // ----------------------------------------------------
            std::cout << "Benchmarking 1. ReferenceCpu (Pristine CPU BGRA8)..." << std::endl;
            std::vector<double> cpu_trial_fps;
            std::vector<double> cpu_all_latencies;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                        auto t0 = Clock::now();
                        ref.Decode(s);
                        auto t1 = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        trial_wall_ms += ms;
                        cpu_all_latencies.push_back(ms);
                    }
                }
                cpu_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            // ----------------------------------------------------
            // 2. HybridPhase1 (CPU Entropy + CUDA Inv + Planar D2H + CPU BGRA)
            // ----------------------------------------------------
            std::cout << "Benchmarking 2. HybridPhase1 (CPU Entropy + CUDA Inv + Planar D2H + CPU BGRA)..." << std::endl;
            std::vector<double> hybrid_trial_fps;
            std::vector<double> hybrid_all_latencies;
            std::vector<int16_t> host_y(w * h, 0);
            std::vector<int16_t> host_v((w / 2) * h, 0);
            std::vector<int16_t> host_u((w / 2) * h, 0);
            int bgra_pitch = w * 4;
            std::vector<uint8_t> hybrid_bgra(bgra_pitch * h, 0);

            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t_start = Clock::now();

                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                        ref.Decode(s);
                        auto* d = ref.GetNative();
                        auto bands = ExtractBandsFromNative(d);

                        uint64_t ticket = pipeline.QueueDecode(bands, DeviceSurfaceType::PlanarYUV422_16S);
                        pipeline.WaitForDecode(ticket);

                        pipeline.DownloadPlanarToHost(
                            ticket,
                            host_y.data(), w * sizeof(int16_t),
                            host_v.data(), (w / 2) * sizeof(int16_t),
                            host_u.data(), (w / 2) * sizeof(int16_t)
                        );
                        pipeline.ReleaseOutput(ticket);

                        ConvertPlanarToBgra8CpuReference(
                            host_y.data(), w * sizeof(int16_t),
                            host_v.data(), (w / 2) * sizeof(int16_t),
                            host_u.data(), (w / 2) * sizeof(int16_t),
                            hybrid_bgra.data(), bgra_pitch,
                            w, h
                        );

                        auto t_finish = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();
                        trial_wall_ms += ms;
                        hybrid_all_latencies.push_back(ms);
                    }
                }
                hybrid_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            // ----------------------------------------------------
            // 3. DevicePlanar (CPU Entropy + H2D + CUDA Inv -> Ready in VRAM)
            // Timed strictly up to GPU ready event. ZERO D2H in timed loop!
            // ----------------------------------------------------
            std::cout << "Benchmarking 3. DevicePlanar (CPU Entropy + H2D + CUDA Inv -> Ready in VRAM)..." << std::endl;
            std::vector<double> dev_planar_trial_fps;
            std::vector<double> dev_planar_all_latencies;
            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t_start = Clock::now();

                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                        ref.Decode(s);
                        auto* d = ref.GetNative();
                        auto bands = ExtractBandsFromNative(d);

                        uint64_t ticket = pipeline.QueueDecode(bands, DeviceSurfaceType::PlanarYUV422_16S);
                        pipeline.WaitForDecode(ticket);

                        auto t_finish = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();
                        trial_wall_ms += ms;
                        dev_planar_all_latencies.push_back(ms);

                        // Verification / Release done outside the timer:
                        pipeline.ReleaseOutput(ticket);
                    }
                }
                dev_planar_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            // ----------------------------------------------------
            // 4. DeviceBGRA (CPU Entropy + H2D + CUDA Inv + CUDA BGRA -> Ready in VRAM)
            // Primary target! ZERO D2H or host conversions in timed loop!
            // ----------------------------------------------------
            std::cout << "Benchmarking 4. DeviceBGRA (CPU Entropy + H2D + CUDA Inv + CUDA BGRA -> Ready in VRAM)..." << std::endl;
            std::vector<double> dev_bgra_trial_fps;
            std::vector<double> dev_bgra_all_latencies;

            std::vector<double> stage_ent_ms;
            std::vector<double> stage_h2d_ms;
            std::vector<double> stage_inv_ms;
            std::vector<double> stage_color_ms;
            std::vector<double> stage_total_gpu_ms;

            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t_start = Clock::now();

                        auto t_e0 = Clock::now();
                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                        ref.Decode(s);
                        auto* d = ref.GetNative();
                        auto bands = ExtractBandsFromNative(d);
                        auto t_e1 = Clock::now();
                        double ent_time = std::chrono::duration<double, std::milli>(t_e1 - t_e0).count();

                        uint64_t ticket = pipeline.QueueDecode(bands, DeviceSurfaceType::BGRA8);
                        pipeline.WaitForDecode(ticket);

                        auto t_finish = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();
                        trial_wall_ms += ms;
                        dev_bgra_all_latencies.push_back(ms);

                        SlotStageTimings timings{};
                        pipeline.GetStageTimings(ticket, &timings);
                        stage_ent_ms.push_back(ent_time);
                        stage_h2d_ms.push_back(timings.h2d_ms);
                        stage_inv_ms.push_back(timings.inverse_ms);
                        stage_color_ms.push_back(timings.color_ms);
                        stage_total_gpu_ms.push_back(timings.total_gpu_ms);

                        // Verification / Release done outside the timer:
                        pipeline.ReleaseOutput(ticket);
                    }
                }
                dev_bgra_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            // ----------------------------------------------------
            // 5. HostBGRAFromDevice (DeviceBGRA + BGRA D2H to host RAM)
            // ----------------------------------------------------
            std::cout << "Benchmarking 5. HostBGRAFromDevice (DeviceBGRA + BGRA D2H to Host RAM)..." << std::endl;
            std::vector<double> host_bgra_trial_fps;
            std::vector<double> host_bgra_all_latencies;
            std::vector<double> stage_bgra_d2h_ms;

            std::vector<uint8_t> downloaded_bgra(bgra_pitch * h, 0);

            for (int t = 0; t < trials; ++t) {
                double trial_wall_ms = 0;
                for (int c = 0; c < cycles; ++c) {
                    for (auto& s : samples) {
                        auto t_start = Clock::now();

                        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA);
                        ref.Decode(s);
                        auto* d = ref.GetNative();
                        auto bands = ExtractBandsFromNative(d);

                        uint64_t ticket = pipeline.QueueDecode(bands, DeviceSurfaceType::BGRA8);
                        pipeline.WaitForDecode(ticket);

                        auto t_d2h0 = Clock::now();
                        pipeline.DownloadBgraToHost(ticket, downloaded_bgra.data(), bgra_pitch);
                        auto t_d2h1 = Clock::now();
                        double d2h_ms = std::chrono::duration<double, std::milli>(t_d2h1 - t_d2h0).count();

                        auto t_finish = Clock::now();
                        double ms = std::chrono::duration<double, std::milli>(t_finish - t_start).count();
                        trial_wall_ms += ms;
                        host_bgra_all_latencies.push_back(ms);
                        stage_bgra_d2h_ms.push_back(d2h_ms);

                        // Verification / Release outside the timer
                        if (std::memcmp(downloaded_bgra.data(), ref.output.data(), downloaded_bgra.size()) != 0) {
                            throw std::runtime_error("HostBGRA exactness check failed!");
                        }
                        pipeline.ReleaseOutput(ticket);
                    }
                }
                host_bgra_trial_fps.push_back(frames_per_trial * 1000.0 / trial_wall_ms);
            }

            auto avg = [](const std::vector<double>& v) {
                return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
            };

            double cpu_med_fps = Percentile(cpu_trial_fps, 0.5);
            double hybrid_med_fps = Percentile(hybrid_trial_fps, 0.5);
            double dev_planar_med_fps = Percentile(dev_planar_trial_fps, 0.5);
            double dev_bgra_med_fps = Percentile(dev_bgra_trial_fps, 0.5);
            double host_bgra_med_fps = Percentile(host_bgra_trial_fps, 0.5);

            double cpu_mean_lat = avg(cpu_all_latencies);
            double hybrid_mean_lat = avg(hybrid_all_latencies);
            double dev_planar_mean_lat = avg(dev_planar_all_latencies);
            double dev_bgra_mean_lat = avg(dev_bgra_all_latencies);
            double host_bgra_mean_lat = avg(host_bgra_all_latencies);

            double dev_bgra_mp_s = dev_bgra_med_fps * megapixels;

            double m_ent = avg(stage_ent_ms);
            double m_h2d = avg(stage_h2d_ms);
            double m_inv = avg(stage_inv_ms);
            double m_color = avg(stage_color_ms);
            double m_gpu = avg(stage_total_gpu_ms);
            double m_d2h = avg(stage_bgra_d2h_ms);

            std::cout << "\nResults Summary for " << names[r] << " (" << w << "x" << h << "):" << std::endl;
            std::cout << "  1. ReferenceCpu:        " << std::setw(7) << std::fixed << std::setprecision(2)
                      << cpu_med_fps << " FPS | " << cpu_mean_lat << " ms" << std::endl;
            std::cout << "  2. HybridPhase1:         " << std::setw(7) << hybrid_med_fps << " FPS | "
                      << hybrid_mean_lat << " ms" << std::endl;
            std::cout << "  3. DevicePlanar:        " << std::setw(7) << dev_planar_med_fps << " FPS | "
                      << dev_planar_mean_lat << " ms" << std::endl;
            std::cout << "  4. DeviceBGRA (target): " << std::setw(7) << dev_bgra_med_fps << " FPS | "
                      << dev_bgra_mean_lat << " ms (" << dev_bgra_mp_s << " MP/s)" << std::endl;
            std::cout << "  5. HostBGRAFromDevice:  " << std::setw(7) << host_bgra_med_fps << " FPS | "
                      << host_bgra_mean_lat << " ms" << std::endl;
            std::cout << "  Stage Breakdown (DeviceBGRA):" << std::endl;
            std::cout << "    CPU Entropy: " << m_ent << " ms" << std::endl;
            std::cout << "    H2D Upload:  " << m_h2d << " ms" << std::endl;
            std::cout << "    CUDA Inverse:" << m_inv << " ms" << std::endl;
            std::cout << "    CUDA Color:  " << m_color << " ms" << std::endl;
            std::cout << "    Total GPU:   " << m_gpu << " ms" << std::endl;
            std::cout << "    BGRA D2H:    " << m_d2h << " ms" << std::endl;

            if (!first_res) json << ",\n";
            first_res = false;

            json << "    {\n"
                 << "      \"name\": \"" << names[r] << "\", \"width\": " << w << ", \"height\": " << h << ",\n"
                 << "      \"reference_cpu\": { \"fps\": " << cpu_med_fps << ", \"latency_ms\": " << cpu_mean_lat << " },\n"
                 << "      \"hybrid_phase1\": { \"fps\": " << hybrid_med_fps << ", \"latency_ms\": " << hybrid_mean_lat << " },\n"
                 << "      \"device_planar\": { \"fps\": " << dev_planar_med_fps << ", \"latency_ms\": " << dev_planar_mean_lat << " },\n"
                 << "      \"device_bgra\": {\n"
                 << "        \"fps\": " << dev_bgra_med_fps << ", \"latency_ms\": " << dev_bgra_mean_lat << ",\n"
                 << "        \"megapixels_per_sec\": " << dev_bgra_mp_s << ",\n"
                 << "        \"stages_ms\": {\n"
                 << "          \"cpu_entropy\": " << m_ent << ",\n"
                 << "          \"upload_h2d\": " << m_h2d << ",\n"
                 << "          \"cuda_inverse\": " << m_inv << ",\n"
                 << "          \"cuda_color_conversion\": " << m_color << ",\n"
                 << "          \"total_gpu\": " << m_gpu << "\n"
                 << "        }\n"
                 << "      },\n"
                 << "      \"host_bgra_from_device\": {\n"
                 << "        \"fps\": " << host_bgra_med_fps << ", \"latency_ms\": " << host_bgra_mean_lat << ",\n"
                 << "        \"d2h_ms\": " << m_d2h << "\n"
                 << "      }\n"
                 << "    }";
        }

        json << "\n  ],\n"
             << "  \"memory\": {\n"
             << "    \"peak_working_set_bytes\": " << PeakMemory() << ",\n"
             << "    \"peak_gpu_allocated_bytes\": " << PeakGpuMemory() << "\n"
             << "  }\n}\n";

        std::cout << "\nSUCCESS: Phase 2A Benchmark completed and results saved to: " << output << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "BENCHMARK ERROR: " << e.what() << std::endl;
        return 1;
    }
}
