// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/wavelet.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
static int64_t floor8(int64_t n) { const auto q=n/8, r=n%8; return q-(r<0); }
static int16_t clamp(int64_t n) { return static_cast<int16_t>(std::max<int64_t>(-32768,std::min<int64_t>(32767,n))); }
int main() {
  uint64_t vectors=0, coefficients=0;
  for(int context : {0,-32768,32767}) for(int tap=0;tap<8;++tap) for(int val=-32768;val<=32767;++val) {
    alignas(32) std::array<int16_t,64> h{},v{},lo{},hi{},vlo{},vhi{};
    h.fill(static_cast<int16_t>(context)); v.fill(12345);
    h[tap]=static_cast<int16_t>(val);
    for(int i=0;i<8;++i) v[i*8]=h[i];
    lo.fill(12345); hi.fill(12345); vlo.fill(12345); vhi.fill(12345);
    if(HyperCine_ForwardHorizontal26_CPU(h.data(),8,1,8,lo.data(),8,hi.data(),8)!=HYPERCINE_STATUS_SUCCESS ||
       HyperCine_ForwardVertical26_CPU(v.data(),1,8,8,vlo.data(),8,vhi.data(),8)!=HYPERCINE_STATUS_SUCCESS) return 2;
    for(int c=0;c<4;++c) {
      const int x=2*c; int64_t high;
      if(c==0) high=floor8(5LL*h[0]-11LL*h[1]+4LL*h[2]+4LL*h[3]-h[4]-h[5]+4);
      else if(c==3) high=floor8(11LL*h[x]-5LL*h[x+1]-4LL*h[x-1]-4LL*h[x-2]+h[x-3]+h[x-4]+4);
      else high=floor8(-int64_t(h[x-2])-h[x-1]+h[x+2]+h[x+3]+4)+h[x]-h[x+1];
      const auto low=clamp(int64_t(h[x])+h[x+1]);
      if(lo[c]!=low || hi[c]!=clamp(high) || vlo[c*8]!=low || vhi[c*8]!=clamp(high)) {
        std::cerr<<"mismatch context="<<context<<" tap="<<tap<<" value="<<val<<" coefficient="<<c<<'\n'; return 1;
      }
      coefficients+=4;
    }
    for(int i=0;i<64;++i) {
      if((i>=4 && (lo[i]!=12345 || hi[i]!=12345)) ||
         ((i%8 || i>=32) && (vlo[i]!=12345 || vhi[i]!=12345))) return 3;
    }
    ++vectors;
  }
  std::cout<<"{\"status\":\"PASS\",\"vectors\":"<<vectors<<",\"coefficients\":"<<coefficients<<",\"axes\":2,\"width_or_height\":8,\"contexts\":3,\"tap_positions\":8,\"values_per_tap\":65536,\"padding_write_canaries\":\"PASS\"}\n";
}
