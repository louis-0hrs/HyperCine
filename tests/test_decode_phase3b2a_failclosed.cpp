// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Failure-path coverage for the fail-closed CUDA submission handling in
// CudaDecoderPipelinePhase2E (issue #16 / PR #187). Every negative case uses
// the deterministic TestCudaSubmitSite failpoint; no CUDA call is made to fail
// by hardware misuse.

#include "decode_phase3a_rc_support.h"
#include "hypercine/research/decode_phase2e.h"
#include "../src/core/decoder_backend.h"
#include "../src/core/decode_fieldplus.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ProductionDecodeTest;
using namespace HyperCine;
using namespace HyperCine::DecodePhase1;

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr uint32_t kSeed = 0x1337BEEF;

void Require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

void Say(const char* name, bool pass, const char* detail = "") {
    std::cout << (pass ? "PASS " : "FAIL ") << name;
    if (*detail) std::cout << " (" << detail << ")";
    std::cout << '\n';
}

struct Gop1Fixture {
    Sample sample;
    std::vector<GpuEntropyBandDescriptor> bands;
};

Gop1Fixture LoadGop1() {
    Bytes input = DecodeRC::HoldoutInput(kWidth, kHeight, 6);
    Gop1Fixture f;
    f.sample = DecodeRC::EncodeInput(input, kWidth, kHeight, CFHD_PIXEL_FORMAT_YUY2,
        CFHD_ENCODING_QUALITY_FILMSCAN3, 6, CFHD_ENCODING_FLAGS_NONE);
    DecodeError status = DecodeError::InvalidSample;
    Require(ParseCineFormSampleValidated(f.sample.compressed.data(), f.sample.compressed.size(),
        24, f.bands, nullptr, &status), "parse GOP-1 sample");
    Require(status == DecodeError::None && f.bands.size() == 30, "GOP-1 band topology");
    return f;
}

struct FieldPlusFixture {
    Bytes sample;
    FieldPlusSampleDescriptor parsed;
};

FieldPlusFixture LoadFieldPlus(const std::filesystem::path& root) {
    const auto path = root / "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-prog-f2-bands54-fmt-1.cfhd";
    std::ifstream in(path, std::ios::binary);
    Require(bool(in), "FIELDPLUS fixture open");
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0);
    FieldPlusFixture f;
    f.sample.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(f.sample.data()), size);
    Require(bool(in), "FIELDPLUS fixture read");
    DecodeError status = DecodeError::InvalidSample;
    Require(ParseProgressiveFieldPlusSampleValidated(f.sample.data(), f.sample.size(), 24,
        f.parsed, nullptr, &status), "parse FIELDPLUS sample");
    Require(status == DecodeError::None, "FIELDPLUS parser status");
    return f;
}

struct Pipe {
    CudaDecoderPipelinePhase2E pipeline;
    Pipe(Phase2EPipelineMode mode = Phase2EPipelineMode::Phase2EFullOverlap) {
        Require(pipeline.Initialize(kWidth, kHeight, 1, 10, 1, 2), "pipeline init");
        pipeline.SetPipelineMode(mode);
        Require(pipeline.SetWorkerCount(1), "single worker");
        Require(pipeline.SetReplayStreamCount(2), "replay streams");
    }
    uint64_t Queue(const Gop1Fixture& f, bool guard = true) {
        return pipeline.QueueDecodeParsed(f.sample.compressed.data(), f.sample.compressed.size(),
            f.bands, DeviceSurfaceType::BGRA8, ColorMatrix::BT709, ColorRange::CG,
            RowOrientation::BottomUp, guard, 0.0f, false);
    }
    uint64_t QueueFieldPlus(const FieldPlusFixture& f, int frame = 0, bool guard = true) {
        return pipeline.QueueDecodeFieldPlusParsed(f.sample.data(), f.sample.size(), f.parsed, frame,
            DeviceSurfaceType::BGRA8, ColorMatrix::BT709, ColorRange::CG,
            RowOrientation::BottomUp, guard);
    }
};

std::string DecodeOk(CudaDecoderPipelinePhase2E& pipeline, const Gop1Fixture& f, const char* what) {
    const uint64_t ticket = pipeline.QueueDecodeParsed(f.sample.compressed.data(),
        f.sample.compressed.size(), f.bands, DeviceSurfaceType::BGRA8, ColorMatrix::BT709,
        ColorRange::CG, RowOrientation::BottomUp, true, 0.0f, false);
    Require(ticket != 0, what);
    Require(pipeline.WaitForDecode(ticket), what);
    Require(pipeline.PollDecode(ticket) == DecodeTicketStatus::Complete, what);
    Bytes image(size_t(f.sample.width) * f.sample.height * 4);
    Require(pipeline.DownloadBgraToHost(ticket, image.data(), size_t(f.sample.width) * 4), what);
    const std::string hash = Hash(image.data(), image.size());
    Require(pipeline.ReleaseOutput(ticket), what);
    return hash;
}

void CheckDagFailure(const char* name, TestCudaSubmitSite site, bool guard) {
    Pipe p;
    const Gop1Fixture f = LoadGop1();
    p.pipeline.SetTestCudaFailureHook([site](TestCudaSubmitSite s) {
        return s == site ? cudaErrorInvalidValue : cudaSuccess;
    });
    const uint64_t ticket = p.Queue(f, guard);
    Require(ticket == 0, "DAG submission failure must not publish a ticket");
    Require(p.pipeline.PollDecode(0) == DecodeTicketStatus::Invalid, "zero ticket is invalid");
    Require(!p.pipeline.WaitForDecode(0), "zero ticket never waits");
    Require(p.pipeline.GetTestLastCudaSubmitError() == cudaErrorInvalidValue,
        "first submission error preserved");
    p.pipeline.SetTestCudaFailureHook(nullptr);
    DecodeOk(p.pipeline, f, "slot recovers after DAG failure");
    Say(name, true, "ticket 0, error preserved, slot recovered");
}

void CheckBlockingFailure(const char* name, TestCudaSubmitSite site) {
    Pipe p(Phase2EPipelineMode::Phase2DFrameBarrier);
    const Gop1Fixture f = LoadGop1();
    p.pipeline.SetTestCudaFailureHook([site](TestCudaSubmitSite s) {
        return s == site ? cudaErrorInvalidValue : cudaSuccess;
    });
    const uint64_t ticket = p.Queue(f, true);
    Require(ticket == 0, "blocking submission failure must not publish a ticket");
    Require(p.pipeline.GetTestLastCudaSubmitError() == cudaErrorInvalidValue,
        "direct submission error preserved (no cudaGetLastError overwrite)");
    p.pipeline.SetTestCudaFailureHook(nullptr);
    DecodeOk(p.pipeline, f, "slot recovers after blocking failure");
    Say(name, true, "ticket 0, error preserved, slot recovered");
}

void CheckGpuIndexFrameErrorCopy() {
    Pipe p(Phase2EPipelineMode::Phase2FAGpuIndexBatch);
    const Gop1Fixture f = LoadGop1();
    // Seed this single slot's ev_ready with a completed frame. Recovery must
    // reuse the same slot and reproduce its bytes after actual abandonment.
    const std::string baseline = DecodeOk(p.pipeline, f, "Phase2FA baseline decode");
    int copy_hits = 0;
    int ready_hits = 0;
    p.pipeline.SetTestCudaFailureHook([&](TestCudaSubmitSite site) {
        if (site == TestCudaSubmitSite::DagReadyEvent) ++ready_hits;
        if (site == TestCudaSubmitSite::DagFrameErrorCopy) {
            ++copy_hits;
            return cudaErrorInvalidValue;
        }
        return cudaSuccess;
    });
    // With the production guard disabled, only GpuIndex() can reach the
    // targeted DAG copy. A missing/unreachable hook must fail this test.
    const uint64_t ticket = p.Queue(f, false);
    Require(copy_hits == 1, "Phase2FA DAG frame-error copy failpoint reached exactly once");
    Require(ready_hits == 0, "failed DAG copy must suppress ready event submission");
    Require(ticket == 0, "failed Phase2FA DAG copy must not publish a ticket");
    Require(p.pipeline.GetTestLastCudaSubmitError() == cudaErrorInvalidValue,
        "direct DAG copy error preserved");
    Require(p.pipeline.PollDecode(ticket) == DecodeTicketStatus::Invalid,
        "failed Phase2FA queue ticket is invalid despite stale ev_ready");
    Require(!p.pipeline.WaitForDecode(ticket), "failed Phase2FA queue cannot wait successfully");
    p.pipeline.SetTestCudaFailureHook(nullptr);
    Require(DecodeOk(p.pipeline, f, "Phase2FA slot recovery") == baseline,
        "same-slot Phase2FA recovery must reproduce baseline BGRA hash");
    Say("dag_frame_error_copy_failure", true,
        "Phase2FA, copy hits=1, ready hits=0, ticket 0, error preserved, same-slot hash unchanged");
}

void CheckFieldPlusFailure(const char* name, TestCudaSubmitSite site, const FieldPlusFixture& f) {
    Pipe p;
    p.pipeline.SetTestCudaFailureHook([site](TestCudaSubmitSite s) {
        return s == site ? cudaErrorInvalidValue : cudaSuccess;
    });
    const uint64_t ticket = p.QueueFieldPlus(f);
    Require(ticket == 0, "FIELDPLUS submission failure must not publish a ticket");
    Require(p.pipeline.GetTestLastCudaSubmitError() == cudaErrorInvalidValue,
        "FIELDPLUS submission error preserved");
    p.pipeline.SetTestCudaFailureHook(nullptr);
    const uint64_t ok = p.QueueFieldPlus(f);
    Require(ok != 0 && p.pipeline.WaitForDecode(ok), "FIELDPLUS slot recovers");
    Require(p.pipeline.ReleaseOutput(ok), "FIELDPLUS release after recovery");
    Say(name, true, "ticket 0, error preserved, slot recovered");
}

void CheckStaleReady() {
    Pipe p;
    const Gop1Fixture f = LoadGop1();
    const std::string baseline = DecodeOk(p.pipeline, f, "stale-ready seed frame");

    // Same single slot and the same ev_ready event object: the previous frame's
    // completed record is exactly the stale state the fix must not trust.
    const uint64_t poll_ticket = p.Queue(f);
    Require(poll_ticket != 0, "second frame queued");
    Require(p.pipeline.InjectTestCudaFailure(poll_ticket, cudaErrorInvalidValue), "inject poll failure");
    Require(p.pipeline.PollDecode(poll_ticket) == DecodeTicketStatus::Failed,
        "PollDecode must not complete from a stale ev_ready");
    Require(!p.pipeline.WaitForDecode(poll_ticket), "WaitForDecode rejects injected failure");
    Require(p.pipeline.ReleaseOutput(poll_ticket), "release failed ticket");
    Say("poll_stale_ev_ready", true, "Failed, no stale Complete");

    const uint64_t wait_ticket = p.Queue(f);
    Require(wait_ticket != 0, "third frame queued");
    Require(p.pipeline.InjectTestCudaFailure(wait_ticket, cudaErrorInvalidValue), "inject wait failure");
    Require(!p.pipeline.WaitForDecode(wait_ticket), "WaitForDecode must reject a stale completed event");
    Require(p.pipeline.PollDecode(wait_ticket) == DecodeTicketStatus::Failed, "PollDecode agrees");
    Require(p.pipeline.ReleaseOutput(wait_ticket), "release failed wait ticket");
    Say("wait_stale_ev_ready", true, "false, no stale success");

    const std::string recovered = DecodeOk(p.pipeline, f, "recovery after stale-ready failures");
    Require(recovered == baseline, "successful decode is unchanged after failure injection");
    Say("slot_recovery_after_stale_ready", true, "BGRA hash unchanged");
}

void CheckInertHook() {
    Pipe p;
    const Gop1Fixture f = LoadGop1();
    const std::string plain = DecodeOk(p.pipeline, f, "baseline decode");
    p.pipeline.SetTestCudaFailureHook([](TestCudaSubmitSite) { return cudaSuccess; });
    const std::string hooked = DecodeOk(p.pipeline, f, "decode with inert failpoint");
    Require(plain == hooked, "inert failpoint must not change decode math");
    Say("inert_failpoint_parity", true, "BGRA hash identical");
}

int Run(const std::filesystem::path& root) {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        std::cout << "SKIP no CUDA device\n";
        return 77;
    }

    CheckInertHook();

    CheckDagFailure("dag_stream_wait_failure", TestCudaSubmitSite::DagStreamWait, false);
    CheckDagFailure("dag_stream_wait_failure_guarded", TestCudaSubmitSite::DagStreamWait, true);
    CheckDagFailure("dag_event_record_failure", TestCudaSubmitSite::DagEventRecord, false);
    CheckDagFailure("dag_launch_result_failure", TestCudaSubmitSite::DagLaunchResult, false);
    CheckDagFailure("dag_color_launch_failure", TestCudaSubmitSite::DagColorLaunch, false);
    CheckDagFailure("dag_ready_event_failure", TestCudaSubmitSite::DagReadyEvent, false);
    CheckGpuIndexFrameErrorCopy();

    CheckBlockingFailure("blocking_color_start_event_failure", TestCudaSubmitSite::BlockingColorStartEvent);
    CheckBlockingFailure("blocking_color_launch_failure", TestCudaSubmitSite::BlockingColorLaunch);
    CheckBlockingFailure("blocking_color_stop_event_failure", TestCudaSubmitSite::BlockingColorStopEvent);
    CheckBlockingFailure("blocking_frame_error_copy_failure", TestCudaSubmitSite::BlockingFrameErrorCopy);
    CheckBlockingFailure("blocking_ready_event_failure", TestCudaSubmitSite::BlockingReadyEvent);

    const FieldPlusFixture fp = LoadFieldPlus(root);
    CheckFieldPlusFailure("fieldplus_color_start_event_failure", TestCudaSubmitSite::FieldPlusColorStartEvent, fp);
    CheckFieldPlusFailure("fieldplus_color_launch_failure", TestCudaSubmitSite::FieldPlusColorLaunch, fp);
    CheckFieldPlusFailure("fieldplus_color_stop_event_failure", TestCudaSubmitSite::FieldPlusColorStopEvent, fp);
    CheckFieldPlusFailure("fieldplus_frame_error_copy_failure", TestCudaSubmitSite::FieldPlusFrameErrorCopy, fp);
    CheckFieldPlusFailure("fieldplus_ready_event_failure", TestCudaSubmitSite::FieldPlusReadyEvent, fp);

    CheckStaleReady();
    Say("all_failclosed_cases", true, "19 negative cases exercised");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: test_decode_phase3b2a_failclosed <repo-root>");
        std::srand(kSeed);
        return Run(std::filesystem::path(argv[1]));
    } catch (const std::exception& e) {
        std::cout << "FAIL " << e.what() << '\n';
        return 1;
    }
}
