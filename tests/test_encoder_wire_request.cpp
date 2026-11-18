// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../src/core/encoder_wire_request.h"
#include <atomic>
#include <cstdlib>
#include <new>
#include <iostream>
#include <stdexcept>
#include <type_traits>
namespace { std::atomic<unsigned long long> allocations{0}; }
void* operator new(std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) { ++allocations; return p; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
using namespace hypercine::standalone;
using namespace hypercine::standalone::detail;
namespace {
unsigned calls = 0, checks = 0;
void Require(bool b, const char* text) { ++checks; if (!b) throw std::runtime_error(text); }
std::unique_ptr<EncoderSession> Count(const EncoderSessionConfig& c, const EncoderDevice& d) {
    ++calls;
    return std::make_unique<EncoderSession>(c, d);
}
EncoderSessionConfig Small() { EncoderSessionConfig c; c.width=128; c.height=24; return c; }
void Reject(WireDialect w, St2073Edition e, std::uint32_t p, DecoderTarget t,
            const EncoderSessionConfig& c, const EncoderDevice& d, WireRefusal reason) {
    EncoderWireRequest r(w,e,p,t,c); calls=0;
    auto q=CheckWireRequest(r,d);
    Require(!q.permitted() && q.refusal==reason, "incorrect refusal");
    const auto before=allocations.load();
    auto s=CreateWireSession(r,d,Count);
    const auto after=allocations.load();
    Require(!s && calls==0, "refusal reached session factory");
    // Existing capability geometry validation may throw and allocate exception
    // storage; only wire-level refusals promise zero generic allocations here.
    if (reason != WireRefusal::CapabilityRefused)
        Require(before==after, "wire refusal allocated memory");
    const auto default_before=allocations.load();
    auto production=CreateWireSession(r,d);
    const auto default_after=allocations.load();
    Require(!production, "default production path accepted refusal");
    if (reason != WireRefusal::CapabilityRefused)
        Require(default_before==default_after, "default wire refusal allocated memory");
}
}
int main() {
 try {
    static_assert(!std::is_copy_assignable<EncoderWireRequest>::value, "immutable request");
    auto c=Small(); EncoderDevice d;
    EncoderWireRequest r(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,c);
    c.width=256;
    Require(r.config().width==128,"request did not snapshot config");
    Require(CheckWireRequest(r,d).permitted(),"CFHD capability refused");
    calls=0; const auto before=allocations.load(); auto session=CreateWireSession(r,d,Count);
    Require(allocations.load()>before,"allocation counter did not observe real session");
    Require(session && session->active() && calls==1,"real CPU session not created");
    Require(session->geometry().width==128 && session->selection().effective.allocation_permitted,
            "real session not bound to preflight");
    auto production=CreateWireSession(r,d);
    Require(production && production->active(), "default CPU session not created");
    Require(production->geometry().width==128 && production->geometry().height==24,
            "default session lost immutable geometry");
    Require(production->selection().effective.effective_backend==SessionBackend::CpuScalar &&
            production->selection().fallback==ScalarFallbackReason::None,
            "default session silently selected another backend");
    c=Small();
    for (auto w : {WireDialect::Vc5, static_cast<WireDialect>(-1), static_cast<WireDialect>(99)})
        for (auto fallback : {false,true}) { c.allow_cpu_scalar_fallback=fallback;
            Reject(w,St2073Edition::None,0,DecoderTarget::Unspecified,c,d,WireRefusal::UnsupportedDialect); }
    c=Small();
    for (auto e : {St2073Edition::Edition2015,St2073Edition::Edition2024,static_cast<St2073Edition>(99)})
        Reject(WireDialect::Cfhd,e,0,DecoderTarget::Unspecified,c,d,WireRefusal::UnsupportedEditionOrParts);
    for (auto p : {1u,2u,0x80000000u,0xffffffffu})
        Reject(WireDialect::Cfhd,St2073Edition::None,p,DecoderTarget::Unspecified,c,d,WireRefusal::UnsupportedEditionOrParts);
    for (auto t : {DecoderTarget::GoPro,DecoderTarget::Ffmpeg,static_cast<DecoderTarget>(99)})
        Reject(WireDialect::Cfhd,St2073Edition::None,0,t,c,d,WireRefusal::UnsupportedTarget);
    auto bad=c; bad.width=129;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,d,WireRefusal::CapabilityRefused);
    bad=c; bad.cpu_memory_budget_bytes=1;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,d,WireRefusal::CapabilityRefused);
    bad=c; bad.max_sample_bytes=1;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,d,WireRefusal::CapabilityRefused);
    bad=c; bad.pixel_format=SourcePixelFormat::Rgb48;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,d,WireRefusal::CapabilityRefused);
    bad=c; bad.backend=SessionBackend::Metal;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,d,WireRefusal::CapabilityRefused);
    bad=c; bad.input_stride_bytes=16; bad.input_bytes=384;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,d,WireRefusal::CapabilityRefused);
    bad=c; bad.require_reference_oracle=true;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,d,WireRefusal::CapabilityRefused);
    // Actual default factory uses only explicitly permitted absent-backend fallback.
    bad=Small(); bad.backend=SessionBackend::Cuda; bad.allow_cpu_scalar_fallback=true;
    EncoderWireRequest fallback(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad);
    production=CreateWireSession(fallback,d);
    Require(production && production->active(), "explicit absent-backend fallback refused");
    Require(production->selection().effective.effective_backend==SessionBackend::CpuScalar &&
            production->selection().fallback==ScalarFallbackReason::BackendNotPresent,
            "explicit fallback lost reason/backend");
    // A present but unqualified backend is conservatively refused, even if the
    // older backend selector could separately admit scalar fallback.
    bad.backend=SessionBackend::Vulkan; auto present=d; present.vulkan_available=true;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,bad,present,
           WireRefusal::CapabilityRefused);
    // --- #259 native wire-request admission -------------------------------
    // The wire boundary must answer the declared source domain. A native
    // YUV422p10 config used to be refused by the interleaved BGRA gate, which
    // is fail-closed but also made the reviewed wire path silently BGRA-only.
    auto native = Small();
    native.pixel_format = SourcePixelFormat::Yuv422p10Native;
    EncoderWireRequest native_request(WireDialect::Cfhd,St2073Edition::None,0,
                                      DecoderTarget::Unspecified,native);
    auto native_capability = CheckWireRequest(native_request,d);
    Require(native_capability.permitted(), "native CPU wire request refused");
    Require(native_capability.capability.allocation_permitted &&
            !native_capability.capability.reasons.valid_but_not_implemented,
            "native CPU wire request was answered by the BGRA gate");
    calls=0;
    auto native_session=CreateWireSession(native_request,d,Count);
    Require(native_session && native_session->active() && calls==1,
            "native wire session not created");
    Require(native_session->config().pixel_format==SourcePixelFormat::Yuv422p10Native &&
            native_session->selection().effective.effective_backend==SessionBackend::CpuScalar,
            "native wire session lost the source domain");
    bool byte_refused=false;
    try { std::uint8_t sample[4]={0,0,0,0}; native_session->Encode(sample,sizeof sample,128); }
    catch (const std::invalid_argument&) { byte_refused=true; }
    Require(byte_refused, "native wire session accepted interleaved BGRA bytes");

    // Bound sample strides are admitted; the addressing domains stay exclusive.
    native.native_planes={128,64};
    EncoderWireRequest bound_request(WireDialect::Cfhd,St2073Edition::None,0,
                                     DecoderTarget::Unspecified,native);
    Require(CheckWireRequest(bound_request,d).permitted(), "bound native request refused");
    auto byte_binding=native; byte_binding.input_stride_bytes=256;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,byte_binding,d,
           WireRefusal::CapabilityRefused);
    auto half_bound=native; half_bound.native_planes={128,0};
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,half_bound,d,
           WireRefusal::CapabilityRefused);
    auto negative=native; negative.native_planes={-128,64};
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,negative,d,
           WireRefusal::CapabilityRefused);
    auto odd_geometry=native; odd_geometry.width=129;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,odd_geometry,d,
           WireRefusal::CapabilityRefused);
    auto reference=native; reference.require_reference_oracle=true;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,reference,d,
           WireRefusal::CapabilityRefused);
    // No GPU planes frontend exists. A native GPU request must refuse, and an
    // explicit scalar fallback must not turn that into a CPU admission.
    auto metal=native; metal.backend=SessionBackend::Metal;
    auto metal_device=d; metal_device.metal_available=true;
    metal_device.metal_max_buffer_length_bytes=64u*1024u*1024u;
    metal_device.metal_recommended_working_set_bytes=256u*1024u*1024u;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,metal,metal_device,
           WireRefusal::CapabilityRefused);
    metal.allow_cpu_scalar_fallback=true;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,metal,metal_device,
           WireRefusal::CapabilityRefused);
    auto cuda=native; cuda.backend=SessionBackend::Cuda;
    auto cuda_device=d; cuda_device.cuda_available=true;
    cuda_device.cuda_total_global_memory_bytes=1024ull*1024ull*1024ull;
    cuda.allow_cpu_scalar_fallback=true;
    Reject(WireDialect::Cfhd,St2073Edition::None,0,DecoderTarget::Unspecified,cuda,cuda_device,
           WireRefusal::CapabilityRefused);
    std::cout << checks << " checks PASS\n";
    return 0;
 } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
