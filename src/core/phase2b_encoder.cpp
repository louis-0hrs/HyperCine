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

#include "hypercine/phase2b.h"

#include "hypercine/entropy_parallel.h"
#include "hypercine/phase2a_gpu.h"

extern "C" {
#include "codec.h"
#include "encoder.h"
}

#include <cuda_runtime.h>

#include <chrono>
#include <cstring>
#include <string>
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

struct QuantTables {
    int prescale[3] = {};
    int quant[36] = {};
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
            for (int band = 0; band < 4; ++band) q.quant[(c * 3 + level) * 4 + band] = wavelet->quant[band];
        }
    }
    q.ok = true;
    return q;
}

int CountDiff(IMAGE* wavelet, int band, const int16_t* src, int width, int height) {
    const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
    const PIXEL* dst = wavelet->band[band];
    int mismatches = 0;
    for (int y = 0; y < height; ++y) {
        const PIXEL* row = dst + static_cast<size_t>(y) * static_cast<size_t>(stride);
        const int16_t* got = src + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) if (row[x] != got[x]) ++mismatches;
    }
    return mismatches;
}

const char* DispatchName(GpuEntropyDispatch dispatch) {
    if (dispatch == GpuEntropyDispatch::MultiStream) return "multi_stream";
    if (dispatch == GpuEntropyDispatch::Batched) return "batched";
    return "one_stream";
}

}  // namespace

Phase2BRun RunPhase2B(encoder* enc_opaque, transform* transforms_opaque[], int channel_count,
                      const uint8_t* bgra, int pitch, void* scratch, size_t scratch_bytes,
                      const Phase2BConfig& config) {
    Phase2BRun run;
    run.input_name = config.input == Phase2Input::DeviceBgra ? "device_bgra" : "host_bgra";
    run.dispatch_name = DispatchName(config.dispatch);
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || !bgra || pitch <= 0 || config.frames <= 0) {
        run.error = "missing phase 2B input";
        return run;
    }
    const QuantTables quant = ReadQuant(encoder, transforms, channel_count);
    if (!quant.ok) {
        run.error = "encoder pyramid is not ready";
        return run;
    }
    const std::vector<BandRef> refs = EnumerateBands(transforms_opaque, channel_count);
    if (refs.size() != 30) {
        run.error = "expected 30 subbands";
        return run;
    }
    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        const BandRef& ref = refs[static_cast<size_t>(i)];
        piece_of[ref.channel][ref.wavelet_index][ref.band] = i;
    }
    const int width = quant.widths[0];
    const int height = quant.heights[0];
    HyperCineGpuSlot* slot = HyperCine_GpuSlotCreate(width, height);
    GpuEntropySession* session = CreateGpuEntropySession();
    if (!slot || !session) {
        run.error = "gpu allocation failed";
        DestroyGpuEntropySession(session);
        HyperCine_GpuSlotDestroy(slot);
        return run;
    }
    const void* device_bgra = nullptr;
    const uint8_t* source = bgra;
    if (config.input == Phase2Input::DeviceBgra) {
        if (HyperCine_GpuSlotUploadBgra(slot, bgra, pitch, width, height, &device_bgra) != 0) {
            run.error = "device upload failed";
            DestroyGpuEntropySession(session);
            HyperCine_GpuSlotDestroy(slot);
            return run;
        }
        source = static_cast<const uint8_t*>(device_bgra);
    }
    const int input_kind = config.input == Phase2Input::DeviceBgra ? HYPERCINE_INPUT_DEVICE_BGRA : HYPERCINE_INPUT_HOST_BGRA;

    auto encode_frame = [&](bool verify, Phase2BRun* into) -> bool {
        if (HyperCine_GpuSlotBegin(slot, input_kind, HYPERCINE_XPORT_DEVICE, source, pitch, width, height,
                                   quant.color_space, quant.widths, quant.heights, quant.prescale, quant.quant) != 0) {
            into->error = "gpu begin failed";
            return false;
        }
        if (HyperCine_GpuSlotKick(slot) != 0) {
            into->error = "gpu pyramid launch failed";
            return false;
        }
        for (;;) {
            const int idle = HyperCine_GpuSlotGpuIdle(slot);
            if (idle < 0) {
                into->error = "gpu pyramid failed";
                return false;
            }
            if (idle == 1) break;
        }
        HyperCineStreamTiming timing{};
        HyperCine_GpuSlotReadTiming(slot, &timing);
        const int published = HyperCine_GpuSlotDeviceBandCount(slot);
        std::vector<GpuBandRequest> requests;
        std::vector<int> request_piece;
        std::vector<EntropyTables> request_tables;
        std::vector<EncodedPiece> pieces(refs.size());
        size_t lowpass_bytes = 0;
        for (int index = 0; index < published; ++index) {
            HyperCineDeviceBand device{};
            if (HyperCine_GpuSlotDeviceBand(slot, index, &device) != 0) {
                into->error = "device band lookup failed";
                return false;
            }
            if (device.channel < 0 || device.channel >= 3 || device.level < 0 || device.level >= 3 || device.band < 0 || device.band >= 4) {
                into->error = "device band is outside the pyramid";
                return false;
            }
            const int piece = piece_of[device.channel][device.level][device.band];
            if (piece < 0) {
                into->error = "device band has no subband index";
                return false;
            }
            const BandRef& ref = refs[static_cast<size_t>(piece)];
            EntropyTables local;
            if (!LoadEntropyTables(enc_opaque, ref.subband, &local)) {
                into->error = "codebook load failed";
                return false;
            }
            EncodedPiece& out = pieces[static_cast<size_t>(piece)];
            out.channel = ref.channel;
            out.level = ref.level;
            out.band = ref.band;
            out.subband = ref.subband;
            out.lowpass = ref.lowpass;
            if (ref.lowpass || local.peaks) {
                std::vector<int16_t> host(static_cast<size_t>(device.width) * static_cast<size_t>(device.height));
                if (cudaMemcpy(host.data(), device.device, host.size() * sizeof(int16_t), cudaMemcpyDeviceToHost) != cudaSuccess) {
                    into->error = "lowpass download failed";
                    return false;
                }
                lowpass_bytes += host.size() * sizeof(int16_t);
                const double lowpass_t0 = NowMs();
                ExternalCoeff coeff;
                coeff.channel = ref.channel;
                coeff.level = ref.level;
                coeff.band = ref.band;
                coeff.subband = ref.subband;
                coeff.lowpass = ref.lowpass;
                coeff.data = host.data();
                coeff.elem_bytes = 2;
                coeff.width = ref.width;
                coeff.height = ref.height;
                if (!EncodeOneExternalBand(enc_opaque, transforms_opaque, coeff, &out)) {
                    into->error = "cpu band encode failed";
                    return false;
                }
                into->entropy.lowpass_ms += NowMs() - lowpass_t0;
            } else {
                requests.push_back(GpuBandRequest{device.device, device.width, device.height, device.width,
                                                  ref.channel, ref.level, ref.band, ref.subband});
                request_piece.push_back(piece);
                request_tables.push_back(std::move(local));
            }
        }
        EntropyTiming wall;
        std::vector<GpuBandResult> gpu_results(requests.size());
        if (!requests.empty() &&
            !EncodeHighpassBatch(session, requests.data(), gpu_results.data(), static_cast<int>(requests.size()),
                                 request_tables.data(), config.dispatch, config.streams, &wall)) {
            into->error = "gpu entropy batch failed";
            return false;
        }
        size_t compressed = 0;
        for (size_t i = 0; i < requests.size(); ++i) {
            if (!gpu_results[i].ok) {
                into->error = gpu_results[i].error.empty() ? "gpu band failed" : gpu_results[i].error;
                return false;
            }
            const int piece = request_piece[i];
            const BandRef& ref = refs[static_cast<size_t>(piece)];
            IMAGE* wavelet = transforms[ref.channel]->wavelet[ref.wavelet_index];
            int quantization = wavelet->quantization[ref.band];
            if (quantization <= 0) quantization = wavelet->quant[ref.band];
            if (quantization <= 0) quantization = 1;
            int bits_free = 0;
            EncodedPiece& out = pieces[static_cast<size_t>(piece)];
            const double header_t0 = NowMs();
            if (!BuildHighpassPayload(enc_opaque, ref.band, ref.width, ref.height, ref.subband, quantization,
                                      wavelet->scale[ref.band], request_tables[i], gpu_results[i].coeff_payload,
                                      &out.payload, &bits_free)) {
                into->error = "band payload assembly failed";
                return false;
            }
            into->entropy.header_ms += NowMs() - header_t0;
            out.ok = true;
            out.bits_free = bits_free;
            out.encode_ms = gpu_results[i].timing.total_ms;
            compressed += gpu_results[i].coeff_payload.size();
            into->entropy.scan_ms += gpu_results[i].timing.scan_ms;
            into->entropy.token_ms += gpu_results[i].timing.token_ms;
            into->entropy.codebook_ms += gpu_results[i].timing.codebook_ms;
            into->entropy.prefix_ms += gpu_results[i].timing.prefix_ms;
            into->entropy.pack_ms += gpu_results[i].timing.pack_ms;
        }
        into->entropy.total_ms += wall.total_ms + into->entropy.header_ms + into->entropy.lowpass_ms;
        if (verify && scratch && scratch_bytes) {
            if (!RefreshUpstreamPyramid(enc_opaque, transforms_opaque, channel_count, bgra, pitch, scratch, scratch_bytes)) {
                into->error = "upstream pyramid refresh failed";
                return false;
            }
            for (int index = 0; index < published; ++index) {
                HyperCineDeviceBand device{};
                HyperCine_GpuSlotDeviceBand(slot, index, &device);
                const int piece = piece_of[device.channel][device.level][device.band];
                const BandRef& ref = refs[static_cast<size_t>(piece)];
                std::vector<int16_t> host(static_cast<size_t>(device.width) * static_cast<size_t>(device.height));
                cudaMemcpy(host.data(), device.device, host.size() * sizeof(int16_t), cudaMemcpyDeviceToHost);
                IMAGE* wavelet = transforms[ref.channel]->wavelet[ref.wavelet_index];
                into->coeff_compared += ref.width * ref.height;
                into->coeff_mismatches += CountDiff(wavelet, ref.band, host.data(), ref.width, ref.height);
            }
        }
        EntropyOutput assembled = AssembleEncodedPieces(enc_opaque, transforms_opaque, channel_count, pieces, verify);
        into->mean_color_ms += timing.color_ms;
        into->mean_h2d_ms += timing.h2d_ms;
        into->mean_gpu_ms += timing.compute_ms;
        into->mean_entropy_ms += wall.total_ms + into->entropy.header_ms + into->entropy.lowpass_ms;
        into->mean_assembly_ms += assembled.assembly_ms;
        into->h2d_bytes = timing.h2d_bytes;
        into->d2h_compressed_bytes = compressed;
        into->d2h_lowpass_bytes = lowpass_bytes;
        into->d2h_bytes = compressed + lowpass_bytes;
        into->encoded_bytes = assembled.bitstream_bytes;
        if (!assembled.success) {
            into->error = assembled.error.empty() ? "assembly failed" : assembled.error;
            return false;
        }
        into->bitstream = std::move(assembled.bitstream);
        if (verify) into->byte_match = assembled.byte_match && into->coeff_mismatches == 0;
        return !verify || into->byte_match;
    };

    const double cpu0 = ProcessCpuSeconds();
    if (config.verify) {
        if (!encode_frame(true, &run)) {
            DestroyGpuEntropySession(session);
            HyperCine_GpuSlotDestroy(slot);
            return run;
        }
        run.verified = true;
        const bool matched = run.byte_match;
        const int mismatches = run.coeff_mismatches;
        const int compared = run.coeff_compared;
        const std::vector<uint8_t> sample = run.bitstream;
        run.entropy = EntropyTiming{};
        run.mean_color_ms = run.mean_h2d_ms = run.mean_gpu_ms = 0;
        run.mean_entropy_ms = run.mean_assembly_ms = run.mean_latency_ms = 0;
        run.byte_match = matched;
        run.coeff_mismatches = mismatches;
        run.coeff_compared = compared;
        run.bitstream = sample;
    }
    const int total = config.warmup + config.frames;
    double timed_wall = 0;
    const double t_wall = NowMs();
    for (int frame = 0; frame < total; ++frame) {
        Phase2BRun one;
        const double t0 = NowMs();
        if (!encode_frame(false, &one)) {
            run.error = one.error;
            run.success = false;
            DestroyGpuEntropySession(session);
            HyperCine_GpuSlotDestroy(slot);
            return run;
        }
        const double dt = NowMs() - t0;
        if (frame >= config.warmup) {
            timed_wall += dt;
            run.mean_latency_ms += dt;
            run.entropy.scan_ms += one.entropy.scan_ms;
            run.entropy.token_ms += one.entropy.token_ms;
            run.entropy.codebook_ms += one.entropy.codebook_ms;
            run.entropy.prefix_ms += one.entropy.prefix_ms;
            run.entropy.pack_ms += one.entropy.pack_ms;
            run.entropy.header_ms += one.entropy.header_ms;
            run.entropy.lowpass_ms += one.entropy.lowpass_ms;
            run.entropy.total_ms += one.entropy.total_ms;
            run.mean_color_ms += one.mean_color_ms;
            run.mean_h2d_ms += one.mean_h2d_ms;
            run.mean_gpu_ms += one.mean_gpu_ms;
            run.mean_entropy_ms += one.mean_entropy_ms;
            run.mean_assembly_ms += one.mean_assembly_ms;
            run.h2d_bytes = one.h2d_bytes;
            run.d2h_compressed_bytes = one.d2h_compressed_bytes;
            run.d2h_lowpass_bytes = one.d2h_lowpass_bytes;
            run.d2h_bytes = one.d2h_bytes;
            run.encoded_bytes = one.encoded_bytes;
            run.bitstream = one.bitstream;
        }
    }
    run.wall_ms = NowMs() - t_wall;
    run.frames = config.frames;
    if (config.frames > 0) {
        run.mean_latency_ms /= config.frames;
        run.mean_color_ms /= config.frames;
        run.mean_h2d_ms /= config.frames;
        run.mean_gpu_ms /= config.frames;
        run.mean_entropy_ms /= config.frames;
        run.mean_assembly_ms /= config.frames;
        run.entropy.scan_ms /= config.frames;
        run.entropy.token_ms /= config.frames;
        run.entropy.codebook_ms /= config.frames;
        run.entropy.prefix_ms /= config.frames;
        run.entropy.pack_ms /= config.frames;
        run.entropy.header_ms /= config.frames;
        run.entropy.lowpass_ms /= config.frames;
        run.entropy.total_ms /= config.frames;
        run.sustained_fps = timed_wall > 0 ? 1000.0 * config.frames / timed_wall : 0;
    }
    run.cpu_seconds = ProcessCpuSeconds() - cpu0;
    if (run.wall_ms > 0) run.cpu_cores = run.cpu_seconds / (run.wall_ms / 1000.0);
    int64_t coeff_count = 0;
    for (const BandRef& ref : refs) coeff_count += static_cast<int64_t>(ref.width) * ref.height;
    if (coeff_count > 0) run.bits_per_coeff = (static_cast<double>(run.encoded_bytes) * 8.0) / static_cast<double>(coeff_count);
    run.streams = config.streams;
    run.scan_occupancy_blocks = GpuEntropyScanOccupancy();
    run.success = !config.verify || run.byte_match;
    DestroyGpuEntropySession(session);
    HyperCine_GpuSlotDestroy(slot);
    return run;
}

}  // namespace hypercine
