// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
#include <thread>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include <string>
#include <sstream>
#include <cuda_runtime.h>
#include "hypercine/decode_phase2e.h"
#include "hypercine/decode_phase2d.h"
#include "../tests/decode_phase0_support.h"
using namespace HyperCine;
using namespace DecodePhase0;
using Clock = std::chrono::steady_clock;
static void Require(bool x,const char* m) { if(!x) throw std::runtime_error(m); }
static double Ms(Clock::time_point a,Clock::time_point b) {
    return std::chrono::duration<double,std::milli>(b-a).count();
}
static double CpuMs() {
#ifdef _WIN32
    FILETIME created,exited,kernel,user;
    Require(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user),"CPU time");
    ULARGE_INTEGER k,u;
    k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
    return (k.QuadPart+u.QuadPart)*1e-4;
#else
    return 1000.0 * std::clock() / CLOCKS_PER_SEC;
#endif
}
struct Completion {
    Clock::time_point begin,ready;
    std::atomic<bool> done{false};
};
static void CUDART_CB Completed(void* data) {
    auto& c=*static_cast<Completion*>(data); c.ready=Clock::now(); c.done.store(true,std::memory_order_release);
}
struct Streams {
    std::array<cudaStream_t,4> s{};
    Streams() { for(auto& p:s) Require(cudaStreamCreateWithFlags(&p,cudaStreamNonBlocking)==cudaSuccess,"consumer stream"); }
    ~Streams() { for(auto p:s) { cudaStreamSynchronize(p); cudaStreamDestroy(p); } }
};
static bool first_row=true;
static void BeginRow(const std::string& type,const std::string& mode,int w,int h) {
    if(!first_row) std::cout<<",\n";
    first_row=false;
    std::cout<<"{\"type\":\""<<type<<"\",\"mode\":\""<<mode<<"\",\"width\":"<<w<<",\"height\":"<<h;
}
static int64_t UnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static void Metrics(int frames,double wall,double cpu,double avg,double lo,double hi,
                    int64_t begin_unix_ms,int64_t end_unix_ms) {
    const unsigned cores=std::max(1u,std::thread::hardware_concurrency());
    size_t peak=0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pm{}; GetProcessMemoryInfo(GetCurrentProcess(),&pm,sizeof(pm));
    peak=pm.PeakWorkingSetSize;
#endif
    size_t free,total; cudaMemGetInfo(&free,&total);
    std::cout<<",\"frames\":"<<frames<<",\"wall_ms\":"<<wall<<",\"fps\":"<<frames*1000/wall
        <<",\"latency_avg_ms\":"<<avg<<",\"latency_min_ms\":"<<lo<<",\"latency_max_ms\":"<<hi
        <<",\"decoder_cpu_ms\":"<<cpu<<",\"cpu_core_percent\":"<<100*cpu/wall
        <<",\"cpu_machine_percent\":"<<100*cpu/wall/cores
        <<",\"peak_working_set_bytes\":"<<peak
        <<",\"device_used_bytes\":"<<total-free
        <<",\"measurement_start_unix_ms\":"<<begin_unix_ms
        <<",\"measurement_end_unix_ms\":"<<end_unix_ms;
}
static void Stages(CudaDecoderPipelinePhase2D& p,uint64_t ticket,bool) {
    Phase2dSlotStageTimings t;
    Require(p.GetStageTimings(ticket,&t),"D timings");
    std::cout<<",\"host_index_wall_ms\":"<<t.host_index_ms
        <<",\"gpu_replay_wall_ms\":"<<t.gpu_vlc_ms<<",\"gpu_index_wall_ms\":0"
        <<",\"host_index_thread_cpu_ms\":null,\"peak_index_workers\":null";
}
static void Stages(CudaDecoderPipelinePhase2E& p,uint64_t ticket,bool timeline) {
    Phase2eSlotStageTimings t;
    Require(p.GetStageTimings(ticket,&t),"E/FA timings");
    std::cout<<",\"host_index_wall_ms\":"<<t.host_index_wall_ms
        <<",\"host_index_thread_cpu_ms\":"<<t.host_index_thread_cpu_ms
        <<",\"peak_index_workers\":"<<t.peak_index_workers
        <<",\"host_schedule_wall_ms\":"<<t.host_schedule_wall_ms
        <<",\"host_schedule_cpu_ms\":"<<t.host_schedule_cpu_ms
        <<",\"gpu_replay_wall_ms\":"<<t.gpu_replay_wall_ms
        <<",\"gpu_index_wall_ms\":"<<t.gpu_index_wall_ms
        <<",\"gpu_index_work_ms\":"<<t.gpu_index_work_ms
        <<",\"max_index_kernel_ms\":"<<t.max_index_kernel_ms;
    if(timeline) std::cout<<",\"timeline\":"<<p.ExportTimelineJson(ticket);
}
template<class Pipeline>
static void Patterns(Pipeline& p,const std::vector<Sample>& samples,const std::string& mode,int repeats) {
    for(size_t pattern=0;pattern<samples.size();++pattern) {
        const auto& s=samples[pattern];
        for(int i=0;i<2;++i) {
            auto t=p.QueueDecode(s.compressed.data(),s.compressed.size());
            Require(t && p.WaitForDecode(t),"warmup"); p.ReleaseOutput(t);
        }
        std::vector<double> latencies;
        double cpu_sum=0,queue_sum=0;
        uint64_t last=0;
        const auto measurement_begin=UnixMs();
        for(int i=0;i<repeats;++i) {
            double cpu0=CpuMs(); auto begin=Clock::now();
            auto t=p.QueueDecode(s.compressed.data(),s.compressed.size());
            auto queued=Clock::now();
            Require(t && p.WaitForDecode(t),"timed decode");
            auto end=Clock::now(); cpu_sum+=CpuMs()-cpu0;
            latencies.push_back(Ms(begin,end)); queue_sum+=Ms(begin,queued);
            if(i+1==repeats) last=t; else p.ReleaseOutput(t);
        }
        const auto measurement_end=UnixMs();
        double sum=0; for(auto v:latencies) sum+=v;
        BeginRow("pattern",mode,s.width,s.height);
        std::cout<<",\"pattern\":\""<<PatternName(int(pattern))<<"\",\"megapixels_per_sec\":"
            <<(s.width*s.height*1e-6)*repeats*1000/sum<<",\"queue_wall_avg_ms\":"<<queue_sum/repeats;
        Metrics(repeats,sum,cpu_sum,sum/repeats,*std::min_element(latencies.begin(),latencies.end()),*std::max_element(latencies.begin(),latencies.end()),measurement_begin,measurement_end);
        Stages(p,last,pattern==2);
        std::cout<<"}"; p.ReleaseOutput(last);
        std::cerr<<mode<<" "<<s.width<<" "<<PatternName(int(pattern))<<" "<<sum/repeats<<" ms\n";
    }
}
template<class Pipeline>
static void Slots(Pipeline& p,const std::vector<Sample>& samples,const std::string& mode,int slots,int cycles) {
    Streams completion_streams;
    std::array<Completion,4> completion;
    std::array<uint64_t,4> tickets{};
    for(auto& s:samples) {
        auto t=p.QueueDecode(s.compressed.data(),s.compressed.size());
        Require(t && p.WaitForDecode(t),"slot warmup"); p.ReleaseOutput(t);
    }
    const int frames=int(samples.size())*cycles;
    std::vector<double> latencies; latencies.reserve(frames);
    const auto measurement_begin=UnixMs();
    auto begin=Clock::now(); double cpu0=CpuMs();
    for(int i=0;i<frames+slots;++i) {
        int slot=i%slots;
        if(tickets[slot]) {
            Require(cudaStreamSynchronize(completion_streams.s[slot])==cudaSuccess,"consumer completion");
            Require(completion[slot].done.load(std::memory_order_acquire),"callback");
            Require(p.WaitForDecode(tickets[slot]),"slot wait");
            latencies.push_back(Ms(completion[slot].begin,completion[slot].ready));
            p.ReleaseOutput(tickets[slot]); tickets[slot]=0;
        }
        if(i<frames) {
            auto& s=samples[i%samples.size()];
            auto& c=completion[slot]; c.done=false; c.begin=Clock::now();
            auto t=p.QueueDecode(s.compressed.data(),s.compressed.size());
            Require(t && p.StreamWait(t,completion_streams.s[slot]),"slot queue/event");
            Require(cudaLaunchHostFunc(completion_streams.s[slot],Completed,&c)==cudaSuccess,"callback launch");
            tickets[slot]=t;
        }
    }
    auto end=Clock::now(); double cpu=CpuMs()-cpu0;
    const auto measurement_end=UnixMs();
    double sum=0; for(auto v:latencies) sum+=v;
    const auto& s=samples.front();
    BeginRow("slot_sweep",mode,s.width,s.height);
    std::cout<<",\"slots\":"<<slots<<",\"megapixels_per_sec\":"
        <<(s.width*s.height*1e-6)*frames*1000/Ms(begin,end);
    Metrics(frames,Ms(begin,end),cpu,sum/frames,*std::min_element(latencies.begin(),latencies.end()),*std::max_element(latencies.begin(),latencies.end()),measurement_begin,measurement_end);
    std::cout<<"}";
    std::cerr<<mode<<" slots="<<slots<<" "<<s.width<<" "<<frames*1000/Ms(begin,end)<<" FPS\n";
}
int main(int argc,char** argv) {
 try {
    int repeats=5,cycles=10;
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        if(a=="--repeats" && i+1<argc) repeats=std::stoi(argv[++i]);
        else if(a=="--cycles" && i+1<argc) cycles=std::stoi(argv[++i]);
        else throw std::runtime_error("unknown argument");
    }
    Require(repeats>0 && cycles>0,"positive repeats/cycles");
    cudaDeviceProp prop; Require(cudaGetDeviceProperties(&prop,0)==cudaSuccess,"device");
    int driver; cudaDriverGetVersion(&driver);
    std::cout<<std::setprecision(9)<<"{\"source_base\":\"3e24e7d\",\"gpu\":\""<<prop.name
        <<"\",\"cuda_driver_version\":"<<driver<<",\"repeats\":"<<repeats<<",\"slot_cycles\":"<<cycles
        <<",\"latency_method\":\"single: QueueDecode through WaitForDecode; slots: submit through consumer-stream ready callback; includes callback overhead\""
        <<",\"stage_clock\":\"CUDA events relative to compressed-copy stream frame-start event; CPU parse uses host clock; do not subtract unlike clock markers\""
        <<",\"checkpoint_bytes\":"<<sizeof(GpuEntropySegmentDescriptor)
        <<",\"index_result_bytes\":"<<sizeof(GpuCheckpointIndexResult)
        <<",\"band_descriptor_bytes\":"<<sizeof(GpuEntropyBandDescriptor)
        <<",\"rows\":[\n";
    for(int resolution=0;resolution<3;++resolution) {
        int w=resolution==0?1920:resolution==1?3840:7680;
        int h=resolution==0?1080:resolution==1?2160:4320;
        auto samples=Generate(w,h,resolution==2?1:5);
        CudaDecoderPipelinePhase2D d; CudaDecoderPipelinePhase2E e,fa;
        Require(d.Initialize(w,h,1,10,8),"D initialize");
        Require(e.Initialize(w,h,1,10,8,4) && fa.Initialize(w,h,1,10,1,4),"E/FA initialize");
        fa.SetPipelineMode(Phase2EPipelineMode::Phase2FAGpuIndexBatch);
        Patterns(d,samples,"Phase2DBarrier",repeats);
        Patterns(e,samples,"Phase2EHostStreaming",repeats);
        Patterns(fa,samples,"Phase2FAGpuIndexBatch",repeats);
        if(resolution<2) for(int slots=1;slots<=4;++slots) {
            Require(d.Initialize(w,h,slots,10,8) && e.Initialize(w,h,slots,10,8,4) &&
                fa.Initialize(w,h,slots,10,1,4),"slot initialize");
            fa.SetPipelineMode(Phase2EPipelineMode::Phase2FAGpuIndexBatch);
            Slots(d,samples,"Phase2DBarrier",slots,cycles);
            Slots(e,samples,"Phase2EHostStreaming",slots,cycles);
            Slots(fa,samples,"Phase2FAGpuIndexBatch",slots,cycles);
        }
        if(resolution==1) {
            // Shared bounded pools, preserving per-band index -> replay ordering.
            auto noise=std::vector<Sample>{samples[2]};
            for(int streams:{2,4,8}) {
                Require(fa.Initialize(w,h,1,10,1,streams),"stream sweep initialize");
                fa.SetPipelineMode(Phase2EPipelineMode::Phase2FAGpuIndexBatch);
                for(int i=0;i<2;++i) {
                    auto t=fa.QueueDecode(noise[0].compressed.data(),noise[0].compressed.size());
                    Require(t && fa.WaitForDecode(t),"stream warmup"); fa.ReleaseOutput(t);
                }
                for(int i=0;i<repeats;++i) {
                    const auto measurement_begin=UnixMs();
                    auto begin=Clock::now(); double cpu0=CpuMs();
                    auto t=fa.QueueDecode(noise[0].compressed.data(),noise[0].compressed.size());
                    Require(t && fa.WaitForDecode(t),"stream decode");
                    auto end=Clock::now(); double cpu=CpuMs()-cpu0;
                    BeginRow("shared_stream_sweep","Phase2FAGpuIndexBatch",w,h);
                    std::cout<<",\"streams\":"<<streams<<",\"repeat\":"<<i;
                    Metrics(1,Ms(begin,end),cpu,Ms(begin,end),Ms(begin,end),Ms(begin,end),measurement_begin,UnixMs());
                    Stages(fa,t,false); std::cout<<"}"; fa.ReleaseOutput(t);
                }
            }
        }
    }
    std::cout<<"\n]}\n";
    Require(bool(std::cout),"JSON output");
    return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
