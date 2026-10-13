// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace HyperCine {
enum class DecodePixelFormat { DeviceBGRA8, PlanarYUV422Signed16 };
enum class DecodeMatrix { BT709, BT601 };
enum class DecodeRange { ComputerGraphics, VideoSystems };
enum class DecodeOrientation { BottomUp, TopDown };
enum class DecodeSchedulingPolicy { Auto, LowLatency, HighThroughput };
enum class DecodeStrategy { Lightweight, Hybrid, HostStreaming };
// For diagnostics only. Normal applications leave this set to Automatic.
enum class DiagnosticDecodeStrategy { Automatic, Lightweight, Hybrid, HostStreaming };
enum class DecodeState { Invalid, Queued, Running, Ready, Failed, Released };
enum class DecodeError {
    None, Busy, NotInitialized, InvalidConfig, BackendUnavailable,
    InvalidRequest, InvalidSample, SampleTooLarge, DeviceFailure, InvalidTicket,
    ConsumerLimit, UnsupportedEncodingMode
};
struct DecoderConfig {
    int maximum_width = 1920;
    int maximum_height = 1080;
    // Zero selects a policy-specific bounded default; explicit range is 1..4.
    int slot_count = 0;
    int host_worker_count = 8;
    int cuda_device = 0;
    DecodeSchedulingPolicy scheduling = DecodeSchedulingPolicy::Auto;
};
struct DecodeRequest {
    DecodePixelFormat format = DecodePixelFormat::DeviceBGRA8;
    DecodeMatrix matrix = DecodeMatrix::BT709;
    DecodeRange range = DecodeRange::ComputerGraphics;
    DecodeOrientation orientation = DecodeOrientation::BottomUp;
    DiagnosticDecodeStrategy diagnostic_strategy = DiagnosticDecodeStrategy::Automatic;
};
struct DecodeTicket {
    uint64_t owner = 0;
    uint64_t value = 0;
    explicit operator bool() const { return owner != 0 && value != 0; }
};
struct DecodeStatus {
    DecodeState state = DecodeState::Invalid;
    DecodeError error = DecodeError::InvalidTicket;
};
// A borrowed native stream handle. CUDA conversion helpers live in decoder_cuda.h.
struct DecodeConsumerStream { void* native_handle = nullptr; };
struct DeviceOutputPlane {
    void* device_pointer = nullptr;
    size_t pitch_bytes = 0;
    int width = 0;
    int height = 0;
};
// Borrowed decoder memory, valid until ReleaseOutput/Release/destruction.
// Planes are BGRA, or Y/U/V (signed 16-bit, effective precision 10).
// Planar output is always top-down. The event is borrowed and must not be destroyed.
struct DeviceOutput {
    std::array<DeviceOutputPlane, 3> planes{};
    int plane_count = 0;
    int width = 0;
    int height = 0;
    int precision = 0;
    int cuda_device = 0;
    DecodePixelFormat format = DecodePixelFormat::DeviceBGRA8;
    DecodeOrientation orientation = DecodeOrientation::BottomUp;
    DecodeMatrix matrix = DecodeMatrix::BT709;
    DecodeRange range = DecodeRange::ComputerGraphics;
    DecodeTicket ticket{};
    void* native_ready_event = nullptr;
};
struct DecoderCapabilities {
    bool device_backend_available = false;
    bool device_bgra8 = false;
    bool planar_yuv422_signed16 = false;
    // GOP-1 interlaced YUV 4:2:2 at coded precision 10. This is not FIELDPLUS,
    // GOP-2, RGB, or any other temporal profile.
    bool interlaced_output = false;
    int maximum_slots = 4;
};
struct DecodeFrameInfo {
    DecodeStrategy strategy = DecodeStrategy::Lightweight;
    uint64_t highpass_bytes = 0;
    uint64_t coefficient_count = 0;
    uint32_t maximum_band_bytes = 0;
    uint32_t dense_band_count = 0;
    std::array<uint64_t, 3> level_payload_bytes{};
    uint64_t classifier_nanoseconds = 0; // Observation only, never an input.
};
struct DecoderMemoryStats {
    int slot_count = 0;
    size_t device_bytes = 0;
    size_t pinned_host_bytes = 0;
    size_t pageable_capacity_bytes = 0; // Owned vector/object capacity, excludes thread stacks/driver.
    size_t immutable_device_bytes = 0; // Included in device_bytes; shared across this instance's slots.
    size_t immutable_host_bytes = 0;
    uint64_t cuda_allocation_calls = 0; // Successful owned buffer allocations at initialization.
};
class DecoderInspector;
class Decoder {
public:
    explicit Decoder(const DecoderConfig& config = {});
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
    DecodeError Initialize();
    void Release();
    bool IsInitialized() const;
    DecoderCapabilities Capabilities() const;
    int SlotCount() const;
    // Accepted malformed samples receive a Failed ticket requiring release.
    // Saturation returns an empty ticket and Busy, without consuming a slot.
    // Input bytes may be reused as soon as this call returns. Host indexing is
    // synchronous; GPU reconstruction is asynchronous.
    DecodeTicket QueueDecode(const uint8_t* sample, size_t size,
                             const DecodeRequest& request = {}, DecodeError* error = nullptr);
    DecodeStatus PollDecode(DecodeTicket ticket);
    DecodeStatus WaitForDecode(DecodeTicket ticket);
    bool GetDeviceOutput(DecodeTicket ticket, DeviceOutput* output);
    // Register up to eight distinct consumer streams per ticket. Enqueue all
    // consumer work before ReleaseOutput, which drains registered streams.
    bool StreamWait(DecodeTicket ticket, DecodeConsumerStream consumer);
    bool ReleaseOutput(DecodeTicket ticket);
    bool GetFrameInfo(DecodeTicket ticket, DecodeFrameInfo* info);
    DecoderMemoryStats MemoryStats() const;
private:
    friend class DecoderInspector;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace HyperCine
