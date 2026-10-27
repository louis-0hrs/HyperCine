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

#include <cstddef>

#include "CFHDEncoder.h"

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
}

#include "VideoBuffers.h"
#include "SampleMetadata.h"

#define private public
#define protected public
#include "SampleEncoder.h"
#undef private
#undef protected

#include "hypercine/entropy_gpu.h"
#include "hypercine/entropy_stage.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int Fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

bool WaitForTerminalStatus(hypercine::GpuEntropyFrameSlot* slot,
                           hypercine::GpuStageStatus* status,
                           std::string* error,
                           int seconds = 8) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto current = hypercine::PollGpuEntropyFrameD2HStatus(slot);
        if (current == hypercine::GpuStageStatus::Pending) continue;
        *status = current;
        if (current == hypercine::GpuStageStatus::Failed) {
            const char* text = hypercine::GpuEntropyFrameError(slot);
            *error = text ? text : "";
        }
        return true;
    }
    return false;
}

bool WordsWithinBound(const hypercine::EntropySymbols& symbols, int virtual_len, std::string* why) {
    if (!symbols.ok) {
        if (why) *why = symbols.error.empty() ? "CPU entropy failed" : symbols.error;
        return false;
    }
    const int words = static_cast<int>(symbols.coeff_bytes.size() / sizeof(uint32_t));
    const int bound = hypercine::EntropyPackedWordUpperBound(virtual_len);
    if (bound > virtual_len + 1) {
        if (why) *why = "analytical bound exceeds N+1";
        return false;
    }
    if (words > bound) {
        if (why) *why = "encoded words " + std::to_string(words) + " exceed bound " + std::to_string(bound);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    for (int n = 1; n <= 64; ++n) {
        const int bound = hypercine::EntropyPackedWordUpperBound(n);
        if (bound > n + 1 || bound < 1) {
            return Fail("word bound is not <= N+1 at N=" + std::to_string(n));
        }
    }

    CFHD_EncoderRef ref = nullptr;
    if (CFHD_OpenEncoder(&ref, nullptr) != CFHD_ERROR_OKAY) return Fail("CFHD_OpenEncoder failed");
    if (CFHD_PrepareToEncode(ref, 1920, 1080, CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_YUV_422,
                             CFHD_ENCODING_FLAGS_NONE, CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
        return Fail("CFHD_PrepareToEncode failed");
    }
    std::vector<uint8_t> frame(1920u * 1080u * 4u, 128u);
    if (CFHD_EncodeSample(ref, frame.data(), 1920 * 4) != CFHD_ERROR_OKAY) return Fail("init encode failed");
    auto* sample = reinterpret_cast<CSampleEncoder*>(ref);
    auto* enc = reinterpret_cast<::encoder*>(sample->m_encoder);

    int tables_checked = 0;
    hypercine::EntropyTables live;
    for (int subband = 0; subband < 17; ++subband) {
        hypercine::EntropyTables tables;
        if (!hypercine::LoadEntropyTables(enc, subband, &tables)) continue;
        std::string why;
        if (!hypercine::ValidateEntropyTablesForWordBound(tables, &why)) {
            return Fail("subband " + std::to_string(subband) + ": " + why);
        }
        ++tables_checked;
        if (live.run_length == 0) live = tables;
    }
    CFHD_CloseEncoder(ref);
    if (tables_checked < 1 || live.run_length <= 1) return Fail("no entropy codebook was validated");

    const int patterns_w = 96;
    const int patterns_h = 64;
    const int virtual_len = patterns_w * patterns_h;
    std::vector<int16_t> coeff(static_cast<size_t>(virtual_len));
    const int16_t patterns[][2] = {{0, 0}, {32767, 32767}, {0, 32767}, {32767, 0}};
    const char* names[] = {"zeros", "all-max", "zero-max", "max-zero"};
    for (int p = 0; p < 4; ++p) {
        for (int i = 0; i < virtual_len; ++i) {
            coeff[static_cast<size_t>(i)] = (i & 1) ? patterns[p][1] : patterns[p][0];
        }
        if (p == 0) std::fill(coeff.begin(), coeff.end(), int16_t{0});
        if (p == 1) std::fill(coeff.begin(), coeff.end(), int16_t{32767});
        const hypercine::EntropySymbols symbols =
            hypercine::EncodeHighpassStages(coeff.data(), patterns_w, patterns_h, patterns_w, live, false);
        std::string why;
        if (!WordsWithinBound(symbols, virtual_len, &why)) {
            return Fail(std::string(names[p]) + ": " + why);
        }
    }

    // Row-gap virtual zeros must stay inside the same bound.
    {
        const int width = 224;
        const int pitch = 256;
        const int height = 16;
        std::vector<int16_t> gapped(static_cast<size_t>(pitch) * height, 32767);
        const hypercine::EntropySymbols symbols =
            hypercine::EncodeHighpassStages(gapped.data(), width, height, pitch, live, false);
        std::string why;
        if (!WordsWithinBound(symbols, height * pitch, &why)) return Fail("row gap: " + why);
    }

    hypercine::GpuEntropySession* session = hypercine::CreateGpuEntropySession();
    hypercine::GpuEntropyFrameSlot* slot = hypercine::CreateGpuEntropyFrameSlot(patterns_w, patterns_h);
    if (!session || !slot) return Fail("GPU entropy slot allocation failed");

    std::vector<int16_t> dense(static_cast<size_t>(virtual_len), 32767);
    int16_t* device = nullptr;
    if (cudaMalloc(&device, dense.size() * sizeof(int16_t)) != cudaSuccess ||
        cudaMemcpy(device, dense.data(), dense.size() * sizeof(int16_t), cudaMemcpyHostToDevice) != cudaSuccess) {
        return Fail("device coefficient upload failed");
    }

    hypercine::GpuBandRequest request{};
    request.device_coeff = device;
    request.width = patterns_w;
    request.height = patterns_h;
    request.pitch_px = patterns_w;
    request.subband = 1;

    hypercine::GpuEntropyFrameConfig cfg{};
    cfg.entropy_streams = 1;
    cfg.d2h_mode = hypercine::CompressedD2HMode::FrameConsolidated;
    cfg.kernel_mode = hypercine::GpuEntropyKernelMode::FusedScanOptimized;
    cfg.use_cuda_graph = false;
    cfg.lpt_schedule = false;

    hypercine::SetGpuEntropyCapacityOverrideForTest(slot, 1u, 1u);
    if (!hypercine::QueueGpuEntropyFrame(session, slot, &request, 1, &live, cfg, nullptr, nullptr)) {
        const char* err = hypercine::GpuEntropyFrameError(slot);
        return Fail(std::string("overflow queue failed early: ") + (err ? err : ""));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    hypercine::GpuStageStatus status = hypercine::GpuStageStatus::Pending;
    while (std::chrono::steady_clock::now() < deadline) {
        status = hypercine::PollGpuEntropyFrameD2HStatus(slot);
        if (status != hypercine::GpuStageStatus::Pending) break;
    }
    if (status != hypercine::GpuStageStatus::Failed) {
        return Fail("capacity overflow was not reported as Failed");
    }
    const std::string overflow_error = hypercine::GpuEntropyFrameError(slot);
    if (overflow_error.find("capacity") == std::string::npos) {
        return Fail("overflow error did not name the capacity bound: " + overflow_error);
    }
    if (hypercine::PollGpuEntropyFrameD2HStatus(slot) != hypercine::GpuStageStatus::Failed) {
        return Fail("a failed entropy poll later looked pending");
    }

    // Fail-closed capacity diagnostics: an induced workspace allocation failure
    // must fail the queue with the requested bytes, device capacity and CUDA
    // status, and must not be converted into a skip or pass.
    {
        hypercine::GpuEntropySession* cap_session = hypercine::CreateGpuEntropySession();
        hypercine::GpuEntropyFrameSlot* cap_slot =
            hypercine::CreateGpuEntropyFrameSlot(patterns_w, patterns_h);
        if (!cap_session || !cap_slot) return Fail("capacity diagnostic slot allocation failed");

        hypercine::SetGpuEntropyAllocationFailureInjectionForTest(1u);
        if (hypercine::QueueGpuEntropyFrame(cap_session, cap_slot, &request, 1, &live, cfg, nullptr, nullptr)) {
            return Fail("injected workspace allocation failure did not fail the queue");
        }
        const std::string capacity_error = hypercine::GpuEntropyFrameError(cap_slot);
        if (capacity_error.find("UNSUPPORTED_RESOURCE_BOUND") == std::string::npos ||
            capacity_error.find("requested") == std::string::npos ||
            capacity_error.find("free ") == std::string::npos ||
            capacity_error.find("CUDA ") == std::string::npos) {
            return Fail("capacity diagnostic is incomplete: " + capacity_error);
        }

        // The injection is single-shot: the same session/slot must allocate and
        // run normally on retry.
        if (!hypercine::QueueGpuEntropyFrame(cap_session, cap_slot, &request, 1, &live, cfg, nullptr, nullptr)) {
            const char* err = hypercine::GpuEntropyFrameError(cap_slot);
            return Fail(std::string("allocation failure injection was not single-shot: ") + (err ? err : ""));
        }
        bool finished = false;
        const auto cap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < cap_deadline) {
            const auto status = hypercine::PollGpuEntropyFrameD2HStatus(cap_slot);
            if (status == hypercine::GpuStageStatus::Complete) {
                finished = true;
                break;
            }
            if (status == hypercine::GpuStageStatus::Failed) {
                const char* err = hypercine::GpuEntropyFrameError(cap_slot);
                return Fail(std::string("capacity retry failed: ") + (err ? err : ""));
            }
        }
        if (!finished) return Fail("capacity retry did not finish");
        hypercine::DestroyGpuEntropyFrameSlot(cap_slot);
        hypercine::DestroyGpuEntropySession(cap_session);
    }

    // Production slot-creation diagnostics must survive session cleanup and
    // retain the first actionable CUDA failure.
    {
        hypercine::GpuEntropySession* cleanup_session = hypercine::CreateGpuEntropySession();
        // 1M x 1M pixels requires ~8.8 TB of device arenas on any current GPU.
        hypercine::GpuEntropyFrameSlot* impossible =
            hypercine::CreateGpuEntropyFrameSlot(1 << 20, 1 << 20);
        if (impossible) {
            hypercine::DestroyGpuEntropyFrameSlot(impossible);
            return Fail("impossible slot allocation unexpectedly succeeded");
        }
        const std::string creation_error = hypercine::GpuEntropySlotCreationError();
        if (creation_error.find("UNSUPPORTED_RESOURCE_BOUND") == std::string::npos ||
            creation_error.find("requested") == std::string::npos ||
            creation_error.find("free ") == std::string::npos ||
            creation_error.find("CUDA ") == std::string::npos) {
            return Fail("slot creation diagnostic is incomplete: " + creation_error);
        }
        hypercine::DestroyGpuEntropySession(cleanup_session);
        if (creation_error != hypercine::GpuEntropySlotCreationError()) {
            return Fail("slot creation diagnostic changed during cleanup");
        }
    }

    // One-shot injection is process-wide: two concurrent sessions must observe
    // exactly one injected failure, and that session must recover on retry.
    {
        constexpr int kWorkers = 2;
        hypercine::GpuEntropySession* sessions[kWorkers] = {};
        hypercine::GpuEntropyFrameSlot* worker_slots[kWorkers] = {};
        bool queued[kWorkers] = {};
        bool completed[kWorkers] = {};
        std::string worker_errors[kWorkers];
        for (int i = 0; i < kWorkers; ++i) {
            sessions[i] = hypercine::CreateGpuEntropySession();
            worker_slots[i] = hypercine::CreateGpuEntropyFrameSlot(patterns_w, patterns_h);
            if (!sessions[i] || !worker_slots[i]) return Fail("concurrent capacity slot allocation failed");
        }

        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::thread workers[kWorkers];
        for (int i = 0; i < kWorkers; ++i) {
            workers[i] = std::thread([&, i]() {
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                queued[i] = hypercine::QueueGpuEntropyFrame(
                    sessions[i], worker_slots[i], &request, 1, &live, cfg, nullptr, nullptr);
                if (!queued[i]) {
                    const char* err = hypercine::GpuEntropyFrameError(worker_slots[i]);
                    worker_errors[i] = err ? err : "";
                    return;
                }
                hypercine::GpuStageStatus status = hypercine::GpuStageStatus::Pending;
                if (!WaitForTerminalStatus(worker_slots[i], &status, &worker_errors[i])) {
                    worker_errors[i] = "concurrent queue did not reach a terminal state";
                    return;
                }
                completed[i] = status == hypercine::GpuStageStatus::Complete;
            });
        }
        while (ready.load(std::memory_order_acquire) < kWorkers) std::this_thread::yield();
        hypercine::SetGpuEntropyAllocationFailureInjectionForTest(1u);
        go.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();

        int failures = 0;
        int successes = 0;
        int failed_index = -1;
        for (int i = 0; i < kWorkers; ++i) {
            if (!queued[i]) {
                ++failures;
                failed_index = i;
                if (worker_errors[i].find("UNSUPPORTED_RESOURCE_BOUND") == std::string::npos) {
                    return Fail("concurrent injected failure lacked the capacity diagnostic: " + worker_errors[i]);
                }
            } else if (completed[i]) {
                ++successes;
            } else {
                return Fail("concurrent capacity run did not complete: " + worker_errors[i]);
            }
        }
        if (failures != 1 || successes != 1) {
            return Fail("one-shot injection was not exclusive: failures=" + std::to_string(failures) +
                        " successes=" + std::to_string(successes));
        }

        if (!hypercine::QueueGpuEntropyFrame(sessions[failed_index], worker_slots[failed_index],
                                            &request, 1, &live, cfg, nullptr, nullptr)) {
            const char* err = hypercine::GpuEntropyFrameError(worker_slots[failed_index]);
            return Fail(std::string("concurrent retry queue failed: ") + (err ? err : ""));
        }
        hypercine::GpuStageStatus retry_status = hypercine::GpuStageStatus::Pending;
        std::string retry_error;
        if (!WaitForTerminalStatus(worker_slots[failed_index], &retry_status, &retry_error) ||
            retry_status != hypercine::GpuStageStatus::Complete) {
            return Fail("concurrent retry did not complete: " + retry_error);
        }

        for (int i = 0; i < kWorkers; ++i) {
            hypercine::DestroyGpuEntropyFrameSlot(worker_slots[i]);
            hypercine::DestroyGpuEntropySession(sessions[i]);
        }
    }

    cudaFree(device);
    hypercine::DestroyGpuEntropyFrameSlot(slot);
    hypercine::DestroyGpuEntropySession(session);

    std::cout << "PASS: entropy word bound holds for " << tables_checked
              << " codebooks, worst-case CPU patterns, GPU overflow is Failed,"
              << " allocation capacity is diagnosed fail-closed, and the one-shot"
              << " injection is exclusive across concurrent sessions\n";
    return 0;
}
