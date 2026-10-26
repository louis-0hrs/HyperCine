// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
#include "hypercine/filmscan3.h"
#include "reference_frame_generator.h"
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace hypercine::standalone;
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool SameState(const FilmScan3State& a, const FilmScan3State& b) {
    return a.frame_index == b.frame_index && a.fs_rate_limiter == b.fs_rate_limiter &&
           a.previous_sample_bytes == b.previous_sample_bytes;
}
template<class F> void Reject(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "negative encode was accepted");
}
struct Run { std::vector<FilmScan3Frame> frames; FilmScan3State state; };
Run Session(bool hash) {
    FilmScan3Encoder encoder(FilmScan3Geometry{256,256});
    encoder.SetCoefficientHashing(hash);
    AlignedVector<uint32_t> input(256*256);
    Run run;
    for (int n = 0; n < 4; ++n) {
        reference_fixture::GeneratePhase3AFrame(256,256,n,
            n % 2 ? reference_fixture::Phase3APattern::DeterministicNoise :
                    reference_fixture::Phase3APattern::FlatBlack,
            reinterpret_cast<uint8_t*>(input.data()),1024,0x1337BEEF);
        const auto before = encoder.state();
        Reject([&] { encoder.Encode(nullptr,input.size()*4,1024); });
        Require(SameState(before,encoder.state()), "failed encode changed rate state");
        run.frames.push_back(encoder.Encode(reinterpret_cast<uint8_t*>(input.data()),input.size()*4,1024));
    }
    run.state = encoder.state();
    return run;
}
}
int main(int argc, char** argv) { try {
    Require(argc == 2, "expected pinned flat packet");
    // Separate encoder objects run simultaneously; no object is shared.
    auto a = std::async(std::launch::async, Session, true);
    auto b = std::async(std::launch::async, Session, false);
    auto c = std::async(std::launch::async, Session, false);
    auto on = a.get(), off = b.get(), other = c.get();
    Require(SameState(on.state,off.state) && SameState(on.state,other.state), "hashing changed rate state");
    for (std::size_t n = 0; n < on.frames.size(); ++n) {
        const auto& x=on.frames[n]; const auto& y=off.frames[n]; const auto& z=other.frames[n];
        Require(x.sample == y.sample && x.sample == z.sample, "concurrent packet mismatch");
        Require(x.tables.quant == y.tables.quant && x.tables.scale == y.tables.scale &&
                SameState(x.state,y.state), "hashing changed quantizer feedback");
        Require(FilmScan3FullCoefficientHashesEqual(x,x), "full gate rejects real hashes");
        Require(!FilmScan3FullCoefficientHashesEqual(y,y) &&
                !FilmScan3FullCoefficientHashesEqual(x,y), "missing diagnostic evidence passed");
        Require(y.coefficient_fnv64 != z.coefficient_fnv64, "independent sessions share tombstones");
    }
    FilmScan3Frame absent;
    Require(!FilmScan3FullCoefficientHashesEqual(absent,absent), "absent evidence passed");
    auto mutation=on.frames.front(); mutation.coefficient_fnv64[0]^=1;
    Require(!FilmScan3FullCoefficientHashesEqual(on.frames.front(),mutation), "hash mutation not detected");
    // A late writer-capacity failure must also preserve packet/rate state.
    AlignedVector<uint32_t> small(256*256);
    reference_fixture::GeneratePhase3AFrame(256,256,1,reference_fixture::Phase3APattern::DeterministicNoise,
        reinterpret_cast<uint8_t*>(small.data()),1024,0x1337BEEF);
    FilmScan3Encoder sizing(FilmScan3Geometry{256,256});
    const auto capacity=sizing.Encode(reinterpret_cast<uint8_t*>(small.data()),small.size()*4,1024).sample.size()-1;
    FilmScan3Encoder bounded(FilmScan3Geometry{256,256,capacity});
    bounded.SetCoefficientHashing(false);
    const auto before=bounded.state();
    Reject([&] { bounded.Encode(reinterpret_cast<uint8_t*>(small.data()),small.size()*4,1024); });
    Require(SameState(before,bounded.state()), "capacity failure changed rate state");
    reference_fixture::GeneratePhase3AFrame(256,256,0,reference_fixture::Phase3APattern::FlatBlack,
        reinterpret_cast<uint8_t*>(small.data()),1024,0x1337BEEF);
    FilmScan3Encoder fresh(FilmScan3Geometry{256,256,capacity});
    fresh.SetCoefficientHashing(false);
    Require(bounded.Encode(reinterpret_cast<uint8_t*>(small.data()),small.size()*4,1024).sample ==
            fresh.Encode(reinterpret_cast<uint8_t*>(small.data()),small.size()*4,1024).sample,
            "recovery after writer failure changed packet");
    AlignedVector<uint32_t> input(1920*1080);
    reference_fixture::GeneratePhase3AFrame(1920,1080,0,reference_fixture::Phase3APattern::FlatBlack,
        reinterpret_cast<uint8_t*>(input.data()),7680,0x1337BEEF);
    std::ifstream file(argv[1],std::ios::binary);
    Require(bool(file), "pinned packet unavailable");
    std::vector<uint8_t> pinned((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    FilmScan3Encoder full, skipped; skipped.SetCoefficientHashing(false);
    const auto one=full.Encode(reinterpret_cast<uint8_t*>(input.data()),input.size()*4,7680);
    const auto two=skipped.Encode(reinterpret_cast<uint8_t*>(input.data()),input.size()*4,7680);
    Require(one.sample == pinned && two.sample == pinned, "pinned autonomous 1080p packet parity changed");
    std::cout << "PASS: three concurrent sessions, 12 packets, diagnostic negatives, early/late failure, rate state, pinned 1080p packet\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
