// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../src/core/encoder_backend_selection.h"
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;
using hypercine::EncodingQuality;

static_assert(!std::is_copy_constructible<FilmScan3Encoder>::value, "encoder copy shares mutable resources");
static_assert(!std::is_copy_assignable<FilmScan3Encoder>::value, "encoder copy duplicates session identity");
static_assert(std::is_nothrow_move_constructible<FilmScan3Encoder>::value, "encoder move must not allocate");
static_assert(std::is_nothrow_move_assignable<FilmScan3Encoder>::value, "encoder move must not allocate");
static_assert(!std::is_copy_constructible<EncoderSession>::value, "session copy shares mutable resources");
static_assert(!std::is_copy_assignable<EncoderSession>::value, "session copy duplicates session identity");
static_assert(std::is_nothrow_move_constructible<EncoderSession>::value, "session move must not allocate");
static_assert(std::is_nothrow_move_assignable<EncoderSession>::value, "session move must not allocate");

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> void InactiveRefusal(F&& operation) {
    bool refused = false;
    try { operation(); }
    catch (const std::logic_error& error) {
        refused = std::string(error.what()).find("inactive") != std::string::npos;
    }
    Require(refused, "moved-from operation did not fail explicitly before encoding");
}

template<class F> void ProfileRefusal(F&& operation) {
    bool refused = false;
    try { operation(); }
    catch (const std::logic_error& error) {
        refused = std::string(error.what()).find("already fixed") != std::string::npos;
    }
    Require(refused, "move lost the encoded session's profile lock");
}

void SameState(const FilmScan3State& a, const FilmScan3State& b) {
    Require(a.frame_index == b.frame_index && a.fs_rate_limiter == b.fs_rate_limiter &&
            a.previous_sample_bytes == b.previous_sample_bytes && a.fs_overbitrate == b.fs_overbitrate,
            "move changed rate-control state");
}

void SamePacket(const FilmScan3Frame& actual, const FilmScan3Frame& reference, bool hashing) {
    Require(actual.sample == reference.sample, "ownership transfer changed the complete CFHD packet");
    Require(actual.tables.quant == reference.tables.quant && actual.tables.scale == reference.tables.scale,
            "ownership transfer changed quantization tables");
    SameState(actual.state, reference.state);
    if (hashing) {
        Require(FilmScan3FullCoefficientHashesEqual(actual, reference), "move changed full coefficient hashes");
    } else {
        Require(actual.coefficient_hash == FilmScan3CoefficientHash::DiagnosticOff,
                "move lost the diagnostic-off policy");
    }
}

struct Input {
    FilmScan3Geometry geometry{128,24,8u * 128u * 24u + 16384u};
    std::size_t stride = 128u * 4u + 16u;
    AlignedVector<uint8_t> bytes = AlignedVector<uint8_t>(stride * 24u, 0xa5);
    void Fill(unsigned frame) {
        uint32_t random = 0x1337beefu ^ (frame * 0x9e3779b9u);
        for (int y = 0; y < geometry.height; ++y) {
            for (int x = 0; x < geometry.width; ++x) {
                auto* pixel = bytes.data() + static_cast<std::size_t>(y) * stride + 4u * x;
                for (unsigned c = 0; c < 3; ++c) {
                    random = random * 1664525u + 1013904223u;
                    pixel[c] = static_cast<uint8_t>(random >> 24);
                }
                pixel[3] = 255;
            }
        }
    }
    FilmScan3Frame Encode(FilmScan3Encoder& encoder) const {
        return encoder.Encode(bytes.data(), bytes.size(), stride);
    }
    FilmScan3Frame Encode(EncoderSession& session) const {
        return session.Encode(bytes.data(), bytes.size(), stride);
    }
};

void InactiveEncoder(FilmScan3Encoder& encoder, const Input& input) {
    Require(!encoder.active(), "move left the source active");
    Require(encoder.geometry().width == 0 && encoder.geometry().height == 0 &&
            encoder.geometry().sample_capacity_bytes == 0 && encoder.wavelet_isa() == WaveletIsa::Auto,
            "inactive source reports an executable backend or geometry");
    SameState(encoder.state(), FilmScan3State{});
    Require(encoder.session_quality() == EncodingQuality::FilmScan3 && encoder.coefficient_hashing(),
            "inactive source retained old policy");
    Require(FrontendAccess::HashSession(encoder) == 0 && FrontendAccess::DiagnosticSerial(encoder) == 1,
            "inactive source retained the transferred diagnostic identity");
    InactiveRefusal([&] { input.Encode(encoder); });
    InactiveRefusal([&] { encoder.Encode(nullptr, 0, 0, FilmScan3Metadata{}); });
    InactiveRefusal([&] { encoder.SetSessionQuality(EncodingQuality::High); });
    InactiveRefusal([&] { FrontendAccess::Use(encoder, CpuFrontend(input.geometry, WaveletIsa::Scalar)); });
    encoder.Reset();
    encoder.SetCoefficientHashing(false);
    Require(!encoder.active() && encoder.coefficient_hashing(), "noexcept source operation reactivated it");
    SameState(encoder.state(), FilmScan3State{});
    InactiveRefusal([&] { input.Encode(encoder); });
}

void PublicMoves(bool hashing) {
    Input input;
    FilmScan3Encoder original(input.geometry, WaveletIsa::Scalar);
    FilmScan3Encoder reference(input.geometry, WaveletIsa::Scalar);
    original.SetSessionQuality(EncodingQuality::High);
    reference.SetSessionQuality(EncodingQuality::High);
    original.SetCoefficientHashing(hashing);
    for (unsigned frame = 0; frame < 3; ++frame) {
        input.Fill(frame);
        SamePacket(input.Encode(original), input.Encode(reference), hashing);
    }
    const auto before = original.state();
    const auto identity = FrontendAccess::HashSession(original);
    const auto serial = FrontendAccess::DiagnosticSerial(original);
    Require(before.frame_index == 3 && before.previous_sample_bytes != 0, "test did not advance a real session");
    Require(hashing || identity != 0, "test never minted the diagnostic identity");

    FilmScan3Encoder moved(std::move(original));
    InactiveEncoder(original, input);
    Require(moved.active() && moved.geometry().width == input.geometry.width &&
            moved.geometry().height == input.geometry.height &&
            moved.geometry().sample_capacity_bytes == input.geometry.sample_capacity_bytes &&
            moved.wavelet_isa() == WaveletIsa::Scalar && moved.coefficient_hashing() == hashing &&
            moved.session_quality() == EncodingQuality::High, "move construction lost session policy");
    SameState(moved.state(), before);
    Require(FrontendAccess::HashSession(moved) == identity && FrontendAccess::DiagnosticSerial(moved) == serial,
            "move construction minted or duplicated a diagnostic sequence");
    ProfileRefusal([&] { moved.SetSessionQuality(EncodingQuality::Medium); });

    auto& self = moved;
    moved = std::move(self);
    SameState(moved.state(), before);
    Require(moved.active() && FrontendAccess::HashSession(moved) == identity &&
            FrontendAccess::DiagnosticSerial(moved) == serial, "self-move changed the session");
    input.Fill(3);
    SamePacket(input.Encode(moved), input.Encode(reference), hashing);

    // Replacing a different, already configured target must replace its whole
    // session, rather than retain the old geometry/profile/capacity.
    FilmScan3Encoder assigned(FilmScan3Geometry{256,48}, WaveletIsa::Scalar);
    assigned.SetSessionQuality(EncodingQuality::Low);
    assigned = std::move(moved);
    InactiveEncoder(moved, input);
    Require(assigned.active() && assigned.geometry().width == input.geometry.width &&
            assigned.geometry().sample_capacity_bytes == input.geometry.sample_capacity_bytes &&
            assigned.session_quality() == EncodingQuality::High, "move assignment mixed source and target policy");
    ProfileRefusal([&] { assigned.SetSessionQuality(EncodingQuality::Low); });
    input.Fill(4);
    SamePacket(input.Encode(assigned), input.Encode(reference), hashing);
    Require(FrontendAccess::HashSession(assigned) == identity &&
            FrontendAccess::DiagnosticSerial(assigned) == serial + (hashing ? 0u : 2u),
            "moved session lost diagnostic continuity");

    assigned.Reset();
    reference.Reset();
    input.Fill(0);
    SamePacket(input.Encode(assigned), input.Encode(reference), hashing);
    ProfileRefusal([&] { assigned.SetSessionQuality(EncodingQuality::Medium); });

    // A fresh session may be installed into an inactive owner by move assignment.
    // It must mint its own diagnostic identity instead of resurrecting the old one.
    original = FilmScan3Encoder(input.geometry, WaveletIsa::Scalar);
    original.SetSessionQuality(EncodingQuality::High);
    original.SetCoefficientHashing(hashing);
    reference.Reset();
    SamePacket(input.Encode(original), input.Encode(reference), hashing);
    Require(original.active(), "move assignment did not install a new session");
    if (!hashing) Require(FrontendAccess::HashSession(original) != identity,
                          "replacement session reused the transferred diagnostic identity");

    FilmScan3Encoder inactive(std::move(moved));
    InactiveEncoder(inactive, input);
    auto& inactive_self = inactive;
    inactive = std::move(inactive_self);
    InactiveEncoder(inactive, input);
    assigned = std::move(inactive);
    InactiveEncoder(assigned, input);
    InactiveEncoder(inactive, input);
}

struct FrontendLife {
    unsigned constructed = 0, destroyed = 0, transforms = 0;
    bool fail_completion = false;
};

// A real CPU transform behind a stateful frontend proves ownership/lifetime and
// pending-failure/geometry-binding transfer without executing a native GPU backend.
class StatefulFrontend final : public Frontend {
public:
    FilmScan3Geometry declared;
    StatefulFrontend(FilmScan3Geometry geometry, std::shared_ptr<FrontendLife> life)
        : declared(geometry), inner_(CpuFrontend(geometry, WaveletIsa::Scalar)),
          life_(std::move(life)) { ++life_->constructed; }
    ~StatefulFrontend() override { ++life_->destroyed; }
    FrontendBackend Backend() const noexcept override { return FrontendBackend::Cpu; }
    FilmScan3Geometry Geometry() const noexcept override { return declared; }
    WaveletIsa Isa() const noexcept override { return inner_->Isa(); }
    Planes Convert(const uint8_t* p, std::size_t n, int w, int h, std::size_t stride) override {
        return inner_->Convert(p, n, w, h, stride);
    }
    WaveletLevel Forward(const Plane& p, int prescale) override { return inner_->Forward(p, prescale); }
    std::string DeviceName() const override { return "stateful CPU ownership test"; }
    FrontendFrame Transform(const uint8_t* p, std::size_t n, int w, int h,
                            std::size_t stride, bool audit) override {
        ++life_->transforms;
        return inner_->Transform(p, n, w, h, stride, audit);
    }
    bool ProfileHost() const override { return true; }
    void RecordHostTiming(const HostTiming&) override {
        if (life_->fail_completion) {
            life_->fail_completion = false;
            throw std::runtime_error("ownership-test completion failure");
        }
    }
private:
    std::shared_ptr<Frontend> inner_;
    std::shared_ptr<FrontendLife> life_;
};

std::weak_ptr<Frontend> Install(FilmScan3Encoder& encoder, const Input& input,
                              const std::shared_ptr<FrontendLife>& life) {
    auto frontend = std::make_shared<StatefulFrontend>(input.geometry, life);
    std::weak_ptr<Frontend> observer = frontend;
    FrontendAccess::Use(encoder, std::move(frontend));
    return observer;
}

void ResourceAndFailureTransfer() {
    Input input;
    auto source_life = std::make_shared<FrontendLife>();
    auto target_life = std::make_shared<FrontendLife>();
    std::weak_ptr<Frontend> transferred, replaced;
    {
        FilmScan3Encoder source(input.geometry, WaveletIsa::Scalar);
        FilmScan3Encoder target(input.geometry, WaveletIsa::Scalar);
        FilmScan3Encoder reference(input.geometry, WaveletIsa::Scalar);
        transferred = Install(source, input, source_life);
        replaced = Install(target, input, target_life);
        source.SetCoefficientHashing(false);
        input.Fill(0);
        SamePacket(input.Encode(source), input.Encode(reference), false);
        const auto before = source.state();
        const auto identity = FrontendAccess::HashSession(source);
        const auto serial = FrontendAccess::DiagnosticSerial(source);
        source_life->fail_completion = true;
        target = std::move(source);
        Require(replaced.expired() && target_life->destroyed == 1,
                "move assignment retained the old target's mutable resources");
        Require(!transferred.expired() && source_life->constructed == 1 && source_life->destroyed == 0,
                "move cloned or prematurely destroyed the source frontend");
        InactiveEncoder(source, input);
        Require(source_life->transforms == 1, "inactive source dispatched through the transferred frontend");
        input.Fill(1);
        bool failed = false;
        try { input.Encode(target); }
        catch (const std::runtime_error& error) {
            failed = std::string(error.what()) == "ownership-test completion failure";
        }
        Require(failed && source_life->transforms == 2, "move lost the frontend's pending failure");
        SameState(target.state(), before);
        Require(FrontendAccess::HashSession(target) == identity && FrontendAccess::DiagnosticSerial(target) == serial,
                "failed moved encode consumed state or a diagnostic identity");
        SamePacket(input.Encode(target), input.Encode(reference), false);
        Require(FrontendAccess::HashSession(target) == identity &&
                FrontendAccess::DiagnosticSerial(target) == serial + 1, "recovery did not continue the same identity");
    }
    Require(transferred.expired() && source_life->destroyed == 1 && target_life->destroyed == 1,
            "transferred resources were not released exactly once");
}

void GeometryBindingAcrossMoves() {
    Input input;
    FilmScan3Encoder source(input.geometry, WaveletIsa::Scalar);
    FilmScan3Encoder reference(input.geometry, WaveletIsa::Scalar);
    auto life = std::make_shared<FrontendLife>();
    auto frontend = std::make_shared<StatefulFrontend>(input.geometry, life);
    FrontendAccess::Use(source, frontend);
    source.SetSessionQuality(EncodingQuality::High);
    reference.SetSessionQuality(EncodingQuality::High);
    source.SetCoefficientHashing(false);
    input.Fill(0);
    SamePacket(input.Encode(source), input.Encode(reference), false);
    const auto first_state = source.state();
    const auto identity = FrontendAccess::HashSession(source);
    const auto first_serial = FrontendAccess::DiagnosticSerial(source);
    Require(identity != 0 && life->transforms == 1, "geometry move test did not establish a real session");

    const auto refuse_drift = [&](FilmScan3Encoder& encoder, const FilmScan3State& before,
                                  uint32_t serial, unsigned transforms) {
        const auto bound = encoder.geometry();
        Require(encoder.active() && bound.width == input.geometry.width &&
                bound.height == input.geometry.height &&
                bound.sample_capacity_bytes == input.geometry.sample_capacity_bytes,
                "move rebound the session to a changed frontend declaration");
        bool refused = false;
        try { input.Encode(encoder); }
        catch (const std::invalid_argument& error) {
            refused = std::string(error.what()).find("declaration changed") != std::string::npos;
        }
        Require(refused && life->transforms == transforms,
                "moved geometry drift was not refused before Transform");
        SameState(encoder.state(), before);
        Require(FrontendAccess::HashSession(encoder) == identity &&
                FrontendAccess::DiagnosticSerial(encoder) == serial,
                "moved geometry refusal consumed diagnostic identity");
    };

    // Drift is present before ownership changes: the move must carry the
    // original snapshot rather than bind again using the new declaration.
    frontend->declared.width = 256;
    FilmScan3Encoder moved(std::move(source));
    InactiveEncoder(source, input);
    refuse_drift(moved, first_state, first_serial, 1);

    // Binding this candidate would throw for its impossible declaration. The
    // inactive-source guard must win before BindFrontend examines it.
    auto invalid = std::make_shared<StatefulFrontend>(input.geometry, std::make_shared<FrontendLife>());
    invalid->declared = {0,0,0};
    InactiveRefusal([&] { FrontendAccess::Use(source, invalid); });

    frontend->declared = input.geometry;
    input.Fill(1);
    SamePacket(input.Encode(moved), input.Encode(reference), false);
    const auto second_state = moved.state();
    Require(FrontendAccess::HashSession(moved) == identity &&
            FrontendAccess::DiagnosticSerial(moved) == first_serial + 1,
            "move construction recovery changed diagnostic continuity");

    // Capacity is part of the same snapshot even when width/height still match.
    --frontend->declared.sample_capacity_bytes;
    FilmScan3Encoder assigned(FilmScan3Geometry{256,48}, WaveletIsa::Scalar);
    assigned = std::move(moved);
    InactiveEncoder(moved, input);
    refuse_drift(assigned, second_state, first_serial + 1, 2);
    frontend->declared = input.geometry;
    input.Fill(2);
    SamePacket(input.Encode(assigned), input.Encode(reference), false);
    Require(FrontendAccess::HashSession(assigned) == identity &&
            FrontendAccess::DiagnosticSerial(assigned) == first_serial + 2 && life->transforms == 3,
            "move assignment recovery changed the continuing session");
}

void InactiveSession(EncoderSession& session, const Input& input) {
    Require(!session.active() && !session.selection() && !session.selection().frontend &&
            session.selection().status == BackendSelectionStatus::CapabilityRefused &&
            !session.selection().effective.allocation_permitted,
            "moved-from session still claims a selected backend");
    Require(session.geometry().width == 0 && session.geometry().height == 0 &&
            session.wavelet_isa() == WaveletIsa::Auto && session.config().width == 1920 &&
            session.config().input_stride_bytes == 0, "inactive session queries are inconsistent");
    InactiveRefusal([&] { input.Encode(session); });
    InactiveRefusal([&] { session.Encode(nullptr, 0, 0); });
    session.Reset();
    Require(!session.active(), "Reset reactivated a moved-from session");
    InactiveRefusal([&] { input.Encode(session); });
}

uint64_t DiagnosticIdentity(const FilmScan3Frame& frame) { return (frame.coefficient_fnv64[0] >> 24) & 0xffffffu; }
uint64_t DiagnosticSerial(const FilmScan3Frame& frame) { return frame.coefficient_fnv64[0] & 0xffffffu; }

void InternalSessionMoves(bool hashing) {
    Input input;
    EncoderSessionConfig config;
    config.width = input.geometry.width;
    config.height = input.geometry.height;
    config.input_stride_bytes = input.stride;
    config.input_bytes = input.bytes.size();
    config.max_sample_bytes = input.geometry.sample_capacity_bytes;
    config.quality = EncodingQuality::Medium;
    config.diagnostic = hashing ? DiagnosticMode::CoefficientFnv64 : DiagnosticMode::None;
    // Preserve observable fallback provenance as well as the effective CPU owner.
    config.backend = SessionBackend::Cuda;
    config.allow_cpu_scalar_fallback = true;
    EncoderSession source(config, EncoderDevice{});
    FilmScan3Encoder reference(input.geometry, WaveletIsa::Scalar);
    reference.SetSessionQuality(config.quality);
    input.Fill(0);
    const auto first = input.Encode(source);
    SamePacket(first, input.Encode(reference), hashing);
    const auto* frontend = source.selection().frontend.get();
    EncoderSession moved(std::move(source));
    InactiveSession(source, input);
    Require(moved.active() && moved.selection() && moved.selection().frontend.get() == frontend &&
            moved.selection().fallback == ScalarFallbackReason::BackendNotPresent &&
            moved.selection().effective.effective_backend == SessionBackend::CpuScalar &&
            moved.config().width == config.width && moved.config().height == config.height &&
            moved.config().quality == config.quality && moved.config().diagnostic == config.diagnostic &&
            moved.config().input_stride_bytes == config.input_stride_bytes &&
            moved.config().input_bytes == config.input_bytes &&
            moved.geometry().sample_capacity_bytes == input.geometry.sample_capacity_bytes,
            "session move separated its config, selection and actual encoder");
    bool invalid_storage = false;
    try { moved.Encode(input.bytes.data(), input.bytes.size(), input.stride - 16); }
    catch (const std::invalid_argument&) { invalid_storage = true; }
    Require(invalid_storage, "move lost bound input storage validation");
    auto& self = moved;
    moved = std::move(self);
    Require(moved.active() && moved.selection().frontend.get() == frontend, "session self-move lost its backend");

    auto replacement_config = config;
    replacement_config.quality = EncodingQuality::Low;
    EncoderSession assigned(replacement_config, EncoderDevice{});
    std::weak_ptr<Frontend> old_target = assigned.selection().frontend;
    assigned = std::move(moved);
    InactiveSession(moved, input);
    Require(old_target.expired() && assigned.selection().frontend.get() == frontend &&
            assigned.config().quality == config.quality, "session assignment retained or mixed previous ownership");
    input.Fill(1);
    const auto next = input.Encode(assigned);
    SamePacket(next, input.Encode(reference), hashing);
    if (!hashing) Require(DiagnosticIdentity(next) == DiagnosticIdentity(first) &&
                          DiagnosticSerial(next) == DiagnosticSerial(first) + 1,
                          "internal moves duplicated or reset diagnostic identity");
    assigned.Reset();
    reference.Reset();
    input.Fill(0);
    const auto reset = input.Encode(assigned);
    SamePacket(reset, input.Encode(reference), hashing);
    if (!hashing) Require(DiagnosticIdentity(reset) == DiagnosticIdentity(first) &&
                          DiagnosticSerial(reset) == DiagnosticSerial(next) + 1,
                          "Reset after move reset diagnostic identity");

    source = EncoderSession(config, EncoderDevice{});
    reference.Reset();
    const auto fresh = input.Encode(source);
    SamePacket(fresh, input.Encode(reference), hashing);
    if (!hashing) Require(DiagnosticIdentity(fresh) != DiagnosticIdentity(first),
                          "reactivated internal owner reused transferred identity");
    EncoderSession inactive(std::move(moved));
    InactiveSession(inactive, input);
    auto& inactive_self = inactive;
    inactive = std::move(inactive_self);
    InactiveSession(inactive, input);
    assigned = std::move(inactive);
    InactiveSession(assigned, input);
}

void LazyDefaultMove() {
    FilmScan3Encoder lazy;
    FilmScan3Encoder moved(std::move(lazy));
    Require(moved.active() && moved.geometry().width == 1920 && moved.geometry().height == 1080 &&
            moved.wavelet_isa() == WaveletIsa::Auto, "move lost a lazy default CPU session");
    Input input;
    InactiveEncoder(lazy, input);
}
} // namespace

int main() {
    try {
        PublicMoves(true);
        PublicMoves(false);
        ResourceAndFailureTransfer();
        GeometryBindingAcrossMoves();
        InternalSessionMoves(true);
        InternalSessionMoves(false);
        LazyDefaultMove();
        std::cout << "PASS move-only ownership: packet/state/profile/identity continuity, inactive sources, "
                     "resource lifetime, geometry binding, failure rollback and session selection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL encoder ownership: " << error.what() << '\n';
        return 1;
    }
}
