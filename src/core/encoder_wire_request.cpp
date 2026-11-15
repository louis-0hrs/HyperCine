// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "encoder_wire_request.h"
namespace hypercine { namespace standalone { namespace detail {
WireCapability CheckWireRequest(const EncoderWireRequest& request, const EncoderDevice& device) {
    WireCapability result;
    if (request.dialect() != WireDialect::Cfhd) return result;
    if (request.edition() != St2073Edition::None || request.parts() != 0) {
        result.refusal = WireRefusal::UnsupportedEditionOrParts;
        return result;
    }
    if (request.target() != DecoderTarget::Unspecified) {
        result.refusal = WireRefusal::UnsupportedTarget;
        return result;
    }
    result.capability = CheckEncoderCapability(request.config(), device);
    result.refusal = result.capability.allocation_permitted ? WireRefusal::None : WireRefusal::CapabilityRefused;
    return result;
}
std::unique_ptr<EncoderSession> CreateWireSession(const EncoderWireRequest& request,
                                                   const EncoderDevice& device,
                                                   WireSessionFactory factory) {
    if (!CheckWireRequest(request, device).permitted()) return nullptr;
    if (factory) return factory(request.config(), device);
    return std::make_unique<EncoderSession>(request.config(), device);
}
}}}
