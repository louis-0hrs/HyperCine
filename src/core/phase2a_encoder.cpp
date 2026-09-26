// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#ifndef _ALLOCATOR
#define _ALLOCATOR 1
#endif

#include "hypercine/phase2a.h"
#include "hypercine/phase2a_gpu.h"
#include "hypercine/entropy_parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "frame.h"
#include "image.h"
}

namespace hypercine {
namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto origin = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - origin).count();
}

double ProcessCpuSeconds() {
#ifdef _WIN32
    FILETIME create_time, exit_time, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel, &user)) return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 1e7;
#else
    return 0;
#endif
}

struct Interval {
    double a = 0;
    double b = 0;
};

double UnionLength(std::vector<Interval> spans) {
    spans.erase(std::remove_if(spans.begin(), spans.end(), [](const Interval& span) { return span.b <= span.a; }), spans.end());
    if (spans.empty()) return 0;
    std::sort(spans.begin(), spans.end(), [](const Interval& lhs, const Interval& rhs) { return lhs.a < rhs.a; });
    double length = 0;
    double start = spans[0].a;
    double end = spans[0].b;
    for (size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].a > end) {
            length += end - start;
            start = spans[i].a;
            end = spans[i].b;
        } else if (spans[i].b > end) {
            end = spans[i].b;
        }
    }
    return length + (end - start);
}

double IntersectionLength(std::vector<Interval> lhs, std::vector<Interval> rhs) {
    lhs.erase(std::remove_if(lhs.begin(), lhs.end(), [](const Interval& span) { return span.b <= span.a; }), lhs.end());
    rhs.erase(std::remove_if(rhs.begin(), rhs.end(), [](const Interval& span) { return span.b <= span.a; }), rhs.end());
    if (lhs.empty() || rhs.empty()) return 0;
    std::sort(lhs.begin(), lhs.end(), [](const Interval& a, const Interval& b) { return a.a < b.a; });
    std::sort(rhs.begin(), rhs.end(), [](const Interval& a, const Interval& b) { return a.a < b.a; });
    auto merge = [](std::vector<Interval> spans) {
        std::vector<Interval> out;
        for (const Interval& span : spans) {
            if (out.empty() || span.a > out.back().b) out.push_back(span);
            else out.back().b = std::max(out.back().b, span.b);
        }
        return out;
    };
    lhs = merge(std::move(lhs));
    rhs = merge(std::move(rhs));
    double length = 0;
    for (const Interval& a : lhs) {
        for (const Interval& b : rhs) {
            const double start = std::max(a.a, b.a);
            const double end = std::min(a.b, b.b);
            if (end > start) length += end - start;
        }
    }
    return length;
}

struct QuantTables {
    int prescale[3] = {};
    int quant[36] = {};
    int widths[3] = {};
    int heights[3] = {};
    int color_space = 2;
    bool ok = false;
};

QuantTables ReadQuant(ENCODER* encoder, TRANSFORM* transforms[], int channel_count) {
    QuantTables q;
    if (!encoder || !encoder->frame || channel_count < 3 || !transforms) return q;
    q.color_space = encoder->input.color_space;
    for (int level = 0; level < 3; ++level) {
        if (!transforms[0]) return q;
        q.prescale[level] = transforms[0]->prescale[level];
    }
    for (int c = 0; c < 3; ++c) {
        IMAGE* image = encoder->frame->channel[c];
        if (!image || !transforms[c]) return q;
        q.widths[c] = image->width;
        q.heights[c] = image->height;
        for (int level = 0; level < 3; ++level) {
            IMAGE* wavelet = transforms[c]->wavelet[level];
            if (!wavelet) return q;
            for (int band = 0; band < 4; ++band) q.quant[(c * 3 + level) * 4 + band] = wavelet->quant[band];
        }
    }
    q.ok = true;
    return q;
}

const char* InputName(Phase2Input input) {
    return input == Phase2Input::DeviceBgra ? "device_bgra" : "host_bgra";
}

const char* TransportName(Phase2Transport transport) {
    switch (transport) {
    case Phase2Transport::FullInt16: return "full_int16";
    case Phase2Transport::Int16Stream: return "int16_band_stream";
    case Phase2Transport::CompactStream: return "compact_band_stream";
    }
    return "unknown";
}

int InputKind(Phase2Input input) {
    return input == Phase2Input::DeviceBgra ? HYPERCINE_INPUT_DEVICE_BGRA : HYPERCINE_INPUT_HOST_BGRA;
}

int TransportKind(Phase2Transport transport) {
    switch (transport) {
    case Phase2Transport::FullInt16: return HYPERCINE_XPORT_FULL_INT16;
    case Phase2Transport::Int16Stream: return HYPERCINE_XPORT_INT16_BAND;
    case Phase2Transport::CompactStream: return HYPERCINE_XPORT_COMPACT_BAND;
    }
    return HYPERCINE_XPORT_FULL_INT16;
}

struct WorkItem {
    int slot = 0;
    int piece = 0;
    ExternalCoeff coeff;
    double ready_ms = 0;
    double copy_ms = 0;
};

struct SlotState {
    HyperCineGpuSlot* gpu = nullptr;
    int frame_index = -1;
    int expected = 0;
    std::atomic<int> encoded{0};
    int polled = 0;
    bool gpu_done = false;
    bool timing_ready = false;
    double submit_ms = 0;
    double last_entropy_ms = 0;
    double schedule_sum = 0;
    int schedule_n = 0;
    double ready_sum = 0;
    int ready_samples = 0;
    int max_ready = 0;
    HyperCineStreamTiming timing{};
    std::vector<EncodedPiece> pieces;
    std::vector<Interval> entropy_iv;
    std::vector<Interval> d2h_iv;
    std::mutex mu;
};

struct Pool {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<WorkItem> queue;
    bool stop = false;
    std::vector<std::thread> threads;
    SlotState* slots = nullptr;
    encoder* enc = nullptr;
    transform** transforms = nullptr;
    std::atomic<int> failed{0};
};

void WorkerMain(Pool* pool) {
    for (;;) {
        WorkItem work;
        {
            std::unique_lock<std::mutex> lock(pool->mu);
            pool->cv.wait(lock, [&] { return pool->stop || !pool->queue.empty(); });
            if (pool->stop && pool->queue.empty()) return;
            work = pool->queue.front();
            pool->queue.pop_front();
        }
        const double start = NowMs();
        EncodedPiece piece;
        const bool ok = EncodeOneExternalBand(pool->enc, pool->transforms, work.coeff, &piece);
        const double end = NowMs();
        if (!ok) {
            piece.ok = false;
            pool->failed.store(1);
        }
        SlotState& slot = pool->slots[work.slot];
        slot.pieces[static_cast<size_t>(work.piece)] = std::move(piece);
        {
            std::lock_guard<std::mutex> lock(slot.mu);
            slot.schedule_sum += std::max(0.0, start - work.ready_ms);
            ++slot.schedule_n;
            slot.entropy_iv.push_back(Interval{start, end});
            slot.last_entropy_ms = std::max(slot.last_entropy_ms, end);
            slot.d2h_iv.push_back(Interval{work.ready_ms - static_cast<double>(work.copy_ms), work.ready_ms});
        }
        slot.encoded.fetch_add(1, std::memory_order_release);
    }
}

void StoreBand(IMAGE* wavelet, int band, const int16_t* src, int width, int height) {
    const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
    PIXEL* dst = wavelet->band[band];
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(stride),
                    src + static_cast<size_t>(y) * static_cast<size_t>(width),
                    static_cast<size_t>(width) * sizeof(int16_t));
    }
}

int CountDiff(IMAGE* wavelet, int band, const int16_t* src, int width, int height) {
    const int stride = wavelet->pitch / static_cast<int>(sizeof(PIXEL));
    const PIXEL* dst = wavelet->band[band];
    int mismatches = 0;
    for (int y = 0; y < height; ++y) {
        const PIXEL* row = dst + static_cast<size_t>(y) * static_cast<size_t>(stride);
        const int16_t* got = src + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) if (row[x] != got[x]) ++mismatches;
    }
    return mismatches;
}

}  // namespace

Phase2Run RunPhase2(encoder* enc_opaque, transform* transforms_opaque[], int channel_count,
                    const uint8_t* bgra, int pitch, void* scratch, size_t scratch_bytes,
                    const Phase2Config& config) {
    Phase2Run run;
    run.input_name = InputName(config.input);
    run.transport_name = TransportName(config.transport);
    run.workers = std::max(1, config.workers);
    run.pipeline_depth = std::max(1, std::min(2, config.pipeline_depth));
    ENCODER* encoder = reinterpret_cast<ENCODER*>(enc_opaque);
    TRANSFORM** transforms = reinterpret_cast<TRANSFORM**>(transforms_opaque);
    if (!encoder || !transforms || !bgra || pitch <= 0 || config.frames <= 0) {
        run.error = "missing phase 2 input";
        return run;
    }
    QuantTables quant = ReadQuant(encoder, transforms, channel_count);
    if (!quant.ok) {
        run.error = "encoder pyramid is not a 3-level 3-channel spatial transform";
        return run;
    }
    const std::vector<BandRef> refs = EnumerateBands(transforms_opaque, channel_count);
    if (refs.size() != 30) {
        run.error = "expected 30 canonical subbands";
        return run;
    }
    int piece_of[3][3][4];
    std::memset(piece_of, 0xff, sizeof(piece_of));
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        const BandRef& ref = refs[static_cast<size_t>(i)];
        if (ref.channel < 0 || ref.channel >= 3 || ref.wavelet_index < 0 || ref.wavelet_index >= 3 || ref.band < 0 || ref.band >= 4) {
            run.error = "band reference is outside the canonical pyramid";
            return run;
        }
        piece_of[ref.channel][ref.wavelet_index][ref.band] = i;
    }

    const int width = quant.widths[0];
    const int height = quant.heights[0];
    std::vector<SlotState> slots(static_cast<size_t>(run.pipeline_depth));
    for (int i = 0; i < run.pipeline_depth; ++i) {
        slots[static_cast<size_t>(i)].gpu = HyperCine_GpuSlotCreate(width, height);
        if (!slots[static_cast<size_t>(i)].gpu) {
            run.error = "GPU slot allocation failed";
            for (SlotState& slot : slots) HyperCine_GpuSlotDestroy(slot.gpu);
            return run;
        }
    }

    const void* device_bgra = nullptr;
    bool registered = false;
    if (config.input == Phase2Input::DeviceBgra) {
        if (HyperCine_GpuSlotUploadBgra(slots[0].gpu, bgra, pitch, width, height, &device_bgra) != 0) {
            run.error = "device BGRA upload failed";
            for (SlotState& slot : slots) HyperCine_GpuSlotDestroy(slot.gpu);
            return run;
        }
    } else if (HyperCine_RegisterHost(const_cast<uint8_t*>(bgra), static_cast<size_t>(pitch) * static_cast<size_t>(height)) == 0) {
        registered = true;
    }
    const uint8_t* source = config.input == Phase2Input::DeviceBgra
        ? static_cast<const uint8_t*>(device_bgra) : bgra;

    Pool pool;
    pool.slots = slots.data();
    pool.enc = enc_opaque;
    pool.transforms = transforms_opaque;
    pool.threads.reserve(static_cast<size_t>(run.workers));
    for (int i = 0; i < run.workers; ++i) pool.threads.emplace_back(WorkerMain, &pool);

    auto fail = [&](const std::string& message) {
        if (run.error.empty()) run.error = message;
        pool.failed.store(1);
    };

    auto submit = [&](int slot_index, int frame_index) {
        SlotState& slot = slots[static_cast<size_t>(slot_index)];
        if (HyperCine_GpuSlotBegin(slot.gpu, InputKind(config.input), TransportKind(config.transport),
                                   source, pitch, width, height, quant.color_space,
                                   quant.widths, quant.heights, quant.prescale, quant.quant) != 0) {
            fail("GPU frame begin failed");
            return;
        }
        slot.frame_index = frame_index;
        slot.expected = static_cast<int>(refs.size());
        slot.encoded.store(0, std::memory_order_relaxed);
        slot.polled = 0;
        slot.gpu_done = false;
        slot.timing_ready = false;
        slot.submit_ms = NowMs();
        slot.last_entropy_ms = slot.submit_ms;
        slot.schedule_sum = 0;
        slot.schedule_n = 0;
        slot.ready_sum = 0;
        slot.ready_samples = 0;
        slot.max_ready = 0;
        slot.pieces.assign(refs.size(), EncodedPiece{});
        slot.entropy_iv.clear();
        slot.d2h_iv.clear();
        slot.timing = HyperCineStreamTiming{};
    };

    auto dispatch = [&](int slot_index, const HyperCineReadyBand& band) {
        if (band.channel < 0 || band.channel >= 3 || band.level < 0 || band.level >= 3 || band.band < 0 || band.band >= 4) {
            fail("GPU band index is outside the pyramid");
            return;
        }
        const int piece = piece_of[band.channel][band.level][band.band];
        if (piece < 0) {
            fail("GPU band has no entropy job");
            return;
        }
        const BandRef& ref = refs[static_cast<size_t>(piece)];
        if (band.width != ref.width || band.height != ref.height) {
            fail("GPU band dimensions do not match the encoder wavelet");
            return;
        }
        WorkItem work;
        work.slot = slot_index;
        work.piece = piece;
        work.ready_ms = NowMs();
        work.copy_ms = band.copy_ms;
        work.coeff.channel = ref.channel;
        work.coeff.level = ref.level;
        work.coeff.band = ref.band;
        work.coeff.subband = ref.subband;
        work.coeff.lowpass = ref.lowpass;
        work.coeff.data = band.data;
        work.coeff.elem_bytes = band.elem_bytes;
        work.coeff.width = band.width;
        work.coeff.height = band.height;
        work.coeff.value_min = band.value_min;
        work.coeff.value_max = band.value_max;
        SlotState& slot = slots[static_cast<size_t>(slot_index)];
        int depth = 0;
        {
            std::lock_guard<std::mutex> lock(pool.mu);
            pool.queue.push_back(work);
            depth = static_cast<int>(pool.queue.size());
        }
        pool.cv.notify_one();
        slot.ready_sum += depth;
        ++slot.ready_samples;
        slot.max_ready = std::max(slot.max_ready, depth);
        ++slot.polled;
    };

    auto finish_frame = [&](int slot_index, bool record, std::vector<Phase2Frame>* out) {
        SlotState& slot = slots[static_cast<size_t>(slot_index)];
        if (!slot.timing_ready) {
            if (HyperCine_GpuSlotReadTiming(slot.gpu, &slot.timing) != 0 && run.error.empty()) {
                fail("GPU timing read failed");
            }
            slot.timing_ready = true;
        }
        const double assemble0 = NowMs();
        EntropyOutput assembled = AssembleEncodedPieces(enc_opaque, transforms_opaque, channel_count, slot.pieces, false);
        const double assemble1 = NowMs();
        Phase2Frame frame;
        frame.latency_ms = assemble1 - slot.submit_ms;
        frame.color_ms = slot.timing.color_ms;
        frame.h2d_ms = slot.timing.h2d_ms;
        frame.gpu_ms = slot.timing.compute_ms;
        frame.range_ms = slot.timing.range_ms;
        frame.pack_ms = slot.timing.pack_ms;
        frame.d2h_ms = slot.timing.d2h_ms;
        frame.assembly_ms = assembled.assembly_ms;
        frame.overlap_gpu_d2h_ms = slot.timing.overlap_compute_d2h_ms;
        frame.h2d_bytes = slot.timing.h2d_bytes;
        frame.d2h_bytes = slot.timing.d2h_bytes;
        frame.encoded_bytes = assembled.bitstream_bytes;
        frame.int8_bands = slot.timing.int8_bands;
        frame.int16_bands = slot.timing.int16_bands;
        {
            std::lock_guard<std::mutex> lock(slot.mu);
            frame.schedule_ms = slot.schedule_n > 0 ? slot.schedule_sum / slot.schedule_n : 0;
            frame.entropy_span_ms = UnionLength(slot.entropy_iv);
            frame.entropy_ms = frame.entropy_span_ms;
            frame.overlap_d2h_entropy_ms = IntersectionLength(slot.d2h_iv, slot.entropy_iv);
            frame.assembly_delay_ms = std::max(0.0, assemble1 - slot.last_entropy_ms);
            frame.mean_ready_bands = slot.ready_samples > 0 ? slot.ready_sum / slot.ready_samples : 0;
            frame.max_ready_bands = slot.max_ready;
        }
        if (!assembled.success) fail(assembled.error.empty() ? "assembly failed" : assembled.error);
        else run.bitstream = assembled.bitstream;
        if (record && out) out->push_back(frame);
        slot.frame_index = -1;
    };

    auto drive = [&](int count, bool record, std::vector<Phase2Frame>* out) {
        int submitted = 0;
        int retired = 0;
        int idle_spins = 0;
        while (retired < count && pool.failed.load() == 0 && run.error.empty()) {
            bool progress = false;
            for (int s = 0; s < run.pipeline_depth && submitted < count; ++s) {
                if (slots[static_cast<size_t>(s)].frame_index < 0) {
                    submit(s, submitted);
                    ++submitted;
                    progress = true;
                }
            }
            for (int s = 0; s < run.pipeline_depth; ++s) {
                SlotState& slot = slots[static_cast<size_t>(s)];
                if (slot.frame_index < 0) continue;
                if (!slot.gpu_done) {
                    if (HyperCine_GpuSlotKick(slot.gpu) != 0) {
                        fail("GPU kick failed");
                        break;
                    }
                    const int idle = HyperCine_GpuSlotGpuIdle(slot.gpu);
                    if (idle < 0) {
                        fail("GPU stream failed");
                        break;
                    }
                    if (idle == 1) {
                        if (HyperCine_GpuSlotReadTiming(slot.gpu, &slot.timing) != 0) fail("GPU timing read failed");
                        slot.timing_ready = true;
                        slot.gpu_done = true;
                        progress = true;
                    }
                }
                HyperCineReadyBand band{};
                int polled = HyperCine_GpuSlotPollBand(slot.gpu, &band);
                while (polled == 1) {
                    dispatch(s, band);
                    progress = true;
                    polled = HyperCine_GpuSlotPollBand(slot.gpu, &band);
                }
                if (polled < 0) {
                    fail("band transport rejected an out-of-range coefficient");
                    break;
                }
                if (slot.polled == slot.expected &&
                    slot.encoded.load(std::memory_order_acquire) == slot.expected) {
                    finish_frame(s, record, out);
                    ++retired;
                    progress = true;
                }
            }
            if (!progress) {
                if (++idle_spins > 32) std::this_thread::yield();
            } else {
                idle_spins = 0;
            }
        }
    };

    const int warmup = std::max(0, config.warmup);
    if (warmup > 0) drive(warmup, false, nullptr);
    const double wall0 = NowMs();
    const double cpu0 = ProcessCpuSeconds();
    drive(config.frames, true, &run.frame_times);
    const double wall1 = NowMs();
    run.cpu_seconds = ProcessCpuSeconds() - cpu0;
    run.wall_ms = wall1 - wall0;

    if (config.verify && run.error.empty() && pool.failed.load() == 0) {
        run.verified = true;
        submit(0, config.frames + warmup);
        std::vector<HyperCineReadyBand> ready;
        while (slots[0].frame_index >= 0 && run.error.empty()) {
            if (HyperCine_GpuSlotKick(slots[0].gpu) != 0) {
                fail("verify kick failed");
                break;
            }
            HyperCineReadyBand band{};
            int polled = HyperCine_GpuSlotPollBand(slots[0].gpu, &band);
            while (polled == 1) {
                ready.push_back(band);
                polled = HyperCine_GpuSlotPollBand(slots[0].gpu, &band);
            }
            if (polled < 0) {
                fail("verify transport rejected an out-of-range coefficient");
                break;
            }
            if (HyperCine_GpuSlotGpuIdle(slots[0].gpu) == 1 && static_cast<int>(ready.size()) >= static_cast<int>(refs.size())) break;
            if (HyperCine_GpuSlotGpuIdle(slots[0].gpu) < 0) {
                fail("verify GPU failed");
                break;
            }
            std::this_thread::yield();
        }
        struct SavedBand {
            int channel, level, band, width, height;
            std::vector<int16_t> samples;
        };
        std::vector<SavedBand> saved;
        for (int index = 0; run.error.empty(); ++index) {
            int w = 0, h = 0, channel = 0, level = 0, band = 0;
            std::vector<int16_t> tmp(static_cast<size_t>(width) * static_cast<size_t>(height));
            const int rc = HyperCine_GpuSlotDownloadInt16(slots[0].gpu, index, tmp.data(), static_cast<int>(tmp.size()),
                                                          &w, &h, &channel, &level, &band);
            if (rc == -4) break;
            if (rc != 0) {
                fail("int16 coefficient download failed");
                break;
            }
            tmp.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
            saved.push_back(SavedBand{channel, level, band, w, h, std::move(tmp)});
        }
        for (const HyperCineReadyBand& band : ready) {
            const SavedBand* match = nullptr;
            for (const SavedBand& item : saved) {
                if (item.channel == band.channel && item.level == band.level && item.band == band.band) match = &item;
            }
            if (!match) {
                fail("compact band has no int16 reference");
                break;
            }
            if (band.elem_bytes == 1) {
                const auto* packed = static_cast<const int8_t*>(band.data);
                for (int i = 0; i < band.width * band.height; ++i) {
                    const int value = static_cast<int>(match->samples[static_cast<size_t>(i)]);
                    ++run.coeff_compared;
                    if (value < -128 || value > 127 || static_cast<int>(packed[i]) != value) ++run.expand_mismatches;
                }
            }
        }
        if (run.error.empty() && scratch && scratch_bytes > 0) {
            if (!RefreshUpstreamPyramid(enc_opaque, transforms_opaque, channel_count, bgra, pitch, scratch, scratch_bytes)) {
                fail("upstream pyramid refresh failed");
            } else {
                for (const SavedBand& item : saved) {
                    IMAGE* wavelet = transforms[item.channel]->wavelet[item.level];
                    const int compared = item.width * item.height;
                    run.coeff_compared += compared;
                    run.coeff_mismatches += CountDiff(wavelet, item.band, item.samples.data(), item.width, item.height);
                }
            }
        }
        for (const SavedBand& item : saved) {
            IMAGE* wavelet = transforms[item.channel] ? transforms[item.channel]->wavelet[item.level] : nullptr;
            if (wavelet) StoreBand(wavelet, item.band, item.samples.data(), item.width, item.height);
        }
        if (run.error.empty()) {
            for (const HyperCineReadyBand& band : ready) dispatch(0, band);
            while (run.error.empty() && slots[0].encoded.load(std::memory_order_acquire) < slots[0].expected) {
                std::this_thread::yield();
            }
            if (run.error.empty()) {
                EntropyOutput check = AssembleEncodedPieces(enc_opaque, transforms_opaque, channel_count, slots[0].pieces, true);
                run.byte_match = check.byte_match && run.expand_mismatches == 0 && run.coeff_mismatches == 0;
                if (check.success) run.bitstream = std::move(check.bitstream);
                else fail(check.error.empty() ? "verify assembly failed" : check.error);
                if (!check.byte_match && run.error.empty()) fail("bitstream differs from EncodeQuantizedGroup");
                slots[0].frame_index = -1;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(pool.mu);
        pool.stop = true;
    }
    pool.cv.notify_all();
    for (std::thread& thread : pool.threads) thread.join();
    if (registered) HyperCine_UnregisterHost(const_cast<uint8_t*>(bgra));
    for (SlotState& slot : slots) HyperCine_GpuSlotDestroy(slot.gpu);

    if (!run.frame_times.empty() && run.error.empty() && pool.failed.load() == 0) {
        double lat = 0, color = 0, h2d = 0, gpu = 0, range = 0, pack = 0, d2h = 0, ent = 0, assembly = 0;
        double schedule = 0, overlap_g = 0, overlap_e = 0, delay = 0, ready = 0;
        for (const Phase2Frame& frame : run.frame_times) {
            lat += frame.latency_ms;
            color += frame.color_ms;
            h2d += frame.h2d_ms;
            gpu += frame.gpu_ms;
            range += frame.range_ms;
            pack += frame.pack_ms;
            d2h += frame.d2h_ms;
            ent += frame.entropy_ms;
            assembly += frame.assembly_ms;
            schedule += frame.schedule_ms;
            overlap_g += frame.overlap_gpu_d2h_ms;
            overlap_e += frame.overlap_d2h_entropy_ms;
            delay += frame.assembly_delay_ms;
            ready += frame.mean_ready_bands;
            run.h2d_bytes = frame.h2d_bytes;
            run.d2h_bytes = frame.d2h_bytes;
            run.encoded_bytes = frame.encoded_bytes;
        }
        const double n = static_cast<double>(run.frame_times.size());
        run.frames = static_cast<int>(n);
        run.mean_latency_ms = lat / n;
        run.mean_color_ms = color / n;
        run.mean_h2d_ms = h2d / n;
        run.mean_gpu_ms = gpu / n;
        run.mean_range_ms = range / n;
        run.mean_pack_ms = pack / n;
        run.mean_d2h_ms = d2h / n;
        run.mean_entropy_ms = ent / n;
        run.mean_assembly_ms = assembly / n;
        run.mean_schedule_ms = schedule / n;
        run.mean_overlap_gpu_d2h_ms = overlap_g / n;
        run.mean_overlap_d2h_entropy_ms = overlap_e / n;
        run.mean_assembly_delay_ms = delay / n;
        run.mean_ready_bands = ready / n;
        if (run.wall_ms > 0) {
            run.sustained_fps = 1000.0 * n / run.wall_ms;
            run.cpu_cores = run.cpu_seconds / (run.wall_ms / 1000.0);
        }
        run.success = !config.verify || (run.byte_match && run.coeff_mismatches == 0 && run.expand_mismatches == 0);
        if (!config.verify) run.success = true;
    }
    if (pool.failed.load() != 0 && run.error.empty()) run.error = "an entropy worker failed";
    if (!run.error.empty()) run.success = false;
    return run;
}

}  // namespace hypercine
