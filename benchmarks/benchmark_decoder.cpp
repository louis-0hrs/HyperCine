// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../tests/decoder_support.h"
#include "hypercine/research/decode_phase2d.h"
#include "hypercine/research/decode_phase2e.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <vector>
using namespace ProductionDecodeTest;
namespace {
struct Stats { double mean=0, minimum=0, maximum=0, cpu=0; };
template<class Fn> Stats Measure(Fn&& fn,int count) {
    for(int i=0;i<5;++i) fn();
    Stats out; out.minimum=1e9; const double cpu0=CpuMs();
    for(int i=0;i<count;++i) { auto start=Clock::now(); fn();
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        out.mean+=ms; out.minimum=(std::min)(out.minimum,ms); out.maximum=(std::max)(out.maximum,ms); }
    out.mean/=count; out.cpu=(CpuMs()-cpu0)/count; return out;
}
void JsonStats(std::ostream& out,const Stats& s) {
    out << "{\"mean_ms\":" << s.mean << ",\"minimum_ms\":" << s.minimum << ",\"maximum_ms\":" << s.maximum << ",\"process_cpu_ms_per_frame\":" << s.cpu << '}';
}
struct Case {
    int w,h,p; DecodeFrameInfo info; Stats legacy_d,legacy_e,legacy_fc,light,hybrid,automatic;
    double oracle=0,selected=0,regret=0;
};
struct Mixed {
    int w,h,slots,frames; std::string label; double fps=0,mean=0,minimum=1e9,maximum=0,cpu=0;
    DecoderMemoryStats memory{}; uint64_t peak_ws=0; int light=0,hybrid=0;
};
Mixed Streaming(const std::vector<Sample>& samples,int slots,int frames,const char* label,
                DecodeSchedulingPolicy policy=DecodeSchedulingPolicy::Auto,bool noise_only=false) {
    const int w=samples[0].width,h=samples[0].height;
    DecoderConfig c; c.maximum_width=w; c.maximum_height=h; c.slot_count=slots; c.scheduling=policy;
    Decoder d(c); Require(d.Initialize()==DecodeError::None,"stream benchmark init");
    slots=d.SlotCount();
    // Warm each slot with each pattern while retaining the other slots.
    std::array<DecodeTicket,4> warm{};
    for(int p=0;p<5;++p) {
        for(int i=0;i<slots;++i) warm[size_t(i)]=Begin(d,samples[size_t(p)]);
        for(int i=0;i<slots;++i) d.ReleaseOutput(warm[size_t(i)]);
    }
    Mixed out{w,h,slots,frames,label}; out.memory=d.MemoryStats();
    struct Pending { DecodeTicket ticket{}; Clock::time_point start{}; };
    std::array<Pending,4> pending{}; int queued=0,retired=0;
    const double cpu0=CpuMs(); const auto start=Clock::now();
    while(retired<frames) {
        if(queued<frames && queued-retired<slots) {
            auto& entry=pending[size_t(queued%slots)]; entry.start=Clock::now();
            const auto& sample=samples[size_t(noise_only?2:queued%5)];
            entry.ticket=d.QueueDecode(sample.compressed.data(),sample.compressed.size());
            Require(bool(entry.ticket),"stream queue"); DecodeFrameInfo info;
            Require(d.GetFrameInfo(entry.ticket,&info),"stream decisions");
            if(info.strategy==DecodeStrategy::Lightweight) ++out.light; else ++out.hybrid;
            ++queued;
        } else {
            auto& entry=pending[size_t(retired%slots)]; Require(d.WaitForDecode(entry.ticket).state==DecodeState::Ready,"stream ready");
            const double ms=std::chrono::duration<double,std::milli>(Clock::now()-entry.start).count();
            out.mean+=ms; out.minimum=(std::min)(out.minimum,ms); out.maximum=(std::max)(out.maximum,ms);
            Require(d.ReleaseOutput(entry.ticket),"stream release"); ++retired;
        }
    }
    const double seconds=std::chrono::duration<double>(Clock::now()-start).count();
    out.fps=frames/seconds; out.mean/=frames; out.cpu=(CpuMs()-cpu0)/frames; out.peak_ws=WorkingSet(true);
    Require(d.MemoryStats().cuda_allocation_calls==out.memory.cuda_allocation_calls,"steady CUDA allocations");
    std::cout << "stream " << w << ' ' << label << " slots=" << slots << " fps=" << out.fps << " latency=" << out.mean << " CPU=" << out.cpu << '\n' << std::flush;
    return out;
}
void JsonMemory(std::ostream& out,const DecoderMemoryStats& m) {
    out << "{\"slots\":" << m.slot_count << ",\"device_bytes\":" << m.device_bytes << ",\"pinned_host_bytes\":" << m.pinned_host_bytes
        << ",\"pageable_owned_capacity_bytes\":" << m.pageable_capacity_bytes << ",\"immutable_device_bytes\":" << m.immutable_device_bytes
        << ",\"immutable_host_bytes\":" << m.immutable_host_bytes << ",\"cuda_owned_buffer_allocation_calls\":" << m.cuda_allocation_calls << '}';
}
void JsonMixed(std::ostream& out,const Mixed& m) {
    out << "{\"width\":" << m.w << ",\"height\":" << m.h << ",\"label\":\"" << m.label << "\",\"frames\":" << m.frames << ",\"slots\":" << m.slots
        << ",\"fps\":" << m.fps << ",\"megapixels_per_second\":" << m.fps*double(m.w)*m.h/1e6
        << ",\"mean_frame_latency_ms\":" << m.mean << ",\"minimum_frame_latency_ms\":" << m.minimum << ",\"maximum_frame_latency_ms\":" << m.maximum
        << ",\"process_cpu_ms_per_frame\":" << m.cpu << ",\"peak_process_working_set_bytes\":" << m.peak_ws
        << ",\"lightweight_decisions\":" << m.light << ",\"hybrid_decisions\":" << m.hybrid << ",\"memory\":";
    JsonMemory(out,m.memory); out << '}';
}
}
int main(int argc,char** argv) {
    try {
        const int count=argc>1?std::stoi(argv[1]):120;
        Require(count>=20,"benchmark minimum 20 frames");
        std::filesystem::create_directories("benchmarks/results");
        std::vector<Case> cases; std::vector<Mixed> mixed;
        cudaDeviceProp prop{}; GpuCheck(cudaGetDeviceProperties(&prop,0));
        for(int size=0;size<3;++size) {
            const int w=size==0?1920:size==1?3840:7680,h=size==0?1080:size==1?2160:4320;
            auto samples=Generate(w,h,size==2?1:5);
            {
                DecoderConfig c; c.maximum_width=w; c.maximum_height=h; c.slot_count=1;
                Decoder production(c); Require(production.Initialize()==DecodeError::None,"matrix production init");
                CudaDecoderPipelinePhase2D d; Require(d.Initialize(w,h,1,10,8),"reference D init"); d.SetSchedulingPolicy(HostIndexSchedulingPolicy::LargestFirst);
                CudaDecoderPipelinePhase2E e; Require(e.Initialize(w,h,1,10,8,4),"reference streaming init");
                for(const auto& s:samples) {
                    Case result{}; result.w=w; result.h=h; result.p=s.pattern;
                    const int runs=size==2?count/4:size==1?count/2:count;
                    result.legacy_d=Measure([&]{auto t=d.QueueDecode(s.compressed.data(),s.compressed.size());Require(t && d.WaitForDecode(t),"reference D");d.ReleaseOutput(t);},runs);
                    e.SetPipelineMode(Phase2EPipelineMode::Phase2EFullOverlap);
                    result.legacy_e=Measure([&]{auto t=e.QueueDecode(s.compressed.data(),s.compressed.size());Require(t && e.WaitForDecode(t),"reference E");e.ReleaseOutput(t);},runs);
                    e.SetPipelineMode(Phase2EPipelineMode::Phase2FCHybridFullFrame); e.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::AdaptiveDynamic);
                    result.legacy_fc=Measure([&]{auto t=e.QueueDecode(s.compressed.data(),s.compressed.size());Require(t && e.WaitForDecode(t),"reference hybrid");e.ReleaseOutput(t);},runs);
                    auto measure=[&](DiagnosticDecodeStrategy strategy){ DecodeRequest request;request.diagnostic_strategy=strategy;
                        return Measure([&]{auto t=Begin(production,s,request);production.ReleaseOutput(t);},runs); };
                    // Alternate which forced strategy runs first per case.
                    if(s.pattern%2) { result.hybrid=measure(DiagnosticDecodeStrategy::Hybrid); result.light=measure(DiagnosticDecodeStrategy::Lightweight); }
                    else { result.light=measure(DiagnosticDecodeStrategy::Lightweight); result.hybrid=measure(DiagnosticDecodeStrategy::Hybrid); }
                    result.automatic=measure(DiagnosticDecodeStrategy::Automatic);
                    auto t=Begin(production,s); Require(production.GetFrameInfo(t,&result.info),"classifier metadata"); production.ReleaseOutput(t);
                    result.oracle=(std::min)(result.light.mean,result.hybrid.mean);
                    result.selected=result.info.strategy==DecodeStrategy::Lightweight?result.light.mean:result.hybrid.mean;
                    result.regret=result.selected-result.oracle;
                    std::cout << w << ' ' << PatternName(s.pattern) << " D=" << result.legacy_d.mean << " E=" << result.legacy_e.mean << " FC=" << result.legacy_fc.mean
                        << " L=" << result.light.mean << " H=" << result.hybrid.mean << " Auto=" << result.automatic.mean << ' ' << StrategyName(result.info.strategy)
                        << " bytes=" << result.info.highpass_bytes << " coefficients=" << result.info.coefficient_count << " max=" << result.info.maximum_band_bytes
                        << " levels=" << result.info.level_payload_bytes[0] << ',' << result.info.level_payload_bytes[1] << ',' << result.info.level_payload_bytes[2] << '\n' << std::flush;
                    cases.push_back(result);
                }
            }
            if(size<2) {
                for(int slots:{2,3,4}) mixed.push_back(Streaming(samples,slots,1000,"equal_five_pattern"));
                mixed.push_back(Streaming(samples,0,1000,"low_latency",DecodeSchedulingPolicy::LowLatency));
                mixed.push_back(Streaming(samples,0,1000,"high_throughput",DecodeSchedulingPolicy::HighThroughput));
                if(size==1) mixed.push_back(Streaming(samples,3,1000,"noise_sustained",DecodeSchedulingPolicy::Auto,true));
            }
        }
        std::ofstream matrix("benchmarks/results/decode_phase3a_windows_msvc.json"), selector("benchmarks/results/decode_phase3a_auto_selector.json"),
            oracle("benchmarks/results/decode_phase3a_oracle.json"), mixed_json("benchmarks/results/decode_phase3a_mixed_workload.json"),memory("benchmarks/results/decode_phase3a_memory.json");
        for(auto* out:{&matrix,&selector,&oracle,&mixed_json,&memory}) *out<<std::setprecision(9);
        matrix << "{\"gpu\":\"" << prop.name << "\",\"seed\":322420463,\"timing\":\"host queue through ready and release; 5 warmups\",\"cases\":[\n";
        selector << "{\"version\":1,\"runtime_timing_inputs\":false,\"cases\":[\n";
        oracle << "{\"definition\":\"minimum same-session mean of forced production Lightweight and Adaptive Hybrid\",\"cases\":[\n";
        double sum_l=0,sum_h=0,sum_selected=0,sum_oracle=0,sum_auto=0,worst=0; int misses=0;
        for(size_t i=0;i<cases.size();++i) {
            const auto& c=cases[i]; if(i){matrix<<",\n";selector<<",\n";oracle<<",\n";}
            matrix<<"{\"width\":"<<c.w<<",\"height\":"<<c.h<<",\"pattern\":\""<<PatternName(c.p)<<"\",\"phase2d\":";JsonStats(matrix,c.legacy_d);
            matrix<<",\"phase2e\":";JsonStats(matrix,c.legacy_e);matrix<<",\"adaptive_reference\":";JsonStats(matrix,c.legacy_fc);
            matrix<<",\"lightweight\":";JsonStats(matrix,c.light);matrix<<",\"hybrid\":";JsonStats(matrix,c.hybrid);matrix<<",\"auto\":";JsonStats(matrix,c.automatic);
            matrix<<",\"auto_fps\":"<<1000/c.automatic.mean<<",\"auto_megapixels_per_second\":"<<double(c.w)*c.h/1000/c.automatic.mean<<'}';
            selector<<"{\"width\":"<<c.w<<",\"pattern\":\""<<PatternName(c.p)<<"\",\"decision\":\""<<StrategyName(c.info.strategy)
                <<"\",\"highpass_bytes\":"<<c.info.highpass_bytes<<",\"coefficient_count\":"<<c.info.coefficient_count<<",\"maximum_band_bytes\":"<<c.info.maximum_band_bytes
                <<",\"dense_band_count\":"<<c.info.dense_band_count<<",\"level_payload_bytes\":["<<c.info.level_payload_bytes[0]<<','<<c.info.level_payload_bytes[1]<<','<<c.info.level_payload_bytes[2]
                <<"],\"classifier_nanoseconds_observed\":"<<c.info.classifier_nanoseconds<<'}';
            oracle<<"{\"width\":"<<c.w<<",\"pattern\":\""<<PatternName(c.p)<<"\",\"oracle_ms\":"<<c.oracle<<",\"selected_strategy_ms\":"<<c.selected
                <<",\"regret_ms\":"<<c.regret<<",\"oracle_efficiency_percent\":"<<100*c.oracle/c.selected<<'}';
            sum_l+=c.light.mean;sum_h+=c.hybrid.mean;sum_selected+=c.selected;sum_oracle+=c.oracle;sum_auto+=c.automatic.mean;worst=(std::max)(worst,c.regret);
            if(c.regret>0)++misses;
        }
        const double fixed=(std::min)(sum_l,sum_h),denom=fixed-sum_oracle;
        matrix<<"]}\n";selector<<"]}\n";oracle<<"],\"mean_regret_ms\":"<<(sum_selected-sum_oracle)/cases.size()<<",\"worst_regret_ms\":"<<worst<<",\"misclassification_count\":"<<misses
            <<",\"capture_definition\":\"(best_fixed_strategy_sum - selected_strategy_sum) / (best_fixed_strategy_sum - oracle_sum)\",\"oracle_improvement_capture_percent\":"<<100*(fixed-sum_selected)/denom
            <<",\"auto_wall_improvement_capture_percent\":"<<100*(fixed-sum_auto)/denom<<",\"oracle_total_ms\":"<<sum_oracle<<",\"best_fixed_total_ms\":"<<fixed<<"}\n";
        mixed_json<<"{\"warmup\":\"every slot and all five patterns\",\"latency\":\"enqueue to host-observed ready; not inverse FPS\",\"runs\":[\n";
        memory<<"{\"definitions\":\"requested owned buffer bytes, actual pitches; pageable vector/object capacity excludes worker stacks, CUDA event/stream/context storage and allocator overhead; peak working set is process cumulative high-water\",\"steady_state_cuda_buffer_allocations_per_frame\":0,\"runs\":[\n";
        for(size_t i=0;i<mixed.size();++i){if(i){mixed_json<<",\n";memory<<",\n";}JsonMixed(mixed_json,mixed[i]);memory<<"{\"width\":"<<mixed[i].w<<",\"label\":\""<<mixed[i].label<<"\",\"owned\":";JsonMemory(memory,mixed[i].memory);memory<<'}';}
        mixed_json<<"]}\n";memory<<"]}\n";
        std::cout << "Oracle improvement capture=" << 100*(fixed-sum_selected)/denom << "%; worst regret=" << worst << " ms\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
