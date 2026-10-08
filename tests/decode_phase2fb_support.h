// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/decode_entropy_cuda.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace Phase2FB {
using HyperCine::GpuFsmEntry;
constexpr uint16_t Marker=32767;
constexpr uint32_t MaxPayload=32u*1024*1024;
constexpr size_t ScratchCeiling=32u*1024*1024;
struct Summary { uint32_t advance; uint16_t state; uint16_t terminal; };
static_assert(sizeof(Summary)==8,"summary accounting");
struct Restart { uint32_t nibble,position; uint16_t state; uint16_t reserved=0; };
struct Resolved {
 std::vector<Restart> checkpoints;
 uint32_t marker=0,position=0,corrections=0,longest=0,full_chunks=0;
 uint16_t state=0;
};
inline uint8_t Nibble(const uint8_t* p,uint32_t n) { return (n&1)?(p[n/2]&15):(p[n/2]>>4); }
inline uint32_t Advance(const GpuFsmEntry& e) { return (e.pre_post_skip&511)+((e.pre_post_skip>>12)&7); }
inline void Validate(const uint8_t* p,size_t bytes,const GpuFsmEntry* t,unsigned states,unsigned initial,uint32_t cap) {
 if(!p||!t||!bytes||bytes>MaxPayload||!states||states>518||initial>=states||!cap||cap>UINT32_MAX-8)
  throw std::runtime_error("invalid research descriptor");
 for(unsigned i=0;i<states*16;++i) {
  const auto& e=t[i];
  if(e.value0!=Marker&&(e.next_state>=states||((e.pre_post_skip>>12)&7)<unsigned(e.value1?2:(e.value0?1:0))))
   throw std::runtime_error("invalid FSM transition / overlapping emission");
 }
}
inline Summary Scan(const uint8_t* p,uint32_t begin,uint32_t end,uint16_t state,const GpuFsmEntry* t) {
 Summary r{0,state,65535};
 if(end<begin||end-begin>=65535) throw std::runtime_error("chunk bound");
 for(uint32_t n=begin;n<end;++n) {
  const auto& e=t[r.state*16+Nibble(p,n)];
  if(e.value0==Marker) { r.terminal=uint16_t(n-begin); break; }
  r.advance+=Advance(e); r.state=e.next_state;
 }
 return r;
}
// Full-state summary composition retains position and absorbing termination.
// Used only by bounded research tests; a full critical-band matrix is rejected.
inline Summary Compose(const Summary& a,uint32_t left_nibbles,const std::vector<Summary>& right) {
 if(a.terminal!=65535) return a;
 if(a.state>=right.size()) throw std::runtime_error("composition state bound");
 auto b=right[a.state];uint64_t advance=uint64_t(a.advance)+b.advance;
 uint64_t terminal=b.terminal==65535?65535:uint64_t(left_nibbles)+b.terminal;
 if(advance>UINT32_MAX||terminal>65535) throw std::runtime_error("composition overflow");
 return {uint32_t(advance),b.state,uint16_t(terminal)};
}
inline void CheckPosition(uint64_t pos,uint32_t cap) {
 if(pos>uint64_t(cap)+8) throw std::runtime_error("coefficient advancement overflow");
}
// Predictions never become restarts until the actual incoming machine meets
// the predicted machine at the same next nibble. Otherwise scan this whole
// chunk exactly. Termination on either machine prevents using an invalid tail.
inline Resolved Resolve(const uint8_t* p,uint32_t bytes,const GpuFsmEntry* t,uint16_t initial,
 uint32_t cap,uint32_t chunk,const std::vector<Summary>& roots) {
 if(!chunk||chunk>1024||roots.size()!=(bytes+chunk-1)/chunk)
  throw std::runtime_error("scratch/summary capacity mismatch");
 Resolved out; uint16_t state=initial; uint64_t pos=0;
 out.checkpoints.reserve(roots.size());
 for(uint32_t c=0;c<roots.size();++c) {
  uint32_t begin=c*chunk*2,end=std::min(bytes*2,begin+chunk*2);
  out.checkpoints.push_back({begin,uint32_t(pos),state,0});
  uint16_t actual=state,root=0; uint32_t a=0,b=0,n=begin; bool live=true;
  while(n<end&&(actual!=root||!live)) {
   const auto& e=t[actual*16+Nibble(p,n)];
   if(e.value0==Marker) {
    CheckPosition(pos+a,cap); out.marker=n; out.position=uint32_t(pos+a); out.state=actual; return out;
   }
   a+=Advance(e); actual=e.next_state;
   if(live) { const auto& f=t[root*16+Nibble(p,n)];
    if(f.value0==Marker) live=false; else { b+=Advance(f); root=f.next_state; }
   }
   ++n; ++out.corrections;
  }
  out.longest=std::max(out.longest,n-begin);
  if(n==end) { ++out.full_chunks; pos+=a; state=actual; }
  else {
   const auto& r=roots[c];
   if(r.advance<b||(r.terminal!=65535&&begin+r.terminal<n)) throw std::runtime_error("inconsistent root summary");
   pos+=uint64_t(a)+r.advance-b; state=r.state;
   if(r.terminal!=65535) {
    CheckPosition(pos,cap); out.marker=begin+r.terminal; out.position=uint32_t(pos); out.state=state; return out;
   }
  }
  CheckPosition(pos,cap);
 }
 throw std::runtime_error("missing end marker / truncated payload");
}
inline std::vector<Summary> RootSummaries(const uint8_t* p,uint32_t bytes,const GpuFsmEntry* t,uint32_t chunk) {
 if(!chunk||chunk>1024) throw std::runtime_error("chunk size");
 std::vector<Summary> out((bytes+chunk-1)/chunk);
 for(uint32_t c=0;c<out.size();++c) out[c]=Scan(p,c*chunk*2,std::min(bytes*2,(c+1)*chunk*2),0,t);
 return out;
}
inline std::vector<uint16_t> Reachable(const GpuFsmEntry* t,unsigned count) {
 std::vector<uint16_t> list{0}; std::vector<bool> seen(count,false); seen[0]=true;
 for(size_t i=0;i<list.size();++i) for(int n=0;n<16;++n) {
  const auto& e=t[list[i]*16+n];
  if(e.value0!=Marker&&!seen[e.next_state]) { seen[e.next_state]=true; list.push_back(e.next_state); }
 }
 return list;
}
}
