// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <vector>
#include <string>
#include <fstream>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include "CFHDDecoder.h"
#define private public
#include "SampleDecoder.h"
#undef private
extern "C" {
#include "wavelet.h"
}

namespace {
struct Record {
    int channel=-1,level=-1,width=0,height=0,out_width=0,out_height=0,scale=0;
    bool inverse=false,valid=false,started=false,stored=false,producer_completed=false;
    unsigned valid_before=0,started_before=0,begin=0,end=0;
    size_t scratch_size=0;
    std::vector<uint8_t> input_before,input_after,output_after;
};
std::atomic<DECODER*> current{nullptr};
std::string destination;
std::mutex records_lock;
std::vector<Record> records;
std::atomic<unsigned> observer_sequence{0};
std::atomic<bool> invalid{false};
std::atomic<unsigned> in_flight{0};
// Published only after the original filter and observer's output copy finish.
// No worker waits for another worker; the queue skips an unready dependency.
std::atomic<bool> completed[3][3]{};
std::atomic<unsigned> readiness_checks{0}, lowpass_deferrals{0}, producer_deferrals{0};
#ifdef HYPERCINE_DEPENDENCY_ENFORCED
constexpr bool enforced=true;
#else
constexpr bool enforced=false;
#endif
struct SdkLockGuard {
    LOCK* lock;
    explicit SdkLockGuard(LOCK* value):lock(value) {Lock(lock);}
    ~SdkLockGuard() {Unlock(lock);}
    SdkLockGuard(const SdkLockGuard&)=delete;
    SdkLockGuard& operator=(const SdkLockGuard&)=delete;
};

std::vector<uint8_t> CopyBand(const IMAGE* image,int band) {
    if (!image || image->width<1 || image->width>1920 || image->height<1 || image->height>1088 ||
        image->pitch<image->width*2 || image->pitch>16384 || !image->band[band])
        throw std::runtime_error("invalid observer band");
    std::vector<uint8_t> bytes(size_t(image->width)*image->height*2);
    for(int y=0;y<image->height;++y) {
        const auto* row=reinterpret_cast<const uint8_t*>(image->band[band])+size_t(y)*image->pitch;
        std::copy(row,row+image->width*2,bytes.begin()+size_t(y)*image->width*2);
    }
    return bytes;
}
std::vector<uint8_t> CopyInputs(const IMAGE* image) {
    std::vector<uint8_t> bytes;
    for(int b=0;b<4;++b) {
        if(image->pixel_type[b]!=PIXEL_TYPE_16S) throw std::runtime_error("non-16S boundary");
        auto band=CopyBand(image,b); bytes.insert(bytes.end(),band.begin(),band.end());
    }
    return bytes;
}
}

void TemporalBegin(CFHD_DecoderRef reference,const std::string& prefix) {
    auto* decoder=static_cast<CSampleDecoder*>(reference)->m_decoder;
    if(!decoder || in_flight!=0) throw std::runtime_error("missing or active decoder");
    std::lock_guard<std::mutex> lock(records_lock);
    current=decoder;destination=prefix; records.clear(); observer_sequence=0; invalid=false;
    for(auto& channel:completed) for(auto& level:channel) level=false;
    readiness_checks=0;lowpass_deferrals=0;producer_deferrals=0;
}

extern "C" bool HyperCineDependencyReady(DECODER* decoder,TRANSFORM* transform,int channel,int index,IMAGE* wavelet) {
    ++readiness_checks;
    if(!enforced || !decoder || !transform || !wavelet || decoder!=current.load() || channel<0 || channel>=3 || index<1 || index>2 ||
       transform!=decoder->transform[channel] || transform->type!=TRANSFORM_TYPE_SPATIAL ||
       wavelet!=transform->wavelet[index] || wavelet->num_bands!=4) {invalid=true;return false;}
    // This manual target is Windows-only: the SDK lock is a recursive Windows
    // CRITICAL_SECTION. The queue selection site already holds it; the two
    // subsequent predicate sites do not. Pair with UpdateWaveletBandValidFlags.
    const SdkLockGuard guard(&decoder->entropy_worker_new.lock);
    const auto valid=wavelet->band_valid_flags, started=wavelet->band_started_flags;
    const bool lowpass=(valid&BAND_VALID_MASK(0))!=0;
    // Index 2 consumes the entropy-decoded lowpass. Index 1 additionally needs
    // this channel's actual index-2 inverse publication, not another channel.
    const bool published=index==2 || completed[channel][2].load(std::memory_order_acquire);
    const bool ready=lowpass && published && (valid&15)==15 && valid==started;
    if(!lowpass) ++lowpass_deferrals;
    if(!published) ++producer_deferrals;
    return ready;
}

extern "C" void HyperCineObserveInverseLowpass(IMAGE* input,IMAGE* output,const SCRATCH* scratch,int scale,bool inverse) {
    Record r;
    struct InFlight { InFlight(){++in_flight;} ~InFlight(){--in_flight;} } activity;
    auto* decoder=current.load();
    try {
        if(decoder) for(int c=0;c<3;++c) if(decoder->transform[c])
            for(int l=1;l<=2;++l) if(decoder->transform[c]->wavelet[l]==input) {r.channel=c;r.level=l;}
        if(r.channel<0) {invalid=true;} else {
            r.begin=++observer_sequence; r.width=input->width; r.height=input->height;
            r.out_width=output->width;r.out_height=output->height;r.scale=scale;r.inverse=inverse;
            r.scratch_size=scratch->free_size;
            r.valid_before=input->band_valid_flags;r.started_before=input->band_started_flags;
            r.valid=BANDS_ALL_VALID(input);r.started=BANDS_ALL_STARTED(input);
            r.producer_completed=r.level==2 || completed[r.channel][2].load(std::memory_order_acquire);
            if(r.valid) {r.input_before=CopyInputs(input);r.stored=true;} else {invalid=true;}
        }
    } catch (...) {invalid=true;}
    // Always call the original implementation exactly once, with unchanged arguments.
    TransformInverseSpatialQuantLowpass(input,output,scratch,scale,inverse);
    try {
        r.end=++observer_sequence;
        if(r.stored) {
            r.input_after=CopyInputs(input);r.output_after=CopyBand(output,0);
            std::lock_guard<std::mutex> lock(records_lock);
            if(records.size()>=24) invalid=true;
            else {
                const int channel=r.channel,level=r.level;
                records.push_back(std::move(r));
                completed[channel][level].store(true,std::memory_order_release);
            }
        }
    } catch (...) {invalid=true;}
}

void TemporalEnd() {
    // DecodeSample's original completion waits have returned before this call.
    auto* decoder=current.load();
    std::lock_guard<std::mutex> lock(records_lock);
    if(!decoder || in_flight!=0 || invalid || records.empty()) throw std::runtime_error("incomplete boundary observation");
    std::vector<uint8_t> raw;
    std::ofstream json(destination+".temporal.json");
    json << "{\"transform_workers\":" << decoder->worker_thread.pool.thread_count
         << ",\"entropy_workers\":" << decoder->entropy_worker_new.pool.thread_count
         << ",\"observer_calls_in_flight_after_decode\":0,\"dependency_enforced\":"<<(enforced?"true":"false")
         << ",\"readiness_checks\":"<<readiness_checks.load()<<",\"lowpass_deferrals\":"<<lowpass_deferrals.load()
         << ",\"producer_deferrals\":"<<producer_deferrals.load()<<",\"events\":[";
    for(size_t i=0;i<records.size();++i) {
        const auto& r=records[i]; if(i) json<<',';
        json << "{\"channel\":"<<r.channel<<",\"input_wavelet\":"<<r.level
             <<",\"width\":"<<r.width<<",\"height\":"<<r.height
             <<",\"output_width\":"<<r.out_width<<",\"output_height\":"<<r.out_height
             <<",\"scale\":"<<r.scale<<",\"inverse_prescale\":"<<(r.inverse?"true":"false")
             <<",\"scratch_free_size\":"<<r.scratch_size<<",\"valid_flags_before\":"<<r.valid_before
             <<",\"started_flags_before\":"<<r.started_before<<",\"all_valid_before\":"<<(r.valid?"true":"false")
             <<",\"all_started_before\":"<<(r.started?"true":"false")
             <<",\"required_producer_completed\":"<<(r.producer_completed?"true":"false")
             <<",\"begin\":"<<r.begin<<",\"end\":"<<r.end;
        for(const auto& segment: {std::make_pair("input_before",&r.input_before),
             std::make_pair("input_after",&r.input_after),std::make_pair("output_after",&r.output_after)}) {
            json<<",\""<<segment.first<<"\":{\"offset\":"<<raw.size()<<",\"size\":"<<segment.second->size()<<'}';
            raw.insert(raw.end(),segment.second->begin(),segment.second->end());
        }
        json<<'}';
    }
    json<<"]}";
    std::ofstream bytes(destination+".temporal",std::ios::binary);
    bytes.write(reinterpret_cast<const char*>(raw.data()),raw.size());
    if(!json || !bytes) throw std::runtime_error("boundary output error");
    current=nullptr;
}
