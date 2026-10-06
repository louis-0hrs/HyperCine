// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <thread>
#include <chrono>
#include "hypercine/decode_phase2e.h"
#include "decode_phase0_support.h"
#include "decode_phase1_support.h"
using namespace HyperCine;
using namespace DecodePhase0;
using namespace HyperCine::DecodePhase1;
namespace HyperCine { bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>&); }
static void Require(bool x, const char* message) { if (!x) throw std::runtime_error(message); }
static void Check(cudaError_t e) { Require(e == cudaSuccess, cudaGetErrorString(e)); }
template<class T> struct Device {
    T* p = nullptr;
    explicit Device(size_t n) { Check(cudaMalloc(&p,sizeof(T)*n)); }
    ~Device() { cudaFree(p); }
};
static bool Equal(const std::vector<GpuEntropySegmentDescriptor>& a, const std::vector<GpuEntropySegmentDescriptor>& b) {
    if (a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i) {
        auto& x=a[i]; auto& y=b[i];
        if(x.band_index!=y.band_index || x.byte_offset!=y.byte_offset || x.nibble_idx!=y.nibble_idx ||
            x.initial_state!=y.initial_state || x.out_coeff_pos!=y.out_coeff_pos ||
            x.end_byte_offset!=y.end_byte_offset || x.end_nibble_idx!=y.end_nibble_idx ||
            x.max_coeff_pos!=y.max_coeff_pos) return false;
    }
    return true;
}
// Stage 1 gate: direct scanner, no replay and no coefficient destination.
static void IndexOnly(const Sample& sample) {
    std::vector<GpuEntropyBandDescriptor> descriptors;
    Require(ParseCineFormSample(sample.compressed.data(),sample.compressed.size(),24,descriptors),"parse");
    std::vector<GpuFsmEntry> tables;
    Require(ExtractCineFormBaseFsmTables(tables),"FSM extraction");
    GpuFsmTableManager tm; Require(tm.Initialize(),"FSM upload");
    Device<uint8_t> dp(sample.compressed.size());
    Device<GpuEntropyBandDescriptor> dd(1);
    Device<GpuEntropySegmentDescriptor> ds(64);
    Device<GpuCheckpointIndexResult> dr(1);
    Check(cudaMemcpy(dp.p,sample.compressed.data(),sample.compressed.size(),cudaMemcpyHostToDevice));
    int band=0;
    for(auto& d:descriptors) {
        if(d.method!=BandEncodingMethod::RunLengthVLC) continue;
        Check(cudaMemcpy(dd.p,&d,sizeof(d),cudaMemcpyHostToDevice));
        Check(LaunchGenerateBandCheckpointsGpu(dp.p,sample.compressed.size(),dd.p,band,tm.GetDeviceTable(),
            tables.size(),GpuEntropyMode::ParallelAdaptive,32,ds.p,64,dr.p,nullptr,d.payload_bytes,nullptr));
        GpuCheckpointIndexResult r{};
        Check(cudaMemcpy(&r,dr.p,sizeof(r),cudaMemcpyDeviceToHost));
        Require(r.finished && r.status==GpuBandDecodeStatus::Complete,"GPU index status");
        std::vector<GpuEntropySegmentDescriptor> cpu,gpu(r.segment_count);
        Require(GenerateSingleBandCheckpointsCpu(sample.compressed.data(),sample.compressed.size(),d,band,
            tables.data(),tables.size(),GpuEntropyMode::ParallelAdaptive,32,cpu),"host checkpoints");
        Check(cudaMemcpy(gpu.data(),ds.p,gpu.size()*sizeof(gpu[0]),cudaMemcpyDeviceToHost));
        Require(Equal(cpu,gpu),"direct CPU/GPU checkpoint mismatch");
        ++band;
    }
    Require(band==27,"27 index bands");
}
static uint64_t Decode(CudaDecoderPipelinePhase2E& p,const Sample& s, DeviceSurfaceType surface=DeviceSurfaceType::BGRA8) {
    auto t=p.QueueDecode(s.compressed.data(),s.compressed.size(),surface);
    Require(t && p.WaitForDecode(t),"decode failed");
    return t;
}
int main(int argc,char** argv) {
 try {
    Require(argc>=2,"manifest path required");
    bool corpus_only=argc>2 && std::string(argv[2])=="--corpus-only";
    bool sanitizer_corpus=argc>2 && std::string(argv[2])=="--sanitizer-corpus";
    bool no_stress=(argc>2 && std::string(argv[2])=="--no-stress") || sanitizer_corpus;
    bool sanitizer_stress=argc>2 && std::string(argv[2])=="--sanitizer-stress";
    std::ifstream input(argv[1]); Require(bool(input),"manifest");
    std::string line;
    int width=0,count=0; std::vector<Sample> samples;
    CudaDecoderPipelinePhase2E host,gpu;
    while(std::getline(input,line)) {
        if(line.empty()||line[0]=='#') continue;
        std::istringstream row(line);
        int w,h,pattern; size_t bytes; std::string ih,ch,bgra_hash,yu64_hash;
        Require(bool(row>>w>>h>>pattern>>ih>>ch>>bytes>>bgra_hash>>yu64_hash),"manifest row");
        if(w!=width) {
            samples=Generate(w,h,w==7680?1:5); width=w;
            Require(host.Initialize(w,h,2,10,8,4) && gpu.Initialize(w,h,2,10,1,4),"initialize");
            gpu.SetPipelineMode(Phase2EPipelineMode::Phase2FAGpuIndexBatch);
        }
        auto& s=samples.at(pattern);
        Require(s.input_hash==ih && s.compressed.size()==bytes && Hash(s.compressed.data(),bytes)==ch,"generation hashes");
        // The normal suite proves the separate Stage 1 scanner. Sanitizers
        // exercise the same scanner through both integrated output paths and
        // still compare every generated checkpoint field for all 11 vectors.
        if (!sanitizer_corpus && !sanitizer_stress) IndexOnly(s);
        ReferenceDecoder ref(s,CFHD_PIXEL_FORMAT_BGRA); ref.Decode(s); ref.VerifyThreads();
        auto bands=ExtractBandsFromNative(ref.GetNative());
        auto ht=Decode(host,s), gt=Decode(gpu,s);
        std::vector<GpuEntropySegmentDescriptor> hc,gc;
        Require(host.GetHostCheckpoints(ht,hc) && gpu.GetGpuCheckpoints(gt,gc) && Equal(hc,gc),"checkpoint mismatch");
        for(int b=0;b<30;++b) {
            std::vector<int16_t> a,c; int aw,ah,ap,cw,cheight,cp;
            Require(host.DownloadBandCoefficientsToHost(ht,b,a,&aw,&ah,&ap) &&
                gpu.DownloadBandCoefficientsToHost(gt,b,c,&cw,&cheight,&cp),"coeff download");
            Require(aw==cw && ah==cheight && ap==cp && cw==bands[b].width && cheight==bands[b].height,"geometry");
            for(int y=0;y<ah;++y) for(int x=0;x<aw;++x)
                Require(a[y*(ap/2)+x]==c[y*(cp/2)+x] &&
                    c[y*(cp/2)+x]==bands[b].host_data[y*(bands[b].pitch/2)+x],"coefficient error");
        }
        std::vector<uint8_t> bgra(ref.pitch*h);
        Require(gpu.DownloadBgraToHost(gt,bgra.data(),ref.pitch),"BGRA download");
        Require(std::memcmp(bgra.data(),ref.output.data(),bgra.size())==0 &&
            Hash(bgra.data(),bgra.size())==bgra_hash,"BGRA exactness/golden hash");
        auto intermediate=ComputeCpuReferenceIntermediates(ref.GetNative());
        for(int c=0;c<3;++c) for(int l=0;l<2;++l) {
            std::vector<int16_t> v;
            Require(gpu.DownloadIntermediateLl(gt,c,l,v),"LL download");
            Require(v==(l?intermediate.ll1[c]:intermediate.ll0[c]),"LL exactness");
        }
        host.ReleaseOutput(ht); gpu.ReleaseOutput(gt);
        ReferenceDecoder planar(s,CFHD_PIXEL_FORMAT_YU64); planar.Decode(s);
        auto expected=ComputeCpuReferenceIntermediates(planar.GetNative());
        auto pt=Decode(gpu,s,DeviceSurfaceType::PlanarYUV422_16S);
        std::vector<int16_t> y(w*h),v(w*h/2),u(w*h/2);
        Require(gpu.DownloadPlanarToHost(pt,y.data(),w*2,v.data(),w,u.data(),w),"planar download");
        Require(y==expected.plane[0] && v==expected.plane[1] && u==expected.plane[2],"planar exactness");
        gpu.ReleaseOutput(pt);
        std::cout<<"[PASS] "<<w<<"x"<<h<<" "<<PatternName(pattern)
            <<" checkpoints=0 mismatches; coefficients=30/30; LL/planar/BGRA error=0\n"<<std::flush;
        ++count;
    }
    Require(count==11,"11 corpus vectors");
    if(corpus_only) return 0;
    // Delays and descriptor ordering vary the index/replay/inverse completion DAG.
    samples=Generate(1920,1080,5);
    Require(gpu.Initialize(1920,1080,2,10,1,4),"race init");
    gpu.SetPipelineMode(Phase2EPipelineMode::Phase2FAGpuIndexBatch);
    auto baseline=Decode(gpu,samples[2]);
    std::vector<uint8_t> base(1920*1080*4);
    gpu.DownloadBgraToHost(baseline,base.data(),1920*4); gpu.ReleaseOutput(baseline);
    for(int scenario=0;scenario<5;++scenario) {
        gpu.SetSchedulingPolicy(scenario==2?Phase2ESchedulingPolicy::PolicyB_InverseCritical:
            scenario==3?Phase2ESchedulingPolicy::DescriptorOrder:Phase2ESchedulingPolicy::PolicyA_LargestFirst);
        gpu.SetTestDelayHook([scenario](int,int ch,int level,int) {
            if((scenario==0 && ch==0)||(scenario==1 && ch!=0)||(scenario==4 && level==0))
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
        });
        auto t=Decode(gpu,samples[2]); std::vector<uint8_t> actual(base.size());
        Require(gpu.DownloadBgraToHost(t,actual.data(),1920*4) && actual==base,"reordering output");
        Phase2eSlotStageTimings timing;
        Require(gpu.GetStageTimings(t,&timing),"race timings");
        if (scenario==2)
            Require(timing.t_first_inverse_start_ms < timing.t_final_index_end_ms,
                "inverse did not overlap unrelated GPU index");
        gpu.ReleaseOutput(t);
    }
    gpu.SetTestDelayHook(nullptr);
    gpu.SetSchedulingPolicy(Phase2ESchedulingPolicy::PolicyA_LargestFirst);
    std::cout<<"[PASS] 5 artificial reordering schedules\n"<<std::flush;
    // Corrupt only sized payloads, leaving tag/container framing valid.
    auto corrupt=samples[2].compressed;
    std::vector<GpuEntropyBandDescriptor> descriptors;
    Require(ParseCineFormSample(corrupt.data(),corrupt.size(),24,descriptors),"malformed parse");
    auto d=descriptors[7];
    std::memset(corrupt.data()+d.payload_offset,0,d.payload_bytes);
    auto failed=gpu.QueueDecode(corrupt.data(),corrupt.size());
    Require(failed && !gpu.WaitForDecode(failed) && gpu.PollDecode(failed)==DecodeTicketStatus::Failed,"device error propagation");
    DeviceBgraSurfaceDescriptor surface;
    Require(!gpu.GetDeviceBgraOutput(failed,&surface),"failed output published");
    std::vector<GpuCheckpointIndexResult> errors;
    Require(gpu.GetGpuIndexResults(failed,errors),"failure diagnostics");
    Require(gpu.ReleaseOutput(failed),"failed slot release");
    auto reclaimed=Decode(gpu,samples[2]); gpu.ReleaseOutput(reclaimed);
    auto early=gpu.QueueDecode(samples[2].compressed.data(),samples[2].compressed.size());
    Require(early && gpu.ReleaseOutput(early),"pending slot drain");
    std::cout<<"[PASS] device failure, no valid output, slot reclamation, pending release\n"<<std::flush;
    if(no_stress) return 0;
    for(int which=0;which<3;++which) {
        const int w=which==0?1920:which==1?3840:7680, h=which==0?1080:which==1?2160:4320;
        const int runs=sanitizer_stress?2:(which==0?200:which==1?100:20), pattern=which==2?0:2;
        auto all=Generate(w,h,which==2?1:5); auto& s=all[pattern];
        Require(gpu.Initialize(w,h,1,10,1,4),"stress init");
        gpu.SetPipelineMode(Phase2EPipelineMode::Phase2FAGpuIndexBatch);
        std::vector<uint8_t> expected,actual(w*h*4);
        std::vector<GpuEntropySegmentDescriptor> expected_cp,cp;
        for(int i=0;i<runs;++i) {
            auto t=Decode(gpu,s);
            Require(gpu.DownloadBgraToHost(t,actual.data(),w*4) && gpu.GetGpuCheckpoints(t,cp),"stress diagnostics");
            if(!i) { expected=actual; expected_cp=cp; }
            Require(actual==expected && Equal(cp,expected_cp),"stress mismatch");
            gpu.ReleaseOutput(t);
        }
        std::cout<<"[PASS] "<<w<<" stress "<<runs<<"/"<<runs<<" output and checkpoints exact\n"<<std::flush;
    }
    return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
