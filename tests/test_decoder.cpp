// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decoder_support.h"
#include "decode_phase1_support.h"
#include "hypercine/research/decode_phase2e.h"
#include "../src/core/decoder_backend.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <new>
#include <thread>
#include <sstream>
namespace HyperCine { bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>&); }

// Count C++ heap requests, including decoder workers, during a closed steady-
// state window. CUDA/driver allocations are accounted separately.
static std::atomic<uint64_t> allocations{0};
void* operator new(size_t n) { allocations.fetch_add(1); if (auto p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
using namespace ProductionDecodeTest;
using namespace HyperCine::DecodePhase1;
namespace {
size_t TagOffset(const Bytes& bytes, uint16_t wanted) {
    for (size_t p=0;p+4<=bytes.size();) {
        const int raw=int16_t(BE16(bytes.data()+p));const uint16_t tag=uint16_t(raw<0?-raw:raw);
        if(tag==wanted)return p;
        const size_t words=BE16(bytes.data()+p+2);p+=4;
        if(tag==2)p+=words*4;
        else if((tag&0xff00)==0x2000)p+=((size_t(tag&255)<<16)|words)*4;
        else if(tag>=0x4000&&tag<0x6000)p+=words*4;
    }
    throw std::runtime_error("mutation tag absent: "+std::to_string(wanted));
}
void SetTag(Bytes& bytes,uint16_t tag,uint16_t value) {
    const size_t p=TagOffset(bytes,tag);bytes[p+2]=uint8_t(value>>8);bytes[p+3]=uint8_t(value);
}
void ClassifierBoundaries() {
    std::vector<GpuEntropyBandDescriptor> bands(2);bands[0].subband=1;bands[1].subband=2;
    bands[0].level=0;bands[1].level=1;DecodeFrameInfo info;
    auto dense=[&](uint32_t bytes,size_t count){bands[0].payload_bytes=bytes;bands[0].coefficient_count=count;return ClassifyDecodeFrame(bands,info);};
    Require(dense(32767,30000)==DecodeStrategy::Lightweight,"dense minimum below boundary");
    Require(dense(32768,31208)==DecodeStrategy::Lightweight,"dense ratio below boundary");
    Require(dense(32768,31207)==DecodeStrategy::Hybrid,"dense integer ratio above boundary");
    bands[0].coefficient_count=1000000;bands[0].payload_bytes=65536;
    bands[1].coefficient_count=1000000;bands[1].payload_bytes=196607;
    Require(ClassifyDecodeFrame(bands,info)==DecodeStrategy::Lightweight,"distributed bytes below boundary");
    bands[0].payload_bytes=65537;
    Require(ClassifyDecodeFrame(bands,info)==DecodeStrategy::Hybrid,"distributed threshold and concentration below boundary");
    bands[0].payload_bytes=65536;bands[1].payload_bytes=196608;
    Require(ClassifyDecodeFrame(bands,info)==DecodeStrategy::Lightweight,"75 percent concentration equality protects texture");
    bands[0].payload_bytes=65535;bands[1].payload_bytes=196609;
    Require(ClassifyDecodeFrame(bands,info)==DecodeStrategy::Lightweight,"concentration above boundary");
    bands.resize(5);for(auto& b:bands){b.subband=1;b.level=0;b.payload_bytes=65535;b.coefficient_count=1000000;}
    Require(ClassifyDecodeFrame(bands,info)==DecodeStrategy::Lightweight,"maximum band below distributed boundary");
    bands[0].payload_bytes=65536;
    Require(ClassifyDecodeFrame(bands,info)==DecodeStrategy::Hybrid,"maximum band equality distributed boundary");
    std::cout<<"integer classifier boundaries PASS\n"<<std::flush;
}
void Coefficients(Decoder& decoder, DecodeTicket ticket, const std::vector<DecodeBandDescriptor>& reference) {
    for (int b = 0; b < 30; ++b) {
        std::vector<int16_t> actual; int w = 0, h = 0, pitch = 0;
        Require(DecoderInspector::DownloadBand(decoder, ticket, b, actual, &w, &h, &pitch), "download coefficient band");
        const auto& r = reference[size_t(b)];
        Require(w == r.width && h == r.height, "coefficient geometry");
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            if (actual[size_t(y) * (pitch / 2) + x] != r.host_data[size_t(y) * (r.pitch / 2) + x])
                throw std::runtime_error("coefficient mismatch band=" + std::to_string(b) + " x=" + std::to_string(x) + " y=" + std::to_string(y) + " gpu=" + std::to_string(actual[size_t(y)*(pitch/2)+x]) + " cpu=" + std::to_string(r.host_data[size_t(y)*(r.pitch/2)+x]));
    }
}
int Golden(const char* manifest, std::ostream& json) {
    std::ifstream input(manifest); Require(bool(input), "golden manifest");
    std::string line; int count = 0, current_width = 0;
    std::vector<Sample> samples;
    std::unique_ptr<Decoder> decoder;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        int w, h, p; size_t bytes; std::string ih, ch, bh, yh;
        std::istringstream row(line);
        Require(bool(row >> w >> h >> p >> ih >> ch >> bytes >> bh >> yh), "manifest row");
        if (w != current_width) {
            current_width = w; samples = Generate(w, h, w == 7680 ? 1 : 5);
            DecoderConfig config; config.maximum_width = w; config.maximum_height = h; config.slot_count = 2;
            decoder = std::make_unique<Decoder>(config);
            Require(decoder->Initialize() == DecodeError::None, "golden decoder init");
        }
        auto& s = samples[size_t(p)];
        Require(s.compressed.size() == bytes && Hash(s.compressed.data(), s.compressed.size()) == ch && s.input_hash == ih, "frozen source sample");
        ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA); ref.Decode(s);
        auto bands = ExtractBandsFromNative(ref.GetNative());
        auto intermediates = ComputeCpuReferenceIntermediates(ref.GetNative());
        for (auto override_strategy : {DiagnosticDecodeStrategy::Automatic, DiagnosticDecodeStrategy::Lightweight,
                                      DiagnosticDecodeStrategy::Hybrid, DiagnosticDecodeStrategy::HostStreaming}) {
            DecodeRequest request; request.diagnostic_strategy = override_strategy;
            auto t = Begin(*decoder, s, request);
            Coefficients(*decoder, t, bands);
            for (int c = 0; c < 3; ++c) for (int l = 0; l < 2; ++l) {
                std::vector<int16_t> ll;
                Require(DecoderInspector::DownloadIntermediate(*decoder, t, c, l, ll), "intermediate download");
                Require(ll == (l ? intermediates.ll1[c] : intermediates.ll0[c]), "intermediate LL exactness");
            }
            auto image = Image(*decoder, t, s);
            Require(Hash(image.data(), image.size()) == bh && image == ref.output, "frozen BGRA exactness");
            Require(decoder->ReleaseOutput(t), "golden release");
        }
        ReferenceDecoder planar_ref(s, CFHD_PIXEL_FORMAT_YU64); planar_ref.Decode(s);
        const auto planar = ComputeCpuReferenceIntermediates(planar_ref.GetNative());
        DecodeRequest request; request.format = DecodePixelFormat::PlanarYUV422Signed16;
        auto t = Begin(*decoder, s, request); DeviceOutput out;
        Require(decoder->GetDeviceOutput(t, &out) && out.plane_count == 3 && out.orientation == DecodeOrientation::TopDown, "planar contract");
        std::array<HyperCineTest::AlignedVector<int16_t>, 3> planes;
        for (int c = 0; c < 3; ++c) {
            const auto& d = out.planes[size_t(c)]; planes[size_t(c)].resize(size_t(d.width) * d.height);
            GpuCheck(cudaMemcpy2D(planes[size_t(c)].data(), size_t(d.width) * 2, d.device_pointer, d.pitch_bytes,
                                  size_t(d.width) * 2, d.height, cudaMemcpyDeviceToHost));
            const int native_channel = c == 1 ? 2 : c == 2 ? 1 : 0;
            Require(std::equal(planes[size_t(c)].begin(), planes[size_t(c)].end(), planar.plane[native_channel].begin()), "planar exactness");
        }
        Bytes yu64(size_t(w) * h * 4);
        ConvertPlanarToYU64(planes[0].data(), w * 2, planes[2].data(), w, planes[1].data(), w, w, h, 10, yu64.data(), w * 4);
        Require(Hash(yu64.data(), yu64.size()) == yh && yu64 == planar_ref.output, "frozen YU64 hash");
        decoder->ReleaseOutput(t);
        if (count) json << ",\n";
        json << "{\"width\":" << w << ",\"height\":" << h << ",\"pattern\":\"" << PatternName(p)
             << "\",\"coefficients_exact\":30,\"max_error\":0,\"ll_exact\":true,\"planar_exact\":true,\"bgra_hash\":\"" << bh << "\"}";
        std::cout << "golden " << w << ' ' << PatternName(p) << " 30/30 LL/planar/BGRA/YU64 exact\n" << std::flush;
        ++count;
    }
    Require(count == 11, "11 frozen golden frames");
    return count;
}
void Lifetime(const std::vector<Sample>& samples) {
    DecoderConfig config; config.slot_count = 2; Decoder d(config);
    Require(d.Initialize() == DecodeError::None, "lifetime init");
    auto a = Begin(d, samples[0]); auto expected = Image(d, a, samples[0]);
    auto b = Begin(d, samples[2]); DecodeError error;
    Require(!d.QueueDecode(samples[1].compressed.data(), samples[1].compressed.size(), {}, &error) && error == DecodeError::Busy, "bounded backpressure");
    for (int i = 0; i < 12; ++i) {
        Require(d.ReleaseOutput(b), "release second slot");
        b = Begin(d, samples[size_t(i % 5)]);
        Require(Image(d, a, samples[0]) == expected, "held output overwritten");
    }
    Require(d.ReleaseOutput(b) && d.ReleaseOutput(a) && !d.ReleaseOutput(a), "out of order and double release");
    Require(d.PollDecode(a).state == DecodeState::Released, "released state");
    auto pending = d.QueueDecode(samples[2].compressed.data(), samples[2].compressed.size());
    Require(bool(pending) && d.ReleaseOutput(pending), "pending release drains");
    auto consumer_sample=samples[2];ReferenceDecoder consumer_reference(consumer_sample,CFHD_PIXEL_FORMAT_BGRA);consumer_reference.Decode(consumer_sample);
    auto ready = d.QueueDecode(samples[2].compressed.data(),samples[2].compressed.size());
    Stream first, second;
    Require(d.StreamWait(ready, CudaDecodeConsumer(first.handle)) && d.StreamWait(ready, CudaDecodeConsumer(second.handle)), "consumer waits while running");
    DecodeStatus readiness;
    do { readiness=d.PollDecode(ready);std::this_thread::yield(); } while(readiness.state==DecodeState::Running);
    Require(readiness.state==DecodeState::Ready,"nonblocking consumer readiness");DeviceOutput output;
    Require(d.GetDeviceOutput(ready, &output) && output.ticket.value == ready.value && output.native_ready_event && output.planes[0].pitch_bytes % 16 == 0, "output descriptor");
    DeviceBuffer<uint8_t> copied(size_t(samples[2].width) * samples[2].height * 4);
    DeviceBuffer<uint8_t> second_copy(size_t(samples[2].width) * samples[2].height * 4);
    Require(d.StreamWait(ready, CudaDecodeConsumer(first.handle)) && d.StreamWait(ready, CudaDecodeConsumer(second.handle)), "consumer waits");
    GpuCheck(cudaMemcpy2DAsync(copied.p, size_t(samples[2].width) * 4, output.planes[0].device_pointer, output.planes[0].pitch_bytes,
        size_t(samples[2].width) * 4, samples[2].height, cudaMemcpyDeviceToDevice, first.handle));
    GpuCheck(cudaMemcpy2DAsync(second_copy.p, size_t(samples[2].width) * 4, output.planes[0].device_pointer, output.planes[0].pitch_bytes,
        size_t(samples[2].width) * 4, samples[2].height, cudaMemcpyDeviceToDevice, second.handle));
    const auto& baseline = consumer_reference.output;
    Require(d.ReleaseOutput(ready), "release consumer output");
    auto reused = Begin(d, samples[0]); d.ReleaseOutput(reused);
    Bytes actual(baseline.size()); GpuCheck(cudaMemcpy(actual.data(), copied.p, actual.size(), cudaMemcpyDeviceToHost));
    Require(actual == baseline, "consumer copy lifetime");
    GpuCheck(cudaMemcpy(actual.data(), second_copy.p, actual.size(), cudaMemcpyDeviceToHost));
    Require(actual == baseline, "second consumer copy lifetime");
    Decoder other(config); Require(other.Initialize() == DecodeError::None, "second instance init");
    auto foreign = Begin(other, samples[0]);
    Require(d.PollDecode(foreign).state == DecodeState::Invalid && !d.ReleaseOutput(foreign), "instance ticket identity");
    other.ReleaseOutput(foreign);
    Bytes temporary=samples[0].compressed;
    auto copied_input=d.QueueDecode(temporary.data(),temporary.size());std::fill(temporary.begin(),temporary.end(),0);
    Require(d.WaitForDecode(copied_input).state==DecodeState::Ready&&Image(d,copied_input,samples[0])==expected,"input ownership after QueueDecode");
    Require(d.Initialize()==DecodeError::None&&d.PollDecode(copied_input).state==DecodeState::Invalid,"reinitialization drains and invalidates old ticket");
    auto renewed=Begin(d,samples[0]);d.ReleaseOutput(renewed);
    {
        Decoder outstanding(config); Require(outstanding.Initialize() == DecodeError::None, "destruction init");
        Require(bool(outstanding.QueueDecode(samples[2].compressed.data(), samples[2].compressed.size())), "destruction pending ticket");
        Require(bool(outstanding.QueueDecode(samples[0].compressed.data(), samples[0].compressed.size())), "destruction held ticket");
    }
    std::cout << "lifetime/backpressure/consumer streams/destruction PASS\n" << std::flush;
}
void Malformed(const std::vector<Sample>& samples, int count) {
    DecoderConfig config; config.slot_count = 2; Decoder d(config);
    Require(d.Initialize() == DecodeError::None, "mutation init");
    const auto& s = samples[2];
    std::vector<GpuEntropyBandDescriptor> descriptors;
    Require(ParseCineFormSample(s.compressed.data(), s.compressed.size(), 24, descriptors), "mutation descriptors");
    std::vector<GpuFsmEntry> tables;Require(ExtractCineFormBaseFsmTables(tables),"mutation FSM reference");
    auto overflow=s.compressed;std::memset(overflow.data()+descriptors[1].payload_offset,0,descriptors[1].payload_bytes);
    std::vector<GpuEntropySegmentDescriptor> checkpoints;std::string reason;
    Require(!GenerateSingleBandCheckpointsValidated(overflow.data(),overflow.size(),descriptors[1],0,tables.data(),tables.size(),GpuEntropyMode::ParallelAdaptive,32,checkpoints,&reason)
        &&reason.find("coefficient overflow")!=std::string::npos,"explicit coefficient-overflow rejection");
    uint32_t seed = Seed;
    for (int i = 0; i < count; ++i) {
        auto bytes = s.compressed; seed = seed * 1664525u + 1013904223u;
        size_t length = bytes.size(); const auto& band = descriptors[1 + (seed % 9)];
        switch (i % 12) {
        case 0: length = size_t(seed) % bytes.size(); break;
        case 1: std::memset(bytes.data() + band.payload_offset, 0, band.payload_bytes); break;
        case 2: std::memset(bytes.data() + band.payload_offset + band.payload_bytes, 0, 4); break;
        case 3: bytes[band.payload_offset - 4] = 0x7f; bytes[band.payload_offset - 3] = 0xff; break;
        case 4: length = 31; break;
        case 5: SetTag(bytes,20,0);break;
        case 6: SetTag(bytes,72,15);break; // Invalid codebook.
        case 7: SetTag(bytes,27,65535);break; // LL bounds.
        case 8: SetTag(bytes,49,0);break; // Band geometry.
        case 9: SetTag(bytes,10,1);break; // Unsupported temporal transform.
        case 10: // Corrupted chunk offset/extent.
            bytes[band.payload_offset-8+2]=255;bytes[band.payload_offset-8+3]=255;break;
        default: // Difference flag with invalid entropy: never publish reconstruction.
            SetTag(bytes,72,0x11);std::memset(bytes.data()+descriptors[1].payload_offset,0,descriptors[1].payload_bytes);break;
        }
        DecodeError error;
        auto t = d.QueueDecode(bytes.data(), length, {}, &error);
        Require(bool(t), "malformed accepted ticket");
        auto status = d.WaitForDecode(t);
        Require(status.state == DecodeState::Failed && status.error != DecodeError::None, "malformed must fail");
        DeviceOutput output; Require(!d.GetDeviceOutput(t, &output) && !output.plane_count, "failed output unpublished");
        Require(d.PollDecode(t).state == DecodeState::Failed && d.PollDecode(t).error == status.error, "first terminal error preserved");
        Require(d.ReleaseOutput(t), "failed release");
        if (i % 25 == 0) { auto good = Begin(d, samples[size_t(i % 5)]); d.ReleaseOutput(good); }
    }
    auto t = d.QueueDecode(nullptr, 1024); Require(d.WaitForDecode(t).state == DecodeState::Failed, "null sample"); d.ReleaseOutput(t);
    t = d.QueueDecode(s.compressed.data(), 32u * 1024 * 1024 + 1);
    Require(d.WaitForDecode(t).error == DecodeError::SampleTooLarge, "bounded scratch/sample capacity"); d.ReleaseOutput(t);
    for(int bad=0;bad<3;++bad){auto bytes=s.compressed;
        if(bad==0)SetTag(bytes,72,0x8000);else if(bad==1)SetTag(bytes,68,0);else{const size_t p=descriptors[0].payload_offset-8;bytes[p+2]=0;bytes[p+3]=1;}
        t=d.QueueDecode(bytes.data(),bytes.size());Require(d.WaitForDecode(t).state==DecodeState::Failed,"reserved flags, interlaced and corrupted LL chunk rejection");
        DeviceOutput unavailable;Require(!d.GetDeviceOutput(t,&unavailable),"malformed no output");d.ReleaseOutput(t);}
    std::cout << "deterministic malformed mutations " << count << " PASS\n" << std::flush;
}
void Concurrent(const std::vector<Sample>& samples, int instances, int runs) {
    std::vector<Bytes> expected;
    for (auto s : samples) { ReferenceDecoder ref(s, CFHD_PIXEL_FORMAT_BGRA); ref.Decode(s); expected.push_back(ref.output); }
    std::vector<std::unique_ptr<Decoder>> decoders;
    for (int i = 0; i < instances; ++i) { DecoderConfig c; c.slot_count = 2; c.host_worker_count = 4;
        decoders.push_back(std::make_unique<Decoder>(c)); Require(decoders.back()->Initialize() == DecodeError::None, "concurrent init"); }
    std::atomic<bool> failed{false}; std::vector<std::thread> workers;
    for (int i = 0; i < instances; ++i) workers.emplace_back([&, i] {
        try { Bytes image(expected[0].size()); uint32_t seed = Seed + uint32_t(i);
            for (int n = 0; n < runs; ++n) { seed = seed * 1664525u + 1013904223u; const size_t p = seed % 5;
                auto t = Begin(*decoders[size_t(i)], samples[p]); Download(*decoders[size_t(i)], t, image.data(), 1920 * 4);
                Require(image == expected[p], "concurrent independent stream exactness"); decoders[size_t(i)]->ReleaseOutput(t); }
        } catch (...) { failed = true; }
    });
    for (auto& thread : workers) thread.join(); Require(!failed, "multi-instance failure");
    std::cout << instances << " decoder instances x " << runs << " random access exact PASS\n" << std::flush;
}
void Difference() {
    void* raw = nullptr; Check(CFHD_OpenEncoder(&raw, nullptr), "difference encoder"); Encoder encoder(raw);
    Check(CFHD_PrepareToEncode(raw,1920,1080,CFHD_PIXEL_FORMAT_YUY2,CFHD_ENCODED_FORMAT_YUV_422,
        CFHD_ENCODING_FLAGS_YUV_INTERLACED,CFHD_ENCODING_QUALITY_FILMSCAN3), "difference prepare");
    auto input = HyperCineTest::GenerateTestVectorYUY2(HyperCineTest::PATTERN_PSEUDO_RANDOM_NOISE,1920,1080);
    Check(CFHD_EncodeSample(raw,input.data(),1920*2), "difference encode"); void* data = nullptr; size_t n = 0;
    Check(CFHD_GetSampleData(raw,&data,&n), "difference sample"); Sample sample{1920,1080,2,Bytes(n),{}}; std::memcpy(sample.compressed.data(),data,n);
    std::vector<GpuEntropyBandDescriptor> parsed; Require(ParseCineFormSample(sample.compressed.data(),n,24,parsed), "real difference parse");
    Require(std::any_of(parsed.begin(),parsed.end(),[](const auto& d){return d.difference_coding;}), "real difference flags");
    Decoder d; Require(d.Initialize() == DecodeError::None, "difference init");
    auto rejected = d.QueueDecode(sample.compressed.data(),n);
    Require(d.WaitForDecode(rejected).state==DecodeState::Failed && d.PollDecode(rejected).error==DecodeError::UnsupportedEncodingMode,"deliberate unsupported interlaced output status");
    DeviceOutput unpublished;Require(!d.GetDeviceOutput(rejected,&unpublished),"unsupported interlaced output unpublished");d.ReleaseOutput(rejected);
    ReferenceDecoder cpu(sample,CFHD_PIXEL_FORMAT_BGRA);cpu.Decode(sample);
    const auto cpu_bands=ExtractBandsFromNative(cpu.GetNative());
    CudaDecoderPipelinePhase2E ref; Require(ref.Initialize(1920,1080,1,10,8,4), "difference legacy init");
    auto rt = ref.QueueDecode(sample.compressed.data(),n); Require(rt && ref.WaitForDecode(rt), "difference legacy decode");
    CudaDecoderPipelinePhase2E hybrid;Require(hybrid.Initialize(1920,1080,1,10,8,4),"difference hybrid init");
    hybrid.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame);hybrid.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::AdaptiveDynamic);
    auto ht=hybrid.QueueDecode(sample.compressed.data(),n);Require(ht&&hybrid.WaitForDecode(ht),"difference hybrid entropy");
    for(int b=0;b<30;++b) { std::vector<int16_t> a,z;int bw,bh,pitch;
        Require(hybrid.DownloadBandCoefficientsToHost(ht,b,a,&bw,&bh,&pitch) && ref.DownloadBandCoefficientsToHost(rt,b,z) && a==z,"difference legacy/hybrid coefficient exactness");
        const auto& r=cpu_bands[size_t(b)];Require(bw==r.width&&bh==r.height,"difference CPU geometry");
        for(int y=0;y<bh;++y)for(int x=0;x<bw;++x)Require(a[size_t(y)*(pitch/2)+x]==r.host_data[size_t(y)*(r.pitch/2)+x],"difference CPU coefficient max error 0"); }
    hybrid.ReleaseOutput(ht);ref.ReleaseOutput(rt);
    std::cout << "real difference-coded entropy/coefficient validation -- interlaced output reconstruction deferred: 30/30 CPU/E/hybrid exact, max error 0 PASS\n" << std::flush;
    // Optional unchanged-encoder search: progressive YUY2 Noise. Failure to
    // find difference coding does not widen or block the production scope.
    void* progressive_raw=nullptr;Check(CFHD_OpenEncoder(&progressive_raw,nullptr),"progressive probe encoder");Encoder progressive_encoder(progressive_raw);
    Check(CFHD_PrepareToEncode(progressive_raw,1920,1080,CFHD_PIXEL_FORMAT_YUY2,CFHD_ENCODED_FORMAT_YUV_422,CFHD_ENCODING_FLAGS_NONE,CFHD_ENCODING_QUALITY_FILMSCAN3),"progressive probe prepare");
    Check(CFHD_EncodeSample(progressive_raw,input.data(),1920*2),"progressive probe encode");Check(CFHD_GetSampleData(progressive_raw,&data,&n),"progressive probe sample");
    Sample probe{1920,1080,2,Bytes(n),{}};std::memcpy(probe.compressed.data(),data,n);
    DecodeError probe_status;const bool progressive=ParseCineFormSampleValidated(probe.compressed.data(),n,24,parsed,nullptr,&probe_status);
    if(progressive&&std::any_of(parsed.begin(),parsed.end(),[](const auto& b){return b.difference_coding;})){
        ReferenceDecoder probe_cpu(probe,CFHD_PIXEL_FORMAT_BGRA);probe_cpu.Decode(probe);auto probe_ticket=Begin(d,probe);
        Coefficients(d,probe_ticket,ExtractBandsFromNative(probe_cpu.GetNative()));Require(Image(d,probe_ticket,probe)==probe_cpu.output,"genuine progressive difference output exact");d.ReleaseOutput(probe_ticket);
        std::cout<<"optional genuine progressive difference-coded sample: end-to-end exact PASS\n"<<std::flush;
    }else std::cout<<"optional unchanged-encoder progressive YUY2 Noise probe: no progressive difference-coded sample found\n"<<std::flush;
}
void Determinism(int w, int h, int pattern, int runs) {
    auto samples = Generate(w,h,w==7680?1:5); DecoderConfig c; c.maximum_width=w; c.maximum_height=h; c.slot_count=2;
    Decoder d(c); Require(d.Initialize()==DecodeError::None,"determinism init");
    auto t=Begin(d,samples[size_t(pattern)]); auto expected=Image(d,t,samples[size_t(pattern)]);
    DecodeFrameInfo base; Require(d.GetFrameInfo(t,&base),"base decision");
    std::array<DecoderBandDecision,27> bands; Require(DecoderInspector::BandDecisions(d,t,&bands),"base band decisions"); d.ReleaseOutput(t);
    Bytes actual(expected.size());
    for(int i=0;i<runs;++i) { t=Begin(d,samples[size_t(pattern)]); Download(d,t,actual.data(),size_t(w)*4);
        DecodeFrameInfo info; std::array<DecoderBandDecision,27> choice;
        Require(d.GetFrameInfo(t,&info) && info.strategy==base.strategy && DecoderInspector::BandDecisions(d,t,&choice) && Same(choice,bands) && actual==expected,"determinism exactness/selection"); d.ReleaseOutput(t); }
    std::cout << "determinism " << w << ' ' << PatternName(pattern) << ' ' << runs << " exact PASS\n" << std::flush;
}
void LongRun() {
    std::filesystem::create_directories("benchmarks/results");
    std::ofstream out("benchmarks/results/decode_phase3a_longrun.json");
    out << "{\"verification\":\"full BGRA byte comparison every frame, frame and per-band decisions, cyclic and seeded random access\",\"cases\":[\n";
    for(int size=0;size<3;++size) {
        const int w=size==0?1920:size==1?3840:7680,h=size==0?1080:size==1?2160:4320,runs=size==0?10000:size==1?5000:1000;
        auto samples=Generate(w,h,size==2?1:5); std::vector<Bytes> expected;
        std::vector<DecodeStrategy> strategies; std::vector<std::array<DecoderBandDecision,27>> decisions;
        DecoderConfig c; c.maximum_width=w; c.maximum_height=h; c.slot_count=3; Decoder d(c);
        Require(d.Initialize()==DecodeError::None,"longrun init");
        std::array<DecodeTicket,3> warm{};
        for(auto& sample:samples) {
            ReferenceDecoder cpu(sample,CFHD_PIXEL_FORMAT_BGRA);cpu.Decode(sample);expected.push_back(cpu.output);
            for(auto& t:warm)t=Begin(d,sample);
            DecodeFrameInfo info; std::array<DecoderBandDecision,27> bands;
            Require(d.GetFrameInfo(warm[0],&info)&&DecoderInspector::BandDecisions(d,warm[0],&bands),"longrun baseline decisions");
            strategies.push_back(info.strategy);decisions.push_back(bands);
            for(auto t:warm){Require(Image(d,t,sample)==expected.back(),"longrun warm exactness");d.ReleaseOutput(t);}
        }
        Bytes actual(expected[0].size());
        struct Snapshot{int frame;uint64_t ws;};std::vector<Snapshot> snapshots;snapshots.reserve(size_t(runs/250+2));
        const auto memory_before=d.MemoryStats();size_t free_before=0,total=0;GpuCheck(cudaMemGetInfo(&free_before,&total));
        const uint64_t ws_before=WorkingSet();const auto allocation_before=allocations.load();const double cpu0=CpuMs();auto start=Clock::now();
        double first_ms=0,last_ms=0;uint32_t seed=Seed;int completed=0;const int cycle[]={0,2,3,4,1,2,0};
        while(completed<runs) {
            std::array<DecodeTicket,3> tickets{};std::array<int,3> patterns{};auto batch_start=Clock::now();
            const int batch=(std::min)(3,runs-completed);
            for(int n=0;n<batch;++n){seed=seed*1664525u+1013904223u;patterns[size_t(n)]=size==2?0:((completed+n)%17==0?int(seed%5):cycle[(completed+n)%7]);
                const auto& sample=samples[size_t(patterns[size_t(n)])];tickets[size_t(n)]=d.QueueDecode(sample.compressed.data(),sample.compressed.size());Require(bool(tickets[size_t(n)]),"longrun no ticket exhaustion");}
            for(int n=0;n<batch;++n){const auto t=tickets[size_t(n)];const int p=patterns[size_t(n)];Require(d.WaitForDecode(t).state==DecodeState::Ready,"longrun no failed frame");
                Download(d,t,actual.data(),size_t(w)*4);Require(actual==expected[size_t(p)],"longrun no output corruption");
                DecodeFrameInfo info;std::array<DecoderBandDecision,27> bands;Require(d.GetFrameInfo(t,&info)&&info.strategy==strategies[size_t(p)]&&DecoderInspector::BandDecisions(d,t,&bands)&&Same(bands,decisions[size_t(p)]),"longrun deterministic strategy transitions");
                Require(d.ReleaseOutput(t),"longrun release");}
            const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-batch_start).count();
            if(completed<runs/10)first_ms+=elapsed;if(completed>=runs-runs/10)last_ms+=elapsed;
            const int previous=completed;completed+=batch;
            if(completed/250!=previous/250 || completed==runs)snapshots.push_back({completed,WorkingSet()});
            if(completed/1000!=previous/1000)std::cout<<"longrun "<<w<<" "<<completed<<'/'<<runs<<" exact\n"<<std::flush;
        }
        const double seconds=std::chrono::duration<double>(Clock::now()-start).count(),cpu=(CpuMs()-cpu0)/runs;
        const uint64_t steady=allocations.load()-allocation_before;size_t free_after=0;GpuCheck(cudaMemGetInfo(&free_after,&total));const auto memory_after=d.MemoryStats();
        Require(steady==0,"longrun zero steady-state C++ allocations");
        Require(memory_before.cuda_allocation_calls==memory_after.cuda_allocation_calls&&memory_before.device_bytes==memory_after.device_bytes&&memory_before.pinned_host_bytes==memory_after.pinned_host_bytes&&memory_before.pageable_capacity_bytes==memory_after.pageable_capacity_bytes,"longrun owned memory stable");
        const uint64_t ws_after=WorkingSet();Require(ws_after<=ws_before+16u*1024*1024,"longrun unexplained working set growth > 16 MiB");
        if(size)out<<",\n";out<<"{\"width\":"<<w<<",\"height\":"<<h<<",\"frames\":"<<runs<<",\"fps_including_download_and_verification\":"<<runs/seconds<<",\"process_cpu_ms_per_frame_including_verification\":"<<cpu
            <<",\"first_decile_ms\":"<<first_ms/(runs/10)<<",\"last_decile_ms\":"<<last_ms/(runs/10)<<",\"steady_state_cpp_allocations\":"<<steady<<",\"owned_cuda_allocations_after_warmup\":0,\"working_set_before_bytes\":"<<ws_before<<",\"working_set_after_bytes\":"<<ws_after
            <<",\"global_free_device_bytes_before\":"<<free_before<<",\"global_free_device_bytes_after\":"<<free_after<<",\"working_set_snapshots\":[";
        for(size_t i=0;i<snapshots.size();++i){if(i)out<<',';out<<"{\"frame\":"<<snapshots[i].frame<<",\"bytes\":"<<snapshots[i].ws<<'}';}out<<"],\"status\":\"PASS\"}";
    }
    out<<"],\"status\":\"PASS\"}\n";
}
} // namespace
int main(int argc,char** argv) {
    try {
        Require(argc>=2,"manifest path required"); bool sanitizer=argc>2 && std::string(argv[2])=="--sanitizer";
        if(argc>2 && std::string(argv[2])=="--difference") { Difference(); return 0; }
        if(argc>2 && std::string(argv[2])=="--longrun") { LongRun(); return 0; }
        ClassifierBoundaries();
        std::filesystem::create_directories("benchmarks/results");
        std::ofstream json("benchmarks/results/decode_phase3a_validation.json");
        json << "{\"mode\":\"" << (sanitizer?"focused_sanitizer":"full") << "\",\"golden\":[\n";
        if(!sanitizer) Golden(argv[1],json); json << "],\n";
        auto samples=Generate(1920,1080,5);
        Lifetime(samples); Malformed(samples,sanitizer?36:1000);
        Concurrent(samples,2,sanitizer?5:100); Concurrent(samples,4,sanitizer?5:100); Difference();
        Determinism(1920,1080,2,sanitizer?3:200);
        Determinism(3840,2160,2,sanitizer?2:100);
        Determinism(3840,2160,3,sanitizer?2:100);
        if(!sanitizer) Determinism(7680,4320,0,20);
        // Warm every strategy/pattern before counting C++ allocations.
        Decoder d; Require(d.Initialize()==DecodeError::None,"steady-state init");
        for(auto strategy:{DiagnosticDecodeStrategy::Lightweight,DiagnosticDecodeStrategy::Hybrid})
            for(int p=0;p<5;++p) { DecodeRequest r; r.diagnostic_strategy=strategy; auto t=Begin(d,samples[size_t(p)],r); d.ReleaseOutput(t); }
        const auto before=allocations.load();
        for(int i=0;i<(sanitizer?10:100);++i) { auto t=Begin(d,samples[size_t(i%5)]); d.ReleaseOutput(t); }
        const auto steady=allocations.load()-before;
        Require(steady==0,"steady-state C++ heap allocations");
        json << "\"malformed_cases\":" << (sanitizer?41:1005) << ",\"coefficient_overflow_rejected\":true,\"two_instances\":true,\"four_instances\":true,\"random_access\":true,"
             << "\"output_lifetime\":true,\"queue_saturation\":true,\"real_difference_coded_coefficients_exact\":30,\"real_difference_max_error\":0,\"interlaced_production_rejected\":true,\"interlaced_output_reconstruction\":\"deferred\",\"determinism\":true,\"steady_state_cpp_allocations\":" << steady << ",\"status\":\"PASS\"}\n";
        std::cout << "production decoder validation PASS; C++ allocations after warm-up=" << steady << '\n';
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
