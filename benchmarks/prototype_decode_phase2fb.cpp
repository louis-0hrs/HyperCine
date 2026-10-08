// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_phase2fb_gpu.h"
#include "../tests/decode_phase0_support.h"
#undef min
#undef max
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <numeric>
namespace HyperCine { bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>&); }
using namespace HyperCine; using namespace Phase2FB; using namespace DecodePhase0;
namespace {
using Clock=std::chrono::steady_clock;
double Ms(Clock::time_point a) { return std::chrono::duration<double,std::milli>(Clock::now()-a).count(); }
void Need(bool b,const char* s) { if(!b) throw std::runtime_error(s); }
void Array(std::ostream& o,const std::vector<uint64_t>& v) { o<<'['; for(size_t i=0;i<v.size();++i) { if(i) o<<','; o<<v[i]; } o<<']'; }
size_t Offset(int c) { if(c<0||c>2) throw std::runtime_error("codebook"); return c==0?0:(c==1?2176:10464); }
unsigned States(int c) { return c==0?136:518; }
void Graph(std::ostream& o,const std::vector<GpuFsmEntry>& tables) {
 o<<"[";
 for(int code=0;code<3;++code) {
  if(code) o<<','; auto* t=tables.data()+Offset(code); auto count=States(code); auto reach=Reachable(t,count);
  std::vector<int> index(count,-1),low(count),stack; std::vector<bool> on(count,false); std::vector<uint64_t> scc,in(count),out(count);
  int serial=0,terminals=0,absorbing=0; std::set<std::string> rows,entries;
  std::function<void(int)> visit=[&](int s) {
   index[s]=low[s]=serial++; stack.push_back(s); on[s]=true;
   for(int n=0;n<16;++n) {
    auto& e=t[s*16+n]; if(e.value0==Marker) continue; int next=e.next_state;
    if(index[next]<0) { visit(next); low[s]=std::min(low[s],low[next]); }
    else if(on[next]) low[s]=std::min(low[s],index[next]);
   }
   if(low[s]==index[s]) { uint64_t size=0; int v; do { v=stack.back();stack.pop_back();on[v]=false;++size; } while(v!=s); scc.push_back(size); }
  }; visit(0);
  for(auto s:reach) {
   std::set<unsigned> neighbors;
   std::ostringstream row;
   for(int n=0;n<16;++n) { const auto& e=t[s*16+n];
    std::ostringstream atom;atom<<e.value0<<':'<<e.value1<<':'<<e.pre_post_skip<<':'<<e.next_state;entries.insert(atom.str());
    row<<e.value0<<':'<<e.value1<<':'<<e.pre_post_skip<<':'<<e.next_state<<';';
    if(e.value0==Marker) ++terminals; else neighbors.insert(e.next_state);
   }
   rows.insert(row.str()); if(neighbors.size()==1&&*neighbors.begin()==s) ++absorbing;out[s]=neighbors.size(); for(auto v:neighbors) ++in[v];
  }
  // Output-aware deterministic transducer partition refinement, not corpus pruning.
  std::vector<int> cls(count,0),next(count); int classes=1;
  for(unsigned round=0;round<count;++round) {
   std::map<std::string,int> keys;
   for(auto s:reach) { std::ostringstream key;
    for(int n=0;n<16;++n) { auto e=t[s*16+n]; key<<e.value0<<':'<<e.value1<<':'<<e.pre_post_skip<<':'<<(e.value0==Marker?-1:cls[e.next_state])<<';'; }
    auto k=key.str(); if(!keys.count(k)) keys[k]=int(keys.size()); next[s]=keys[k];
   }
   bool same=true; for(auto s:reach) if(cls[s]!=next[s]) same=false;
   cls=next; classes=int(keys.size()); if(same) break;
  }
  o<<"{\"codebook\":"<<code<<",\"theoretical_states\":"<<count<<",\"reachable_states\":"<<reach.size()
   <<",\"unique_transition_entries\":"<<entries.size()<<",\"unique_transition_rows\":"<<rows.size()<<",\"output_equivalence_classes\":"<<classes<<",\"terminal_edges\":"<<terminals
   <<",\"absorbing_nonterminal_states\":"<<absorbing<<",\"scc_sizes\":"; Array(o,scc); o<<",\"in_degree\":";Array(o,in);o<<",\"out_degree\":";Array(o,out);o<<'}';
 } o<<']';
}
struct Trace {
 std::vector<uint64_t> states,transitions,boundary_states,previous_transition_class;
 uint64_t nonzero=0,symbols=0,zeros=0,zero_sum=0,zero_max=0;
 uint32_t marker=0,position=0; uint16_t final_state=0; uint64_t hash=14695981039346656037ull;
};
Trace Reference(const uint8_t* p,uint32_t bytes,const GpuFsmEntry* t,unsigned states,uint16_t initial,uint32_t cap,
 const Resolved* candidate=nullptr) {
 Trace r; r.states.resize(states);r.transitions.resize(states*16);r.boundary_states.resize(states);r.previous_transition_class.resize(16);unsigned previous_class=0; uint16_t s=initial; uint64_t pos=0; size_t checkpoint=0;
 for(uint32_t n=0;n<bytes*2;++n) {
  if(n%512==0) { ++r.boundary_states[s];++r.previous_transition_class[previous_class]; }
  if(candidate&&checkpoint<candidate->checkpoints.size()&&candidate->checkpoints[checkpoint].nibble==n) {
   const auto& c=candidate->checkpoints[checkpoint++]; Need(c.state==s&&c.position==pos,"checkpoint state/position mismatch");
  }
  unsigned entry=s*16+Nibble(p,n); const auto& e=t[entry]; ++r.states[s]; ++r.transitions[entry];
  if(!candidate) for(uint64_t x:{uint64_t(n),pos,uint64_t(s),uint64_t(uint16_t(e.value0)),uint64_t(uint16_t(e.value1)),uint64_t(e.pre_post_skip),uint64_t(e.next_state)})
   for(int b=0;b<8;++b) { r.hash^=(x>>(b*8))&255;r.hash*=1099511628211ull; }
  if(e.value0==Marker) {
   r.marker=n;r.position=uint32_t(pos);r.final_state=s;
   if(candidate) Need(checkpoint==candidate->checkpoints.size()&&candidate->marker==n&&candidate->position==pos&&candidate->state==s,"termination mismatch");
   return r;
  }
  unsigned pre=e.pre_post_skip&511,post=(e.pre_post_skip>>12)&7,values=(e.value0!=0)+(e.value1!=0);
  r.nonzero+=values; r.symbols+=values;
  if(pre) { ++r.zeros;r.zero_sum+=pre;r.zero_max=std::max(r.zero_max,uint64_t(pre)); }
  if(post>values) { ++r.zeros;r.zero_sum+=post-values;r.zero_max=std::max(r.zero_max,uint64_t(post-values)); }
  if((e.value0&&pos+pre>=cap)||(e.value1&&pos+pre+1>=cap)) throw std::runtime_error("reference coefficient write overflow");
  previous_class=values+(pre?4:0)+(post>values?8:0);pos+=pre+post; CheckPosition(pos,cap);s=e.next_state;
 }
 throw std::runtime_error("reference missing marker");
}
void Coefficients(const Measurement& m,DECODER* native,const GpuEntropyBandDescriptor& d) {
 auto* wave=native->transform[d.channel]->wavelet[d.level]; const auto* values=reinterpret_cast<const int16_t*>(wave->band[d.band]);
 for(int y=0;y<d.height;++y) for(int x=0;x<d.width;++x)
  Need(m.coefficients[size_t(y)*d.width+x]==values[size_t(y)*(wave->pitch/2)+x],"upstream coefficient mismatch");
}
void Selective(std::ostream& o,const uint8_t* p,uint32_t bytes,const GpuFsmEntry* t,unsigned states,uint32_t chunk) {
 auto reachable=Reachable(t,states); unsigned count=(bytes+chunk-1)/chunk,samples=std::min(64u,count);
 o<<"{\"initial_width\":"<<reachable.size()<<",\"samples\":"<<samples<<",\"widths\":[";
 for(unsigned window:{2u,4u,8u,16u,32u,64u}) {
  if(window!=2) o<<','; uint64_t widths=0; unsigned largest=0; uint64_t classes=0;
  for(unsigned sample=0;sample<samples;++sample) {
   unsigned c=samples==1?0:sample*(count-1)/(samples-1),begin=c*chunk*2,end=std::min(bytes*2,begin+window);
   std::set<std::pair<uint16_t,uint16_t>> exits; std::set<std::tuple<uint16_t,uint16_t,uint32_t>> maps;
   for(auto state:reachable) { auto r=Scan(p,begin,end,state,t); exits.insert({r.state,r.terminal}); maps.insert({r.state,r.terminal,r.advance}); }
   widths+=exits.size();largest=std::max(largest,unsigned(exits.size()));classes+=maps.size();
  }
  o<<"{\"window_nibbles\":"<<window<<",\"mean_state_terminal_width\":"<<double(widths)/samples<<",\"max_width\":"<<largest
   <<",\"mean_state_terminal_advance_classes\":"<<double(classes)/samples<<'}';
 } o<<"]}";
}
}
int main(int argc,char** argv) { try {
 bool quick=argc>1&&std::string(argv[1])=="--quick";
 bool sanitizer=argc>1&&std::string(argv[1])=="--sanitizer-corpus";
 bool allstate=argc>1&&std::string(argv[1])=="--all-state-critical";
 if(argc>2||(argc==2&&!quick&&!sanitizer&&!allstate)) throw std::runtime_error("usage: prototype_decode_phase2fb [--quick|--sanitizer-corpus]");
 std::ifstream golden("tests/decode_phase0_golden.txt");Need(bool(golden),"run from repository root; frozen golden manifest missing");
 std::map<std::tuple<int,int,int>,std::pair<std::string,size_t>> manifest;
 std::string line;while(std::getline(golden,line)) { if(line.empty()||line[0]=='#') continue;
  std::istringstream row(line);int w,h,p;std::string in,hash;size_t bytes;row>>w>>h>>p>>in>>hash>>bytes;Need(bool(row),"golden manifest syntax");manifest[{w,h,p}]={hash,bytes};
 }
 std::vector<GpuFsmEntry> tables; Need(ExtractCineFormBaseFsmTables(tables),"extract tables");
 if(allstate) {
  auto samples=Generate(3840,2160);auto& sample=samples[2];
  auto frozen=manifest.at({3840,2160,2});Need(Hash(sample.compressed.data(),sample.compressed.size())==frozen.first,"full-state frozen fixture");
  std::vector<GpuEntropyBandDescriptor> parsed,vlc;Need(ParseCineFormSample(sample.compressed.data(),sample.compressed.size(),24,parsed),"parse");
  for(auto b:parsed) if(b.method==BandEncodingMethod::RunLengthVLC) vlc.push_back(b);
  std::cout<<std::setprecision(9)<<"{\"license\":\"MIT OR Apache-2.0\",\"bands\":[";
  for(int id:{6,7}) {
   if(id==7) std::cout<<',';auto d=vlc[id];auto* p=sample.compressed.data()+d.payload_offset;auto* t=tables.data()+Offset(d.codebook_id);auto states=States(d.codebook_id);
   uint32_t count=(d.payload_bytes+1023)/1024;
   size_t scratch=size_t(count)*states*sizeof(Summary)+states*(16*sizeof(GpuFsmEntry)+sizeof(Summary));
   Need(scratch<=ScratchCeiling,"full-state scratch bound");
   std::cout<<"{\"band_index\":"<<id<<",\"chunk_bytes\":1024,\"scratch_bytes\":"<<scratch<<",\"gpu_summary_ms\":[";
   for(int run=0;run<3;++run) { if(run) std::cout<<',';std::cout<<SummaryPilotGpu(p,d.payload_bytes,t,states,1024,count); }
   std::cout<<"],\"verification\":\"64 uniformly sampled chunks x all incoming states, state/advance/termination exact\",\"coefficient_replay_performed\":false}";
  }std::cout<<"]}\n";return 0;
 }
 std::cout<<std::setprecision(9)<<"{\"license\":\"MIT OR Apache-2.0\",\"base_commit\":\"773669c\",\"seed\":322420463,\"fsm_graph\":";
 Graph(std::cout,tables); std::cout<<",\"gpu\":"<<DeviceDiagnostics();std::cout<<",\"bands\":["; bool first=true;
 for(auto geometry: {std::pair<int,int>{1920,1080},{3840,2160},{7680,4320}}) {
  if(quick&&geometry.first!=3840) continue;
  auto samples=Generate(geometry.first,geometry.second,geometry.first==7680?1:5);
  for(auto& sample:samples) {
   if(quick&&sample.pattern!=2) continue;
   auto frozen=manifest.at({sample.width,sample.height,sample.pattern});
   Need(Hash(sample.compressed.data(),sample.compressed.size())==frozen.first&&sample.compressed.size()==frozen.second,"frozen compressed fixture mismatch");
   std::vector<GpuEntropyBandDescriptor> bands; std::string error;
   Need(ParseCineFormSample(sample.compressed.data(),sample.compressed.size(),24,bands,&error),error.c_str());
   ReferenceDecoder decoder(sample,CFHD_PIXEL_FORMAT_BGRA); decoder.SeedDither(); decoder.Decode(sample);
   std::vector<GpuEntropyBandDescriptor> vlc; for(const auto& b:bands) if(b.method==BandEncodingMethod::RunLengthVLC) vlc.push_back(b);
   std::vector<int> selected;
   // Noise: historical last-finishing critical band 6 and second-largest by payload (6).
   // Include largest band 7 explicitly as the alternate critical L0 orientation.
   if(sample.pattern==2) selected={6,7};
   else { int best=0; for(int i=0;i<int(vlc.size());++i) if(vlc[i].channel==0&&vlc[i].payload_bytes>vlc[best].payload_bytes) best=i; selected={best}; }
   for(int id:selected) {
    const auto& d=vlc[id]; auto* p=sample.compressed.data()+d.payload_offset; auto* t=tables.data()+Offset(d.codebook_id);auto states=States(d.codebook_id);
    Need(d.coefficient_count==size_t(d.width)*d.height&&!d.has_peaks,"research supports tightly pitched non-peak bands");
    Validate(p,d.payload_bytes,t,states,d.initial_fsm_state,uint32_t(d.coefficient_count));
    auto trace=Reference(p,d.payload_bytes,t,states,d.initial_fsm_state,uint32_t(d.coefficient_count));
    auto baseline=sanitizer?BaselineMeasurement{}:Baselines(p,d.payload_bytes,tables,d,sample.width==3840&&sample.pattern==2);
    double host=0; std::vector<GpuEntropySegmentDescriptor> reference;
    for(int repeat=0;repeat<5;++repeat) { auto start=Clock::now();
     Need(GenerateSingleBandCheckpointsCpu(sample.compressed.data(),sample.compressed.size(),d,id,tables.data(),tables.size(),GpuEntropyMode::ParallelAdaptive,64,reference),"E checkpoint reference"); host+=Ms(start)/5;
    }
    if(!first) std::cout<<','; first=false;
    std::cout<<"{\"width\":"<<sample.width<<",\"height\":"<<sample.height<<",\"pattern\":\""<<PatternName(sample.pattern)<<"\",\"band_index\":"<<id
     <<",\"channel\":"<<d.channel<<",\"level\":"<<d.level<<",\"orientation\":"<<d.band<<",\"payload_offset\":"<<d.payload_offset
     <<",\"compressed_bytes\":"<<d.payload_bytes<<",\"payload_hash\":\""<<Hash(p,d.payload_bytes)<<"\",\"sample_hash\":\""<<Hash(sample.compressed.data(),sample.compressed.size())
     <<"\",\"input_hash\":\""<<sample.input_hash<<"\",\"band_width\":"<<d.width<<",\"band_height\":"<<d.height<<",\"codebook\":"<<d.codebook_id
     <<",\"quantization\":"<<d.quantization<<",\"difference_coding\":"<<(d.difference_coding?"true":"false")<<",\"coefficient_count\":"<<d.coefficient_count
     <<",\"nibbles\":"<<uint64_t(d.payload_bytes)*2<<",\"nonzero_coefficients\":"<<trace.nonzero<<",\"emitted_values\":"<<trace.symbols
     <<",\"zero_run_count\":"<<trace.zeros<<",\"mean_zero_run\":"<<(trace.zeros?double(trace.zero_sum)/trace.zeros:0)<<",\"max_zero_run\":"<<trace.zero_max
     <<",\"marker_nibble\":"<<trace.marker<<",\"final_position\":"<<trace.position<<",\"trace_hash\":\""<<std::hex<<trace.hash<<std::dec<<"\",\"host_index_ms\":"<<host
     <<",\"phase2e_gpu_replay_ms\":"<<baseline.replay_ms<<",\"phase2fa_gpu_index_ms\":"<<baseline.gpu_index_ms<<",\"fa_max_index_launch_ns\":"<<baseline.max_index_launch_ns
     <<",\"reference_checkpoints\":"<<reference.size()<<",\"observed_states\":"<<std::count_if(trace.states.begin(),trace.states.end(),[](uint64_t n){return n!=0;})
     <<",\"state_histogram\":"; Array(std::cout,trace.states); std::cout<<",\"transition_histogram\":";Array(std::cout,trace.transitions);
    std::cout<<",\"boundary_256byte_state_histogram\":";Array(std::cout,trace.boundary_states);std::cout<<",\"previous_transition_class_histogram\":";Array(std::cout,trace.previous_transition_class);std::cout<<",\"boundary_nibble_alignment\":0";
    std::cout<<",\"reference_checkpoint_trace\":[";
    for(size_t ri=0;ri<reference.size();++ri) { if(ri) std::cout<<',';const auto& cp=reference[ri];
     std::cout<<"{\"nibble\":"<<cp.byte_offset*2+cp.nibble_idx<<",\"state\":"<<cp.initial_state<<",\"position\":"<<cp.out_coeff_pos
      <<",\"end_nibble\":"<<cp.end_byte_offset*2+cp.end_nibble_idx<<",\"capacity\":"<<cp.max_coeff_pos<<'}';
    }std::cout<<"]";
    std::cout<<",\"selective_states\":"; Selective(std::cout,p,d.payload_bytes,t,states,256);
    std::cout<<",\"chunk_sweep\":["; bool firstchunk=true;
    for(uint32_t chunk:{16u,32u,64u,128u,256u,512u,1024u}) {
     if(sanitizer&&chunk!=256) continue;
     if(!firstchunk) std::cout<<',';firstchunk=false;
     Measurement best; double avg=0,maximum=0,cpu_avg=0,pre_avg=0,replay_avg=0,transfer_avg=0,ready_avg=0; int repeats=sanitizer?1:(quick?2:5);
     auto warm=RunGpu(p,d.payload_bytes,t,states,d.initial_fsm_state,uint32_t(d.coefficient_count),d.quantization,d.difference_coding,d.width,d.height,chunk);
     for(int repeat=0;repeat<repeats;++repeat) {
      auto m=RunGpu(p,d.payload_bytes,t,states,d.initial_fsm_state,uint32_t(d.coefficient_count),d.quantization,d.difference_coding,d.width,d.height,chunk);
      Reference(p,d.payload_bytes,t,states,d.initial_fsm_state,uint32_t(d.coefficient_count),&m.resolved);Coefficients(m,decoder.GetNative(),d);
      avg+=m.total_ms/repeats;maximum=std::max(maximum,m.total_ms);cpu_avg+=m.cpu_resolve_ms/repeats;pre_avg+=m.gpu_pre_ms/repeats;replay_avg+=m.replay_ms/repeats;transfer_avg+=m.checkpoint_transfer_ms/repeats;ready_avg+=m.ready_ms/repeats;if(best.total_ms==0||m.total_ms<best.total_ms) best=std::move(m);
     }
     if(chunk==256) std::cerr<<sample.width<<' '<<PatternName(sample.pattern)<<" band "<<id<<" exact, avg "<<avg<<" ms; CPU "<<best.cpu_resolve_ms<<" ms\n";
     size_t chunks=(d.payload_bytes+chunk-1)/chunk,summary=chunks*states*sizeof(Summary);
     double pilot=sanitizer?0:SummaryPilotGpu(p,d.payload_bytes,t,states,chunk,64);
     std::cout<<"{\"chunk_bytes\":"<<chunk<<",\"chunks\":"<<chunks<<",\"average_total_ms\":"<<avg<<",\"best_total_ms\":"<<best.total_ms<<",\"maximum_total_ms\":"<<maximum<<",\"average_cpu_resolution_ms\":"<<cpu_avg<<",\"average_gpu_preprocessing_and_summary_d2h_ms\":"<<pre_avg<<",\"average_replay_ms\":"<<replay_avg<<",\"average_checkpoint_h2d_ms\":"<<transfer_avg<<",\"average_replay_ready_ms\":"<<ready_avg
      <<",\"gpu_preprocessing_and_summary_d2h_ms\":"<<best.gpu_pre_ms<<",\"cpu_state_resolution_ms\":"<<best.cpu_resolve_ms
      <<",\"checkpoint_h2d_ms\":"<<best.checkpoint_transfer_ms<<",\"gpu_replay_and_difference_ms\":"<<best.replay_ms
      <<",\"replay_ready_ms\":"<<best.ready_ms<<",\"coefficient_hash\":\""<<Hash(best.coefficients.data(),best.coefficients.size()*2)<<"\",\"scratch_bytes\":"<<best.scratch<<",\"correction_nibbles\":"<<best.resolved.corrections
      <<",\"max_correction_nibbles\":"<<best.resolved.longest<<",\"full_chunk_corrections\":"<<best.resolved.full_chunks
      <<",\"exact\":true,\"max_error\":0,\"all_state_summary_bytes\":"<<summary<<",\"all_state_pilot_chunks\":"<<std::min(size_t(64),chunks)
      <<",\"all_state_pilot_ms\":"<<pilot<<",\"all_state_linear_extrapolation_ms\":"<<pilot*chunks/std::min(size_t(64),chunks)<<'}';
    }
    std::cout<<"]}";
   }
  }
 }
 std::cout<<"]}\n";return 0;
 } catch(const std::exception& e) { std::cerr<<"Phase2FB failed: "<<e.what()<<'\n';return 1; }
}
