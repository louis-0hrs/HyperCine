// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../tests/metal_audit.h"
#include "../tests/reference_frame_generator.h"
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
 double conversion_ms=0,wavelet_ms=0;detail::HostTiming host;
 // The geometry must travel with the meter. Without this override the encoder
 // falls back to the default 1920x1080 while the Metal side encodes the real
 // geometry, and the packet comparison then fails with a size mismatch that
 // looks like an encoder bug rather than a benchmark bug.
 FilmScan3Geometry geometry{};
 FilmScan3Geometry Geometry() const noexcept override { return geometry; }
 Planes Convert(const uint8_t* p,std::size_t n,int w,int h,std::size_t s) override {
  auto t=Clock::now();auto out=ConvertFilmScan3Bgra(p,n,w,h,s);conversion_ms+=ms(t);return out;
 }
 WaveletLevel Forward(const Plane& p,int s) override {auto t=Clock::now();auto out=ForwardLevel(p,s);wavelet_ms+=ms(t);return out;}
 std::string DeviceName() const override {return "CPU scalar measured";}
 bool ProfileHost() const override {return true;}
 void RecordHostTiming(const detail::HostTiming& t) override {host=t;}
};
std::string sha256(const uint8_t* p,std::size_t n) {
 unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(p,static_cast<CC_LONG>(n),digest);
 std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto v:digest)s<<std::setw(2)<<unsigned(v);return s.str();
}
int main(){try{
 std::cout<<std::setprecision(10)<<"{\"schema\":1,\"runs\":[";bool first=true;
 // Two real geometries, not one relabelled. Scenarios 0-4 stay at 1080p so the
 // earlier numbers remain comparable; 5-8 are genuine 3840x2160 input with the
 // matching 15360-byte stride. A report that cannot tell these apart is how the
 // first version of this benchmark ended up calling 1080p timings 4K.
 AlignedVector<uint8_t> input_1080(1920*1080*4);
 AlignedVector<uint8_t> input_4k(3840*2160*4);
 for(int scenario=0;scenario<9;++scenario){
  const bool is_4k = scenario >= 5;
  const int gw = is_4k ? 3840 : 1920;
  const int gh = is_4k ? 2160 : 1080;
  const std::size_t gstride = static_cast<std::size_t>(gw) * 4;
  auto& input = is_4k ? input_4k : input_1080;
  FilmScan3Geometry geom; geom.width = gw; geom.height = gh;
  FilmScan3Encoder cpu{geom};
  FilmScan3Encoder metal;
  auto gpu=detail::MetalFrontend(geom);detail::FrontendAccess::Use(metal,gpu);
  auto cpu_meter=std::make_shared<CpuMeter>();
  cpu_meter->geometry = geom;
  detail::FrontendAccess::Use(cpu,cpu_meter);
  const int frames = (scenario==4?120 : scenario==3?16 : scenario==8?16 : 32);
  for(int i=0;i<frames;++i){
   const int p = scenario==0?1 : scenario==1?3 : scenario==2?7
               : scenario==5?1 : scenario==6?3 : scenario==7?7 : i%11;
   reference_fixture::GeneratePhase3AFrame(gw,gh,scenario<3?7:i,static_cast<reference_fixture::Phase3APattern>(p),input.data(),static_cast<int>(gstride),0x1337beef);
   FilmScan3Frame a,b;double ct,mt;auto before=detail::ReadMetalTiming(*gpu);double cpu_color_before=cpu_meter->conversion_ms,cpu_wave_before=cpu_meter->wavelet_ms;
   auto c=[&]{auto t=Clock::now();a=cpu.Encode(input.data(),input.size(),gstride);ct=ms(t);};
   auto m=[&]{auto t=Clock::now();b=metal.Encode(input.data(),input.size(),gstride);mt=ms(t);};
   if(i%2){m();c();}else{c();m();}
   metal_test::EqualPacket(a.sample,b.sample);auto after=detail::ReadMetalTiming(*gpu);
   if(!first)std::cout<<',';first=false;
   std::cout<<"{\"device\":\""<<gpu->DeviceName()<<"\",\"scenario\":"<<scenario<<",\"width\":"<<gw<<",\"height\":"<<gh<<",\"frame\":"<<i<<",\"cpu_ms\":"<<ct<<",\"metal_ms\":"<<mt
    <<",\"input_sha256\":\""<<sha256(input.data(),input.size())<<"\",\"packet_sha256\":\""<<sha256(b.sample.data(),b.sample.size())<<"\",\"sample_bytes\":"<<b.sample.size()<<",\"limiter\":"<<b.state.fs_rate_limiter
    <<",\"upload_ms\":"<<after.upload_ms-before.upload_ms<<",\"readback_ms\":"<<after.readback_ms-before.readback_ms
    <<",\"submit_ms\":"<<after.submit_ms-before.submit_ms<<",\"wait_ms\":"<<after.wait_ms-before.wait_ms
    <<",\"color_gpu_ms\":"<<after.color_gpu_ms-before.color_gpu_ms<<",\"wavelet_gpu_ms\":"<<after.wavelet_gpu_ms-before.wavelet_gpu_ms
    <<",\"batch_gpu_ms\":"<<after.batch_gpu_ms-before.batch_gpu_ms
    <<",\"allocation_ms\":"<<after.allocation_ms-before.allocation_ms
    <<",\"buffer_allocations\":"<<after.buffer_allocations-before.buffer_allocations
    <<",\"command_buffers\":"<<after.command_buffers-before.command_buffers
    <<",\"waits\":"<<after.waits-before.waits
    <<",\"upload_copies\":"<<after.upload_copies-before.upload_copies
    <<",\"readback_copies\":"<<after.readback_copies-before.readback_copies
    <<",\"uploaded_bytes\":"<<after.uploaded_bytes-before.uploaded_bytes
    <<",\"readback_bytes\":"<<after.readback_bytes-before.readback_bytes
    <<",\"cpu_conversion_ms\":"<<cpu_meter->conversion_ms-cpu_color_before
    <<",\"cpu_wavelet_ms\":"<<cpu_meter->wavelet_ms-cpu_wave_before
    <<",\"quant_ms\":"<<after.host.quant_ms-before.host.quant_ms
    <<",\"coefficient_hash_ms\":"<<after.host.coefficient_hash_ms-before.host.coefficient_hash_ms
    <<",\"entropy_ms\":"<<after.host.entropy_ms-before.host.entropy_ms
    <<",\"framing_ms\":"<<after.host.framing_ms-before.host.framing_ms
    <<",\"peak_buffer_bytes\":"<<after.peak_buffer_bytes<<",\"device_allocated_bytes\":"<<after.peak_device_allocated_bytes<<"}";
  }
 }
 rusage u{};getrusage(RUSAGE_SELF,&u);std::cout<<"],\"peak_rss_bytes\":"<<u.ru_maxrss<<",\"packet_equality\":\"PASS\"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
