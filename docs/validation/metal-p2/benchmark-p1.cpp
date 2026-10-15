// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "tests/metal_audit.h"
#include "tests/reference_frame_generator.h"
#include <chrono>
#include <iomanip>
#include <sys/resource.h>
#include <CommonCrypto/CommonDigest.h>
#include <sstream>
using namespace hypercine::standalone;
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
class CpuMeter final : public detail::Frontend {
public:
 double conversion_ms=0,wavelet_ms=0;
 Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {
  auto t=Clock::now();auto out=ConvertFilmScan3Bgra(p,n,w,h,s);conversion_ms+=ms(t);return out;
 }
 WaveletLevel Forward(const Plane& p,int s) override {auto t=Clock::now();auto out=ForwardLevel(p,s);wavelet_ms+=ms(t);return out;}
 std::string DeviceName() const override {return "CPU scalar measured";}
};
std::string sha256(const uint8_t* p,std::size_t n) {
 unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(p,static_cast<CC_LONG>(n),digest);
 std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto v:digest)s<<std::setw(2)<<unsigned(v);return s.str();
}
int main(){try{
 std::cout<<std::setprecision(10)<<"{\"schema\":1,\"runs\":[";bool first=true;
 AlignedVector<uint8_t> input(1920*1080*4);
 for(int scenario=0;scenario<5;++scenario){
  FilmScan3Encoder cpu,metal;auto gpu=detail::MetalFrontend();detail::FrontendAccess::Use(metal,gpu);
  auto cpu_meter=std::make_shared<CpuMeter>();detail::FrontendAccess::Use(cpu,cpu_meter);
  for(int i=0;i<(scenario==4?120:scenario==3?16:32);++i){
   int p=scenario<3?(scenario==0?1:scenario==1?3:7):i%11;
   reference_fixture::GeneratePhase3AFrame(1920,1080,scenario<3?7:i,static_cast<reference_fixture::Phase3APattern>(p),input.data(),7680,0x1337beef);
   FilmScan3Frame a,b;double ct,mt;auto before=detail::ReadMetalTiming(*gpu);double cpu_color_before=cpu_meter->conversion_ms,cpu_wave_before=cpu_meter->wavelet_ms;
   auto c=[&]{auto t=Clock::now();a=cpu.Encode(input.data(),input.size(),7680);ct=ms(t);};
   auto m=[&]{auto t=Clock::now();b=metal.Encode(input.data(),input.size(),7680);mt=ms(t);};
   if(i%2){m();c();}else{c();m();}
   metal_test::EqualPacket(a.sample,b.sample);auto after=detail::ReadMetalTiming(*gpu);
   if(!first)std::cout<<',';first=false;
   std::cout<<"{\"device\":\""<<gpu->DeviceName()<<"\",\"scenario\":"<<scenario<<",\"frame\":"<<i<<",\"cpu_ms\":"<<ct<<",\"metal_ms\":"<<mt
    <<",\"input_sha256\":\""<<sha256(input.data(),input.size())<<"\",\"packet_sha256\":\""<<sha256(b.sample.data(),b.sample.size())<<"\",\"sample_bytes\":"<<b.sample.size()<<",\"limiter\":"<<b.state.fs_rate_limiter
    <<",\"upload_ms\":"<<after.upload_ms-before.upload_ms<<",\"readback_ms\":"<<after.readback_ms-before.readback_ms
    <<",\"submit_ms\":"<<after.submit_ms-before.submit_ms<<",\"wait_ms\":"<<after.wait_ms-before.wait_ms
    <<",\"color_gpu_ms\":"<<after.color_gpu_ms-before.color_gpu_ms<<",\"wavelet_gpu_ms\":"<<after.wavelet_gpu_ms-before.wavelet_gpu_ms
    <<",\"cpu_conversion_ms\":"<<cpu_meter->conversion_ms-cpu_color_before
    <<",\"cpu_wavelet_ms\":"<<cpu_meter->wavelet_ms-cpu_wave_before
    <<",\"host_remaining_ms\":"<<mt-(after.upload_ms-before.upload_ms)-(after.readback_ms-before.readback_ms)-(after.submit_ms-before.submit_ms)-(after.wait_ms-before.wait_ms)
    <<",\"peak_buffer_bytes\":"<<after.peak_buffer_bytes<<",\"device_allocated_bytes\":"<<after.peak_device_allocated_bytes<<"}";
  }
 }
 rusage u{};getrusage(RUSAGE_SELF,&u);std::cout<<"],\"peak_rss_bytes\":"<<u.ru_maxrss<<",\"packet_equality\":\"PASS\"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
