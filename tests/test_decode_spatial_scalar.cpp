// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_spatial_scalar.h"
#include "reference_scalar_inverse/synthetic_vectors.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

// Actual aligned allocations, including scratch, can fail deterministically.
thread_local long aligned_budget = -1;
std::atomic<int> aligned_live{0};
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (aligned_budget == 0) throw std::bad_alloc();
    if (aligned_budget > 0) --aligned_budget;
    void* p = nullptr;
#ifdef _WIN32
    p = _aligned_malloc(size ? size : 1, std::size_t(alignment));
#else
    if (posix_memalign(&p, std::size_t(alignment), size ? size : 1) != 0) p = nullptr;
#endif
    if (!p) throw std::bad_alloc();
    ++aligned_live; return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
    if (!p) return;
    --aligned_live;
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p,a); }
using namespace hypercine::detail;
namespace {
constexpr Generation gen{144,1,1};
int checks = 0;
void check(bool v) { ++checks; if (!v) throw std::runtime_error("Scalar research contract failed"); }
template<class F> void invalid(F f) {
    bool caught=false;
    try { f(); } catch (const std::invalid_argument&) { caught=true; }
    check(caught);
}
struct Frame {
    OwnedSignedPlane input[4]; OwnedSignedPlane output;
    Frame(unsigned w, unsigned h):input{OwnedSignedPlane(w,h),OwnedSignedPlane(w,h),OwnedSignedPlane(w,h),OwnedSignedPlane(w,h)},output(w*2,h*2) {}
    std::array<SpatialBandInput,4> bands(Generation g=gen) const {
        return {{{input[0].view(),g,0,5,0},{input[1].view(),g,0,5,1},
                 {input[2].view(),g,0,5,2},{input[3].view(),g,0,5,3}}};
    }
    SpatialLowpassOutput out(Generation g=gen) { return {output.view(),g,0,4,0}; }
};
void fill(MutableSignedPlaneView p, std::int16_t value) { std::fill_n(p.data,p.bytes/2,value); }
void copy(MutableSignedPlaneView p, const std::vector<std::int16_t>& values) {
    check(values.size()==std::size_t(p.width)*p.height);
    for (unsigned y=0;y<p.height;++y) std::copy_n(values.data()+y*p.width,p.width,p.data+y*p.pitch/2);
}
void expected(SignedPlaneView p, const std::vector<std::int16_t>& values) {
    for (unsigned y=0;y<p.height;++y) {
        for (unsigned x=0;x<p.width;++x) check(p.data[y*p.pitch/2+x]==values[y*p.width+x]);
        for (std::size_t x=p.width;x<p.pitch/2;++x) check(p.data[y*p.pitch/2+x]==0);
    }
}
void arithmetic() {
    for (const auto& c:ScalarSynthetic::cases) {
        Frame f(c.width,c.height);
        for (unsigned b=0;b<4;++b) copy(f.input[b].view(),c.input[b]);
        fill(f.output.view(),12345);
        check(InverseWavelet5To4(f.bands(),f.out())==SpatialStageResult::Complete);
        expected(f.output.view(),c.expected);
        // Padding is deliberately poisoned on every input; it must be ignored.
        for (auto& p:f.input) { auto v=p.view();
            for(unsigned y=0;y<v.height;++y) for(std::size_t x=v.width;x<v.pitch/2;++x) v.data[y*v.pitch/2+x]=-22222;
        }
        check(InverseWavelet5To4(f.bands(),f.out())==SpatialStageResult::Complete);
        expected(f.output.view(),c.expected);
    }
    // Independent analytic DC vector at every real fixture shape, including
    // chroma's padded 120-wide rows. LL=4000 with zero HP gives output1000.
    for(auto size:std::vector<std::pair<unsigned,unsigned>>{{120,135},{240,135},{480,270},{240,270}}) {
        Frame f(size.first,size.second); fill(f.input[0].view(),4000);
        check(InverseWavelet5To4(f.bands(),f.out())==SpatialStageResult::Complete);
        auto v=f.output.view();
        for(unsigned y=0;y<v.height;++y) for(unsigned x=0;x<v.width;++x) check(v.data[y*v.pitch/2+x]==1000);
    }
    // Independent analytic lower-boundary ramp: LL rows 0,8,16,24.
    // Original LL pointer stops at row2, so the final vertical pair is
    // (5*16+4*8-0+4)/8/2=7 and (11*16-4*8+0+4)/8/2=9.
    // Horizontal DC halves those to 3 and 4; a symmetric endpoint gives 5,6.
    // The separate LH ring is tested independently.
    for (unsigned band : {0u,1u}) {
        Frame f(16,4);
        auto low=f.input[band].view();
        for(unsigned y=0;y<4;++y) for(unsigned x=0;x<16;++x)
            low.data[y*low.pitch/2+x]=static_cast<std::int16_t>(8*y);
        check(InverseWavelet5To4(f.bands(),f.out())==SpatialStageResult::Complete);
        auto out=f.output.view();
        for(unsigned x=0;x<32;++x) {
            const int sign=(band==1 && x%2)?-1:1;
            check(out.data[6*out.pitch/2+x]==(band==0?3:(sign==1?5:-6)));
            check(out.data[7*out.pitch/2+x]==(band==0?4:(sign==1?6:-7)));
        }
    }
    OwnedSignedPlane a(17,3); auto before=a.view().data;
    OwnedSignedPlane b(std::move(a)); check(!a.view().data && !a.view().bytes && b.view().data==before);
    a=std::move(b);check(!b.view().data && !b.view().pitch && a.view().data==before);
    invalid([]{OwnedSignedPlane p(0,1);});invalid([]{OwnedSignedPlane p(961,1);});
    invalid([]{OwnedSignedPlane p(16,std::numeric_limits<unsigned>::max());});
}
void rejections() {
    Frame f(24,4); fill(f.output.view(),12345);
    auto run=[&](auto edit){auto in=f.bands();auto out=f.out();edit(in,out);invalid([&]{InverseWavelet5To4(in,out);});
        auto v=f.output.view();for(std::size_t i=0;i<v.bytes/2;++i)check(v.data[i]==12345);};
    for(unsigned b=0;b<4;++b) {
        run([&](auto& i,auto&){i[b].plane.data=nullptr;});
        run([&](auto& i,auto&){i[b].plane.data+=1;});
        run([&](auto& i,auto&){--i[b].plane.bytes;});
        run([&](auto& i,auto&){i[b].plane.pitch=48;});
        run([&](auto& i,auto&){i[b].plane.pitch=std::numeric_limits<std::size_t>::max()-31; i[b].plane.bytes=std::numeric_limits<std::size_t>::max();});
        run([&](auto& i,auto&){++i[b].generation.value;});
        run([&](auto& i,auto&){++i[b].channel;});
        run([&](auto& i,auto&){i[b].wavelet=4;});
        run([&](auto& i,auto&){i[b].band=9;});
        run([&](auto& i,auto&){--i[b].plane.height;});
    }
    run([](auto& i,auto&){i[0].plane.pitch=2*1024*1024;i[0].plane.bytes=16*1024*1024;});
    run([](auto& i,auto&){i[0].plane.data=reinterpret_cast<const std::int16_t*>(std::numeric_limits<std::uintptr_t>::max()-31);});
    run([](auto& i,auto&){i[1].plane=i[0].plane;});
    run([](auto& i,auto&){i[1].plane.data=i[0].plane.data+16;});
    run([](auto& i,auto& o){i[0].plane.data=o.plane.data;});
    run([](auto&,auto& o){o.plane.data=nullptr;});
    run([](auto&,auto& o){o.plane.data+=1;});
    run([](auto&,auto& o){--o.plane.bytes;});
    run([](auto&,auto& o){--o.plane.width;});
    run([](auto&,auto& o){++o.generation.session;});
    run([](auto&,auto& o){++o.generation.slot;});
    run([](auto&,auto& o){o.wavelet=5;});
    run([](auto&,auto& o){o.band=1;});
    run([](auto& i,auto& o){for(auto& b:i)b.generation.session=0;o.generation.session=0;});
    run([](auto& i,auto& o){for(auto& b:i)b.channel=3;o.channel=3;});
    for(auto size:std::vector<std::pair<unsigned,unsigned>>{{8,3},{17,3},{488,3},{16,2},{16,3},{16,271}}) {
        invalid([&]{Frame other(size.first,size.second);InverseWavelet5To4(other.bands(),other.out());});
    }
    const auto live=aligned_live.load(); bool failed=false;
    aligned_budget=0;
    try { InverseWavelet5To4(f.bands(),f.out()); } catch(const std::bad_alloc&) {failed=true;}
    aligned_budget=-1;check(failed && aligned_live==live);
    for(std::size_t n=0;n<f.output.view().bytes/2;++n)check(f.output.view().data[n]==12345);
    bool callback_threw=false;int polls=0;
    try { InverseWavelet5To4(f.bands(),f.out(),[&]{if(++polls==3)throw std::runtime_error("Synthetic callback exception");return false;}); }
    catch(const std::runtime_error&){callback_threw=true;}
    check(callback_threw && aligned_live==live);
    // Admission cancel: no output write. Mid-row-pair and final cancel: never
    // success, even though earlier row pairs may be present in private output.
    for(int cancel_at:{1,2,4,6}) {
        int calls=0;fill(f.output.view(),12345);
        check(InverseWavelet5To4(f.bands(),f.out(),[&]{return ++calls==cancel_at;})==SpatialStageResult::Canceled);
        if(cancel_at<=2) for(std::size_t n=0;n<f.output.view().bytes/2;++n)check(f.output.view().data[n]==12345);
    }
}
struct Gate {
    std::mutex m; std::condition_variable cv;bool entered=false,released=false;
    void hold(){std::unique_lock<std::mutex> lock(m);entered=true;cv.notify_all();
        if(!cv.wait_for(lock,std::chrono::seconds(3),[&]{return released;}))throw std::runtime_error("Synthetic gate deadline");}
    bool await(){std::unique_lock<std::mutex> lock(m);return cv.wait_for(lock,std::chrono::seconds(3),[&]{return entered;});}
    void release(){std::lock_guard<std::mutex> lock(m);released=true;cv.notify_all();}
};
TaskKey key(unsigned band) {return {0,5,4,0,band};}
void graph_tests(unsigned workers) {
    for(std::uint64_t generation=1;generation<=4;++generation) {
        Generation g{144,1,generation};
        auto owner=std::make_shared<Frame>(24,4);std::weak_ptr<Frame> weak=owner;
        Gate gate;bool published=false;
        std::vector<TaskSpec> tasks;
        tasks.push_back({key(4),{key(0),key(1),key(2),key(3)},[owner,g](const TaskContext& context){
            auto result=InverseWavelet5To4(owner->bands(g),owner->out(g),[&]{return context.cancellation_requested();});
            if(result!=SpatialStageResult::Complete)throw std::runtime_error("Canceled scalar callback");
        }});
        for(unsigned b=0;b<4;++b)tasks.push_back({key(b),{},[owner,&gate,b](const TaskContext&){
            if(b==0) gate.hold();
            fill(owner->input[b].view(),b==0?4000:0);
        }});
        tasks.push_back({key(5),{key(4)},[owner,&published](const TaskContext&){
            const auto p=owner->output.view();
            for(unsigned y=0;y<p.height;++y) for(unsigned x=0;x<p.width;++x)
                check(p.data[y*p.pitch/2+x]==1000);
            published=true; // downstream consumer after arithmetic completion
        }});
        {
            DecodeTaskGraph graph(g,GraphProfile::Synthetic,std::move(tasks), {4096, 65536, false});
            owner.reset();bool ok=false;
            std::thread thread([&]{ok=graph.run(workers);});
            const bool entered=gate.await();const bool pending=graph.state(0)==TaskState::Pending;
            const bool untouched=!published;
            gate.release();thread.join();
            check(entered && pending && untouched && ok && published && graph.quiescent());
            check(!graph.cancel({144,1,generation+1}));
            check(!weak.expired());
        }
        check(weak.expired());
    }
    // Cancellation with a completed private row pair and live callback scratch.
    for(bool fail_producer:{false,true}) {
        auto owner=std::make_shared<Frame>(24,4);std::weak_ptr<Frame> weak=owner;Gate gate;bool published=false;
        std::vector<TaskSpec> tasks;
        for(unsigned b=0;b<4;++b)tasks.push_back({key(b),{},[&,b](const TaskContext&){
            if(fail_producer && b==0)throw std::runtime_error("Synthetic producer failure");
        }});
        tasks.push_back({key(4),{key(0),key(1),key(2),key(3)},[owner,&gate](const TaskContext& context){
            int polls=0;
            auto result=InverseWavelet5To4(owner->bands(),owner->out(),[&]{
                if(++polls==3) gate.hold(); // first pair written, scratch live
                return context.cancellation_requested();
            });
            if(result==SpatialStageResult::Complete) throw std::runtime_error("Cancellation fixture escaped");
        }});
        tasks.push_back({key(5),{key(4)},[&](const TaskContext&){published=true;}});
        {
        DecodeTaskGraph graph(gen,GraphProfile::Synthetic,std::move(tasks), {4096, 65536, false});bool ok=false;
        owner.reset();
        std::thread thread([&]{ok=graph.run(workers);});
        if(!fail_producer){const bool entered=gate.await();const bool stopped=graph.cancel(gen);gate.release();thread.join();check(entered && stopped);}
        else thread.join();
        check(!ok && !published && graph.quiescent());
        check(graph.state(4)==(fail_producer?TaskState::Failed:TaskState::Canceled));
        check(graph.state(5)==(fail_producer?TaskState::Failed:TaskState::Canceled));
        check(!weak.expired());
        }
        check(weak.expired());
    }
    // These graphs are worker pools, not publication contracts: the callbacks
    // write into caller-owned frames and never call publish(), so the graph-wide
    // S2 rule is opted out explicitly (GraphLimits::require_published_output).
    // The default stays strict for every decoder-facing graph.
    // A wrong-generation actual scalar callback fails before any consumer can
    // publish, even though all synthetic producers completed successfully.
    Frame f(24,4);bool consumed=false;
    DecodeTaskGraph wrong(gen,GraphProfile::Synthetic,{
        {key(4),{},[&](const TaskContext&){InverseWavelet5To4(f.bands({144,1,2}),f.out());}},
        {key(5),{key(4)},[&](const TaskContext&){consumed=true;}}}, {4096, 65536, false});
    check(!wrong.run(workers) && !consumed && wrong.quiescent());
}
// Test-only runner: original compact little-endian files enter aligned planes;
// only a newly-created output directory is written. Errors never print paths.
unsigned number(const char* text) {
    const std::string value(text);std::size_t consumed=0;
    const auto n=std::stoul(value,&consumed);
    if(consumed!=value.size() || n>std::numeric_limits<unsigned>::max() || value.empty() || value[0]=='-')
        throw std::invalid_argument("Replay integer invalid");
    return static_cast<unsigned>(n);
}
void replay(int argc,char** argv) {
    if(argc!=10)throw std::invalid_argument("Replay arguments invalid");
    const auto w=number(argv[2]),h=number(argv[3]);
    Frame f(w,h);
    for(unsigned b=0;b<4;++b) {
        std::ifstream in(argv[4+b],std::ios::binary|std::ios::ate);
        if(!in || in.tellg()!=std::streamoff(std::size_t(w)*h*2))throw std::runtime_error("Original input size invalid");
        in.seekg(0);auto p=f.input[b].view();
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){unsigned char bytes[2];
            if(!in.read(reinterpret_cast<char*>(bytes),2))throw std::runtime_error("Original input read failed");
            const int v=int(bytes[0])+256*int(bytes[1]);p.data[y*p.pitch/2+x]=static_cast<std::int16_t>(v>=32768?v-65536:v);
        }
    }
    auto in=f.bands();const auto channel=number(argv[8]);
    for(auto& b:in) b.channel=channel;
    auto out=f.out();out.channel=channel;
    if(InverseWavelet5To4(in,out)!=SpatialStageResult::Complete)throw std::runtime_error("Scalar stage incomplete");
    const std::filesystem::path directory(argv[9]);
    if(!std::filesystem::create_directory(directory))throw std::runtime_error("Fresh replay output required");
    std::ofstream stream(directory/"stage.i16le",std::ios::binary);auto p=f.output.view();
    for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x){int v=p.data[y*p.pitch/2+x];if(v<0)v+=65536;
        const unsigned char bytes[2]={static_cast<unsigned char>(v%256),static_cast<unsigned char>(v/256)};
        stream.write(reinterpret_cast<const char*>(bytes),2);}
    stream.close();if(!stream)throw std::runtime_error("Scalar output write failed");
    std::cout<<"SCALAR_OUTPUT_ONLY; original equality belongs to verified replay harness\n";
}
} // namespace
int main(int argc,char** argv) {
    try {
        if(argc>1 && std::string(argv[1])=="--replay"){replay(argc,argv);return 0;}
        if(argc!=2)throw std::invalid_argument("Expected unit/rejection or worker count");
        const std::string mode=argv[1];
        if(mode=="arithmetic")arithmetic();else if(mode=="rejection")rejections();
        else {const auto workers=std::stoul(mode);if(workers!=1 && workers!=2 && workers!=4 && workers!=16)throw std::invalid_argument("Worker count");graph_tests(static_cast<unsigned>(workers));}
        check(aligned_live==0);
        std::cout<<"PASS SYNTHETIC scalar research checks="<<checks<<"; original parity NOT TESTED\n";
        return 0;
    } catch(...) {std::cerr<<"Scalar research test failed (details redacted).\n";return 1;}
}
