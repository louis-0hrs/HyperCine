// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../benchmarks/decode_phase2fb_gpu.h"
#include <iostream>
#include <functional>
using namespace Phase2FB;
namespace {
void Need(bool b) { if(!b) throw std::runtime_error("exactness assertion"); }
void Reject(const std::function<void()>& f) { bool failed=false;try { f(); }catch(const std::exception&) { failed=true; } Need(failed); }
void Verify(const std::vector<uint8_t>& p,const std::vector<GpuFsmEntry>& t,unsigned states,uint16_t initial,uint32_t chunk,bool diff=false) {
 unsigned nibbles=unsigned(p.size()*2),cap=nibbles*3;
 auto roots=RootSummaries(p.data(),unsigned(p.size()),t.data(),chunk);
 auto r=Resolve(p.data(),unsigned(p.size()),t.data(),initial,cap,chunk,roots);
 std::vector<int16_t> expected(cap,0); unsigned s=initial,pos=0;size_t ci=0;bool ended=false;
 for(unsigned n=0;n<nibbles;++n) {
  if(ci<r.checkpoints.size()&&r.checkpoints[ci].nibble==n) { Need(r.checkpoints[ci].state==s&&r.checkpoints[ci].position==pos);++ci; }
  auto e=t[s*16+Nibble(p.data(),n)];
  if(e.value0==Marker) { Need(r.marker==n&&r.position==pos&&r.state==s&&ci==r.checkpoints.size());ended=true;break; }
  pos+=e.pre_post_skip&511;if(e.value0) expected[pos]=int16_t(int(e.value0)*7);if(e.value1) expected[pos+1]=int16_t(int(e.value1)*7);
  pos+=(e.pre_post_skip>>12)&7;s=e.next_state;
 }
 Need(ended);
 if(diff) for(unsigned x=1;x<cap;++x) expected[x]=int16_t(int(expected[x])+expected[x-1]);
 auto m=RunGpu(p.data(),unsigned(p.size()),t.data(),states,initial,cap,7,diff,int(cap),1,chunk);
 Need(m.coefficients==expected&&m.resolved.marker==r.marker&&m.scratch<=ScratchCeiling);
}
}
int main() { try {
 std::vector<GpuFsmEntry> t(48);
 for(int s=0;s<3;++s) for(int n=0;n<16;++n) {
  auto& e=t[s*16+n];e.value0=int16_t((n&1)?-(s+1):(s+1));e.pre_post_skip=1<<12;e.next_state=uint16_t(n%3);
  if(n==15) e.value0=Marker;
 }
 std::vector<uint8_t> p(2049);uint32_t rng=0x1337BEEF;
 for(auto& b:p) { rng=rng*1664525+1013904223;b=uint8_t(((rng>>24)%15)<<4|((rng>>16)%15)); } p.back()=0x0f;
 unsigned cases=0;
 // Associativity on complete 3-state maps, with continuation and absorbing end.
 std::vector<std::vector<Summary>> maps(3,std::vector<Summary>(3));
 for(unsigned c=0;c<3;++c) for(unsigned state=0;state<3;++state)
  maps[c][state]=Scan(p.data(),c*16,(c+1)*16,uint16_t(state),t.data());
 std::vector<Summary> ab(3),bc(3);
 for(unsigned state=0;state<3;++state) { ab[state]=Compose(maps[0][state],16,maps[1]);bc[state]=Compose(maps[1][state],16,maps[2]); }
 for(unsigned state=0;state<3;++state) {
  auto left=Compose(ab[state],32,maps[2]),right=Compose(maps[0][state],16,bc);
  auto direct=Scan(p.data(),0,48,uint16_t(state),t.data());
  Need(left.advance==right.advance&&left.state==right.state&&left.terminal==right.terminal);
  Need(left.advance==direct.advance&&left.state==direct.state&&left.terminal==direct.terminal);
 }++cases;

 for(auto chunk:{16u,32u,64u,128u,256u,512u,1024u}) for(uint16_t initial:{uint16_t(0),uint16_t(1),uint16_t(2)}) {
  Verify(p,t,3,initial,chunk);++cases;
 }
 Verify(p,t,3,1,32,true);++cases;
 // Root takes a false terminal, so actual correction must traverse this entire chunk.
 auto false_marker=t; false_marker[14].value0=Marker;auto q=p;q[0]=0xe0;
 Verify(q,false_marker,3,1,16);++cases;
 // End marker on first/odd nibble, partial final chunk, and long non-converging cycle.
 Verify(std::vector<uint8_t>{0xf0},t,3,0,16);++cases;
 Verify(std::vector<uint8_t>{0x0f},t,3,0,16);++cases;
 auto cycle=t;for(int s=0;s<3;++s) for(int n=0;n<15;++n) cycle[s*16+n].next_state=uint16_t(s);
 Verify(p,cycle,3,1,64);++cases;
 auto overlapping=t;overlapping[0].pre_post_skip=0;
 Reject([&]{RunGpu(p.data(),unsigned(p.size()),overlapping.data(),3,0,20000,1,false,20000,1,64);});++cases;
 auto budget=std::vector<uint8_t>(MaxPayload,0);
 Reject([&]{RunGpu(budget.data(),unsigned(budget.size()),t.data(),3,0,20000,1,false,20000,1,16);});++cases;
 auto missing=std::vector<uint8_t>(2048,0);
 Reject([&]{RunGpu(missing.data(),unsigned(missing.size()),t.data(),3,0,20000,1,false,20000,1,64);});++cases;
 Reject([&]{RunGpu(p.data(),unsigned(p.size()),t.data(),3,0,1,1,false,1,1,64);});++cases;
 Reject([&]{RunGpu(p.data(),unsigned(p.size()),t.data(),3,3,20000,1,false,20000,1,64);});++cases;
 auto bad=t;bad[0].next_state=3;
 Reject([&]{RunGpu(p.data(),unsigned(p.size()),bad.data(),3,0,20000,1,false,20000,1,64);});++cases;
 Reject([&]{RunGpu(p.data(),unsigned(p.size()),t.data(),0,0,20000,1,false,20000,1,64);});++cases;
 Reject([&]{RunGpu(p.data(),MaxPayload+1,t.data(),3,0,20000,1,false,20000,1,64);});++cases;
 Reject([&]{RunGpu(p.data(),unsigned(p.size()),t.data(),3,0,UINT32_MAX,1,false,1,1,64);});++cases;
 Reject([&]{RunGpu(p.data(),unsigned(p.size()),t.data(),3,0,20000,1,false,20000,1,0);});++cases;
 Reject([&]{Resolve(p.data(),unsigned(p.size()),t.data(),0,20000,64,{});});++cases;
 auto truncated=p;truncated.pop_back();
 Reject([&]{RunGpu(truncated.data(),unsigned(truncated.size()),t.data(),3,0,20000,1,false,20000,1,64);});++cases;
 std::cout<<"Phase 2F-B "<<cases<<" deterministic exactness/bounds cases passed, max error 0\n";return 0;
 }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
