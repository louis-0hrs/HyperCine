// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// DESIGN PROOF ONLY: not linked to the codec or a qualification registry.
#pragma once
#include <array>
#include <cstdint>

namespace dialect_proof {
enum class Wire { Cfhd, Vc5 };
enum class Feature : unsigned { Layers = 1, Sections = 2, Metadata = 4 };
enum class Target { ExperimentalOnly, GoPro, Ffmpeg, Application };
enum class Status { InvalidRequest, Unsupported, Unqualified, ExperimentalPlan };
enum class Reason { UnknownValue, ContradictoryStandard, MissingEdition,
    FeaturePartMismatch, InvalidGeometry, WireUnavailable, FormatUnavailable,
    BackendUnavailable, ResourceRefused, QualificationMissing, ExperimentalOnly };
// Slot = ST 2073 part number; part 2 is an RP, part 10 belongs to a wrapper.
struct Standard {
    std::array<unsigned, 8> edition{};
    unsigned features = 0;
};
struct Request {
    Wire wire = Wire::Cfhd;
    Standard standard{};
    Target target = Target::ExperimentalOnly;
    // Explicit actual source and encoded domain; not quality-profile names.
    unsigned source_bits = 8, coded_bits = 10;
    bool bgra_source = true, yuv422_coded = true, progressive_gop1 = true;
    std::int64_t width = 128, height = 24;
};
// Opaque results of existing format/backend/resource preflight, NOT facts
// inferred from a GPU name. This proof never fabricates a runtime device.
struct ExistingAdmission {
    bool format = true, backend = true, resources = true;
};
struct Result {
    Status status;
    Reason reason;
    Wire requested_wire;
    bool may_plan;
};
// Fixed-size, pure preflight. No allocator, factory, codec or reference call.
inline Result Check(const Request& r, ExistingAdmission a = {}) noexcept {
    const auto refuse = [&](Status s, Reason why) { return Result{s, why, r.wire, false}; };
    if ((r.wire != Wire::Cfhd && r.wire != Wire::Vc5) ||
        (r.target != Target::ExperimentalOnly && r.target != Target::GoPro &&
         r.target != Target::Ffmpeg && r.target != Target::Application) ||
        (r.standard.features & ~7u))
        return refuse(Status::InvalidRequest, Reason::UnknownValue);
    bool any_standard = r.standard.features != 0;
    for (unsigned e : r.standard.edition) any_standard = any_standard || e != 0;
    if (r.wire == Wire::Cfhd && any_standard)
        return refuse(Status::InvalidRequest, Reason::ContradictoryStandard);
    if (r.standard.edition[0] || r.standard.edition[2])
        return refuse(Status::InvalidRequest, Reason::ContradictoryStandard);
    if (r.wire == Wire::Vc5 && !r.standard.edition[1])
        return refuse(Status::InvalidRequest, Reason::MissingEdition);
    const unsigned parts[] = {5, 6, 7};
    for (unsigned i = 0; i != 3; ++i)
        if ((r.standard.features & (1u << i)) && !r.standard.edition[parts[i]])
            return refuse(Status::InvalidRequest, Reason::FeaturePartMismatch);
    // Structural subset of the current FilmScan3 input contract, not a
    // universal VC-5 legality rule. Large positive geometry goes to preflight.
    if (r.width < 128 || r.height < 24 || r.width % 128 || r.height % 8)
        return refuse(Status::InvalidRequest, Reason::InvalidGeometry);
    // No VC-5 serializer/edition coverage is established by this proof.
    if (r.wire == Wire::Vc5)
        return refuse(Status::Unsupported, Reason::WireUnavailable);
    if (!r.bgra_source || !r.yuv422_coded || !r.progressive_gop1 ||
        r.source_bits != 8 || r.coded_bits != 10 || !a.format)
        return refuse(Status::Unsupported, Reason::FormatUnavailable);
    if (!a.backend) return refuse(Status::Unsupported, Reason::BackendUnavailable);
    if (!a.resources) return refuse(Status::Unsupported, Reason::ResourceRefused);
    // Named target requires exact external evidence. No caller-set "qualified"
    // boolean can manufacture it. Even FFmpeg is unqualified in this proof.
    if (r.target != Target::ExperimentalOnly)
        return refuse(Status::Unqualified, Reason::QualificationMissing);
    return {Status::ExperimentalPlan, Reason::ExperimentalOnly, r.wire, true};
}
} // namespace dialect_proof
