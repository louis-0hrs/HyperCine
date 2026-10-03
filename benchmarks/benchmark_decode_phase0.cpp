// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include "../tests/decode_phase0_support.h"
#ifdef HYPERCINE_DECODE_PROFILE
#include "decode_profile_hooks.h"
#endif
#ifndef HYPERCINE_DECODE_SOURCE_COMMIT
#define HYPERCINE_DECODE_SOURCE_COMMIT "unknown"
#endif
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
namespace {
using namespace DecodePhase0;
using Clock = std::chrono::steady_clock;
double Percentile(std::vector<double> v, double q) {
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
const char* FormatName(CFHD_PixelFormat f) { return f == CFHD_PIXEL_FORMAT_BGRA ? "BGRA8" : "YU64"; }
}
int main(int argc, char** argv) {
    try {
        int trials = 5, cycles = 3;
        std::string resolution = "all", output = "decode_phase0.json", corpus;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (arg == "--trials") trials = std::stoi(value);
            else if (arg == "--cycles") cycles = std::stoi(value);
            else if (arg == "--resolution") resolution = value;
            else if (arg == "--output") output = value;
            else if (arg == "--corpus") corpus = value;
            else throw std::runtime_error("unknown option: " + arg);
        }
        if (trials < 1 || cycles < 1 || trials > 100 || cycles > 1000)
            throw std::runtime_error("trials/cycles out of range");
        if (resolution != "all" && resolution != "1080" && resolution != "4k" && resolution != "8k")
            throw std::runtime_error("resolution must be all/1080/4k/8k");
        std::ofstream json_file, golden;
        if (output != "-") json_file.open(output);
        std::ostream& json = output == "-" ? std::cout : json_file;
        if (!json) throw std::runtime_error("cannot write output");
        if (!corpus.empty()) {
            golden.open(corpus);
            if (!golden) throw std::runtime_error("cannot write corpus");
            golden << "# HyperCine decode corpus v1; MIT OR Apache-2.0\n"
                   << "# upstream_encoder_commit=11574d0295771edccadd17af14af74a539f924c7\n"
                   << "# hypercine_source_commit=" HYPERCINE_DECODE_SOURCE_COMMIT "\n"
                   << "# YUY2 -> YUV422 FilmScan3; metadata GUID/DATE/TIME/TIMC payloads zeroed\n"
                   << "# FNV1a64; MSVC Release x64 CRT; seed=0x1337BEEF; TAG_CPU_MAX=1\n"
                   << "# width height pattern input compressed bytes BGRA8 YU64\n";
        }
        json << std::fixed << std::setprecision(6)
             << "{\n  \"schema\": \"hypercine_decode_phase0_v1\",\n"
             << "  \"source_commit\": \"" HYPERCINE_DECODE_SOURCE_COMMIT "\",\n"
             << "  \"upstream_commit\": \"11574d0295771edccadd17af14af74a539f924c7\",\n"
             << "  \"contract\": \"host CFHD in memory -> complete host output; CFHD_DecodeSample only\",\n"
             << "  \"input\": \"YUY2 YUV422 FilmScan3\", \"seed\": 322420463,\n"
             << "  \"cpu_max\": 1, \"trials\": " << trials << ", \"cycles_per_trial\": " << cycles << ",\n"
             << "  \"hash_algorithm\": \"FNV1a64\",\n"
#ifdef HYPERCINE_DECODE_PROFILE
             << "  \"instrumented\": true,\n"
#else
             << "  \"instrumented\": false,\n"
#endif
             << "  \"rows\": [\n";
        bool comma = false;
        const int widths[] = {1920, 3840, 7680}, heights[] = {1080, 2160, 4320};
        const char* names[] = {"1080", "4k", "8k"};
        for (int r = 0; r < 3; ++r) {
            if (resolution != "all" && resolution != names[r]) continue;
            std::cerr << "Generating " << widths[r] << 'x' << heights[r] << " sequence...\n" << std::flush;
            auto samples = Generate(widths[r], heights[r]);
            std::array<std::vector<std::string>, 2> decoded_hashes;
            for (int f = 0; f < 2; ++f) {
                const auto format = f == 0 ? CFHD_PIXEL_FORMAT_BGRA : CFHD_PIXEL_FORMAT_YU64;
                ReferenceDecoder decoder(samples.front(), format);
                // Two warmup decodes of every sample, compare every output byte across
                // a repeated decode before timing. No hashes in timed regions.
                for (auto& s : samples) {
                    decoder.SeedDither(); decoder.Decode(s);
                    const Bytes expected = decoder.output;
                    decoder.SeedDither(); decoder.Decode(s);
                    decoder.VerifyThreads();
                    if (decoder.output != expected) throw std::runtime_error("non-repeatable pixels");
                    decoded_hashes[f].push_back(Hash(expected.data(), expected.size()));
                }
                std::vector<double> trial_fps, all_latency;
                std::array<double, 8> stages{};
                std::array<unsigned long long, 8> stage_calls{};
                const char* stage_names[] = {"parsing_metadata", "entropy_coefficients",
                    "inverse_quantization", "inverse_wavelet_reconstruction",
                    "active_color", "color_conversion_output_packing", "state_lock_overhead",
                    "fused_horizontal_inverse_color_packing"};
                size_t compressed_bytes = 0;
                for (const auto& s : samples) compressed_bytes += s.compressed.size();
                const int frames = int(samples.size()) * cycles;
                if (comma) json << ",\n"; comma = true;
                json << "    {\"width\": " << widths[r] << ", \"height\": " << heights[r]
                     << ", \"output\": \"" << FormatName(format) << "\", \"pitch\": " << decoder.pitch
                     << ", \"frames_per_trial\": " << frames << ", \"trial_results\": [";
                double total_decode_ms = 0;
                for (int t = 0; t < trials; ++t) {
                    std::vector<double> latency; latency.reserve(frames);
                    double wall_ms = 0;
                    for (int c = 0; c < cycles; ++c) for (auto& s : samples) {
                        decoder.SeedDither();
#ifdef HYPERCINE_DECODE_PROFILE
                        DecodeProfileReset();
#endif
                        const auto start = Clock::now();
                        decoder.Decode(s);
                        const auto finish = Clock::now();
                        const double ms = std::chrono::duration<double, std::milli>(finish - start).count();
                        if (ms > 30000) throw std::runtime_error("decode interval exceeded 30s; rerun after interruption");
                        latency.push_back(ms); wall_ms += ms;
#ifdef HYPERCINE_DECODE_PROFILE
                        for (int k = 0; k < 8; ++k) {
                            stages[k] += DecodeProfileMilliseconds(k);
                            stage_calls[k] += DecodeProfileCalls(k);
                        }
#endif
                    }
                    decoder.VerifyThreads();
                    if (Hash(decoder.output.data(), decoder.output.size()) != decoded_hashes[f].back())
                        throw std::runtime_error("timed output hash mismatch");
                    const double fps = frames * 1000.0 / wall_ms;
                    trial_fps.push_back(fps); total_decode_ms += wall_ms;
                    all_latency.insert(all_latency.end(), latency.begin(), latency.end());
                    if (t) json << ',';
                    json << "{\"fps\":" << fps << ",\"decode_wall_ms\":" << wall_ms
                         << ",\"mean_ms\":" << wall_ms / frames << ",\"median_ms\":" << Percentile(latency, .5)
                         << ",\"p95_ms\":" << Percentile(latency, .95) << ",\"p99_ms\":" << Percentile(latency, .99) << '}';
                }
                const double fps = Percentile(trial_fps, .5);
                const double mean = total_decode_ms / all_latency.size();
                json << "], \"headline_median_fps\": " << fps << ", \"mean_ms\": " << mean
                     << ", \"median_ms\": " << Percentile(all_latency, .5)
                     << ", \"p95_ms\": " << Percentile(all_latency, .95)
                     << ", \"p99_ms\": " << Percentile(all_latency, .99)
                     << ", \"raw_output_GBps\": " << fps * double(decoder.output.size()) / 1e9
                     << ", \"compressed_input_MBps\": " << fps * double(compressed_bytes) / samples.size() / 1e6
                     << ", \"peak_process_working_set_bytes\": " << PeakMemory() << ", \"stages\": [";
#ifdef HYPERCINE_DECODE_PROFILE
                double accounted = 0;
                for (int k = 0; k < 8; ++k) {
                    if (k) json << ',';
                    const double ms = stages[k] / all_latency.size(); accounted += ms;
                    json << "{\"name\":\"" << stage_names[k] << "\",\"mean_ms\":" << ms
                         << ",\"percent_wall\":" << 100 * ms / mean << ",\"calls\":" << stage_calls[k] << '}';
                }
                json << ", {\"name\":\"unattributed_glue_and_timer_overhead\",\"mean_ms\":" << mean - accounted
                     << ",\"percent_wall\":" << 100 * (mean - accounted) / mean << '}';
#endif
                json << "], \"samples\": [";
                for (size_t p = 0; p < samples.size(); ++p) {
                    const auto& s = samples[p];
                    if (p) json << ',';
                    json << "{\"pattern\":\"" << PatternName(s.pattern) << "\",\"input_hash\":\"" << s.input_hash
                         << "\",\"compressed_hash\":\"" << Hash(s.compressed.data(), s.compressed.size())
                         << "\",\"compressed_bytes\":" << s.compressed.size() << ",\"decoded_hash\":\""
                         << decoded_hashes[f][p] << "\"}";
                }
                json << "]}";
                std::cerr << names[r] << ' ' << FormatName(format) << " median FPS=" << fps
                          << " mean=" << mean << "ms p95=" << Percentile(all_latency, .95) << "ms\n" << std::flush;
            }
            if (golden) for (size_t p = 0; p < samples.size(); ++p) {
                if (r == 2 && p != 0) continue; // minimal 8K corpus, full benchmark sequence
                const auto& s = samples[p];
                golden << s.width << ' ' << s.height << ' ' << s.pattern << ' ' << s.input_hash << ' '
                       << Hash(s.compressed.data(), s.compressed.size()) << ' ' << s.compressed.size()
                       << ' ' << decoded_hashes[0][p] << ' ' << decoded_hashes[1][p] << '\n';
            }
        }
        json << "\n  ]\n}\n";
        json.flush();
        if (golden.is_open()) golden.flush();
        if (!json || (golden.is_open() && !golden)) throw std::runtime_error("output write failed");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
