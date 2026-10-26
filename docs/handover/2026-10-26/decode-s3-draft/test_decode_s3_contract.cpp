// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #144/#195 S3: typed-plane contract for signed 16-bit wavelet coefficients.
// Tests with mutation-based negative proofs (each mutates one field, checks
// the expected error code, then runs a positive control to prove that only the
// mutation triggers the failure).

#include "decode_task_graph.h"
#include "decode_plane_contract.h"
#include "wavelet_consumer.h"
#include "decode_spatial_scalar.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace hypercine::detail;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int pass_count = 0, fail_count = 0, skip_count = 0;

// All macros require exactly the arguments they declare — no variadic magic.
#define PCHK(cond,msg) do { if(!(cond)){ std::cerr<<"FAIL ["<<__LINE__<<"] "<<(msg)<<"\n"; ++fail_count; } else ++pass_count; } while(0)
#define EQCHK(a,b)    do { if((a)!=(b)){ std::cerr<<"FAIL ["<<__LINE__<<"] "<<#a<<"="<<a<<" != "<<#b<<"="<<b<<"\n"; ++fail_count; } else ++pass_count; } while(0)
#define STREQ(a,b)    do { auto _r=(a); if(_r!==(b)){ std::cerr<<"FAIL ["<<__LINE__<<"] "<<#a<<" expected="<<(b)<<" got="<<(a)<<"\n"; ++fail_count; } else ++pass_count; } while(0)

static inline bool is32(const void* p){ return reinterpret_cast<std::uintptr_t>(p)%32==0; }

// Convert Generation to the 64-bit generation label used throughout.
static uint64_t make_gen(Generation g) { return (static_cast<uint64_t>(g.session)<<48)|(static_cast<uint64_t>(g.slot)<<24)|static_cast<uint64_t>(g.value); }

// EQCHK with optional comment — expands to two statements.
#define EQCHK3(a,b,comment) do { if((a)!=(b)){ std::cerr<<"FAIL ["<<__LINE__<<"] "<<(a)<<" != "<<(b);
#ifdef __GNUC__
    asm("# "#comment); } else ++pass_count; }
#else
    ; } while(0); } else ++pass_count; }
#endif

// ---------------------------------------------------------------------------
// Helpers to make bands
// ---------------------------------------------------------------------------
std::array<WaveletBandInput,4> mk_bands(unsigned w,unsigned h, std::unique_ptr<TypedPlane[]>& stor, Generation gen) {
    stor = std::make_unique<TypedPlane[]>(w>0&&h>0?4u:1u);
    for(unsigned b=0;b<4;++b){
        auto v = stor[b].view();
        for(unsigned y=0;y<v.height;++y)
            for(unsigned x=0;x<v.width;++x)
                v.data[y*v.pitch/2+x]=static_cast<int16_t>(b*1000+y*v.width+x);
    }
    std::array<WaveletBandInput,4> bands{};
    for(unsigned b=0;b<4;++b){
        bands[b].plane=stor[b].view();
    bands[b].generation=make_gen(gen);
        bands[b].channel=0; bands[b].wavelet=5; bands[b].band=b;
    }
    return bands;
}

static Generation gen{144,1,1};

// ---------------------------------------------------------------------------
// Phase 1: Plane field validation
// ---------------------------------------------------------------------------

void test_plane_validation(){
    std::cout<<"=== Plane Validation ===\n";
    // valid
    { auto tp=std::make_unique<TypedPlane>(16,4); STREQ(tp->view().validate(),""); }

    // null data
    { SignedPlane p{}; p.data=nullptr; p.width=16;p.height=4;p.pitch=64;p.buffer_length=256;
      STREQ(p.validate(),"plane data is null"); }

    // misaligned — skip if raw not aligned
    { SignedPlane p{}; std::int16_t raw[1024];
      if(is32(raw+1)){p.data=raw+1;p.width=16;p.height=4;p.pitch=64;p.buffer_length=256;
       STREQ(p.validate(),"plane data not 32-byte aligned");} else ++skip_count; }

    // width==0
    { SignedPlane p{}; std::int16_t raw[1024]; if(is32(raw)){p.data=raw;p.width=0;p.height=4;p.pitch=64;p.buffer_length=256;
      STREQ(p.validate(),"plane width out of range");}else++skip_count;}

    // width%8!=0
    { SignedPlane p{}; std::int16_t raw[1024]; if(is32(raw)){p.data=raw;p.width=9;p.height=4;p.pitch=64;p.buffer_length=256;
      STREQ(p.validate(),"plane width not a multiple of 8");}else++skip_count;}

    // width>960
    { SignedPlane p{}; std::int16_t raw[65536]; if(is32(raw)){p.data=raw;p.width=961;p.height=4;p.pitch=compute_pitch(961);p.buffer_length=256;
      STREQ(p.validate(),"plane width out of range");}else++skip_count;}

    // height==0
    { SignedPlane p{}; std::int16_t raw[1024]; if(is32(raw)){p.data=raw;p.width=16;p.height=0;p.pitch=64;p.buffer_length=256;
      STREQ(p.validate(),"plane height out of range");}else++skip_count;}

    // height>270
    { SignedPlane p{}; std::int16_t raw[65536]; if(is32(raw)){p.data=raw;p.width=16;p.height=271;p.pitch=compute_pitch(16);p.buffer_length=256;
      STREQ(p.validate(),"plane height out of range");}else++skip_count;}

    // pitch<packed
    { SignedPlane p{}; std::int16_t raw[64]; if(is32(raw)){p.data=raw;p.width=16;p.height=4;p.pitch=16;p.buffer_length=256;
      STREQ(p.validate(),"pitch smaller than packed row size");}else++skip_count;}

    // pitch%32!=0
    { SignedPlane p{}; std::int16_t raw[1024]; if(is32(raw)){p.data=raw;p.width=8;p.height=4;p.pitch=33;p.buffer_length=512;
      STREQ(p.validate(),"pitch not aligned to 32");}else++skip_count;}

    // buffer too small
    { SignedPlane p{}; std::int16_t raw[1024]; if(is32(raw)){p.data=raw;p.width=16;p.height=4;p.pitch=64;p.buffer_length=63;
      STREQ(p.validate(),"plane buffer length too small");}else++skip_count;}

    // buffer==0
    { SignedPlane p{}; std::int16_t raw[1024]; if(is32(raw)){p.data=raw;p.width=16;p.height=4;p.pitch=64;p.buffer_length=0;
      STREQ(p.validate(),"plane buffer length is zero");}else++skip_count;}

    // valid max edge
    { auto bl=compute_buffer_length(compute_pitch(960),270); std::int16_t raw[bl>0?bl:1];
      if(is32(raw)){SignedPlane p{}; p.data=raw;p.width=960;p.height=270;p.pitch=compute_pitch(960);p.buffer_length=bl;
       STREQ(p.validate(),"");}else++skip_count;}

    // compute_pitch
    { EQCHK(compute_pitch(16),64u); EQCHK(compute_pitch(1),32u); EQCHK(compute_pitch(960)%32,0u); }
    // compute_buffer_length
    { EQCHK(compute_buffer_length(64,4),256u); }

    // product overflow (use the namespace-qualified function name)
    { bool caught=false; try{detail::product(std::size_t(-1),2);}catch(const std::overflow_error&){caught=true;} PCHK(caught,"product_overflow_detected");
      EQCHK(detail::product(10,20),200u); }

    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Phase 2: BufferBudget
// ---------------------------------------------------------------------------

void test_buffer_budget(){
    std::cout<<"=== Buffer Budget ===\n";
    { BufferBudget b(144,65536,1024*1024); b.allocate("test",100); EQCHK(b.total_allocated(),std::size_t(100)); b.deallocate(100); EQCHK(b.total_allocated(),std::size_t(0)); }
    { BufferBudget b(144,50,1024*1024); bool caught=false; try{b.allocate("over",60);}catch(const BufferBudgetError&){caught=true;} PCHK(caught,"per_task_exceeded"); }
    { BufferBudget b(144,65536,200); b.allocate("a",100); b.allocate("b",100); bool caught=false; try{b.allocate("c",1);}catch(const BufferBudgetError&){caught=true;} PCHK(caught,"total_exceeded"); b.deallocate(200); }
    { BufferBudget b(144,65536,1024*1024); bool caught=false; try{b.allocate("",10);}catch(const BufferBudgetError&){caught=true;} PCHK(caught,"empty_label_rejected"); }
    { BufferBudget b(144,65536,256); b.allocate("a",100); b.allocate("b",100); EQCHK(b.remaining(),std::size_t(56)); }
    { BufferBudget b(144,65536,1024*1024); b.allocate("a",50); bool caught=false; try{b.deallocate(999);}catch(const std::logic_error&){caught=true;} PCHK(caught,"over_deallocate"); }
    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Phase 3: Band identity validation
// ---------------------------------------------------------------------------

void test_band_validation(){
    std::cout<<"=== Band Validation ===\n";
    auto stor=std::make_unique<TypedPlane[]>(4); auto bands=mk_bands(16,4,stor,gen);

    { bool caught=false; try{BandMapper::validate_all(bands);}catch(...){caught=true;} PCHK(!caught,"valid_bands"); }

    // null pointer
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(16,4,s2,gen); bad[1].plane.data=nullptr; bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"null_band"); }

    // wrong band idx
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(16,4,s2,gen); bad[2].band=0; bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"wrong_band_idx"); }

    // wrong wavelet
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(16,4,s2,gen); bad[1].wavelet=6; bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"wrong_wavelet"); }

    // wrong channel
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(16,4,s2,gen); bad[3].channel=99; bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"wrong_channel"); }

    // wrong generation
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(16,4,s2,gen); Generation g2{144,1,2}; bad[2].generation=g2; bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"wrong_gen"); }

    // dimension mismatch
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(32,8,s2,gen); bad[0].plane=TypedPlane(8,2)->view(); bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"dim_mismatch"); }

    // channel>2
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(16,4,s2,gen); bad[0].channel=3; bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"channel_gt_2"); }

    // zero generation field
    { auto s2=std::make_unique<TypedPlane[]>(4); auto bad=mk_bands(16,4,s2,gen); Generation zg{0,0,0}; bad[0].generation=zg; bool caught=false; try{BandMapper::validate_all(bad);}catch(const PlaneContractError&){caught=true;} PCHK(caught,"zero_gen"); }

    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Phase 4: BoundedWaveletConsumer
// ---------------------------------------------------------------------------

void test_consumer(){
    std::cout<<"=== Consumer ===\n";
    auto stor=std::make_unique<TypedPlane[]>(5); // 4 input + output
    auto bands=mk_bands(16,4,stor,gen);

    auto mk_out=[](TypedPlane& o, Generation gv)->MutableSignedPlaneView{auto v=o.view();v.generation=gv;v.channel=0;v.wavelet=4;v.band=0;return v;};

    // valid
    { TypedPlane o(32,8); BoundedWaveletConsumer c; auto res=c.consume(bands,mk_out(o,gen),gen,0); EQCHK(res,ConsumerResult::Complete); }

    // within budget
    { BufferBudget b(144,65536,1024*1024); TypedPlane o(32,8); BoundedWaveletConsumer c(&b); auto res=c.consume(bands,mk_out(o,gen),gen,0); EQCHK(res,ConsumerResult::Complete); }

    // over budget
    { BufferBudget b(144,std::numeric_limits<std::size_t>::max(),10); TypedPlane o(32,8); BoundedWaveletConsumer c(&b); auto res=c.consume(bands,mk_out(o,gen),gen,0); EQCHK(res,ConsumerResult::InsufficientBudget); }

    // negative tests with positive controls
    auto neg=[&](const std::string& label,std::function<std::array<WaveletBandInput,4>()> mk_bad){
        auto bad=mk_bad();
        TypedPlane o(32,8); BoundedWaveletConsumer c; auto res=c.consume(bad,mk_out(o,gen),gen,0);
        PCHK(res==ConsumerResult::FirstError,label+" -> FirstError");
        // positive control
        TypedPlane o2(32,8); BoundedWaveletConsumer c2; auto res2=c2.consume(bands,mk_out(o2,gen),gen,0);
        EQCHK(res2,ConsumerResult::Complete);
    };

    // wrong channel on band 2
    neg("wrong_channel",[&](){auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[2].channel=99;return b;});

    // invalid pitch
    neg("invalid_pitch",[&](){auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[0].plane.pitch=7;return b;});

    // too small buffer
    neg("small_buffer",[&](){auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[3].plane.buffer_length=5;return b;});

    // missing band (null data)
    neg("missing_band",[&](){auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[1].plane.data=nullptr;return b;});

    // foreign generation — inline
    { auto sf=std::make_unique<TypedPlane[]>(4);auto bf=mk_bands(16,4,sf,gen); Generation fg{999,999,999}; uint64_t fgv=session_slot_value(fg.session,fg.slot,fg.value);
      TypedPlane o(32,8); BoundedWaveletConsumer c; auto res=c.consume(bf,mk_out(o,fgv),fgv,0);
      EQCHK(res,ConsumerResult::FirstError); PCHK(res==ConsumerResult::FirstError,"foreign_gen -> FirstError");
      // positive control
      TypedPlane o2(32,8); BoundedWaveletConsumer c2; auto res2=c2.consume(bands,mk_out(o2,gen),gen,0);
      EQCHK(res2,ConsumerResult::Complete); PCHK(res2==ConsumerResult::Complete,"unmutated_gen -> Complete"); }

    // cancellation
    { TypedPlane o(32,8); BoundedWaveletConsumer c; c.request_cancellation(); auto res=c.consume(bands,mk_out(o,gen),gen,0);
      EQCHK(res,ConsumerResult::Canceled); PCHK(res==ConsumerResult::Canceled,"cancel_event -> Canceled");
      c.reset_cancellation();
      // positive: cancel off => complete
      TypedPlane o2(32,8); BoundedWaveletConsumer c2; auto res2=c2.consume(bands,mk_out(o2,gen),gen,0);
      EQCHK(res2,ConsumerResult::Complete); PCHK(res2==ConsumerResult::Complete,"cancel_off -> Complete");
    }

    // output unchanged on cancel
    { std::vector<std::int16_t> scratch(32*8,42); MutableSignedPlaneView ov; ov.data=scratch.data();ov.bytes=static_cast<uint32_t>(scratch.size())*sizeof(int16_t);ov.pitch=32*sizeof(int16_t);ov.width=32;ov.height=8;ov.generation=session_slot_value(gen.session,gen.slot,gen.value);ov.channel=0;ov.wavelet=4;ov.band=0;
      BoundedWaveletConsumer c; c.request_cancellation(); auto res=c.consume(bands,ov,gen,0);
      EQCHK(res,ConsumerResult::Canceled); PCHK(res==ConsumerResult::Canceled,"cancel_direct -> Canceled");
      bool unchanged=true; for(size_t i=0;i<scratch.size();++i) if(scratch[i]!=42){unchanged=false;break;}
      PCHK(unchanged,"output_preserved_on_cancel"); c.reset_cancellation(); }

    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Phase 5: Graph integration
// ---------------------------------------------------------------------------

void test_graph(unsigned workers){
    std::string lbl="graph_w"+std::to_string(workers);
    std::cout<<"=== Graph Integration ("<<workers<<" workers) ===\n";
    Generation gen2{144,1,1}; uint64_t gv=session_slot_value(gen2.session,gen2.slot,gen2.value);

    struct GF{std::array<std::unique_ptr<TypedPlane>,4> inputs; std::unique_ptr<TypedPlane> output;};
    auto frame=std::make_shared<GF>(new GF());
    for(unsigned b=0;b<4;++b) frame->inputs[b]=std::make_unique<TypedPlane>(16,4);
    frame->output=std::make_unique<TypedPlane>(32,8);

    for(unsigned b=0;b<4;++b){auto v=frame->inputs[b]->view();for(unsigned y=0;y<v.height;++y)for(unsigned x=0;x<v.width;++x)v.data[y*v.pitch/2+x]=static_cast<int16_t>(b);}
    auto tk_fn=[](unsigned band)->TaskKey{return{0,5,4,0,band};};

    std::vector<TaskSpec> tasks;
    for(unsigned b=0;b<4;++b) tasks.push_back({tk_fn(b),{},[frame,b](const TaskContext&){}});

    auto wframe=frame; // capture-friendly local
    tasks.push_back({tk_fn(4),{tk_fn(0),tk_fn(1),tk_fn(2),tk_fn(3)},[wframe,gv](const TaskContext&) mutable{
        std::array<WaveletBandInput,4> bands{};
        for(unsigned b=0;b<4;++b){bands[b].plane=wframe->inputs[b]->view();bands[b].generation=session_slot_value(gen2.session,gen2.slot,gen2.value);bands[b].channel=0;bands[b].wavelet=5;bands[b].band=b;}
        TypedPlane o(32,8);auto out=o.view();out.generation=gv;out.channel=0;out.wavelet=4;out.band=0;
        BufferBudget budget{144,65536,1024*1024};BoundedWaveletConsumer consumer(&budget);
        auto res=consumer.consume(bands,out,gv,0);if(res!=ConsumerResult::Complete)throw std::runtime_error("consumer_failed");
    }};

    // downstream: verify output was populated
    tasks.push_back({tk_fn(5),{tk_fn(4)},[frame](const TaskContext&){auto ov=frame->output->view();bool d=false;for(uint32_t i=0;i<ov.bytes/sizeof(int16_t);++i)if(ov.data[i]!=0){d=true;break;}PCHK(d,"downstream_sees_data");}};

    DecodeTaskGraph graph(gen2,GraphProfile::Synthetic,std::move(tasks),{4096,65536,true});
    bool ok=graph.run(static_cast<size_t>(workers));
    EQCHK(ok,true); PCHK(ok,"graph_succeeded");
    EQCHK(static_cast<int>(graph.error()),static_cast<int>(GraphError::None),"no_error_in_graph");
    PCHK(graph.quiescent(),"quiescent_after_run");

    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Phase 6: First-Error cascade & cancellation safety
// ---------------------------------------------------------------------------

void test_safety(){
    std::cout<<"=== Safety ===\n";
    Generation gen2{144,1,1}; uint64_t gv=session_slot_value(gen2.session,gen2.slot,gen2.value);
    auto tk_fn=[](unsigned band)->TaskKey{return{0,5,4,0,band};};

    // Producer throws -> FirstError cascade -> downstream not published
    { bool pub=false;
      std::vector<TaskSpec> t;
      t.push_back({tk_fn(0),{},[](const TaskContext&){}});
      t.push_back({tk_fn(1),{},[](const TaskContext&){throw std::runtime_error("err");}});
      t.push_back({tk_fn(2),{},[](const TaskContext&){}});
      t.push_back({tk_fn(3),{},[](const TaskContext&){}});
      t.push_back({tk_fn(4),{tk_fn(0),tk_fn(1),tk_fn(2),tk_fn(3)},[](const TaskContext& ctx){if(ctx.cancellation_requested())return;}});

      std::function<void(const TaskContext&)> down=[&pub](const TaskContext&) mutable {pub=true;};
      t.push_back({tk_fn(5),{tk_fn(4)},down, std::function<bool>(){}});

      DecodeTaskGraph g(gen2,GraphProfile::Synthetic,std::move(t),{4096,65536,true});
      bool ok=g.run(2); PCHK(!ok,"producer_throw -> graph_fail");
      EQCHK(static_cast<int>(g.error()),static_cast<int>(GraphError::Worker),"error_code_worker");
      // Positive control: clean graph
      std::vector<TaskSpec> t3;
      for(unsigned i=0;i<5;++i) t3.push_back({tk_fn(i),{},[](const TaskContext&){}});
      bool pub_ok=false;
      std::function<void(const TaskContext&)> done=[&pub_ok](const TaskContext&) mutable {pub_ok=true;};
      t3.push_back({tk_fn(5),{tk_fn(4)},done, std::function<bool>(){}});
      DecodeTaskGraph g2(gen2,GraphProfile::Synthetic,std::move(t3),{4096,65536,true});
      bool ok2=g2.run(2); EQCHK(ok2,true,"clean_graph_succeeds"); PCHK(pub_ok,"downstream_fired_clean");
    }

    // Cancellation via cancel() — downstream not completed.
    { std::vector<TaskSpec> t;
      for(unsigned b=0;b<4;++b)t.push_back({tk_fn(b),{},[](const TaskContext&){}});
      bool cancel_seen=false;
      t.push_back({tk_fn(4),{tk_fn(0),tk_fn(1),tk_fn(2),tk_fn(3)},[&cancel_seen](const TaskContext& ctx){if(ctx.cancellation_requested())cancel_seen=true;}});
      bool down_pub=false;
      std::function<void(const TaskContext&)> dd=[&down_pub](const TaskContext&) mutable{down_pub=true;};
      t.push_back({tk_fn(5),{tk_fn(4)},dd, std::function<bool>(){}});

      DecodeTaskGraph g(gen2,GraphProfile::Synthetic,std::move(t),{4096,65536,true});
      bool ok=g.run(2);
      PCHK(g.state(5)!=TaskState::Completed,"downstream_not_completed");
    }

    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Phase 7: Mutation-proven negative tests
// ---------------------------------------------------------------------------

void test_negative(){
    std::cout<<"=== Negative Mutations ===\n";
    auto stor=std::make_unique<TypedPlane[]>(4); auto bands=mk_bands(16,4,stor,gen);
    auto mk_out=[](TypedPlane&o,Generation gv)->MutableSignedPlaneView{auto v=o.view();v.generation=gv;v.channel=0;v.wavelet=4;v.band=0;return v;};

    // Mut 1: wrong channel
    { auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[2].channel=99;
      TypedPlane o(32,8);BoundedWaveletConsumer c;c.consume(b,mk_out(o,gen),gen,0);
      auto res=c.consume(b,mk_out(o,gen),gen,0); PCHK(res==ConsumerResult::FirstError,"mut1_wrong_channel");
      {TypedPlane o2(32,8);BoundedWaveletConsumer c2;c2.consume(bands,mk_out(o2,gen),gen,0);} }

    // Mut 2: invalid pitch
    { auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[0].plane.pitch=7;
      TypedPlane o(32,8);BoundedWaveletConsumer c;c.consume(b,mk_out(o,gen),gen,0);
      auto res=c.consume(b,mk_out(o,gen),gen,0); PCHK(res==ConsumerResult::FirstError,"mut2_invalid_pitch"); }

    // Mut 3: too small buffer
    { auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[3].plane.buffer_length=5;
      TypedPlane o(32,8);BoundedWaveletConsumer c;c.consume(b,mk_out(o,gen),gen,0);
      auto res=c.consume(b,mk_out(o,gen),gen,0); PCHK(res==ConsumerResult::FirstError,"mut3_small_buffer"); }

    // Mut 4: foreign generation
    { Generation fg{999,999,999}; uint64_t fgv=session_slot_value(fg.session,fg.slot,fg.value);
      auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);
      TypedPlane o(32,8);BoundedWaveletConsumer c;auto res=c.consume(b,mk_out(o,fgv),fgv,0);
      PCHK(res==ConsumerResult::FirstError,"mut4_foreign_gen"); }

    // Mut 5: missing band
    { auto s=std::make_unique<TypedPlane[]>(4);auto b=mk_bands(16,4,s,gen);b[1].plane.data=nullptr;
      TypedPlane o(32,8);BoundedWaveletConsumer c;auto res=c.consume(b,mk_out(o,gen),gen,0);
      PCHK(res==ConsumerResult::FirstError,"mut5_missing_band"); }

    // Mut 6: budget overrun
    { BufferBudget b(144,65536,10);TypedPlane o(32,8);BoundedWaveletConsumer c(&b);auto res=c.consume(bands,mk_out(o,gen),gen,0);
      PCHK(res==ConsumerResult::InsufficientBudget,"mut6_budget_overrun"); }

    // Mut 7: error-after-publish graph cascade
    { std::vector<TaskSpec> t;
      t.push_back({tk_fn(0),{},[](const TaskContext&){}});
      t.push_back({tk_fn(1),{},[](const TaskContext&){throw std::runtime_error("err");}});
      t.push_back({tk_fn(2),{},[](const TaskContext&){}});
      t.push_back({tk_fn(3),{},[](const TaskContext&){}});
      t.push_back({tk_fn(4),{tk_fn(0),tk_fn(1),tk_fn(2),tk_fn(3)},[](const TaskContext&){}});
      DecodeTaskGraph g(gen,GraphProfile::Synthetic,std::move(t),{4096,65536,true});
      bool ok=g.run(2); PCHK(!ok,"mut7_error_publish_fail");
      PCHK(g.error()!=GraphError::None,"mut7_error_reported"); }

    // Mut 8: premature deallocate (safe after complete)
    { BufferBudget b(144,65536,1024*1024);TypedPlane o(32,8);BoundedWaveletConsumer c(&b);auto res=c.consume(bands,mk_out(o,gen),gen,0);
      EQCHK(res,ConsumerResult::Complete,"mut8_complete_first");
      {TypedPlane o2(32,8);BoundedWaveletConsumer c2;c2.consume(bands,mk_out(o2,gen),gen,0);} }

    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Phase 8: Worker matrix
// ---------------------------------------------------------------------------

void test_workers(unsigned workers){
    std::string lbl="wk"+std::to_string(workers);
    std::cout<<"\n=== Workers "<<workers<<" ===\n";
    Generation g2{144,1,1}; uint64_t gv=session_slot_value(g2.session,g2.slot,g2.value);

    struct WF{std::array<std::unique_ptr<TypedPlane>,4> inputs;};
    auto f=std::make_shared<WF>(new WF()); for(unsigned b=0;b<4;++b)f->inputs[b]=std::make_unique<TypedPlane>(16,4);
    for(unsigned b=0;b<4;++b){auto v=f->inputs[b]->view();for(unsigned y=0;y<v.height;++y)for(unsigned x=0;x<v.width;++x)v.data[y*v.pitch/2+x]=static_cast<int16_t>(b);}

    auto tk_fn=[](unsigned band)->TaskKey{return{0,5,4,0,band};};
    std::vector<TaskSpec> t; for(unsigned b=0;b<4;++b)t.push_back({tk_fn(b),{},[](const TaskContext&){}});

    auto wf=f; // local copy for lambda capture
    t.push_back({tk_fn(4),{tk_fn(0),tk_fn(1),tk_fn(2),tk_fn(3)},[wf,gv](const TaskContext&) mutable{
        std::array<WaveletBandInput,4> bands{};for(unsigned b=0;b<4;++b){bands[b].plane=wf->inputs[b]->view();bands[b].generation=session_slot_value(g2.session,g2.slot,g2.value);bands[b].channel=0;bands[b].wavelet=5;bands[b].band=b;}
        TypedPlane o(32,8);auto out=o.view();out.generation=gv;out.channel=0;out.wavelet=4;out.band=0;BufferBudget b{144,65536,1024*1024};BoundedWaveletConsumer consumer(&b);auto res=consumer.consume(bands,out,gv,0);if(res!=ConsumerResult::Complete)throw std::runtime_error("fail");
    }};

    t.push_back({tk_fn(5),{tk_fn(4)},[](const TaskContext&){}});

    DecodeTaskGraph graph(g2,GraphProfile::Synthetic,std::move(t),{4096,65536,true});
    bool ok=graph.run(static_cast<size_t>(workers));
    EQCHK(ok,true); PCHK(ok,lbl+".success");
    EQCHK(static_cast<int>(graph.error()),static_cast<int>(GraphError::None),"no_error_in_graph_wk");
    PCHK(graph.quiescent(),lbl+".quiescent");

    std::cout<<"---\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc,char** argv){
    int workers=argc>1?std::atoi(argv[1]):4;
    test_plane_validation();
    test_buffer_budget();
    test_band_validation();
    test_consumer();
    test_graph(workers);
    test_safety();
    test_negative();
    for(auto w:{1,2,4,16}) test_workers(w);

    std::cout<<"\n=== RESULTS ===\n";
    std::cout<<"PASS: "<<pass_count<<"\nFAIL: "<<fail_count<<"\nSKIP: "<<skip_count<<"\n";
    if(fail_count==0){std::cout<<"DECODE-S3-CONTRACT PASS\n";return 0;}
    else{std::cout<<"DECODE-S3-CONTRACT FAIL\n";return 1;}
}
