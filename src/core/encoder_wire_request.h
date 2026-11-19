// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "encoder_backend_selection.h"
#include <memory>

namespace hypercine { namespace standalone { namespace detail {
// Private, by-value snapshot: no setters or public ABI. None means no claim.
enum class WireDialect { Cfhd = 1, Vc5 = 2 };
enum class St2073Edition { None = 0, Edition2015 = 1, Edition2024 = 2 };
enum class DecoderTarget { Unspecified = 0, GoPro = 1, Ffmpeg = 2 };
class EncoderWireRequest final {
public:
    EncoderWireRequest(WireDialect dialect, St2073Edition edition, std::uint32_t parts,
                       DecoderTarget target, EncoderSessionConfig config)
        : dialect_(dialect), edition_(edition), parts_(parts), target_(target), config_(config) {}
    WireDialect dialect() const noexcept { return dialect_; }
    St2073Edition edition() const noexcept { return edition_; }
    std::uint32_t parts() const noexcept { return parts_; }
    DecoderTarget target() const noexcept { return target_; }
    const EncoderSessionConfig& config() const noexcept { return config_; }
private:
    const WireDialect dialect_;
    const St2073Edition edition_;
    const std::uint32_t parts_;
    const DecoderTarget target_;
    const EncoderSessionConfig config_;
};
enum class WireRefusal { None, UnsupportedDialect, UnsupportedEditionOrParts, UnsupportedTarget, CapabilityRefused };
struct WireCapability {
    WireRefusal refusal = WireRefusal::UnsupportedDialect;
    EncoderCapabilityResult capability{};
    bool permitted() const noexcept { return refusal == WireRefusal::None && capability.allocation_permitted; }
};
WireCapability CheckWireRequest(const EncoderWireRequest&, const EncoderDevice&);
// Returns null on refusal, before allocating even the session object or invoking
// a backend factory. Factory overload is per-call and exercises the real path.
// Admission is format-aware: a request whose config declares native YUV422p10
// planes is answered by the native planes gate, never by the interleaved BGRA
// gate, so the wire boundary cannot silently redefine the caller's source.
using WireSessionFactory = std::unique_ptr<EncoderSession>(*)(const EncoderSessionConfig&, const EncoderDevice&);
std::unique_ptr<EncoderSession> CreateWireSession(const EncoderWireRequest&, const EncoderDevice&,
                                                  WireSessionFactory = nullptr);
}}}
