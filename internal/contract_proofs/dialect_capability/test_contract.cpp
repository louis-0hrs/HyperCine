// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "contract.h"
#include <cstdlib>
#include <iostream>
using namespace dialect_proof;
unsigned cases = 0, factories = 0;
void Expect(Request r, ExistingAdmission a, Status s, Reason why) {
    const auto result = Check(r, a);
    const auto before = factories;
    // Simulated allocation boundary; no real codec resources are acquired.
    if (result.may_plan) ++factories;
    if (result.status != s || result.reason != why || result.requested_wire != r.wire ||
        result.may_plan != (s == Status::ExperimentalPlan) ||
        (s != Status::ExperimentalPlan && factories != before)) {
        std::cerr << "contract case " << cases << " failed" << std::endl;
        std::exit(1);
    }
    ++cases;
}
int main() {
    Request r;
    Expect(r, {}, Status::ExperimentalPlan, Reason::ExperimentalOnly);
    for (const auto shape : {std::array<int,2>{1280,720}, {2048,1080}}) {
        r.width=shape[0]; r.height=shape[1];
        Expect(r, {}, Status::ExperimentalPlan, Reason::ExperimentalOnly);
    }
    r={}; r.standard.edition[1]=2017;
    Expect(r, {}, Status::InvalidRequest, Reason::ContradictoryStandard);
    r={}; r.wire=Wire::Vc5;
    Expect(r, {}, Status::InvalidRequest, Reason::MissingEdition);
    r.standard.edition[1]=2017;
    Expect(r, {}, Status::Unsupported, Reason::WireUnavailable);
    for (unsigned i=0;i<3;++i) {
        r.standard.features=1u<<i;
        Expect(r, {}, Status::InvalidRequest, Reason::FeaturePartMismatch);
        r.standard.edition[5+i]=2015;
        Expect(r, {}, Status::Unsupported, Reason::WireUnavailable);
        r.standard.edition[5+i]=0;
    }
    r.standard={}; r.standard.edition[1]=2099;
    Expect(r, {}, Status::Unsupported, Reason::WireUnavailable);
    for (unsigned part : {0u,2u}) {
        r.standard.edition[part]=2022;
        Expect(r, {}, Status::InvalidRequest, Reason::ContradictoryStandard);
        r.standard.edition[part]=0;
    }
    for (auto target : {Target::GoPro, Target::Ffmpeg, Target::Application}) {
        r={}; r.target=target;
        Expect(r, {}, Status::Unqualified, Reason::QualificationMissing);
        r.wire=Wire::Vc5; r.standard.edition[1]=2017;
        Expect(r, {}, Status::Unsupported, Reason::WireUnavailable);
    }
    r={}; r.wire=static_cast<Wire>(99);
    Expect(r, {}, Status::InvalidRequest, Reason::UnknownValue);
    r={}; r.target=static_cast<Target>(99);
    Expect(r, {}, Status::InvalidRequest, Reason::UnknownValue);
    r={}; r.standard.features=8;
    Expect(r, {}, Status::InvalidRequest, Reason::UnknownValue);
    for (auto shape : {std::array<int,2>{0,24}, {129,24}, {128,25}, {-128,24}}) {
        r={}; r.width=shape[0]; r.height=shape[1];
        Expect(r, {}, Status::InvalidRequest, Reason::InvalidGeometry);
    }
    r={}; r.source_bits=10;
    Expect(r, {}, Status::Unsupported, Reason::FormatUnavailable);
    r={}; r.coded_bits=12;
    Expect(r, {}, Status::Unsupported, Reason::FormatUnavailable);
    r={}; r.bgra_source=false;
    Expect(r, {}, Status::Unsupported, Reason::FormatUnavailable);
    r={}; r.yuv422_coded=false;
    Expect(r, {}, Status::Unsupported, Reason::FormatUnavailable);
    r={}; r.progressive_gop1=false;
    Expect(r, {}, Status::Unsupported, Reason::FormatUnavailable);
    r={}; Expect(r, {false,true,true}, Status::Unsupported, Reason::FormatUnavailable);
    Expect(r, {true,false,true}, Status::Unsupported, Reason::BackendUnavailable);
    Expect(r, {true,true,false}, Status::Unsupported, Reason::ResourceRefused);
    // One passing axis must not waive another, including target qualification.
    r.target=Target::GoPro;
    Expect(r, {true,true,false}, Status::Unsupported, Reason::ResourceRefused);
    std::cout << cases << " cases passed; simulated factories=" << factories << std::endl;
}
