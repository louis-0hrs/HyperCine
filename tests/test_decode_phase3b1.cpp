// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Phase 3B-1 smoke: GOP-1 interlaced YUV422 p10 against the upstream CPU decoder.
// --smoke checks the committed 64x64 fixture and the frozen 1920x1080 recipe.
#include "decoder_support.h"
#include "decode_phase1_support.h"
#include "hypercine/research/decode_phase2e.h"
#include "../src/core/decoder_backend.h"
extern "C" {
#include "spatial.h"
#include "temporal.h"
#include "convert.h"
#include "bayer.h"
#include "RGB2YUV.h"
}
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace ProductionDecodeTest;
using HyperCineTest::AlignedVector;

void RequireMsg(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

Bytes LoadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    RequireMsg(bool(in), "open " + path.string());
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    in.seekg(0);
    Bytes bytes(static_cast<size_t>(n));
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    RequireMsg(bool(in), "read " + path.string());
    return bytes;
}

std::string CoeffHash(DECODER* d) {
    uint64_t hash = 14695981039346656037ULL;
    int count = 0;
    const int channels = (std::max)(0, (std::min)(d->codec.num_channels, 3));
    for (int ch = 0; ch < channels; ++ch) {
        TRANSFORM* t = d->transform[ch];
        if (!t) continue;
        for (int wi = 0; wi < TRANSFORM_MAX_WAVELETS; ++wi) {
            IMAGE* image = t->wavelet[wi];
            if (!image) continue;
            for (int b = 0; b < image->num_bands && b < IMAGE_NUM_BANDS; ++b) {
                const PIXEL* row = image->band[b];
                if (!row || image->width <= 0 || image->height <= 0 || image->pitch <= 0) continue;
                const int stride = image->pitch / int(sizeof(PIXEL));
                if (stride < image->width) continue;
                for (int y = 0; y < image->height; ++y) for (int x = 0; x < image->width; ++x) {
                    hash ^= uint64_t(uint16_t(row[y * stride + x]));
                    hash *= 1099511628211ULL;
                    ++count;
                }
            }
        }
    }
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << hash;
    (void)count;
    return s.str();
}

int16_t SatAdd(int16_t a, int16_t b) {
    const int32_t s = int32_t(a) + int32_t(b);
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return int16_t(s);
}
int16_t SatSub(int16_t a, int16_t b) {
    const int32_t s = int32_t(a) - int32_t(b);
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return int16_t(s);
}

void ReportMismatch(Decoder& d, DecodeTicket t, const Sample& s, const Bytes& gpu, const Bytes& cpu) {
    DeviceOutput surface;
    Require(d.GetDeviceOutput(t, &surface), "mismatch surface");
    DecoderReconstruction reconstruction;
    Require(DecoderInspector::Reconstruction(d, t, &reconstruction), "mismatch planes");
    std::array<AlignedVector<int16_t>, 3> planes;
    int outside = 0, negative = 0, total = 0;
    for (int ch = 0; ch < 3; ++ch) {
        const auto& p = reconstruction.planes[ch];
        planes[ch].resize(size_t(p.width) * p.height);
        GpuCheck(cudaMemcpy2D(planes[ch].data(), size_t(p.width) * 2, p.device_pointer, p.pitch_bytes,
                              size_t(p.width) * 2, p.height, cudaMemcpyDeviceToHost));
        for (auto v : planes[ch]) {
            ++total;
            if (v < 0) ++negative;
            if (v < 0 || v > 1023) ++outside;
        }
    }
    int mismatches = 0, in_range = 0, out_range = 0;
    std::ostringstream sample;
    for (size_t i = 0; i + 3 < gpu.size() && i + 3 < cpu.size(); i += 4) {
        if (std::memcmp(gpu.data() + i, cpu.data() + i, 4) == 0) continue;
        const int x = int((i / 4) % s.width);
        const int out_row = int((i / 4) / s.width);
        const int row = s.height - 1 - out_row;
        const int yv = planes[0][size_t(row) * s.width + x];
        const int uv = planes[1][size_t(row) * (s.width / 2) + x / 2];
        const int vv = planes[2][size_t(row) * (s.width / 2) + x / 2];
        const bool outside_pix = yv < 0 || yv > 1023 || uv < 0 || uv > 1023 || vv < 0 || vv > 1023;
        ++mismatches;
        if (outside_pix) ++out_range; else ++in_range;
        if (mismatches <= 6) {
            sample << " x=" << x << " out=" << out_row << " row=" << row
                   << " gpu=" << int(gpu[i]) << ',' << int(gpu[i + 1]) << ',' << int(gpu[i + 2])
                   << " cpu=" << int(cpu[i]) << ',' << int(cpu[i + 1]) << ',' << int(cpu[i + 2])
                   << " YUV=" << yv << ',' << uv << ',' << vv << (outside_pix ? " OOR" : " in") << "\n";
        }
    }
    throw std::runtime_error("BGRA mismatches=" + std::to_string(mismatches) + " in_range=" +
        std::to_string(in_range) + " oor=" + std::to_string(out_range) + " neg=" + std::to_string(negative) +
        "\n" + sample.str());
}

int MaxAbs(const int16_t* a, const int16_t* b, size_t n) {
    int m = 0;
    for (size_t i = 0; i < n; ++i) m = (std::max)(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

void Diagnose(Decoder& d, DecodeTicket t, Sample& s, DECODER* native) {
    const auto ref = HyperCine::DecodePhase1::ComputeCpuReferenceIntermediates(native);
    std::cout << "  ll0";
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<int16_t> ll;
        Require(DecoderInspector::DownloadIntermediate(d, t, ch, 0, ll), "ll0 download");
        std::cout << " ch" << ch << "=" << MaxAbs(ll.data(), ref.ll0[ch].data(), ll.size());
    }
    std::cout << "\n";
    DecoderReconstruction reconstruction;
    Require(DecoderInspector::Reconstruction(d, t, &reconstruction), "diag planes");
    for (int ch = 0; ch < 3; ++ch) {
        auto* w0 = native->transform[ch]->wavelet[0];
        const int lw = w0->width, lh = w0->height, stride = w0->pitch / int(sizeof(PIXEL));
        const int ow = lw * 2;
        std::vector<int16_t> low(size_t(ow) * lh), high(size_t(ow) * lh), plane(size_t(ow) * lh * 2), swapped(plane.size());
        const int16_t* ll = ref.ll0[ch].data();
        int stored_ll = 0;
        if (w0->band[0]) {
            for (int y = 0; y < lh; ++y) for (int x = 0; x < lw; ++x)
                stored_ll = (std::max)(stored_ll, std::abs(int(w0->band[0][size_t(y) * stride + x]) - int(ll[size_t(y) * lw + x])));
        }
        std::cout << "  stored_ll ch" << ch << "=" << stored_ll
                  << " q=" << w0->quantization[0] << ',' << w0->quantization[1] << ','
                  << w0->quantization[2] << ',' << w0->quantization[3] << "\n";
        for (int row = 0; row < lh; ++row) {
            InvertHorizontalRow16s(const_cast<PIXEL*>(reinterpret_cast<const PIXEL*>(ll + size_t(row) * lw)),
                w0->band[1] + size_t(row) * stride, low.data() + size_t(row) * ow, lw);
            InvertHorizontalRow16s(w0->band[2] + size_t(row) * stride, w0->band[3] + size_t(row) * stride,
                high.data() + size_t(row) * ow, lw);
        }
        for (int r = 0; r < lh; ++r) for (int c = 0; c < ow; ++c) {
            const int16_t lo = low[size_t(r) * ow + c];
            const int16_t hi = high[size_t(r) * ow + c];
            plane[size_t(2 * r) * ow + c] = int16_t(int32_t(SatSub(lo, hi)) >> 1);
            plane[size_t(2 * r + 1) * ow + c] = int16_t(int32_t(SatAdd(lo, hi)) >> 1);
            swapped[size_t(2 * r) * ow + c] = int16_t(int32_t(SatAdd(lo, hi)) >> 1);
            swapped[size_t(2 * r + 1) * ow + c] = int16_t(int32_t(SatSub(lo, hi)) >> 1);
        }
        const int gpu_plane = ch == 0 ? 0 : ch == 1 ? 2 : 1;
        const auto& p = reconstruction.planes[gpu_plane];
        AlignedVector<int16_t> gpu(size_t(p.width) * p.height);
        GpuCheck(cudaMemcpy2D(gpu.data(), size_t(p.width) * 2, p.device_pointer, p.pitch_bytes,
                              size_t(p.width) * 2, p.height, cudaMemcpyDeviceToHost));
        const int spatial = MaxAbs(gpu.data(), ref.plane[ch].data(), gpu.size());
        const int field = MaxAbs(gpu.data(), plane.data(), (std::min)(gpu.size(), plane.size()));
        const int flip = MaxAbs(gpu.data(), swapped.data(), (std::min)(gpu.size(), swapped.size()));
        int shown = 0;
        std::cout << "  ch" << ch << " spatial=" << spatial << " field=" << field << " swapped=" << flip
                  << " gpu0=" << gpu[0] << " cpu0=" << plane[0] << " ll=" << ll[0]
                  << " hp=" << w0->band[1][0] << "," << w0->band[2][0] << "," << w0->band[3][0] << "\n";
        if (field) {
            for (size_t i = 0; i < plane.size() && shown < 4; ++i) if (gpu[i] != plane[i]) {
                std::cout << "    diff i=" << i << " gpu=" << gpu[i] << " cpu=" << plane[i] << "\n";
                ++shown;
            }
        }
    }
}

void ExactFrame(Decoder& d, Sample& s, const char* label) {
    ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA);
    cpu.Decode(s);
    const auto t = Begin(d, s);
    Diagnose(d, t, s, cpu.GetNative());
    const auto gpu = Image(d, t, s);
    Bytes flipped(cpu.output.size());
    const size_t stride = size_t(s.width) * 4;
    for (int y = 0; y < s.height; ++y)
        std::memcpy(flipped.data() + size_t(y) * stride, cpu.output.data() + size_t(s.height - 1 - y) * stride, stride);
    auto diffs = [](const Bytes& a, const Bytes& b) {
        int n = 0;
        for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
            n += std::memcmp(a.data() + i, b.data() + i, 4) != 0;
        return n;
    };
    DecodeRequest top; top.orientation = DecodeOrientation::TopDown;
    const auto t_top = Begin(d, s, top);
    const auto gpu_top = Image(d, t_top, s);
    {
        ReferenceDecoder yu64(s, CFHD_PIXEL_FORMAT_YU64);
        yu64.Decode(s);
        DecoderReconstruction reconstruction;
        Require(DecoderInspector::Reconstruction(d, t, &reconstruction), "yu64 planes");
        std::array<AlignedVector<int16_t>, 3> planes;
        for (int ch = 0; ch < 3; ++ch) {
            const auto& p = reconstruction.planes[ch];
            planes[ch].resize(size_t(p.width) * p.height);
            GpuCheck(cudaMemcpy2D(planes[ch].data(), size_t(p.width) * 2, p.device_pointer, p.pitch_bytes,
                                  size_t(p.width) * 2, p.height, cudaMemcpyDeviceToHost));
        }
        // Real upstream field combine on the same horizontal rows the replica uses.
        // Row16u clamps to [0,2047], arithmetic >> 1, then << 6. Compare >> 6 to the GPU.
        auto* native = yu64.GetNative();
        const auto ref = HyperCine::DecodePhase1::ComputeCpuReferenceIntermediates(native);
        std::array<AlignedVector<int16_t>, 3> upstream_code;
        upstream_code[0].assign(size_t(s.width) * s.height, -9999);
        upstream_code[1].assign(size_t(s.width / 2) * s.height, -9999);
        upstream_code[2].assign(size_t(s.width / 2) * s.height, -9999);
        int formula_vs_real = 0, shown_real = 0;
        std::cout << "  precision=" << native->codec.precision
                  << " chroma_offset=" << native->codec.chroma_offset << "\n";
        for (int ch = 0; ch < 3; ++ch) {
            auto* w0 = native->transform[ch]->wavelet[0];
            const int lw = w0->width, lh = w0->height, stride = w0->pitch / int(sizeof(PIXEL));
            const int ow = lw * 2;
            const int plane = ch == 0 ? 0 : ch == 1 ? 2 : 1;
            AlignedVector<PIXEL> low_row(static_cast<size_t>(ow));
            AlignedVector<PIXEL> high_row(static_cast<size_t>(ow));
            AlignedVector<PIXEL16U> row16(static_cast<size_t>(ow) * 2);
            const int row_pitch = ow * int(sizeof(PIXEL16U));
            const int16_t* ll = ref.ll0[ch].data();
            for (int r = 0; r < lh; ++r) {
                InvertHorizontalRow16s(const_cast<PIXEL*>(reinterpret_cast<const PIXEL*>(ll + size_t(r) * lw)),
                    w0->band[1] + size_t(r) * stride, low_row.data(), lw);
                InvertHorizontalRow16s(w0->band[2] + size_t(r) * stride, w0->band[3] + size_t(r) * stride,
                    high_row.data(), lw);
                std::memset(row16.data(), 0, row16.size() * sizeof(PIXEL16U));
                InvertInterlacedRow16sToRow16u(low_row.data(), high_row.data(), row16.data(), row_pitch, ow, ow,
                    native->codec.chroma_offset, native->codec.precision);
                for (int c = 0; c < ow; ++c) {
                    const int even_f = int32_t(SatSub(low_row[c], high_row[c])) >> 1;
                    const int odd_f = int32_t(SatAdd(low_row[c], high_row[c])) >> 1;
                    const int even_r = int(row16[c]) >> 6;
                    const int odd_r = int(row16[size_t(ow) + c]) >> 6;
                    if (even_f != even_r || odd_f != odd_r) {
                        ++formula_vs_real;
                        if (shown_real < 4) {
                            std::cout << "  real!=formula ch" << ch << " r=" << r << " c=" << c
                                      << " low=" << low_row[c] << " high=" << high_row[c]
                                      << " formula=" << even_f << ',' << odd_f
                                      << " real=" << even_r << ',' << odd_r
                                      << " raw=" << row16[c] << ',' << row16[size_t(ow) + c] << "\n";
                            ++shown_real;
                        }
                    }
                    upstream_code[plane][size_t(2 * r) * ow + c] = int16_t(even_r);
                    upstream_code[plane][size_t(2 * r + 1) * ow + c] = int16_t(odd_r);
                }
            }
        }
        std::cout << "  formula_vs_real=" << formula_vs_real << "\n";
        {
            auto* bgra_native = cpu.GetNative();
            int band_max = 0, band_n = 0, band_diff = 0;
            for (int ch = 0; ch < 3; ++ch) {
                for (int wi = 0; wi < 3; ++wi) {
                    auto* a = bgra_native->transform[ch]->wavelet[wi];
                    auto* b = native->transform[ch]->wavelet[wi];
                    if (!a || !b) continue;
                    const int bands = (std::min)(a->num_bands, b->num_bands);
                    for (int bi = 0; bi < bands; ++bi) {
                        if (!a->band[bi] || !b->band[bi]) continue;
                        const int as = a->pitch / int(sizeof(PIXEL));
                        const int bs = b->pitch / int(sizeof(PIXEL));
                        const int w = (std::min)(a->width, b->width);
                        const int h = (std::min)(a->height, b->height);
                        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                            const int d = std::abs(int(a->band[bi][y * as + x]) - int(b->band[bi][y * bs + x]));
                            ++band_n;
                            if (d) ++band_diff;
                            band_max = (std::max)(band_max, d);
                        }
                    }
                }
            }
            std::cout << "  wavelet bgra-vs-yu64 n=" << band_n << " diff=" << band_diff
                      << " max=" << band_max << "\n";
            for (int ch = 0; ch < 3; ++ch) {
                auto* a = bgra_native->transform[ch]->wavelet[2];
                auto* b = native->transform[ch]->wavelet[2];
                const int as = a->pitch / int(sizeof(PIXEL));
                const int bs = b->pitch / int(sizeof(PIXEL));
                std::cout << "  w2ch" << ch << " q=" << a->quantization[0] << "/" << b->quantization[0] << " ";
                for (int i = 0; i < 8; ++i) {
                    const int av = a->band[0][i];
                    const int bv = b->band[0][(i % a->width) + (i / a->width) * as];
                    const int bb = b->band[0][i < b->width ? i : 0];
                    (void)bs; (void)bv;
                    std::cout << av << ":" << bb << " ";
                }
                std::cout << "\n";
            }
            std::vector<int16_t> gpu_ll;
            int gw = 0, gh = 0, gp = 0;
            if (DecoderInspector::DownloadBand(d, t, 0, gpu_ll, &gw, &gh, &gp)) {
                auto* a = bgra_native->transform[0]->wavelet[2];
                auto* b = native->transform[0]->wavelet[2];
                int da = 0, db = 0;
                const int as = a->pitch / int(sizeof(PIXEL));
                const int bs = b->pitch / int(sizeof(PIXEL));
                const int gs = gp / 2;
                for (int y = 0; y < gh && y < a->height; ++y) for (int x = 0; x < gw && x < a->width; ++x) {
                    const int g = gpu_ll[size_t(y) * gs + x];
                    da = (std::max)(da, std::abs(g - int(a->band[0][y * as + x])));
                    db = (std::max)(db, std::abs(g - int(b->band[0][y * bs + x])));
                }
                std::cout << "  gpuLL vs bgra=" << da << " vs yu64=" << db << "\n";
            }
            for (int ch = 0; ch < 3; ++ch) {
                for (int wi = 0; wi < 3; ++wi) {
                    auto* a = bgra_native->transform[ch]->wavelet[wi];
                    auto* b = native->transform[ch]->wavelet[wi];
                    if (!a || !b) continue;
                    const int bands = (std::min)(a->num_bands, b->num_bands);
                    for (int bi = 0; bi < bands; ++bi) {
                        if (!a->band[bi] || !b->band[bi]) continue;
                        const int as = a->pitch / int(sizeof(PIXEL));
                        const int bs = b->pitch / int(sizeof(PIXEL));
                        const int w = (std::min)(a->width, b->width);
                        const int h = (std::min)(a->height, b->height);
                        int dcount = 0, dmax = 0;
                        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                            const int d = std::abs(int(a->band[bi][y * as + x]) - int(b->band[bi][y * bs + x]));
                            if (d) ++dcount;
                            dmax = (std::max)(dmax, d);
                        }
                        if (dcount)
                            std::cout << "    ch" << ch << " w" << wi << " b" << bi
                                      << " " << w << "x" << h << " diff=" << dcount << " max=" << dmax << "\n";
                    }
                }
            }
        }
        int yu_vs_real = 0;
        for (int plane = 0; plane < 3; ++plane) {
            const int pw = plane == 0 ? s.width : s.width / 2;
            for (int y = 0; y < s.height; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(yu64.output.data() + size_t(y) * yu64.pitch);
                for (int x = 0; x < pw; ++x) {
                    int got = 0;
                    if (plane == 0) got = row[(x & ~1) * 2 + (x & 1) * 2] >> 6;
                    else if (plane == 2) got = row[(x * 2) * 2 + 1] >> 6;
                    else got = row[(x * 2) * 2 + 3] >> 6;
                    if (got != upstream_code[plane][size_t(y) * pw + x]) ++yu_vs_real;
                }
            }
        }
        std::cout << "  yu64_vs_replay=" << yu_vs_real << "\n";
        auto dump_hist = [&](const char* name, bool flip) {
            int hist[17] = {};
            int outliers = 0, n = 0, exact = 0;
            int min_d = 999, max_d = -999;
            for (int plane = 0; plane < 3; ++plane) {
                const int pw = plane == 0 ? s.width : s.width / 2;
                for (int y = 0; y < s.height; ++y) {
                    const int sy = flip ? s.height - 1 - y : y;
                    for (int x = 0; x < pw; ++x) {
                        const int gpu = planes[plane][size_t(y) * pw + x];
                        const int up = upstream_code[plane][size_t(sy) * pw + x];
                        const int d = gpu - up;
                        ++n;
                        if (d == 0) ++exact;
                        if (d < min_d) min_d = d;
                        if (d > max_d) max_d = d;
                        if (d >= -8 && d <= 8) ++hist[d + 8];
                        else ++outliers;
                    }
                }
            }
            std::cout << "  " << name << " n=" << n << " exact=" << exact
                      << " min=" << min_d << " max=" << max_d << " out=" << outliers << " hist";
            for (int i = 0; i < 17; ++i) if (hist[i]) std::cout << " " << (i - 8) << ":" << hist[i];
            std::cout << "\n";
        };
        dump_hist("gpu-vs-row16u", false);
        dump_hist("gpu-vs-row16u-flip", true);
        int match_same = 0, match_flip = 0, words = 0, shown = 0;
        int hist[17] = {};
        int outliers = 0;
        for (int y = 0; y < s.height; ++y) {
            const auto* row = reinterpret_cast<const uint16_t*>(yu64.output.data() + size_t(y) * yu64.pitch);
            const int fy = s.height - 1 - y;
            for (int x = 0; x < s.width; x += 2) {
                auto pack = [&](int row_y, uint16_t out[4]) {
                    const int y0 = planes[0][size_t(row_y) * s.width + x];
                    const int y1 = planes[0][size_t(row_y) * s.width + x + 1];
                    const int u = planes[1][size_t(row_y) * (s.width / 2) + x / 2];
                    const int v = planes[2][size_t(row_y) * (s.width / 2) + x / 2];
                    auto clamp6 = [](int v) {
                        if (v < 0) v = 0;
                        if (v > 1023) v = 1023;
                        return uint16_t(v << 6);
                    };
                    out[0] = clamp6(y0); out[1] = clamp6(v); out[2] = clamp6(y1); out[3] = clamp6(u);
                };
                uint16_t same[4], flipped[4];
                pack(y, same);
                pack(fy, flipped);
                const uint16_t got[4] = {row[x * 2], row[x * 2 + 1], row[x * 2 + 2], row[x * 2 + 3]};
                match_same += std::memcmp(got, same, 8) == 0;
                match_flip += std::memcmp(got, flipped, 8) == 0;
                ++words;
                for (int k = 0; k < 4; ++k) {
                    const int d = int(got[k] >> 6) - int(same[k] >> 6);
                    if (d >= -8 && d <= 8) ++hist[d + 8];
                    else ++outliers;
                }
                if (shown < 2 && std::memcmp(got, same, 8)) {
                    std::cout << "  yu64 y=" << y << " x=" << x
                              << " got=" << (got[0] >> 6) << ',' << (got[1] >> 6) << ',' << (got[2] >> 6) << ',' << (got[3] >> 6)
                              << " gpu=" << (same[0] >> 6) << ',' << (same[1] >> 6) << ',' << (same[2] >> 6) << ',' << (same[3] >> 6)
                              << " raw=" << got[0] << ',' << got[1] << ',' << got[2] << ',' << got[3] << "\n";
                    ++shown;
                }
            }
        }
        auto color15 = [](int code_y, int code_u, int code_v, int y_off, int ymult, int rv, int gv, int gu, int bu) {
            auto sat = [](int v) {
                if (v > 32767) return 32767;
                if (v < -32768) return -32768;
                return v;
            };
            auto clamp10 = [](int v) {
                if (v < 0) return 0;
                if (v > 1023) return 1023;
                return v;
            };
            auto mulhi = [](int a, int b) {
                return int(int16_t((int32_t(int16_t(a)) * int32_t(int16_t(b))) >> 16));
            };
            int y = clamp10(code_y) << 6;
            int u = clamp10(code_u) << 6;
            int v = clamp10(code_v) << 6;
            y >>= 1; u >>= 1; v >>= 1;
            int uu = sat(u - 16384);
            int vv = sat(v - 16384);
            int yy = sat(y - (y_off << 7));
            yy = mulhi(yy, ymult);
            yy = int16_t(yy << 2);
            auto chan = [&](int acc) {
                int s = acc >> 1;
                s >>= 5;
                if (s < 0) s = 0;
                if (s > 255) s = 255;
                return s;
            };
            int t = int16_t(mulhi(vv, rv) << 2);
            int r = chan(sat(yy + t));
            t = int16_t(mulhi(vv, gv) << 2);
            int g = sat(yy - t);
            t = int16_t(mulhi(uu, gu) << 2);
            g = chan(sat(g - t));
            t = int16_t(mulhi(uu, bu) << 2);
            int b = chan(sat(yy + t));
            return std::array<int, 3>{b, g, r};
        };
        const float mp = float(1 << 13);
        const int ymult = int(mp * 1.164f);
        const int rv = int(mp * 1.793f);
        const int gv = int(mp * 0.534f);
        const int gu = int(mp * 0.213f);
        const int bu = int(mp * 2.115f);
        int match_gpu_c = 0, match_up_c = 0, pixels = 0;
        for (int y = 0; y < s.height; ++y) {
            const int brow = s.height - 1 - y;
            const uint8_t* cpu_row = cpu.output.data() + size_t(brow) * s.width * 4;
            for (int x = 0; x < s.width; ++x) {
                const int cx = x / 2;
                const auto g = color15(planes[0][size_t(y) * s.width + x],
                    planes[1][size_t(y) * (s.width / 2) + cx],
                    planes[2][size_t(y) * (s.width / 2) + cx], 16, ymult, rv, gv, gu, bu);
                const auto u = color15(upstream_code[0][size_t(y) * s.width + x],
                    upstream_code[1][size_t(y) * (s.width / 2) + cx],
                    upstream_code[2][size_t(y) * (s.width / 2) + cx], 16, ymult, rv, gv, gu, bu);
                const uint8_t* p = cpu_row + x * 4;
                match_gpu_c += g[0] == p[0] && g[1] == p[1] && g[2] == p[2];
                match_up_c += u[0] == p[0] && u[1] == p[1] && u[2] == p[2];
                ++pixels;
            }
        }
        std::cout << "  color15 ymult=" << ymult << " rv=" << rv << " gv=" << gv
                  << " gu=" << gu << " bu=" << bu
                  << " gpu=" << match_gpu_c << " yu64=" << match_up_c << " /" << pixels << "\n";
        std::array<AlignedVector<int16_t>, 3> bgra_code;
        bgra_code[0].assign(size_t(s.width) * s.height, -9999);
        bgra_code[1].assign(size_t(s.width / 2) * s.height, -9999);
        bgra_code[2].assign(size_t(s.width / 2) * s.height, -9999);
        {
            auto* bgra_native = cpu.GetNative();
            const auto ref_b = HyperCine::DecodePhase1::ComputeCpuReferenceIntermediates(bgra_native);
            for (int ch = 0; ch < 3; ++ch) {
                auto* w0 = bgra_native->transform[ch]->wavelet[0];
                const int lw = w0->width, lh = w0->height, stride = w0->pitch / int(sizeof(PIXEL));
                const int ow = lw * 2;
                const int plane = ch == 0 ? 0 : ch == 1 ? 2 : 1;
                AlignedVector<PIXEL> low_row(static_cast<size_t>(ow));
                AlignedVector<PIXEL> high_row(static_cast<size_t>(ow));
                AlignedVector<PIXEL16U> row16(static_cast<size_t>(ow) * 2);
                const int row_pitch = ow * int(sizeof(PIXEL16U));
                const int16_t* ll = ref_b.ll0[ch].data();
                for (int r = 0; r < lh; ++r) {
                    InvertHorizontalRow16s(const_cast<PIXEL*>(reinterpret_cast<const PIXEL*>(ll + size_t(r) * lw)),
                        w0->band[1] + size_t(r) * stride, low_row.data(), lw);
                    InvertHorizontalRow16s(w0->band[2] + size_t(r) * stride, w0->band[3] + size_t(r) * stride,
                        high_row.data(), lw);
                    std::memset(row16.data(), 0, row16.size() * sizeof(PIXEL16U));
                    InvertInterlacedRow16sToRow16u(low_row.data(), high_row.data(), row16.data(), row_pitch, ow, ow,
                        bgra_native->codec.chroma_offset, bgra_native->codec.precision);
                    for (int c = 0; c < ow; ++c) {
                        bgra_code[plane][size_t(2 * r) * ow + c] = int16_t(int(row16[c]) >> 6);
                        bgra_code[plane][size_t(2 * r + 1) * ow + c] = int16_t(int(row16[size_t(ow) + c]) >> 6);
                    }
                }
            }
            int exact_bgra = 0, ncode = 0;
            for (int plane = 0; plane < 3; ++plane) {
                const int pw = plane == 0 ? s.width : s.width / 2;
                for (int i = 0; i < pw * s.height; ++i) {
                    ++ncode;
                    exact_bgra += planes[plane][size_t(i)] == bgra_code[plane][size_t(i)];
                }
            }
            std::cout << "  bgra_replay_vs_gpu exact=" << exact_bgra << "/" << ncode << "\n";
        }
        auto match_real_color = [&](const char* name, const std::array<AlignedVector<int16_t>, 3>& codes) {
            auto* dec = cpu.GetNative();
            int match_bot = 0, match_top = 0, n = 0, shown = 0;
            AlignedVector<PIXEL16U> packed(static_cast<size_t>(s.width) * 2 + 32);
            AlignedVector<uint16_t> scratch(static_cast<size_t>(s.width) * 16);
            AlignedVector<uint8_t> outrow(static_cast<size_t>(s.width) * 4 + 64);
            static bool printed = false;
            if (!printed) {
                printed = true;
                std::cout << "  meta format=" << dec->frame.format
                          << " outfmt=" << dec->frame.output_format
                          << " cs=" << dec->frame.colorspace
                          << " wp=" << dec->frame.white_point
                          << " active=" << dec->use_active_metadata_decoder
                          << " apply=" << dec->apply_color_active_metadata
                          << " path=0x" << std::hex << dec->cfhddata.process_path_flags << std::dec
                          << " curves=" << dec->cfhddata.encode_curve << "/" << dec->cfhddata.decode_curve
                          << " cube=" << (dec->RawCube != nullptr) << "\n";
            }
            for (int y = 0; y < s.height; ++y) {
                std::memset(packed.data(), 0, packed.size() * sizeof(PIXEL16U));
                PIXEL16U* yptr = packed.data();
                PIXEL16U* vptr = yptr + s.width;
                PIXEL16U* uptr = vptr + s.width / 2;
                for (int x = 0; x < s.width; ++x) {
                    int v = codes[0][size_t(y) * s.width + x];
                    if (v < 0) v = 0;
                    if (v > 1023) v = 1023;
                    yptr[x] = PIXEL16U(v << 6);
                }
                for (int x = 0; x < s.width / 2; ++x) {
                    int u = codes[1][size_t(y) * (s.width / 2) + x];
                    int v = codes[2][size_t(y) * (s.width / 2) + x];
                    if (u < 0) u = 0;
                    if (u > 1023) u = 1023;
                    if (v < 0) v = 0;
                    if (v > 1023) v = 1023;
                    uptr[x] = PIXEL16U(u << 6);
                    vptr[x] = PIXEL16U(v << 6);
                }
                uint8_t* planar_row[3] = {
                    reinterpret_cast<uint8_t*>(yptr),
                    reinterpret_cast<uint8_t*>(vptr),
                    reinterpret_cast<uint8_t*>(uptr)};
                int pitches[3] = {0, 0, 0};
                ROI roi;
                roi.width = s.width;
                roi.height = 1;
                (void)roi;
                (void)pitches;
                auto* scanline = scratch.data();
                auto* scanline2 = scratch.data() + size_t(s.width) * 6;
                std::memset(scratch.data(), 0, scratch.size() * sizeof(uint16_t));
                unsigned short* planar_u16[3] = {yptr, vptr, uptr};
                ChannelYUYV16toPlanarYUV16(planar_u16, scanline, s.width, dec->frame.colorspace);
                PlanarYUV16toPlanarRGB16(scanline, scanline2, s.width,
                    dec->frame.colorspace | COLOR_SPACE_8_PIXEL_PLANAR);
                const int whitebitdepth = 16;
                const int flags = COLOR_FORMAT_RGB_8PIXEL_PLANAR;
                std::memset(outrow.data(), 0, outrow.size());
                ConvertLinesToOutput(dec, s.width, 1, y, scanline2,
                    outrow.data(), s.width * 4, dec->frame.format, whitebitdepth, flags);
                const uint8_t* cpu_bot = cpu.output.data() + size_t(s.height - 1 - y) * s.width * 4;
                const uint8_t* cpu_top = cpu.output.data() + size_t(y) * s.width * 4;
                for (int x = 0; x < s.width; ++x) {
                    const bool bot = std::memcmp(outrow.data() + x * 4, cpu_bot + x * 4, 3) == 0;
                    const bool top = std::memcmp(outrow.data() + x * 4, cpu_top + x * 4, 3) == 0;
                    match_bot += bot;
                    match_top += top;
                    ++n;
                    if (!bot && shown < 3 && y == s.height - 1) {
                        std::cout << "  real " << name << " x=" << x
                                  << " got=" << int(outrow[x * 4]) << ',' << int(outrow[x * 4 + 1]) << ',' << int(outrow[x * 4 + 2])
                                  << " cpu=" << int(cpu_bot[x * 4]) << ',' << int(cpu_bot[x * 4 + 1]) << ',' << int(cpu_bot[x * 4 + 2])
                                  << " flags=" << flags << " wp=" << whitebitdepth << "\n";
                        ++shown;
                    }
                }
            }
            std::cout << "  realcolor " << name << " bottom=" << match_bot
                      << " top=" << match_top << " /" << n << "\n";
        };
        match_real_color("gpu", planes);
        match_real_color("bgra-replay", bgra_code);
        match_real_color("yu64", upstream_code);
        std::cout << "  yu64 words=" << words << " same=" << match_same << " flip=" << match_flip
                  << " outliers=" << outliers << " dgot-gpu";
        for (int i = 0; i < 17; ++i) if (hist[i]) std::cout << " " << (i - 8) << ":" << hist[i];
        std::cout << "\n" << std::flush;
    }
    std::cout << "  bgra diffs bottom-vs-cpu=" << diffs(gpu, cpu.output)
              << " bottom-vs-flip=" << diffs(gpu, flipped)
              << " top-vs-cpu=" << diffs(gpu_top, cpu.output)
              << " top-vs-flip=" << diffs(gpu_top, flipped) << "\n" << std::flush;
    d.ReleaseOutput(t_top);
    if (gpu != cpu.output) ReportMismatch(d, t, s, gpu, cpu.output);
    const auto bands = HyperCine::DecodePhase1::ExtractBandsFromNative(cpu.GetNative());
    Require(bands.size() == 30, "30 coefficient bands");
    int coeff = 0;
    for (int b = 0; b < 30; ++b) {
        std::vector<int16_t> v; int w = 0, h = 0, pitch = 0;
        Require(DecoderInspector::DownloadBand(d, t, b, v, &w, &h, &pitch), "coefficient download");
        Require(w == bands[b].width && h == bands[b].height, "coefficient geometry");
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const auto got = v[size_t(y) * (pitch / 2) + x];
            const auto want = bands[b].host_data[size_t(y) * (bands[b].pitch / 2) + x];
            if (got != want) {
                throw std::runtime_error(std::string(label) + " coefficient mismatch band " + std::to_string(b));
            }
            ++coeff;
        }
    }
    CudaDecoderPipelinePhase2E pipe;
    // The production decoder already reconstructed this frame. Stage download
    // uses a second pipeline ticket so the field buffers can be inspected.
    Require(pipe.Initialize(s.width, s.height, 1, 10, 4, 2), "stage pipeline");
    std::vector<GpuEntropyBandDescriptor> parsed;
    FrameReconstruction mode = FrameReconstruction::Progressive;
    Require(ParseCineFormSampleValidated(s.compressed.data(), s.compressed.size(), 24, parsed, nullptr, nullptr, &mode), "stage parse");
    Require(mode == FrameReconstruction::InterlacedGop1, "stage mode");
    auto ticket = pipe.QueueDecodeParsed(s.compressed.data(), s.compressed.size(), parsed,
        DeviceSurfaceType::PlanarYUV422_16S, ColorMatrix::BT709, ColorRange::CG, RowOrientation::TopDown,
        true, 0.0f, true);
    Require(ticket && pipe.WaitForDecode(ticket), "stage decode");
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<int16_t> low, high, plane;
        int w = 0, rows = 0;
        Require(pipe.DownloadInterlacedFieldStage(ticket, ch, low, high, plane, &w, &rows), "field stage");
        Require(int(plane.size()) == w * rows * 2, "field plane geometry");
        for (int r = 0; r < rows; ++r) for (int c = 0; c < w; ++c) {
            const int16_t lo = low[size_t(r) * w + c];
            const int16_t hi = high[size_t(r) * w + c];
            auto field_code = [](int16_t diff) {
                constexpr int kBias = 32767 - 2047;
                int32_t biased = int32_t(diff) + kBias;
                if (biased > 32767) biased = 32767;
                if (biased < -32768) biased = -32768;
                const uint16_t bits = static_cast<uint16_t>(static_cast<int16_t>(biased));
                const uint16_t limited = bits < static_cast<uint16_t>(kBias)
                    ? 0 : static_cast<uint16_t>(bits - static_cast<uint16_t>(kBias));
                return int16_t(int32_t(int16_t(limited)) >> 1);
            };
            const int16_t even = field_code(SatSub(lo, hi));
            const int16_t odd = field_code(SatAdd(lo, hi));
            if (plane[size_t(2 * r) * w + c] != even || plane[size_t(2 * r + 1) * w + c] != odd)
                throw std::runtime_error(std::string(label) + " field kernel diverged from sat-add/sub shift");
        }
    }
    pipe.ReleaseOutput(ticket);
    d.ReleaseOutput(t);
    std::cout << label << " exact BGRA and " << coeff << " coefficients\n" << std::flush;
}

void Smoke64(const std::filesystem::path& root) {
    const auto path = root / "tests/fixtures/phase3b0/YUV422-p10-ch3-spatial-field-f1-bands30-fmt-1.cfhd";
    Sample s{64, 64, 0, LoadFile(path), {}};
    Decoder d;
    Require(d.Initialize() == DecodeError::None, "64 init");
    auto caps = d.Capabilities();
    Require(caps.interlaced_output, "capability flag");
    ExactFrame(d, s, "64x64 FilmScan1");
}

void Smoke1080() {
    void* raw = nullptr;
    Check(CFHD_OpenEncoder(&raw, nullptr), "1080 encoder");
    Encoder encoder(raw);
    Check(CFHD_PrepareToEncode(raw, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODED_FORMAT_YUV_422,
          CFHD_ENCODING_FLAGS_YUV_INTERLACED, CFHD_ENCODING_QUALITY_FILMSCAN3), "1080 prepare");
    std::srand(0x1337BEEF);
    auto input = HyperCineTest::GenerateTestVectorYUY2(HyperCineTest::PATTERN_PSEUDO_RANDOM_NOISE, 1920, 1080);
    Check(CFHD_EncodeSample(raw, input.data(), 1920 * 2), "1080 encode");
    void* data = nullptr; size_t n = 0;
    Check(CFHD_GetSampleData(raw, &data, &n), "1080 sample");
    uint64_t folded = 14695981039346656037ULL;
    folded ^= HyperCineTest::ComputeFNV1a64(input.data(), input.size());
    folded *= 1099511628211ULL;
    std::ostringstream folded_hex;
    folded_hex << std::hex << std::setfill('0') << std::setw(16) << folded;
    Sample s{1920, 1080, 2, Bytes(n), folded_hex.str()};
    std::memcpy(s.compressed.data(), data, n);
    NormalizeMetadata(s.compressed);
    RequireMsg(n == 4655384, "compressed size " + std::to_string(n));
    RequireMsg(s.input_hash == "2d12a276f3594b09", "source hash " + s.input_hash);
    const auto sample_hash = Hash(s.compressed.data(), s.compressed.size());
    RequireMsg(sample_hash == "356ad5c204104b7d", "sample hash " + sample_hash);
    ReferenceDecoder cpu(s, CFHD_PIXEL_FORMAT_BGRA);
    cpu.Decode(s);
    const auto coeff = CoeffHash(cpu.GetNative());
    RequireMsg(coeff == "3455cbb3a354825f", "coefficient hash " + coeff);
    const auto bgra = Hash(cpu.output.data(), cpu.output.size());
    RequireMsg(bgra == "41a4e58b09555e3e", "upstream BGRA hash " + bgra);
    DecoderConfig config;
    config.maximum_width = 1920;
    config.maximum_height = 1080;
    Decoder d(config);
    Require(d.Initialize() == DecodeError::None, "1080 init");
    ExactFrame(d, s, "1920x1080 FilmScan3");
}
}

int main(int argc, char** argv) {
    try {
        Require(argc >= 2, "repo root required");
        const std::filesystem::path root = argv[1];
        Smoke64(root);
        Smoke1080();
        std::cout << "phase 3b1 smoke PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
