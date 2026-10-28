// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_s3_attachment.h"
#include "decode_spatial_scalar.h"
#include "../reference_scalar_inverse/synthetic_vectors.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace hypercine::detail;
namespace {
constexpr Generation gen{292, 1, 1};
void require(bool ok, const char* predicate) {
    if (!ok) {
        // A callback exception is converted to GraphError by the scheduler.
        // Print the failing predicate here, before that conversion hides it.
        std::fprintf(stderr,"DECODER-ATTACHMENT-292 BROKEN first_predicate=%s\n",predicate);
        throw std::runtime_error(predicate);
    }
}
enum class Mutant { None, WrongBand, WrongChannel, Untyped, Cancel };
void exercise(unsigned workers, unsigned channel, unsigned w, unsigned h,
              const ScalarSynthetic::Case* fixture, Mutant mutant) {
    std::array<OwnedSignedPlane,4> planes{
        OwnedSignedPlane(w,h),OwnedSignedPlane(w,h),OwnedSignedPlane(w,h),OwnedSignedPlane(w,h)};
    for (unsigned b=0;b<4;++b) {
        auto v=planes[b].view();
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x)
            v.data[y*v.pitch/2+x]=fixture?fixture->input[b][y*w+x]:
                static_cast<std::int16_t>(b==0?4000*(channel+1):0);
    }
    const auto input=planes[0].view();
    auto consumer=MakeSpatialLowpassTask(gen,channel,w,h,input.pitch);
    if(mutant==Mutant::WrongBand) std::swap(consumer.inputs[0],consumer.inputs[1]);
    // A LEGAL foreign channel, so the refusal must come from the consumer's
    // identity check and not from publish_plane's channel>2 key guard. The
    // earlier 3-channel mapping made the ch==0 case unexercisable (3 is not a
    // channel), which left a zero-only skip undetected.
    const unsigned source_channel=mutant==Mutant::WrongChannel?(channel+1)%3:channel;
    if(mutant==Mutant::WrongChannel)
        for(auto& key:consumer.inputs) key.channel=source_channel;
    DecodeTaskGraph* running=nullptr;
    std::vector<TaskSpec> tasks;
    // LL is deliberately queued after all highpasses. Even at one worker the
    // consumer must remain Pending until this last required band completes.
    for(unsigned b:std::array<unsigned,4>{{1,2,3,0}}) tasks.push_back({
        {source_channel,0,5,0,b},{},[&,b](const TaskContext& ctx) {
            auto v=planes[b].view();
            if(b==0) require(running->state(4)==TaskState::Pending,
                "spatial consumer admitted before LL publication");
            // Rejection must be atomic, and a later valid extent may publish.
            require(!ctx.publish_plane(v.data,v.bytes-1,w,h,v.pitch),"short-span admitted");
            const bool ok=mutant==Mutant::Untyped?ctx.publish(v.data,v.bytes):
                ctx.publish_plane(v.data,v.bytes,w,h,v.pitch);
            require(ok,"valid source publish rejected");
            std::fill_n(v.data,v.bytes/2,static_cast<std::int16_t>(-12345));
        },[] {return true;}});
    if(mutant==Mutant::Cancel) {
        auto original=consumer.execute;
        consumer.execute=[&,original](const TaskContext& ctx) {
            require(running->cancel(gen),"cancel not observed");
            original(ctx);
        };
    }
    tasks.push_back(std::move(consumer));
    unsigned sinks=0, sink_entries=0;
    tasks.push_back({{channel,1,0,0,0},{{channel,0,4,0,0}},
        [&](const TaskContext& ctx) {
            ++sink_entries;
            const unsigned ow=w*2,oh=h*2;
            const std::size_t pitch=(ow*2+31)/32*32;
            PlaneIdentity expected{gen,channel,4,0,0,0,ow,oh,pitch};
            const auto result=ctx.input_plane(0,expected);
            require(result.valid()&&result.version==1&&result.sample==PlaneSample::Signed16LittleEndian,
                "output typed version");
            auto foreign=expected;foreign.generation.value++;
            require(!ctx.input_plane(0,foreign).valid(),"foreign generation admitted");
            foreign=expected;foreign.band=1;
            require(!ctx.input_plane(0,foreign).valid(),"wrong output band admitted");
            for(unsigned y=0;y<oh;++y) for(unsigned x=0;x<ow;++x) {
                std::int16_t value;
                std::memcpy(&value,static_cast<const unsigned char*>(result.data)+y*pitch+x*2,2);
                require(value==(fixture?fixture->expected[y*ow+x]:1000*(channel+1)),
                        "bit-exact coefficient");
            }
            ++sinks;
            const unsigned char token=1;require(ctx.publish(&token,1),"sink publication");
        },[] {return true;}});
    DecodeTaskGraph graph(gen,GraphProfile::Synthetic,std::move(tasks)); running=&graph;
    const bool ok=graph.run(workers);
    require(graph.quiescent(),"worker drain");
    if(mutant==Mutant::None) require(ok&&sinks==1,"complete spatial attachment");
    else {
        require(!ok&&sinks==0&&sink_entries==0,"mutant reached spatial output consumer");
        require(graph.state(5)!=TaskState::Ready&&graph.state(5)!=TaskState::Completed,"failed successor ready");
        require(graph.error()==(mutant==Mutant::Cancel?GraphError::Canceled:GraphError::Callback),
            "first error changed");
    }
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"workers argument");
        const unsigned workers=static_cast<unsigned>(std::stoul(argv[1]));
        require(workers==1||workers==2||workers==4||workers==16,"worker matrix");
        for(unsigned ch=0;ch<3;++ch) {
            for(const auto& fixture:ScalarSynthetic::cases)
                exercise(workers,ch,fixture.width,fixture.height,&fixture,Mutant::None);
            for(auto extent:std::array<std::array<unsigned,2>,4>{{{{120,135}},{{240,135}},{{480,270}},{{240,270}}}})
                exercise(workers,ch,extent[0],extent[1],nullptr,Mutant::None);
            for(auto mutant:{Mutant::WrongBand,Mutant::Untyped,Mutant::Cancel})
                exercise(workers,ch,16,4,nullptr,mutant);
            // Every channel, including Y=0, is crossed with a legal foreign
            // channel. Removing this for ch==0 let a zero-only skip survive.
            exercise(workers,ch,16,4,nullptr,Mutant::WrongChannel);
        }
        std::printf("DECODER-ATTACHMENT-292 PASS workers=%u\n",workers);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"DECODER-ATTACHMENT-292 BROKEN first_predicate=%s\n",e.what());
        return 1;
    }
}
