// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../core/filmscan3_backend.h"
#include "filmscan3_msl.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace hypercine { namespace standalone { namespace detail {
namespace {
using Clock=std::chrono::steady_clock;
double Milliseconds(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
std::runtime_error Error(const char* context,NSError* error=nil) {
    return std::runtime_error(std::string(context)+(error ? ": "+std::string([[error localizedDescription] UTF8String]) : ""));
}
struct ColorArgs { uint32_t width,height,stride,ys,cs; };
struct WaveArgs { uint32_t width,height,src_stride,dst_stride,prescale; };
class Metal final : public Frontend {
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLComputePipelineState> color_,horizontal_,vertical_;
    MetalTiming timing_;
    id<MTLBuffer> Buffer(std::size_t bytes,const void* input=nullptr) {
        if(bytes>device_.maxBufferLength) throw std::length_error("Metal buffer capacity exceeded");
        id<MTLBuffer> buffer=[device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if(!buffer)throw std::bad_alloc();
        timing_.peak_device_allocated_bytes=std::max(timing_.peak_device_allocated_bytes,static_cast<std::size_t>(device_.currentAllocatedSize));
        if(input)std::memcpy(buffer.contents,input,bytes);else std::memset(buffer.contents,0,bytes);
        return buffer;
    }
    static std::size_t Bytes(const Plane& p) { return p.pixels.size()*sizeof(int16_t); }
    void Dispatch(id<MTLComputeCommandEncoder> encoder,id<MTLComputePipelineState> pipeline,
                  int width,int height) {
        [encoder setComputePipelineState:pipeline];
        const NSUInteger tx=std::min<NSUInteger>(pipeline.threadExecutionWidth,32);
        const NSUInteger ty=std::min<NSUInteger>(8,pipeline.maxTotalThreadsPerThreadgroup/tx);
        [encoder dispatchThreads:MTLSizeMake(width,height,1) threadsPerThreadgroup:MTLSizeMake(tx,ty,1)];
    }
    void Finish(id<MTLCommandBuffer> command,bool color,Clock::time_point submitted) {
        [command commit];timing_.submit_ms+=Milliseconds(submitted);
        const auto wait=Clock::now();[command waitUntilCompleted];timing_.wait_ms+=Milliseconds(wait);
        if(command.status!=MTLCommandBufferStatusCompleted)throw Error("Metal command failed",command.error);
        const double gpu=(command.GPUEndTime-command.GPUStartTime)*1000;
        if(color){timing_.color_gpu_ms+=gpu;++timing_.color_dispatches;}
        else {timing_.wavelet_gpu_ms+=gpu;++timing_.wavelet_dispatches;}
    }
public:
    Metal() {
        @autoreleasepool {
            device_=MTLCreateSystemDefaultDevice();if(!device_)throw Error("No Metal device");
            queue_=[device_ newCommandQueue];if(!queue_)throw Error("No Metal command queue");
            NSError* error=nil;
            MTLCompileOptions* options=[MTLCompileOptions new];
            if (@available(macOS 15.0, *)) options.mathMode=MTLMathModeSafe;
            id<MTLLibrary> library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:kFilmScan3Msl]
                options:options error:&error];
            if(!library)throw Error("FilmScan3 MSL compilation failed",error);
            auto pipeline=[&](NSString* name) -> id<MTLComputePipelineState> {
                id<MTLFunction> function=[library newFunctionWithName:name];
                if(!function)throw Error("Missing Metal function");
                id<MTLComputePipelineState> state=[device_ newComputePipelineStateWithFunction:function error:&error];
                if(!state)throw Error("Metal pipeline failed",error);return state;
            };
            color_=pipeline(@"color709");horizontal_=pipeline(@"horizontal26");vertical_=pipeline(@"vertical26");
        }
    }
    std::string DeviceName() const override { return [device_.name UTF8String]; }
    MetalTiming Timing() const { return timing_; }
    Planes Convert(const uint8_t* input,std::size_t bytes,int w,int h,std::size_t stride) override {
        ValidateFilmScan3Input(input,bytes,w,h,stride);
        if(stride>UINT32_MAX || stride*static_cast<std::size_t>(h)>UINT32_MAX)
            throw std::length_error("Metal input stride exceeds 32-bit indexing");
        @autoreleasepool {
            Planes out{Plane(w,h),Plane(w/2,h),Plane(w/2,h)};
            const auto upload=Clock::now();
            const auto inputBytes=stride*(h-1)+static_cast<std::size_t>(w)*4;
            id<MTLBuffer> src=Buffer(inputBytes,input);
            id<MTLBuffer> y=Buffer(Bytes(out[0])),cr=Buffer(Bytes(out[1])),cb=Buffer(Bytes(out[2]));
            timing_.peak_buffer_bytes=std::max(timing_.peak_buffer_bytes,inputBytes+Bytes(out[0])+Bytes(out[1])+Bytes(out[2]));
            timing_.upload_ms+=Milliseconds(upload);
            const auto submit=Clock::now();
            id<MTLCommandBuffer> command=[queue_ commandBuffer];
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            if(!command || !encoder)throw Error("Metal color command allocation failed");
            [encoder setBuffer:src offset:0 atIndex:0];[encoder setBuffer:y offset:0 atIndex:1];
            [encoder setBuffer:cr offset:0 atIndex:2];[encoder setBuffer:cb offset:0 atIndex:3];
            ColorArgs args{uint32_t(w),uint32_t(h),uint32_t(stride),uint32_t(out[0].stride),uint32_t(out[1].stride)};
            [encoder setBytes:&args length:sizeof(args) atIndex:4];Dispatch(encoder,color_,w,h);[encoder endEncoding];
            Finish(command,true,submit);
            const auto read=Clock::now();
            std::memcpy(out[0].pixels.data(),y.contents,Bytes(out[0]));
            std::memcpy(out[1].pixels.data(),cr.contents,Bytes(out[1]));std::memcpy(out[2].pixels.data(),cb.contents,Bytes(out[2]));
            timing_.readback_ms+=Milliseconds(read);return out;
        }
    }
    WaveletLevel Forward(const Plane& input,int prescale) override {
        ValidateWaveletInput(input,prescale);
        @autoreleasepool {
            const int w=input.width/2,h=input.height/2;
            WaveletLevel out{{Plane(w,h),Plane(w,h),Plane(w,h),Plane(w,h)}};
            const auto upload=Clock::now();const std::size_t tempBytes=static_cast<std::size_t>(out.bands[0].stride)*input.height*2;
            id<MTLBuffer> src=Buffer(Bytes(input),input.pixels.data()),low=Buffer(tempBytes),high=Buffer(tempBytes);
            id<MTLBuffer> bands[4];for(int i=0;i<4;++i)bands[i]=Buffer(Bytes(out.bands[i]));
            timing_.peak_buffer_bytes=std::max(timing_.peak_buffer_bytes,Bytes(input)+2*tempBytes+4*Bytes(out.bands[0]));
            timing_.upload_ms+=Milliseconds(upload);
            const auto submit=Clock::now();id<MTLCommandBuffer> command=[queue_ commandBuffer];
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            if(!command || !encoder)throw Error("Metal wavelet command allocation failed");
            WaveArgs args{uint32_t(input.width),uint32_t(input.height),uint32_t(input.stride),uint32_t(out.bands[0].stride),uint32_t(prescale)};
            [encoder setBuffer:src offset:0 atIndex:0];[encoder setBuffer:low offset:0 atIndex:1];[encoder setBuffer:high offset:0 atIndex:2];
            [encoder setBytes:&args length:sizeof(args) atIndex:3];Dispatch(encoder,horizontal_,w,input.height);
            // End encoders between dependent passes: tracked-resource hazard ordering.
            [encoder endEncoding];encoder=[command computeCommandEncoder];
            if(!encoder)throw Error("Metal vertical encoder allocation failed");
            args.width=w;args.src_stride=out.bands[0].stride;args.prescale=0;
            [encoder setBytes:&args length:sizeof(args) atIndex:3];
            for(int i=0;i<2;++i) {
                [encoder setBuffer:i ? high : low offset:0 atIndex:0];
                [encoder setBuffer:bands[i] offset:0 atIndex:1];[encoder setBuffer:bands[i+2] offset:0 atIndex:2];
                Dispatch(encoder,vertical_,w,h);
            }
            [encoder endEncoding];Finish(command,false,submit);
            const auto read=Clock::now();for(int i=0;i<4;++i)std::memcpy(out.bands[i].pixels.data(),bands[i].contents,Bytes(out.bands[i]));
            timing_.readback_ms+=Milliseconds(read);return out;
        }
    }
};
}
std::shared_ptr<Frontend> MetalFrontend() { return std::make_shared<Metal>(); }
MetalTiming ReadMetalTiming(const Frontend& frontend) {
    const auto* metal=dynamic_cast<const Metal*>(&frontend);
    if(!metal)throw std::invalid_argument("frontend is not Metal");return metal->Timing();
}
}}}
