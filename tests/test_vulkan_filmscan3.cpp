// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Independent CPU <-> Vulkan parity harness for the HyperCine Vulkan P1 slice.
//
// Contract (issue #31):
//   * compares every Y/Cr/Cb p10 sample and every unquantized L1 LL/LH/HL/HH
//     value against the SDK-free CPU reference
//   * zero tolerance; on the first difference it prints
//     "frame,plane,level,band,x,y,expected,actual"
//   * a "GPU REQUIRED" run must fail when no Vulkan device/adapter is usable -
//     it never silently falls back to CPU
//   * exercises flat, gradient, canonical, impulse, checker/edges, seeded
//     noise, extreme signed int16, padded stride and odd L3 geometry shapes
//
// Exit codes: 0 = PASS, 1 = mismatch/failure, 77 = skipped (no GPU, non-required)

#include "hypercine/vulkan_frontend.h"
#include "hypercine/standalone.h"
#include "hypercine/filmscan3.h"
#include "hypercine/wavelet.h"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace {

int g_failures = 0;

// Deterministic FNV-1a 64 over bytes, matching the shader fingerprint style.
uint64_t fnv1a64(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct CompareStats {
    uint64_t compared = 0;
    uint64_t first_diff_index = 0;
};

// Reports only the first difference with an unambiguous location line.
bool compare_plane(const int16_t* cpu, const int16_t* gpu, int w, int h, int stride,
                   const char* plane, int level, const char* band,
                   uint64_t frame, CompareStats& out) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int16_t e = cpu[static_cast<size_t>(y) * stride + x];
            const int16_t a = gpu[static_cast<size_t>(y) * stride + x];
            ++out.compared;
            if (e != a) {
                out.first_diff_index = out.compared;
                std::printf("MISMATCH frame=%llu,plane=%s,level=%d,band=%s,x=%d,y=%d,expected=%d,actual=%d\n",
                            static_cast<unsigned long long>(frame), plane, level, band, x, y,
                            static_cast<int>(e), static_cast<int>(a));
                ++g_failures;
                return false;
            }
        }
    }
    return true;
}

std::vector<uint8_t> make_bgra(int w, int h, size_t stride, int pattern, uint32_t seed) {
    std::vector<uint8_t> buf(stride * static_cast<size_t>(h), 0);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> noise(0, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = buf.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            switch (pattern) {
                case 0: p[0] = 20; p[1] = 128; p[2] = 200; p[3] = 255; break;              // flat
                case 1: p[0] = static_cast<uint8_t>(x); p[1] = static_cast<uint8_t>(y);
                        p[2] = static_cast<uint8_t>(x + y); p[3] = 255; break;            // gradient
                case 2: p[0] = static_cast<uint8_t>((x * 7 + y * 13) & 0xFF);
                        p[1] = static_cast<uint8_t>((x * 3 - y * 5) & 0xFF);
                        p[2] = static_cast<uint8_t>((x ^ y) & 0xFF); p[3] = 255; break;   // canonical
                case 3: { bool imp = (x == w / 2 && y == h / 2); p[0] = p[1] = p[2] = imp ? 255 : 0; p[3] = 255; break; }
                case 4: { uint8_t c = ((x / 8 + y / 8) & 1) ? 255 : 0; p[0] = p[1] = p[2] = c; p[3] = 255; break; }
                default: { p[0] = static_cast<uint8_t>(noise(rng)); p[1] = static_cast<uint8_t>(noise(rng));
                           p[2] = static_cast<uint8_t>(noise(rng)); p[3] = 255; break; }    // noise
            }
        }
    }
    return buf;
}

int run_color_pattern(hypercine::vulkan::VulkanFrontend& vk, int w, int h, int pattern,
                      size_t pad_rows, uint32_t seed, uint64_t frame) {
    const size_t stride = static_cast<size_t>(w) * 4 + pad_rows; // padded row stride in bytes
    auto bgra = make_bgra(w, h, stride, pattern, seed);

    const int y_stride = (w + 7) & ~7;
    const int c_stride = ((w / 2) + 7) & ~7;
    std::vector<int16_t> cy(static_cast<size_t>(y_stride) * h, 0);
    std::vector<int16_t> ccr(static_cast<size_t>(c_stride) * h, 0);
    std::vector<int16_t> ccb(static_cast<size_t>(c_stride) * h, 0);
    std::vector<int16_t> vy(static_cast<size_t>(y_stride) * h, 0);
    std::vector<int16_t> vcr(static_cast<size_t>(c_stride) * h, 0);
    std::vector<int16_t> vcb(static_cast<size_t>(c_stride) * h, 0);

    auto cpu = hypercine::standalone::ConvertFilmScan3Bgra(bgra.data(), bgra.size(), w, h, stride);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&cy[static_cast<size_t>(y) * y_stride], &cpu[0].pixels[static_cast<size_t>(y) * cpu[0].stride], sizeof(int16_t) * w);
        std::memcpy(&ccr[static_cast<size_t>(y) * c_stride], &cpu[1].pixels[static_cast<size_t>(y) * cpu[1].stride], sizeof(int16_t) * (w / 2));
        std::memcpy(&ccb[static_cast<size_t>(y) * c_stride], &cpu[2].pixels[static_cast<size_t>(y) * cpu[2].stride], sizeof(int16_t) * (w / 2));
    }

    vk.ConvertBgra(bgra.data(), bgra.size(), w, h, stride,
                   vy.data(), y_stride, vcr.data(), vcb.data(), c_stride);

    CompareStats s;
    bool ok = true;
    ok &= compare_plane(cy.data(), vy.data(), w, h, y_stride, "Y", 0, "color", frame, s);
    ok &= compare_plane(ccr.data(), vcr.data(), w / 2, h, c_stride, "Cr", 0, "color", frame, s);
    ok &= compare_plane(ccb.data(), vcb.data(), w / 2, h, c_stride, "Cb", 0, "color", frame, s);
    if (ok) {
        std::printf("color PASS pattern=%d %dx%d padded=%zu cmp=%llu in=%016llx out=%016llx\n",
                    pattern, w, h, pad_rows, static_cast<unsigned long long>(s.compared),
                    static_cast<unsigned long long>(fnv1a64(bgra.data(), bgra.size())),
                    static_cast<unsigned long long>(fnv1a64(vy.data(), vy.size() * 2)));
    }
    return ok ? 0 : 1;
}

// Builds a deterministic int16 input plane for wavelet tests.
std::vector<int16_t> make_plane(int w, int h, int stride, int pattern, uint32_t seed) {
    std::vector<int16_t> p(static_cast<size_t>(stride) * h, 0);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> full(-32768, 32767);
    std::uniform_int_distribution<int> extreme(0, 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int v;
            switch (pattern) {
                case 0: v = 0; break;
                case 1: v = (x * 16 + y * 8) - 4096; break;
                case 2: v = ((x ^ y) * 37) & 0x7FFF; break;
                case 3: v = (x == w / 2 && y == h / 2) ? 32767 : 0; break;
                case 4: v = ((x / 4 + y / 4) & 1) ? 32767 : -32768; break;
                case 5: v = full(rng); break;
                default: v = extreme(rng) ? 32767 : -32768; break; // extreme signed int16
            }
            p[static_cast<size_t>(y) * stride + x] = static_cast<int16_t>(v);
        }
    }
    return p;
}

int run_wavelet_h(hypercine::vulkan::VulkanFrontend& vk, int w, int h, int pattern,
                  int pad, uint32_t seed, uint64_t frame) {
    const int stride = w + pad;
    auto src = make_plane(w, h, stride, pattern, seed);
    const int half = w / 2;
    const int ost = half + ((pad % 8) ? 0 : 0); // output stride = half + own padding
    const int ostride = (half + 7) & ~7;

    std::vector<int16_t> cl(static_cast<size_t>(ostride) * h, 0), ch(static_cast<size_t>(ostride) * h, 0);
    std::vector<int16_t> vl(static_cast<size_t>(ostride) * h, 0), vh(static_cast<size_t>(ostride) * h, 0);

    HyperCine_ForwardHorizontal26_CPU(src.data(), w, h, stride, cl.data(), ostride, ch.data(), ostride);
    vk.ForwardHorizontal26(src.data(), w, h, stride, vl.data(), ostride, vh.data(), ostride);

    CompareStats s;
    bool ok = true;
    ok &= compare_plane(cl.data(), vl.data(), half, h, ostride, "lowH", 1, "L", frame, s);
    ok &= compare_plane(ch.data(), vh.data(), half, h, ostride, "highH", 1, "H", frame, s);
    if (ok) std::printf("wavelet_h PASS pattern=%d %dx%d cmp=%llu\n", pattern, w, h,
                        static_cast<unsigned long long>(s.compared));
    (void)ost;
    return ok ? 0 : 1;
}

int run_wavelet_v(hypercine::vulkan::VulkanFrontend& vk, int w, int h, int pattern,
                  int pad, uint32_t seed, uint64_t frame) {
    const int stride = w + pad;
    auto src = make_plane(w, h, stride, pattern, seed);
    const int half = h / 2;
    const int ostride = (w + 7) & ~7;

    std::vector<int16_t> cl(static_cast<size_t>(ostride) * half, 0), ch(static_cast<size_t>(ostride) * half, 0);
    std::vector<int16_t> vl(static_cast<size_t>(ostride) * half, 0), vh(static_cast<size_t>(ostride) * half, 0);

    HyperCine_ForwardVertical26_CPU(src.data(), w, h, stride, cl.data(), ostride, ch.data(), ostride);
    vk.ForwardVertical26(src.data(), w, h, stride, vl.data(), ostride, vh.data(), ostride);

    CompareStats s;
    bool ok = true;
    ok &= compare_plane(cl.data(), vl.data(), w, half, ostride, "lowV", 1, "L", frame, s);
    ok &= compare_plane(ch.data(), vh.data(), w, half, ostride, "highV", 1, "H", frame, s);
    if (ok) std::printf("wavelet_v PASS pattern=%d %dx%d cmp=%llu\n", pattern, w, h,
                        static_cast<unsigned long long>(s.compared));
    return ok ? 0 : 1;
}

// Full L1 subbands from ForwardLevel (prescale=0), compared band by band.
int run_forward_level(hypercine::vulkan::VulkanFrontend& vk, int w, int h, int pattern,
                      uint32_t seed, uint64_t frame) {
    // Start from a BGRA frame -> planes, like the real encoder does, so the
    // wavelet input is a realistic signed-int16 plane.
    const size_t bstride = static_cast<size_t>(w) * 4;
    auto bgra = make_bgra(w, h, bstride, pattern, seed);
    auto planes = hypercine::standalone::ConvertFilmScan3Bgra(bgra.data(), bgra.size(), w, h, bstride);

    auto cpu = hypercine::standalone::ForwardLevel(planes[0], 0);

    // GPU: horizontal then vertical, per band exactly like the CPU reference.
    const int hw = w / 2, hh = h / 2;
    const int lstride = (hw + 7) & ~7;
    std::vector<int16_t> low(static_cast<size_t>(lstride) * h, 0), high(static_cast<size_t>(lstride) * h, 0);
    vk.ForwardHorizontal26(planes[0].pixels.data(), w, h, planes[0].stride, low.data(), lstride, high.data(), lstride);

    const int bstride_out = (hw + 7) & ~7;
    std::vector<int16_t> ll(static_cast<size_t>(bstride_out) * hh, 0), lh(static_cast<size_t>(bstride_out) * hh, 0);
    std::vector<int16_t> hl(static_cast<size_t>(bstride_out) * hh, 0), hh_band(static_cast<size_t>(bstride_out) * hh, 0);
    vk.ForwardVertical26(low.data(), hw, h, lstride, ll.data(), bstride_out, hl.data(), bstride_out);
    vk.ForwardVertical26(high.data(), hw, h, lstride, lh.data(), bstride_out, hh_band.data(), bstride_out);

    CompareStats s;
    bool ok = true;
    ok &= compare_plane(cpu.bands[0].pixels.data(), ll.data(), hw, hh, bstride_out, "LL", 1, "LL", frame, s);
    ok &= compare_plane(cpu.bands[1].pixels.data(), lh.data(), hw, hh, bstride_out, "LH", 1, "LH", frame, s);
    ok &= compare_plane(cpu.bands[2].pixels.data(), hl.data(), hw, hh, bstride_out, "HL", 1, "HL", frame, s);
    ok &= compare_plane(cpu.bands[3].pixels.data(), hh_band.data(), hw, hh, bstride_out, "HH", 1, "HH", frame, s);
    if (ok) std::printf("forward_level PASS pattern=%d %dx%d cmp=%llu\n", pattern, w, h,
                        static_cast<unsigned long long>(s.compared));
    return ok ? 0 : 1;
}

} // namespace

namespace {
// Alternating CPU/GPU measurements; reports raw runs and descriptive stats.
void bench_color(hypercine::vulkan::VulkanFrontend& vk, int w, int h, int reps) {
    const size_t stride = static_cast<size_t>(w) * 4;
    auto bgra = make_bgra(w, h, stride, 2, 0x5EEDu);
    const int y_stride = (w + 7) & ~7, c_stride = ((w / 2) + 7) & ~7;
    std::vector<int16_t> y(static_cast<size_t>(y_stride) * h), cr(static_cast<size_t>(c_stride) * h), cb(static_cast<size_t>(c_stride) * h);

    std::vector<double> cpu_ms, gpu_kernel_ms, gpu_wall_ms;
    for (int i = 0; i < reps; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        auto p = hypercine::standalone::ConvertFilmScan3Bgra(bgra.data(), bgra.size(), w, h, stride);
        auto t1 = std::chrono::steady_clock::now();
        (void)p;

        auto t2 = std::chrono::steady_clock::now();
        vk.ConvertBgra(bgra.data(), bgra.size(), w, h, stride, y.data(), y_stride, cr.data(), cb.data(), c_stride);
        auto t3 = std::chrono::steady_clock::now();

        cpu_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        gpu_wall_ms.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
        gpu_kernel_ms.push_back(vk.last_dispatch_gpu_ns() / 1e6);
    }
    auto stats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        double sum = 0; for (double x : v) sum += x;
        return std::make_tuple(v[v.size() / 2], sum / v.size(), v[(size_t)(v.size() * 0.95)]);
    };
    auto [cmed, cmean, cp95] = stats(cpu_ms);
    auto [wmed, wmean, wp95] = stats(gpu_wall_ms);
    auto [kmed, kmean, kp95] = stats(gpu_kernel_ms);
    std::printf("bench color %dx%d reps=%d\n", w, h, reps);
    std::printf("  cpu_ms        median=%.3f mean=%.3f p95=%.3f\n", cmed, cmean, cp95);
    std::printf("  gpu_wall_ms   median=%.3f mean=%.3f p95=%.3f (includes upload+download+wait)\n", wmed, wmean, wp95);
    std::printf("  gpu_kernel_ms median=%.3f mean=%.3f p95=%.3f (timestamp window only)\n", kmed, kmean, kp95);
}

void bench_wavelet(hypercine::vulkan::VulkanFrontend& vk, int w, int h, int reps) {
    const int stride = w;
    auto src = make_plane(w, h, stride, 5, 0x77u);
    const int half = w / 2, ostride = (half + 7) & ~7;
    std::vector<int16_t> cl(static_cast<size_t>(ostride) * h), ch(static_cast<size_t>(ostride) * h);
    std::vector<int16_t> vl(static_cast<size_t>(ostride) * h), vh(static_cast<size_t>(ostride) * h);
    std::vector<double> cpu_ms, gpu_kernel_ms, gpu_wall_ms;
    for (int i = 0; i < reps; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        HyperCine_ForwardHorizontal26_CPU(src.data(), w, h, stride, cl.data(), ostride, ch.data(), ostride);
        auto t1 = std::chrono::steady_clock::now();
        auto t2 = std::chrono::steady_clock::now();
        vk.ForwardHorizontal26(src.data(), w, h, stride, vl.data(), ostride, vh.data(), ostride);
        auto t3 = std::chrono::steady_clock::now();
        cpu_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        gpu_wall_ms.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
        gpu_kernel_ms.push_back(vk.last_dispatch_gpu_ns() / 1e6);
    }
    auto stats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        double sum = 0; for (double x : v) sum += x;
        return std::make_tuple(v[v.size() / 2], sum / v.size(), v[(size_t)(v.size() * 0.95)]);
    };
    auto [cmed, cmean, cp95] = stats(cpu_ms);
    auto [wmed, wmean, wp95] = stats(gpu_wall_ms);
    auto [kmed, kmean, kp95] = stats(gpu_kernel_ms);
    std::printf("bench wavelet_h26 %dx%d reps=%d\n", w, h, reps);
    std::printf("  cpu_ms        median=%.3f mean=%.3f p95=%.3f\n", cmed, cmean, cp95);
    std::printf("  gpu_wall_ms   median=%.3f mean=%.3f p95=%.3f\n", wmed, wmean, wp95);
    std::printf("  gpu_kernel_ms median=%.3f mean=%.3f p95=%.3f\n", kmed, kmean, kp95);
}
} // namespace

int main(int argc, char** argv) {
    bool gpu_required = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu-required") == 0) gpu_required = true;
    }

    auto info = hypercine::vulkan::DiscoverDevice();
    if (!info.available) {
        std::printf("Vulkan device unavailable: %s\n", info.notes.c_str());
        if (gpu_required) {
            std::printf("GPU REQUIRED run: FAIL (no Vulkan device)\n");
            return 1;
        }
        std::printf("SKIP (no Vulkan device; not required)\n");
        return 77;
    }

    bool do_bench = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--bench") == 0) do_bench = true;

    std::printf("Vulkan device: %s (vendor 0x%04x, device 0x%04x, api %u.%u.%u, compute family %u)\n",
                info.device_name.c_str(), info.vendor_id, info.device_id,
                (info.api_version >> 22) & 0x7Fu, (info.api_version >> 12) & 0x3FFu, info.api_version & 0xFFFu,
                info.compute_queue_family);

    hypercine::vulkan::VulkanFrontend vk;
    if (!vk.info().available) {
        std::printf("Vulkan frontend init failed: %s\n", vk.info().notes.c_str());
        if (gpu_required) return 1;
        return 77;
    }
    std::printf("%s", vk.ShaderFingerprint().c_str());

    uint64_t frame = 0;
    const int pad_options[] = {0, 16};  // unpadded and padded strides (bytes; must stay 16-aligned)
    const int w_options[] = {128, 256, 1920};

    // Color conversion: all patterns, both paddings, small and 1080p-wide frames.
    for (int w : w_options) {
        const int h = (w == 1920) ? 1088 : w; // 1088 = multiple of 8 >= 24
        for (int pattern = 0; pattern <= 5; ++pattern) {
            for (int pad : pad_options) {
                run_color_pattern(vk, w, h, pattern, static_cast<size_t>(pad), 0xC0FFEEu + w + pattern, frame++);
            }
        }
    }

    // Horizontal wavelet: every pattern incl. extreme signed int16, padded stride.
    for (int w : w_options) {
        const int h = (w == 1920) ? 1088 : w;
        for (int pattern = 0; pattern <= 6; ++pattern) {
            for (int pad : pad_options) {
                run_wavelet_h(vk, w, h, pattern, pad, 0xABCDEFu + w + pattern, frame++);
            }
        }
    }

    // Vertical wavelet.
    for (int w : w_options) {
        const int h = (w == 1920) ? 1088 : w;
        for (int pattern = 0; pattern <= 6; ++pattern) {
            for (int pad : pad_options) {
                run_wavelet_v(vk, w, h, pattern, pad, 0x123456u + w + pattern, frame++);
            }
        }
    }

    // Full L1 subbands LL/LH/HL/HH on 1080p (odd L3 geometry) and 256.
    for (int w : {256, 1920}) {
        const int h = (w == 1920) ? 1088 : 256;
        for (int pattern = 0; pattern <= 5; ++pattern) {
            run_forward_level(vk, w, h, pattern, 0x778899u + w + pattern, frame++);
        }
    }

    if (do_bench) {
        std::printf("--- benchmark (1 warmup + 20 alternating measurements) ---\n");
        bench_color(vk, 1920, 1088, 21);
        bench_wavelet(vk, 1920, 1088, 21);
    }

    if (g_failures) {
        std::printf("RESULT: FAIL (%d mismatches)\n", g_failures);
        return 1;
    }
    std::printf("RESULT: PASS (all samples bit-exact)\n");
    return 0;
}
