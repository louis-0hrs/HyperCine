// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Read-only internal-state observer for the matching Release SDK build only.
// No flags, buffers, thread limits or upstream instructions are modified.
#include <sstream>
#define private public
#include "SampleDecoder.h"
#undef private

void Snapshot(CFHD_DecoderRef reference, const std::string& prefix) {
    const auto* decoder=static_cast<CSampleDecoder*>(reference)->m_decoder;
    if (!decoder) throw std::runtime_error("missing internal decoder");
    Buffer bytes;
    std::ostringstream json;
    json << "{\"observation\":\"retained bands after DecodeSample, before CloseDecoder\","
         << "\"transform_workers\":" << decoder->worker_thread.pool.thread_count
         << ",\"entropy_workers\":" << decoder->entropy_worker_new.pool.thread_count
         << ",\"bands\":[";
    bool comma=false;
    for (int channel=0;channel<3;++channel) {
        const auto* transform=decoder->transform[channel];
        if (!transform || transform->num_wavelets<1 || transform->num_wavelets>TRANSFORM_MAX_WAVELETS)
            throw std::runtime_error("invalid internal transform");
        for (int level=0;level<transform->num_wavelets;++level) {
            const auto* image=transform->wavelet[level];
            if (!image) continue;
            if (image->width<1 || image->width>1920 || image->height<1 || image->height>1088 ||
                image->pitch<image->width*2 || image->pitch>16384 || image->num_bands<1 || image->num_bands>4)
                throw std::runtime_error("invalid retained band geometry");
            for (int band=0;band<image->num_bands;++band) {
                // Ignore invalid/untyped storage and row padding, which may be scratch.
                if (!(image->band_valid_flags & (1u<<band)) || !image->band[band] ||
                    image->pixel_type[band]!=PIXEL_TYPE_16S) continue;
                const size_t offset=bytes.size();
                for (int y=0;y<image->height;++y) {
                    const auto* row=reinterpret_cast<const uint8_t*>(image->band[band])+size_t(y)*image->pitch;
                    bytes.insert(bytes.end(),row,row+image->width*2);
                }
                if (comma) json << ',';
                comma=true;
                json << "{\"channel\":" << channel << ",\"wavelet_index\":" << level
                     << ",\"band\":" << band << ",\"width\":" << image->width
                     << ",\"height\":" << image->height << ",\"offset\":" << offset
                     << ",\"size\":" << bytes.size()-offset << '}';
            }
        }
    }
    if (bytes.empty()) throw std::runtime_error("no retained bands");
    json << "]}";
    Write(prefix+".bands",bytes);
    std::ofstream out(prefix+".bands.json"); out << json.str();
    if (!out) throw std::runtime_error("snapshot output error");
}
