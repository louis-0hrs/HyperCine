// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Complete SDK-free FilmScan3 CPU/CUDA packet parity gate.
#include "hypercine/filmscan3.h"
#include "filmscan3_backend.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef HYPERCINE_CUDA_ARCHITECTURES_STRING
#define HYPERCINE_CUDA_ARCHITECTURES_STRING "unknown"
#endif

namespace fs=std::filesystem;
using namespace hypercine::standalone;
using hypercine::standalone::detail::CudaTestMode;

namespace {
struct Options {
    fs::path output_dir;
    // Geometry of the comparison. 1920x1080 is the historical reference, but the
    // CUDA frontend is already geometry-capable, so the test has to be able to
    // prove other sizes instead of only the one it was originally written for.
    int width=1920, height=1080;
    // Number of frames to encode in one session. Above 1 this becomes an ordered
    // multi-frame test: same session, changing content, so rate-state and frame
    // counter are exercised instead of only the first frame.
    int frames=1;
    CudaTestMode mode=CudaTestMode::Normal;
    // Alters one pre-quantization coefficient in the GPU audit frame only. The
    // encoder path is untouched, so coefficient hashes and the complete packet
    // stay byte-equal: this isolates the stage check from the packet check.
    bool alter_pre_quantized_stage=false;
};

Options Parse(int argc,char** argv) {
    Options out;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--output-dir" && i+1<argc) out.output_dir=argv[++i];
        else if(arg=="--geometry" && i+1<argc) {
            const std::string value=argv[++i];
            const auto separator=value.find('x');
            if(separator==std::string::npos) throw std::invalid_argument("--geometry expects WIDTHxHEIGHT, got: "+value);
            const int width=std::stoi(value.substr(0,separator));
            const int height=std::stoi(value.substr(separator+1));
            if(width<=0 || height<=0) throw std::invalid_argument("--geometry needs positive dimensions, got: "+value);
            out.width=width; out.height=height;
        }
        else if(arg=="--frames" && i+1<argc) {
            const int frames=std::stoi(argv[++i]);
            if(frames<1) throw std::invalid_argument("--frames needs at least 1");
            out.frames=frames;
        }
        else if(arg=="--negative-control" && i+1<argc) {
            const std::string value=argv[++i];
            if(value=="skip-kernel") out.mode=CudaTestMode::SkipKernel;
            else if(value=="alter-output") out.mode=CudaTestMode::AlterOutput;
            else if(value=="alter-pre-quantized-stage") out.alter_pre_quantized_stage=true;
            else throw std::invalid_argument("unknown negative control: "+value);
        } else throw std::invalid_argument("usage: hypercine_cuda_frontend_compare [--output-dir DIR] [--geometry WxH] [--frames N] [--negative-control skip-kernel|alter-output|alter-pre-quantized-stage]");
    }
    return out;
}

void CudaCheck(cudaError_t error,const char* operation) {
    if(error!=cudaSuccess) throw std::runtime_error(std::string(operation)+": "+cudaGetErrorString(error));
}

AlignedVector<uint32_t> MakeInput(int width,int height,int frame=0) {
    AlignedVector<uint32_t> input(static_cast<std::size_t>(width)*height);
    // The seed moves with the frame index so consecutive frames carry genuinely
    // different content. Without that, a multi-frame test would only prove the
    // encoder is deterministic on identical input, which is a weaker claim.
    uint32_t state=0x33c0ffeeu ^ (static_cast<uint32_t>(frame)*0x9e3779b9u);
    const int shift=frame*7;
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        state=state*1664525u+1013904223u;
        const uint8_t texture=static_cast<uint8_t>(state>>24);
        const uint8_t r=static_cast<uint8_t>(((x+shift)*255/width + (texture&31))&255);
        const uint8_t g=static_cast<uint8_t>(((y+shift)*255/height + ((texture>>2)&31))&255);
        const uint8_t b=static_cast<uint8_t>(((x+y+2*shift)*127/(width+height) + ((texture>>4)&15))&255);
        input[static_cast<std::size_t>(y)*width+x]=(255u<<24)|(static_cast<uint32_t>(r)<<16)|
            (static_cast<uint32_t>(g)<<8)|b;
    }
    return input;
}

std::size_t FirstByteDifference(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b) {
    const auto common=std::min(a.size(),b.size());
    for(std::size_t i=0;i<common;++i) if(a[i]!=b[i]) return i;
    return a.size()==b.size() ? std::numeric_limits<std::size_t>::max() : common;
}

struct BandLocation { int channel,level,band; const char* stage; };
BandLocation LocateBand(std::size_t index) {
    const int channel=static_cast<int>(index/10);
    const int sub=static_cast<int>(index%10);
    if(sub==0) return {channel,3,0,"wavelet/L3-LL"};
    const int group=(sub-1)/3;
    return {channel,2-group,1+(sub-1)%3,"wavelet/highpass"};
}

struct StageDifference {
    bool found=false;
    int channel=0,level=0,band=0,x=0,y=0;
    std::size_t byte=0;
    int16_t cpu=0,gpu=0;
};
StageDifference ComparePlane(const Plane& cpu,const Plane& gpu,int channel,int level,int band) {
    if(cpu.width!=gpu.width || cpu.height!=gpu.height)
        return {true,channel,level,band,0,0,0,0,0};
    for(int y=0;y<cpu.height;++y) for(int x=0;x<cpu.width;++x) {
        const auto a=cpu.pixels[static_cast<std::size_t>(y)*cpu.stride+x];
        const auto b=gpu.pixels[static_cast<std::size_t>(y)*gpu.stride+x];
        if(a!=b) return {true,channel,level,band,x,y,
                         2*(static_cast<std::size_t>(y)*cpu.width+x),a,b};
    }
    return {};
}
StageDifference FirstStageDifference(const detail::FrontendFrame& cpu,const detail::FrontendFrame& gpu) {
    for(int channel=0;channel<3;++channel) {
        auto difference=ComparePlane(cpu.levels[channel][2].bands[0],gpu.levels[channel][2].bands[0],channel,3,0);
        if(difference.found) return difference;
        for(int level=2;level>=0;--level) for(int band=1;band<4;++band) {
            difference=ComparePlane(cpu.levels[channel][level].bands[band],gpu.levels[channel][level].bands[band],
                                    channel,level,band);
            if(difference.found) return difference;
        }
    }
    return {};
}

void WriteFile(const fs::path& path,const std::vector<uint8_t>& bytes) {
    std::ofstream stream(path,std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if(!stream) throw std::runtime_error("cannot write "+path.string());
}

void WriteTrace(const fs::path& path,const FilmScan3Frame& cpu,const FilmScan3Frame& gpu) {
    std::ofstream out(path);
    if(!out) throw std::runtime_error("cannot write "+path.string());
    out<<"{\n  \"cpu_bytes\": "<<cpu.sample.size()<<",\n  \"gpu_bytes\": "<<gpu.sample.size()<<",\n";
    for(const auto item : {std::pair<const char*,const FilmScan3Frame*>("cpu",&cpu),
                           std::pair<const char*,const FilmScan3Frame*>("gpu",&gpu)}) {
        out<<"  \""<<item.first<<"_coefficient_fnv64\": [";
        for(std::size_t i=0;i<item.second->coefficient_fnv64.size();++i) {
            if(i) out<<", ";
            out<<'"'<<std::hex<<std::setfill('0')<<std::setw(16)<<item.second->coefficient_fnv64[i]<<'"'<<std::dec;
        }
        out<<"]"<<(item.first==std::string("cpu") ? "," : "")<<"\n";
    }
    out<<"}\n";
    out.close();
    if(!out) throw std::runtime_error("cannot finalize "+path.string());
}

int Run(const Options& options) {
    int device_count=0;
    const auto count_status=cudaGetDeviceCount(&device_count);
    if(count_status!=cudaSuccess || device_count==0) {
        std::cerr<<"SKIP77: CUDA GPU required ("<<cudaGetErrorString(count_status)<<")\n";
        return 77;
    }
    cudaDeviceProp device{};
    CudaCheck(cudaGetDeviceProperties(&device,0),"cudaGetDeviceProperties");
    std::size_t free_bytes=0,total_bytes=0;
    CudaCheck(cudaMemGetInfo(&free_bytes,&total_bytes),"cudaMemGetInfo");
    int runtime=0,driver=0;
    CudaCheck(cudaRuntimeGetVersion(&runtime),"cudaRuntimeGetVersion");
    CudaCheck(cudaDriverGetVersion(&driver),"cudaDriverGetVersion");
    std::cout<<"GPU: "<<device.name<<" compute "<<device.major<<'.'<<device.minor<<"\n"
             <<"VRAM: "<<(free_bytes/(1024*1024))<<" MiB free / "<<(total_bytes/(1024*1024))<<" MiB total\n"
             <<"CUDA runtime/driver: "<<runtime<<'/'<<driver<<"\n"
             <<"CMAKE_CUDA_ARCHITECTURES="<<HYPERCINE_CUDA_ARCHITECTURES_STRING<<"\n"
             <<"Device native architecture: sm_"<<device.major<<device.minor<<"\n";

    const int width=options.width,height=options.height;
    auto cpu_frontend=detail::CpuFrontend({width,height,0});
    auto gpu_frontend=detail::CudaFrontend({width,height,0},options.mode);
    FilmScan3Encoder cpu({width,height,0});
    FilmScan3Encoder gpu({width,height,0});
    detail::FrontendAccess::Use(gpu,gpu_frontend);

    bool stages_equal=true,hashes_equal=true,packet_equal=true;
    bool state_equal=true;
    std::size_t encoded_bytes=0;
    std::size_t first_hash=0,first_byte=0;
    int first_bad_frame=-1;

    for(int frame=0;frame<options.frames;++frame) {
        auto input=MakeInput(width,height,frame);
        const auto* bytes=reinterpret_cast<const uint8_t*>(input.data());
        const auto byte_count=input.size()*sizeof(uint32_t);
        FilmScan3Metadata metadata;

        const auto cpu_audit=cpu_frontend->Transform(bytes,byte_count,width,height,width*4,true);
        auto gpu_audit=gpu_frontend->Transform(bytes,byte_count,width,height,width*4,true);
        if(options.alter_pre_quantized_stage) {
            // Flip one coefficient bit in a highpass band that the audit exposes.
            // Chosen so the value stays representable; quantization may well absorb
            // it, which is exactly why the stage check must be able to fail alone.
            auto& band=gpu_audit.levels[0][2].bands[3].pixels;
            if(!band.empty()) band[0]=static_cast<int16_t>(band[0]^0x0001);
        }
        const auto stage_difference=FirstStageDifference(cpu_audit,gpu_audit);

        const auto cpu_frame=cpu.Encode(bytes,byte_count,width*4,metadata);
        const auto gpu_frame=gpu.Encode(bytes,byte_count,width*4,metadata);

        std::size_t hash_index=cpu_frame.coefficient_fnv64.size();
        for(std::size_t i=0;i<cpu_frame.coefficient_fnv64.size();++i) {
            if(cpu_frame.coefficient_fnv64[i]!=gpu_frame.coefficient_fnv64[i]) {hash_index=i;break;}
        }
        const auto byte_index=FirstByteDifference(cpu_frame.sample,gpu_frame.sample);
        const bool frame_hashes=hash_index==cpu_frame.coefficient_fnv64.size();
        const bool frame_packet=byte_index==std::numeric_limits<std::size_t>::max();
        // Rate state and the frame counter must evolve identically in both
        // sessions. A differing limiter would encode later frames at a different
        // quality while the very first frame still matched.
        const bool frame_state=cpu_frame.state.frame_index==gpu_frame.state.frame_index &&
                               cpu_frame.state.fs_rate_limiter==gpu_frame.state.fs_rate_limiter;

        if(!stage_difference.found) {
            if(options.frames==1)
                std::cout<<"WAVELET_STAGES: PASS (all 30 logical coefficient bands byte-equal before quantization)\n";
        } else {
            stages_equal=false;
            if(first_bad_frame<0) first_bad_frame=frame;
            std::cerr<<"FIRST_STAGE_DIFFERENCE: frame="<<frame<<" stage=wavelet channel="<<stage_difference.channel
                     <<" level="<<stage_difference.level<<" band="<<stage_difference.band
                     <<" band_byte="<<stage_difference.byte<<" row="<<stage_difference.y
                     <<" column="<<stage_difference.x<<" cpu="<<stage_difference.cpu
                     <<" gpu="<<stage_difference.gpu<<"\n";
        }
        if(frame_hashes) {
            if(options.frames==1) std::cout<<"COEFFICIENT_HASHES: PASS (30/30)\n";
        } else {
            hashes_equal=false;
            if(first_bad_frame<0) first_bad_frame=frame;
            if(hashes_equal==false && first_hash==0) {first_hash=hash_index;}
            const auto where=LocateBand(hash_index);
            std::cerr<<"COEFFICIENT_HASHES: FAIL at frame "<<frame<<" index "<<hash_index
                     <<" stage="<<where.stage<<" channel="<<where.channel
                     <<" level="<<where.level<<" band="<<where.band
                     <<" cpu=0x"<<std::hex<<cpu_frame.coefficient_fnv64[hash_index]
                     <<" gpu=0x"<<gpu_frame.coefficient_fnv64[hash_index]<<std::dec<<"\n";
        }
        if(frame_packet) {
            if(options.frames==1)
                std::cout<<"FULL_PACKET: PASS ("<<cpu_frame.sample.size()<<" bytes byte-equal)\n";
        } else {
            packet_equal=false;
            if(first_bad_frame<0) first_bad_frame=frame;
            first_byte=byte_index;
            std::cerr<<"FULL_PACKET: FAIL at frame "<<frame<<" first_byte="<<byte_index
                     <<" cpu_size="<<cpu_frame.sample.size()<<" gpu_size="<<gpu_frame.sample.size();
            if(byte_index<std::min(cpu_frame.sample.size(),gpu_frame.sample.size()))
                std::cerr<<" cpu=0x"<<std::hex<<static_cast<unsigned>(cpu_frame.sample[byte_index])
                         <<" gpu=0x"<<static_cast<unsigned>(gpu_frame.sample[byte_index])<<std::dec;
            std::cerr<<"\n";
        }
        if(!frame_state) {
            state_equal=false;
            std::cerr<<"STATE_DIVERGENCE: frame="<<frame
                     <<" cpu_index="<<cpu_frame.state.frame_index
                     <<" gpu_index="<<gpu_frame.state.frame_index
                     <<" cpu_limiter="<<cpu_frame.state.fs_rate_limiter
                     <<" gpu_limiter="<<gpu_frame.state.fs_rate_limiter<<"\n";
        }

        encoded_bytes+=cpu_frame.sample.size();
        if(options.frames>1) {
            std::cout<<"FRAME "<<frame<<": bytes="<<cpu_frame.sample.size()
                     <<" limiter="<<cpu_frame.state.fs_rate_limiter
                     <<" index="<<cpu_frame.state.frame_index
                     <<(frame_hashes&&frame_packet&&frame_state&&!stage_difference.found?" PASS":" FAIL")<<"\n";
        }

        if(!options.output_dir.empty()) {
            fs::create_directories(options.output_dir);
            if(options.frames==1) {
                WriteFile(options.output_dir/"cpu.cfhd",cpu_frame.sample);
                WriteFile(options.output_dir/"gpu.cfhd",gpu_frame.sample);
                WriteTrace(options.output_dir/"trace.json",cpu_frame,gpu_frame);
            } else {
                std::ostringstream name;
                name<<"cpu.f"<<std::setw(3)<<std::setfill('0')<<frame<<".cfhd";
                WriteFile(options.output_dir/name.str(),cpu_frame.sample);
                std::ostringstream other;
                other<<"gpu.f"<<std::setw(3)<<std::setfill('0')<<frame<<".cfhd";
                WriteFile(options.output_dir/other.str(),gpu_frame.sample);
                // Per-frame trace, so a multi-frame run can be audited against the
                // encoder's own coefficient hashes instead of only this tool's
                // stdout. Without it a validator could confirm the packets decode
                // but would have no independent view of the 30 stage hashes.
                std::ostringstream trace_name;
                trace_name<<"trace.f"<<std::setw(3)<<std::setfill('0')<<frame<<".json";
                WriteTrace(options.output_dir/trace_name.str(),cpu_frame,gpu_frame);
            }
        }
    }

    if(options.frames>1) {
        std::cout<<"MULTIFRAME: "<<(stages_equal&&hashes_equal&&packet_equal&&state_equal?"PASS":"FAIL")
                 <<" ("<<options.frames<<" ordered frames, "<<encoded_bytes<<" bytes total"
                 <<", rate state and frame counter identical)" <<"\n";
    }

    if(options.mode!=CudaTestMode::Normal || options.alter_pre_quantized_stage) {
        if(!stages_equal || !hashes_equal || !packet_equal) {
            std::cerr<<"NEGATIVE_CONTROL: FIRED; deliberate "
                     <<(options.mode==CudaTestMode::SkipKernel ? "kernel skip (0x5A poison preserved)"
                        : options.alter_pre_quantized_stage ? "pre-quantized stage alteration"
                        : "GPU output alteration")
                     <<" was rejected\nFAIL\n";
            return 1;
        }
        std::cerr<<"NEGATIVE_CONTROL: DID NOT FIRE\n";
        return 0;
    }
    if(!stages_equal) {
        // Byte-equal hashes and packet do NOT excuse a proved difference in the
        // bands claimed bit-exact before quantization.
        std::cerr<<"FAIL: pre-quantized wavelet stage differs while the packet is equal\n";
        return 1;
    }
    if(!hashes_equal || !packet_equal || !state_equal) {std::cerr<<"FAIL\n";return 1;}
    std::cout<<"PASS: complete "<<width<<'x'<<height
             <<(options.frames>1?" x "+std::to_string(options.frames)+" frames":"")
             <<" FilmScan3 CPU/A2 CUDA sample\n";
    return 0;
}
} // namespace

int main(int argc,char** argv) {
    try { return Run(Parse(argc,argv)); }
    catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}
