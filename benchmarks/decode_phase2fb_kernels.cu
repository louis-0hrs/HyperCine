// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_phase2fb_gpu.h"
#include <cuda_runtime.h>
#include "hypercine/decode_gpu_index.h"
#include <chrono>
#include <sstream>
namespace Phase2FB { namespace {
void Check(cudaError_t e) { if(e!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(e)); }
template<class T> struct Device {
 T* p=nullptr; explicit Device(size_t n) { Check(cudaMalloc(&p,n*sizeof(T))); }
 ~Device() { if(p) cudaFree(p); }
 Device(const Device&)=delete; Device& operator=(const Device&)=delete;
};
struct Events {
 cudaEvent_t a=nullptr,b=nullptr;
 Events() { Check(cudaEventCreate(&a)); try { Check(cudaEventCreate(&b)); } catch(...) { cudaEventDestroy(a); throw; } }
 ~Events() { cudaEventDestroy(a); cudaEventDestroy(b); }
 void Start() { Check(cudaEventRecord(a)); }
 double Stop() { Check(cudaEventRecord(b)); Check(cudaEventSynchronize(b)); float ms; Check(cudaEventElapsedTime(&ms,a,b)); return ms; }
};
__device__ unsigned GetNibble(const uint8_t* p,unsigned n) { return (n&1)?(p[n/2]&15):(p[n/2]>>4); }
__global__ void Summaries(const uint8_t* p,unsigned bytes,const GpuFsmEntry* t,unsigned states,
 unsigned chunk,unsigned count,bool all,Summary* out) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x,work=count*(all?states:1);
 if(i>=work) return;
 unsigned c=all?i/states:i,state=all?i%states:0;
 unsigned begin=c*chunk*2,end=min(bytes*2,begin+chunk*2);
 Summary r{0,uint16_t(state),65535};
 for(unsigned n=begin;n<end;++n) {
  auto e=t[r.state*16+GetNibble(p,n)];
  if(e.value0==Marker) { r.terminal=uint16_t(n-begin); break; }
  r.advance+=(e.pre_post_skip&511)+((e.pre_post_skip>>12)&7); r.state=e.next_state;
 }
 out[i]=r;
}
__global__ void Replay(const uint8_t* p,const GpuFsmEntry* t,const Restart* cp,unsigned count,
 unsigned marker,unsigned cap,int quant,int16_t* output,unsigned* error) {
 unsigned c=blockIdx.x*blockDim.x+threadIdx.x;
 if(c>=count) return;
 unsigned end=c+1<count?cp[c+1].nibble:marker;
 uint64_t pos=cp[c].position; unsigned state=cp[c].state;
 for(unsigned n=cp[c].nibble;n<end;++n) {
  auto e=t[state*16+GetNibble(p,n)];
  unsigned pre=e.pre_post_skip&511,post=(e.pre_post_skip>>12)&7;
  if(e.value0==Marker) { atomicExch(error,1); return; }
  pos+=pre;
  if((e.value0&&pos>=cap)||(e.value1&&pos+1>=cap)||pos+post>uint64_t(cap)+8) { atomicExch(error,2); return; }
  if(e.value0) output[pos]=int16_t(int(e.value0)*quant);
  if(e.value1) output[pos+1]=int16_t(int(e.value1)*quant);
  pos+=post; state=e.next_state;
 }
 if(c+1<count&&(pos!=cp[c+1].position||state!=cp[c+1].state)) atomicExch(error,3);
}
__global__ void Difference(int16_t* p,int w,int h) {
 unsigned y=blockIdx.x*blockDim.x+threadIdx.x; if(y>=unsigned(h)) return;
 for(int x=1;x<w;++x) p[y*w+x]=int16_t(int(p[y*w+x])+p[y*w+x-1]);
}
}
Measurement RunGpu(const uint8_t* p,uint32_t bytes,const GpuFsmEntry* t,unsigned states,
 uint16_t initial,uint32_t cap,int quant,bool diff,int w,int h,uint32_t chunk) {
 Validate(p,bytes,t,states,initial,cap);
 if(!chunk||chunk>1024||w<=0||h<=0||uint64_t(w)*h!=cap||quant<1||quant>65535)
  throw std::runtime_error("unsupported research geometry/quantizer/chunk");
 size_t count=(bytes+chunk-1)/chunk; Measurement r;
 r.scratch=count*(2*sizeof(Summary)+2*sizeof(Restart))+states*16*sizeof(GpuFsmEntry)+8;
 if(r.scratch>ScratchCeiling) throw std::runtime_error("scratch ceiling exceeded");
 Device<uint8_t> input(bytes); Device<GpuFsmEntry> table(states*16);
 Device<Summary> summaries(count); Device<Restart> cp(count); Device<int16_t> output(cap); Device<unsigned> error(1);
 Check(cudaMemcpy(input.p,p,bytes,cudaMemcpyHostToDevice));
 Check(cudaMemcpy(table.p,t,states*16*sizeof(GpuFsmEntry),cudaMemcpyHostToDevice));
 Events ev; std::vector<Summary> host(count);
 auto start=std::chrono::steady_clock::now();
 ev.Start(); Summaries<<<unsigned((count+127)/128),128>>>(input.p,bytes,table.p,states,chunk,unsigned(count),false,summaries.p);
 Check(cudaGetLastError()); Check(cudaMemcpy(host.data(),summaries.p,count*sizeof(Summary),cudaMemcpyDeviceToHost));
 r.gpu_pre_ms=ev.Stop();
 auto resolve=std::chrono::steady_clock::now(); r.resolved=Resolve(p,bytes,t,initial,cap,chunk,host);
 r.cpu_resolve_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-resolve).count();
 ev.Start(); Check(cudaMemcpy(cp.p,r.resolved.checkpoints.data(),r.resolved.checkpoints.size()*sizeof(Restart),cudaMemcpyHostToDevice));
 r.checkpoint_transfer_ms=ev.Stop();
 r.ready_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 ev.Start(); Check(cudaMemset(output.p,0,cap*2)); Check(cudaMemset(error.p,0,4));
 Replay<<<unsigned((r.resolved.checkpoints.size()+127)/128),128>>>(input.p,table.p,cp.p,unsigned(r.resolved.checkpoints.size()),r.resolved.marker,cap,quant,output.p,error.p);
 Check(cudaGetLastError()); if(diff) { Difference<<<(h+127)/128,128>>>(output.p,w,h); Check(cudaGetLastError()); }
 r.replay_ms=ev.Stop();
 unsigned fault; Check(cudaMemcpy(&fault,error.p,4,cudaMemcpyDeviceToHost));
 if(fault) throw std::runtime_error("exact replay validation failed: "+std::to_string(fault));
 r.total_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 r.coefficients.resize(cap); Check(cudaMemcpy(r.coefficients.data(),output.p,cap*2,cudaMemcpyDeviceToHost)); return r;
}
double SummaryPilotGpu(const uint8_t* p,uint32_t bytes,const GpuFsmEntry* t,unsigned states,uint32_t chunk,unsigned count) {
 count=std::min(count,(bytes+chunk-1)/chunk);
 Device<uint8_t> input(bytes); Device<GpuFsmEntry> table(states*16); Device<Summary> out(size_t(count)*states);
 Check(cudaMemcpy(input.p,p,bytes,cudaMemcpyHostToDevice)); Check(cudaMemcpy(table.p,t,states*16*sizeof(GpuFsmEntry),cudaMemcpyHostToDevice));
 Events ev; ev.Start(); Summaries<<<(count*states+127)/128,128>>>(input.p,bytes,table.p,states,chunk,count,true,out.p);
 Check(cudaGetLastError());double ms=ev.Stop();
 unsigned checks=std::min(count,64u);
 std::vector<Summary> verify(states);
 for(unsigned sample=0;sample<checks;++sample) {
  unsigned c=checks==1?0:sample*(count-1)/(checks-1);
  Check(cudaMemcpy(verify.data(),out.p+size_t(c)*states,states*sizeof(Summary),cudaMemcpyDeviceToHost));
  for(unsigned state=0;state<states;++state) {
   auto expected=Scan(p,c*chunk*2,std::min(bytes*2,(c+1)*chunk*2),uint16_t(state),t);
   auto actual=verify[state];
   if(expected.advance!=actual.advance||expected.state!=actual.state||expected.terminal!=actual.terminal)
    throw std::runtime_error("full-state pilot summary mismatch");
  }
 }return ms;
}
}

namespace Phase2FB {
BaselineMeasurement Baselines(const uint8_t* p,uint32_t bytes,const std::vector<HyperCine::GpuFsmEntry>& tables,
 const HyperCine::GpuEntropyBandDescriptor& original,bool fa) {
 using namespace HyperCine;
 Device<uint8_t> input(bytes); Device<GpuFsmEntry> table(tables.size());
 Device<int16_t> output(original.coefficient_count);
 Device<GpuEntropyBandDescriptor> desc(1); Device<GpuEntropySegmentDescriptor> cp(64);
 Device<GpuSegmentDecodeResult> results(64); Device<GpuCheckpointIndexResult> index(1); Device<unsigned> fault(1);
 auto d=original;d.payload_offset=0;d.d_destination=output.p;
 Check(cudaMemcpy(input.p,p,bytes,cudaMemcpyHostToDevice));
 Check(cudaMemcpy(table.p,tables.data(),tables.size()*sizeof(GpuFsmEntry),cudaMemcpyHostToDevice));
 Check(cudaMemcpy(desc.p,&d,sizeof(d),cudaMemcpyHostToDevice));
 std::vector<GpuEntropySegmentDescriptor> host;
 if(!GenerateSingleBandCheckpointsCpu(p,bytes,d,0,tables.data(),tables.size(),GpuEntropyMode::ParallelAdaptive,64,host))
  throw std::runtime_error("baseline checkpoints");
 Check(cudaMemcpy(cp.p,host.data(),host.size()*sizeof(host[0]),cudaMemcpyHostToDevice));
 Events ev;BaselineMeasurement out;
 for(int run=0;run<4;++run) {
  ev.Start();
  Check(LaunchZeroVlcBandsGpu(desc.p,1,nullptr));
  Check(LaunchDecodeEntropyParallelGpu(input.p,bytes,desc.p,1,cp.p,int(host.size()),table.p,results.p,nullptr));
  Check(LaunchCooperativeDifferenceCodingGpu(desc.p,1,nullptr));
  double ms=ev.Stop();if(run) out.replay_ms+=ms/3;
 }
 std::vector<GpuSegmentDecodeResult> report(host.size());
 Check(cudaMemcpy(report.data(),results.p,report.size()*sizeof(report[0]),cudaMemcpyDeviceToHost));
 for(auto& r:report) if(r.status!=GpuBandDecodeStatus::Complete) throw std::runtime_error("baseline replay status");
 if(fa) {
  Check(cudaMemset(index.p,0,sizeof(GpuCheckpointIndexResult)));Check(cudaMemset(fault.p,0,4));
  ev.Start();
  Check(LaunchGenerateBandCheckpointsGpu(input.p,bytes,desc.p,0,table.p,tables.size(),GpuEntropyMode::ParallelAdaptive,64,
   cp.p,64,index.p,fault.p,bytes,nullptr));out.gpu_index_ms=ev.Stop();
  GpuCheckpointIndexResult r;Check(cudaMemcpy(&r,index.p,sizeof(r),cudaMemcpyDeviceToHost));
  if(r.status!=GpuBandDecodeStatus::Complete||!r.finished||r.segment_count!=host.size()) throw std::runtime_error("FA index status");
  out.max_index_launch_ns=r.max_launch_ns;
  std::vector<GpuEntropySegmentDescriptor> check(host.size());Check(cudaMemcpy(check.data(),cp.p,check.size()*sizeof(check[0]),cudaMemcpyDeviceToHost));
  for(size_t i=0;i<host.size();++i) {
   auto a=host[i],b=check[i];
   if(a.byte_offset!=b.byte_offset||a.nibble_idx!=b.nibble_idx||a.initial_state!=b.initial_state||
     a.out_coeff_pos!=b.out_coeff_pos||a.end_byte_offset!=b.end_byte_offset||a.end_nibble_idx!=b.end_nibble_idx||a.max_coeff_pos!=b.max_coeff_pos)
    throw std::runtime_error("FA baseline exactness");
  }
 }
 return out;
}
}

namespace Phase2FB {
std::string DeviceDiagnostics() {
 cudaDeviceProp p{};int device=0,driver=0,runtime=0;
 Check(cudaGetDevice(&device));Check(cudaGetDeviceProperties(&p,device));
 Check(cudaDriverGetVersion(&driver));Check(cudaRuntimeGetVersion(&runtime));
 std::ostringstream o;
 o<<"{\"name\":\""<<p.name<<"\",\"sm_count\":"<<p.multiProcessorCount<<",\"compute_major\":"<<p.major
  <<",\"compute_minor\":"<<p.minor<<",\"driver_api\":"<<driver<<",\"runtime_api\":"<<runtime<<",\"kernels\":[";
 cudaFuncAttributes a{},b{};int blocks_a=0,blocks_b=0;
 Check(cudaFuncGetAttributes(&a,Summaries));Check(cudaFuncGetAttributes(&b,Replay));
 Check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks_a,Summaries,128,0));
 Check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks_b,Replay,128,0));
 for(int i=0;i<2;++i) { if(i) o<<',';auto f=i?b:a;int blocks=i?blocks_b:blocks_a;
  o<<"{\"name\":\""<<(i?"Replay":"Summaries")<<"\",\"registers_per_thread\":"<<f.numRegs
   <<",\"static_shared_bytes\":"<<f.sharedSizeBytes<<",\"local_bytes_per_thread\":"<<f.localSizeBytes
   <<",\"active_blocks_per_sm_limit\":"<<blocks<<",\"theoretical_thread_occupancy\":"<<double(blocks*128)/p.maxThreadsPerMultiProcessor<<'}';
 } o<<"]}";return o.str();
}
}
