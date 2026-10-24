// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/decoder.h"
#include "hypercine/research/decoder_diagnostics.h"
#include "hypercine/research/decode_phase2e.h"
#include "decoder_backend.h"
#include "decode_fieldplus.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace HyperCine {
namespace {
std::atomic<uint64_t> next_owner{1};
using Clock = std::chrono::steady_clock;
bool ValidRequest(const DecodeRequest& r) {
    return (r.format == DecodePixelFormat::DeviceBGRA8 || r.format == DecodePixelFormat::PlanarYUV422Signed16) &&
        (r.matrix == DecodeMatrix::BT709 || r.matrix == DecodeMatrix::BT601) &&
        (r.range == DecodeRange::ComputerGraphics || r.range == DecodeRange::VideoSystems) &&
        (r.orientation == DecodeOrientation::BottomUp || r.orientation == DecodeOrientation::TopDown) &&
        r.frame_index >= 0 &&
        unsigned(r.diagnostic_strategy) <= unsigned(DiagnosticDecodeStrategy::HostStreaming);
}
}
struct Decoder::Impl {
    explicit Impl(DecoderConfig c) : config(c) { parsed.reserve(30); }
    struct Slot {
        DecodeTicket ticket{};
        uint64_t backend_ticket = 0;
        DecodeStatus status{DecodeState::Released, DecodeError::None};
        DecodeRequest request{};
        DecodeFrameInfo info{};
        std::array<void*, 8> consumers{};
        size_t consumer_count = 0;
    };
    DecoderConfig config;
    uint64_t owner = next_owner.fetch_add(1);
    uint64_t next_ticket = 1;
    bool initialized = false;
    mutable std::mutex mutex;
    CudaDecoderPipelinePhase2E backend;
    std::vector<Slot> slots;
    std::vector<GpuEntropyBandDescriptor> parsed;
    std::vector<Phase2FCBandRecord> band_records;
    Slot* Find(DecodeTicket t) {
        if (!t || t.owner != owner) return nullptr;
        for (auto& s : slots) if (s.ticket.value == t.value) return &s;
        return nullptr;
    }
    void SelectDevice() const {
#ifdef HYPERCINE_CUDA_AVAILABLE
        cudaSetDevice(config.cuda_device);
#endif
    }
    DecodeStatus Update(Slot* s, bool wait) {
        if (!s) return {};
        if (s->status.state != DecodeState::Running) return s->status;
        const auto state = wait ? (backend.WaitForDecode(s->backend_ticket) ? DecodeTicketStatus::Complete : DecodeTicketStatus::Failed)
                                : backend.PollDecode(s->backend_ticket);
        if (state == DecodeTicketStatus::Complete) s->status = {DecodeState::Ready, DecodeError::None};
        else if (state == DecodeTicketStatus::Failed || state == DecodeTicketStatus::Invalid)
            s->status = {DecodeState::Failed, DecodeError::DeviceFailure};
        return s->status;
    }
    bool ReleaseSlot(Slot& s) {
        if (s.status.state == DecodeState::Released) return false;
        Update(&s, true);
#ifdef HYPERCINE_CUDA_AVAILABLE
        for (size_t i = 0; i < s.consumer_count; ++i)
            cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(s.consumers[i]));
#endif
        if (s.backend_ticket) backend.ReleaseOutput(s.backend_ticket);
        s.backend_ticket = 0;
        s.consumer_count = 0;
        s.status.state = DecodeState::Released;
        return true;
    }
};
Decoder::Decoder(const DecoderConfig& config) : impl_(std::make_unique<Impl>(config)) {}
Decoder::~Decoder() { Release(); }
DecodeError Decoder::Initialize() {
    Release();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto& c = impl_->config;
    if (c.maximum_width < 64 || c.maximum_height < 64 || c.maximum_width > 7680 || c.maximum_height > 4320 ||
        c.maximum_width % 16 || c.maximum_height % 8 || c.slot_count < 0 || c.slot_count > 4 ||
        c.host_worker_count < 0 || c.host_worker_count > 16 || c.cuda_device < 0 ||
        unsigned(c.scheduling) > unsigned(DecodeSchedulingPolicy::HighThroughput)) return DecodeError::InvalidConfig;
#ifndef HYPERCINE_CUDA_AVAILABLE
    return DecodeError::BackendUnavailable;
#else
    const int slots = c.slot_count ? c.slot_count : (c.scheduling == DecodeSchedulingPolicy::LowLatency ? 1 :
        c.scheduling == DecodeSchedulingPolicy::HighThroughput ? 3 : 2);
    try {
        if (!impl_->backend.Initialize(c.maximum_width, c.maximum_height, slots, 10, c.host_worker_count, 4, c.cuda_device)) {
            impl_->backend.Release();
            return DecodeError::BackendUnavailable;
        }
        impl_->backend.PrepareProductionScratch();
        impl_->backend.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::AdaptiveDynamic);
        impl_->slots.resize(size_t(slots));
        impl_->band_records.reserve(27);
        impl_->initialized = true;
        return DecodeError::None;
    } catch (...) {
        impl_->backend.Release();
        impl_->slots.clear();
        return DecodeError::BackendUnavailable;
    }
#endif
}
void Decoder::Release() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->SelectDevice();
    for (auto& s : impl_->slots) impl_->ReleaseSlot(s);
    impl_->backend.Release();
    impl_->slots.clear();
    impl_->initialized = false;
    impl_->owner = next_owner.fetch_add(1);
}
bool Decoder::IsInitialized() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->initialized; }
int Decoder::SlotCount() const { std::lock_guard<std::mutex> lock(impl_->mutex); return int(impl_->slots.size()); }
DecoderCapabilities Decoder::Capabilities() const {
    DecoderCapabilities out;
#ifdef HYPERCINE_CUDA_AVAILABLE
    int count = 0;
    out.device_backend_available = cudaGetDeviceCount(&count) == cudaSuccess && count > impl_->config.cuda_device;
    out.device_bgra8 = out.planar_yuv422_signed16 = out.device_backend_available;
    // Both production profiles are limited to YUV 4:2:2 at coded precision 10.
    out.interlaced_output = out.device_backend_available;
    out.progressive_fieldplus_gop2 = out.device_backend_available;
#endif
    return out;
}
DecodeTicket Decoder::QueueDecode(const uint8_t* sample, size_t size, const DecodeRequest& r, DecodeError* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto reject = [&](DecodeError e) { if (error) *error = e; return DecodeTicket{}; };
    if (!impl_->initialized) return reject(DecodeError::NotInitialized);
    if (!ValidRequest(r)) return reject(DecodeError::InvalidRequest);
    Impl::Slot* slot = nullptr;
    for (auto& s : impl_->slots) if (s.status.state == DecodeState::Released) { slot = &s; break; }
    if (!slot) return reject(DecodeError::Busy);
    if (impl_->next_ticket == UINT64_MAX) return reject(DecodeError::InvalidTicket);
    *slot = Impl::Slot{};
    slot->ticket = {impl_->owner, impl_->next_ticket++};
    slot->status = {DecodeState::Queued, DecodeError::None};
    slot->request = r;
    auto fail = [&](DecodeError e) {
        slot->status = {DecodeState::Failed, e};
        if (error) *error = e;
        return slot->ticket;
    };
    if (!sample || !size) return fail(DecodeError::InvalidSample);
    if (size > 32u * 1024 * 1024) return fail(DecodeError::SampleTooLarge);
    impl_->SelectDevice();
    const auto parse_start = Clock::now();
    std::string parse_error;
    DecodeError parse_status = DecodeError::InvalidSample;
    try {
        FieldPlusSampleDescriptor fieldplus;
        DecodeError fieldplus_status = DecodeError::InvalidSample;
        if (ParseProgressiveFieldPlusSampleValidated(sample, size,
                r.format == DecodePixelFormat::DeviceBGRA8 ? 24 : 4,
                fieldplus, nullptr, &fieldplus_status)) {
            if (r.frame_index >= fieldplus.frame_count) return fail(DecodeError::InvalidRequest);
            if (fieldplus.width > impl_->config.maximum_width || fieldplus.height > impl_->config.maximum_height)
                return fail(DecodeError::InvalidSample);
            // FIELDPLUS uses a fixed, validated checkpoint/replay route; GOP-1's
            // Auto classifier and its three diagnostic strategy overrides are
            // deliberately not applied to the 51-band profile.
            if (r.diagnostic_strategy != DiagnosticDecodeStrategy::Automatic)
                return fail(DecodeError::UnsupportedEncodingMode);
            slot->info = {};
            slot->info.strategy = DecodeStrategy::Hybrid;
            slot->backend_ticket = impl_->backend.QueueDecodeFieldPlusParsed(sample, size, fieldplus,
                r.frame_index,
                r.format == DecodePixelFormat::DeviceBGRA8 ? DeviceSurfaceType::BGRA8 : DeviceSurfaceType::PlanarYUV422_16S,
                r.matrix == DecodeMatrix::BT709 ? ColorMatrix::BT709 : ColorMatrix::BT601,
                r.range == DecodeRange::ComputerGraphics ? ColorRange::CG : ColorRange::VS,
                r.orientation == DecodeOrientation::BottomUp ? RowOrientation::BottomUp : RowOrientation::TopDown,
                true);
            if (!slot->backend_ticket) return fail(DecodeError::InvalidSample);
            slot->status = {DecodeState::Running, DecodeError::None};
            if (error) *error = DecodeError::None;
            return slot->ticket;
        }
        FrameReconstruction mode = FrameReconstruction::Progressive;
        if (!ParseCineFormSampleValidated(sample, size, r.format == DecodePixelFormat::DeviceBGRA8 ? 24 : 4,
                                         impl_->parsed, &parse_error, &parse_status, &mode)) return fail(parse_status);
        // GOP-1 samples contain exactly one output image. Keep existing callers
        // unchanged while rejecting accidental requests for a nonexistent frame.
        if (r.frame_index != 0) return fail(DecodeError::InvalidRequest);
        if (impl_->parsed[7].width * 2 > impl_->config.maximum_width ||
            impl_->parsed[7].height * 2 > impl_->config.maximum_height) return fail(DecodeError::InvalidSample);
        const float parse_ms = std::chrono::duration<float, std::milli>(Clock::now() - parse_start).count();
        const auto classify_start = Clock::now();
        auto strategy = ClassifyDecodeFrame(impl_->parsed, slot->info);
        slot->info.classifier_nanoseconds = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - classify_start).count());
        if (r.diagnostic_strategy != DiagnosticDecodeStrategy::Automatic)
            strategy = r.diagnostic_strategy == DiagnosticDecodeStrategy::Lightweight ? DecodeStrategy::Lightweight :
                       r.diagnostic_strategy == DiagnosticDecodeStrategy::Hybrid ? DecodeStrategy::Hybrid : DecodeStrategy::HostStreaming;
        slot->info.strategy = strategy;
        impl_->backend.SetPipelineMode(strategy == DecodeStrategy::Lightweight ? Phase2EPipelineMode::Phase2DFrameBarrier :
            strategy == DecodeStrategy::Hybrid ? Phase2EPipelineMode::Phase2FCHybridFullFrame : Phase2EPipelineMode::Phase2EFullOverlap);
        slot->backend_ticket = impl_->backend.QueueDecodeParsed(sample, size, impl_->parsed,
            r.format == DecodePixelFormat::DeviceBGRA8 ? DeviceSurfaceType::BGRA8 : DeviceSurfaceType::PlanarYUV422_16S,
            r.matrix == DecodeMatrix::BT709 ? ColorMatrix::BT709 : ColorMatrix::BT601,
            r.range == DecodeRange::ComputerGraphics ? ColorRange::CG : ColorRange::VS,
            r.orientation == DecodeOrientation::BottomUp ? RowOrientation::BottomUp : RowOrientation::TopDown, true, parse_ms,
            mode == FrameReconstruction::InterlacedGop1);
        if (!slot->backend_ticket) return fail(DecodeError::InvalidSample);
        slot->status = {DecodeState::Running, DecodeError::None};
        if (error) *error = DecodeError::None;
        return slot->ticket;
    } catch (...) { return fail(DecodeError::InvalidSample); }
}
DecodeStatus Decoder::PollDecode(DecodeTicket t) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->SelectDevice();
    return impl_->Update(impl_->Find(t), false);
}
DecodeStatus Decoder::WaitForDecode(DecodeTicket t) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->SelectDevice();
    return impl_->Update(impl_->Find(t), true);
}
bool Decoder::GetDeviceOutput(DecodeTicket t, DeviceOutput* out) {
    if (!out) return false;
    *out = {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->SelectDevice();
    auto* s = impl_->Find(t);
    if (impl_->Update(s, false).state != DecodeState::Ready) return false;
    DeviceOutput result;
    result.ticket = t;
    result.format = s->request.format;
    result.matrix = s->request.matrix;
    result.range = s->request.range;
    if (result.format == DecodePixelFormat::DeviceBGRA8) {
        DeviceBgraSurfaceDescriptor d;
        if (!impl_->backend.GetDeviceBgraOutput(s->backend_ticket, &d)) return false;
        result.planes[0] = {d.d_bgra, d.pitch, d.width, d.height};
        result.plane_count = 1; result.width = d.width; result.height = d.height;
        result.precision = 8; result.cuda_device = d.cuda_device; result.native_ready_event = d.ready_event;
        result.orientation = s->request.orientation;
    } else {
        DevicePlanarSurfaceDescriptor d;
        if (!impl_->backend.GetDevicePlanarOutput(s->backend_ticket, &d)) return false;
        result.planes[0] = {d.d_y, d.y_pitch, d.width, d.height};
        result.planes[1] = {d.d_u, d.u_pitch, d.width / 2, d.height};
        result.planes[2] = {d.d_v, d.v_pitch, d.width / 2, d.height};
        result.plane_count = 3; result.width = d.width; result.height = d.height;
        result.precision = d.precision; result.cuda_device = d.cuda_device; result.native_ready_event = d.ready_event;
        result.orientation = DecodeOrientation::TopDown;
    }
    *out = result;
    return true;
}
bool Decoder::StreamWait(DecodeTicket t, DecodeConsumerStream consumer) {
    if (!consumer.native_handle) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->SelectDevice();
    auto* s = impl_->Find(t);
    if (!s || (s->status.state != DecodeState::Running && s->status.state != DecodeState::Ready)) return false;
    size_t i = 0;
    for (; i < s->consumer_count; ++i) if (s->consumers[i] == consumer.native_handle) break;
    if (i == s->consumer_count && i == s->consumers.size()) return false;
    if (!impl_->backend.StreamWait(s->backend_ticket, reinterpret_cast<cudaStream_t>(consumer.native_handle))) return false;
    if (i == s->consumer_count) s->consumers[s->consumer_count++] = consumer.native_handle;
    return true;
}
bool Decoder::ReleaseOutput(DecodeTicket t) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->SelectDevice();
    auto* s = impl_->Find(t);
    return s && impl_->ReleaseSlot(*s);
}
bool Decoder::GetFrameInfo(DecodeTicket t, DecodeFrameInfo* info) {
    if (!info) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* s = impl_->Find(t);
    if (!s || (s->status.state != DecodeState::Running && s->status.state != DecodeState::Ready)) return false;
    *info = s->info;
    return true;
}
DecoderMemoryStats Decoder::MemoryStats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto out = impl_->backend.GetMemoryStats();
    if (impl_->initialized) out.pageable_capacity_bytes += sizeof(Impl) +
        impl_->slots.capacity() * sizeof(Impl::Slot) + impl_->parsed.capacity() * sizeof(GpuEntropyBandDescriptor) +
        impl_->band_records.capacity() * sizeof(Phase2FCBandRecord);
    return out;
}
bool DecoderInspector::BandDecisions(Decoder& decoder, DecodeTicket t, std::array<DecoderBandDecision, 27>* out) {
    if (!out) return false;
    auto& d = *decoder.impl_;
    std::lock_guard<std::mutex> lock(d.mutex);
    d.SelectDevice();
    auto* s = d.Find(t);
    if (d.Update(s, true).state != DecodeState::Ready) return false;
    if (!d.backend.GetPhase2FCBandRecords(s->backend_ticket, d.band_records)) return false;
    if (d.band_records.size() != out->size()) return false;
    for (size_t i = 0; i < out->size(); ++i) {
        const auto& r = d.band_records[i];
        (*out)[i] = {DecoderBandMethod(r.chosen_method), r.fell_back, r.corrected_nibbles, r.full_chunk_corrections};
    }
    return true;
}
bool DecoderInspector::DownloadBand(Decoder& decoder, DecodeTicket t, int band, std::vector<int16_t>& out,
                                   int* width, int* height, int* pitch) {
    auto& d = *decoder.impl_; std::lock_guard<std::mutex> lock(d.mutex); d.SelectDevice();
    auto* s = d.Find(t);
    return d.Update(s, true).state == DecodeState::Ready &&
        d.backend.DownloadBandCoefficientsToHost(s->backend_ticket, band, out, width, height, pitch);
}
bool DecoderInspector::DownloadFieldPlusBand(Decoder& decoder, DecodeTicket t, int channel, int logical_subband,
        std::vector<int16_t>& out, int* width, int* height, int* pitch) {
    if (channel < 0 || channel >= kFieldPlusChannelCount || logical_subband < 0 ||
        logical_subband >= kFieldPlusLogicalBandsPerChannel) return false;
    auto& d = *decoder.impl_; std::lock_guard<std::mutex> lock(d.mutex); d.SelectDevice();
    auto* s = d.Find(t);
    return s && d.Update(s, true).state == DecodeState::Ready &&
        d.backend.DownloadBandCoefficientsToHost(s->backend_ticket,
            channel * kFieldPlusLogicalBandsPerChannel + logical_subband, out, width, height, pitch);
}
bool DecoderInspector::DownloadFieldPlusTemporalStages(Decoder& decoder, DecodeTicket t, int channel,
        std::vector<int16_t>& temporal_low, std::vector<int16_t>& temporal_high,
        std::vector<int16_t>& wavelet4_ll, std::vector<int16_t>& frame_base_ll, int* width, int* height) {
    if (channel < 0 || channel >= kFieldPlusChannelCount) return false;
    auto& d = *decoder.impl_; std::lock_guard<std::mutex> lock(d.mutex); d.SelectDevice();
    auto* s = d.Find(t);
    return s && d.Update(s, true).state == DecodeState::Ready &&
        d.backend.DownloadFieldPlusTemporalStages(s->backend_ticket, channel, temporal_low,
            temporal_high, wavelet4_ll, frame_base_ll, width, height);
}
bool DecoderInspector::DownloadFieldPlusSpatialStages(Decoder& decoder, DecodeTicket t, int channel,
        std::vector<int16_t>& vertical_low, std::vector<int16_t>& vertical_high,
        int* width, int* height) {
    if (channel < 0 || channel >= kFieldPlusChannelCount) return false;
    auto& d = *decoder.impl_; std::lock_guard<std::mutex> lock(d.mutex); d.SelectDevice();
    auto* s = d.Find(t);
    return s && d.Update(s, true).state == DecodeState::Ready &&
        d.backend.DownloadFieldPlusSpatialStages(s->backend_ticket, channel,
            vertical_low, vertical_high, width, height);
}
bool DecoderInspector::DownloadIntermediate(Decoder& decoder, DecodeTicket t, int channel, int level, std::vector<int16_t>& out) {
    auto& d = *decoder.impl_; std::lock_guard<std::mutex> lock(d.mutex); d.SelectDevice();
    auto* s = d.Find(t);
    return d.Update(s, true).state == DecodeState::Ready && d.backend.DownloadIntermediateLl(s->backend_ticket, channel, level, out);
}
bool DecoderInspector::Reconstruction(Decoder& decoder, DecodeTicket t, DecoderReconstruction* out) {
    if (!out) return false;
    *out = {};
    auto& d = *decoder.impl_; std::lock_guard<std::mutex> lock(d.mutex); d.SelectDevice();
    auto* s = d.Find(t);
    return d.Update(s, true).state == DecodeState::Ready && d.backend.InspectReconstruction(s->backend_ticket, out);
}
} // namespace HyperCine
