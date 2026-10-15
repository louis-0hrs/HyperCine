// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <metal_stdlib>
using namespace metal;
int floor_div(int v,int d) { return v>=0 ? v/d : -((-v+d-1)/d); }
int sat(int v) { return clamp(v,-32768,32767); }
int channel(int r,int g,int b,int cr,int cg,int cb,int offset) {
    int v=floor_div((r*128)*cr,65536);
    v=sat(v+floor_div((g*128)*cg,65536));
    v=sat(v+floor_div((b*128)*cb,65536));
    uint wrapped=(uint(v)&65535u)*4u & 65535u;
    v=wrapped<=32767u ? int(wrapped) : int(wrapped)-65536;
    v=sat(v+offset); v=sat(v+16383);
    uint u=uint(v)&65535u;
    uint n=u<16383u ? 0u : u-16383u;
    return int(((n*4u)&65535u)/64u);
}
struct ColorArgs { uint width,height,stride,ys,cs; };
kernel void color709(device const uchar* input [[buffer(0)]],
                     device short* y [[buffer(1)]], device short* cr [[buffer(2)]],
                     device short* cb [[buffer(3)]],constant ColorArgs& a [[buffer(4)]],
                     uint2 pos [[thread_position_in_grid]]) {
    if(pos.x>=a.width || pos.y>=a.height)return;
    uint at=(a.height-1-pos.y)*a.stride+pos.x*4;
    int b=input[at],g=input[at+1],r=input[at+2];
    y[pos.y*a.ys+pos.x]=short(channel(r,g,b,1499,5029,507,1024));
    if(!(pos.x&1)) {
        cr[pos.y*a.cs+pos.x/2]=short(channel(r,g,b,3596,-3268,-327,8192));
        cb[pos.y*a.cs+pos.x/2]=short(channel(r,g,b,-827,-2768,3596,8192));
    }
}
struct WaveArgs { uint width,height,src_stride,dst_stride,prescale; };
int tap(device const short* p,uint at,uint prescale) {
    int v=p[at];return prescale==2 ? floor_div(v+3,4) : v;
}
int high26(device const short* p,uint at,uint step,uint pair,uint pairs,uint prescale) {
    int a=tap(p,at,prescale),b=tap(p,at+step,prescale);
    if(pair==0) return sat(floor_div(5*a-11*b+4*tap(p,at+2*step,prescale)+4*tap(p,at+3*step,prescale)-tap(p,at+4*step,prescale)-tap(p,at+5*step,prescale)+4,8));
    if(pair+1==pairs) return sat(floor_div(11*a-5*b-4*tap(p,at-step,prescale)-4*tap(p,at-2*step,prescale)+tap(p,at-3*step,prescale)+tap(p,at-4*step,prescale)+4,8));
    return sat(floor_div(-tap(p,at-2*step,prescale)-tap(p,at-step,prescale)+tap(p,at+2*step,prescale)+tap(p,at+3*step,prescale)+4,8)+a-b);
}
kernel void horizontal26(device const short* input [[buffer(0)]],device short* low [[buffer(1)]],
                         device short* high [[buffer(2)]],constant WaveArgs& a [[buffer(3)]],
                         uint2 pos [[thread_position_in_grid]]) {
    if(pos.x>=a.width/2 || pos.y>=a.height)return;
    uint at=pos.y*a.src_stride+2*pos.x,dst=pos.y*a.dst_stride+pos.x;
    int sum=int(input[at])+int(input[at+1]);
    // CPU prescale lowpass narrows the pair sum without signed saturation.
    if(a.prescale==2) {
        int v=floor_div(sum+3,4); uint bits=uint(v)&65535u;
        low[dst]=short(bits<=32767u ? int(bits) : int(bits)-65536);
    } else low[dst]=short(sat(sum));
    high[dst]=short(high26(input,at,1,pos.x,a.width/2,a.prescale));
}
kernel void vertical26(device const short* input [[buffer(0)]],device short* low [[buffer(1)]],
                       device short* high [[buffer(2)]],constant WaveArgs& a [[buffer(3)]],
                       uint2 pos [[thread_position_in_grid]]) {
    if(pos.x>=a.width || pos.y>=a.height/2)return;
    uint at=2*pos.y*a.src_stride+pos.x,dst=pos.y*a.dst_stride+pos.x;
    low[dst]=short(sat(int(input[at])+int(input[at+a.src_stride])));
    high[dst]=short(high26(input,at,a.src_stride,pos.y,a.height/2,0));
}
