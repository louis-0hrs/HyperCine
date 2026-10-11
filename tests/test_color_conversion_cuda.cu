// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_color_cuda.h"
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>
#include <algorithm>

namespace {

using namespace HyperCine;

int16_t clamp255_simd_cpu(int16_t v) {
    int32_t add = int32_t(v) + 0x7F00;
    if (add > 32767) add = 32767;
    if (add < -32768) add = -32768;
    uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(add));
    if (u < 0x7F00) return 0;
    return static_cast<int16_t>(u - 0x7F00);
}

int16_t sat_add16_cpu(int16_t a, int16_t b) {
    int32_t sum = int32_t(a) + int32_t(b);
    if (sum > 32767) return 32767;
    if (sum < -32768) return -32768;
    return static_cast<int16_t>(sum);
}

int16_t sat_sub16_cpu(int16_t a, int16_t b) {
    int32_t diff = int32_t(a) - int32_t(b);
    if (diff > 32767) return 32767;
    if (diff < -32768) return -32768;
    return static_cast<int16_t>(diff);
}

uint8_t packus8_cpu(int32_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

void ConvertYuv422ToBgra8Cpu(
    const int16_t* y_plane, size_t y_pitch,
    const int16_t* v_plane, size_t v_pitch,
    const int16_t* u_plane, size_t u_pitch,
    uint8_t* bgra_out, size_t bgra_pitch,
    int width, int height,
    ColorMatrix matrix,
    ColorRange range,
    RowOrientation orientation)
{
    ColorCoefficients coeff = GetColorCoefficients(matrix, range);

    int lowpass_width = width / 2;
    int post_column_lowpass = lowpass_width - (lowpass_width % 16);
    while (post_column_lowpass > (lowpass_width - 2 - 2)) {
        post_column_lowpass -= 16;
    }
    int post_column_pixels = post_column_lowpass * 2;

    for (int y = 0; y < height; ++y) {
        int out_y = (orientation == RowOrientation::BottomUp) ? (height - 1 - y) : y;
        const int16_t* y_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(y_plane) + y * y_pitch);
        const int16_t* v_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(v_plane) + y * v_pitch);
        const int16_t* u_row = reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(u_plane) + y * u_pitch);
        uint8_t* out_row = bgra_out + out_y * bgra_pitch;

        for (int x = 0; x < width; ++x) {
            int16_t raw_y = y_row[x];
            int16_t raw_v = v_row[x / 2];
            int16_t raw_u = u_row[x / 2];

            // Signed inverse overshoot must reach the limiter as a negative
            // value, rather than being reinterpreted as an unsigned highlight.
            int16_t y8 = static_cast<int16_t>(raw_y >> 2);
            int16_t v8 = static_cast<int16_t>(raw_v >> 2);
            int16_t u8 = static_cast<int16_t>(raw_u >> 2);

            uint8_t b_out = 0, g_out = 0, r_out = 0;

            if (x < post_column_pixels) {
                int16_t yy = clamp255_simd_cpu(sat_sub16_cpu(y8, coeff.y_offset));
                int16_t uu = sat_sub16_cpu(clamp255_simd_cpu(u8), coeff.chroma_offset);
                int16_t vv = sat_sub16_cpu(clamp255_simd_cpu(v8), coeff.chroma_offset);

                int16_t yy_slli = static_cast<int16_t>(yy << 7);
                int16_t yy_hi = static_cast<int16_t>((int32_t(yy_slli) * int32_t(coeff.ymult)) >> 16);
                int16_t yy_sse = static_cast<int16_t>(yy_hi << 1);

                int16_t t_r = static_cast<int16_t>((int16_t(vv * coeff.r_vmult)) >> 1);
                int16_t r1 = sat_add16_cpu(yy_sse, t_r);
                r1 = sat_add16_cpu(r1, 32);
                r1 = static_cast<int16_t>(r1 >> 6);

                int16_t t_gv = static_cast<int16_t>((int16_t(vv * coeff.g_vmult)) >> 2);
                int16_t g1 = sat_sub16_cpu(yy_sse, t_gv);
                int16_t t_gu = static_cast<int16_t>((int16_t(uu * coeff.g_umult)) >> 2);
                g1 = sat_sub16_cpu(g1, t_gu);
                g1 = sat_add16_cpu(g1, 32);
                g1 = static_cast<int16_t>(g1 >> 6);

                int16_t t_bu = static_cast<int16_t>(uu * coeff.b_umult);
                int16_t b1 = sat_add16_cpu(yy_sse, t_bu);
                b1 = sat_add16_cpu(b1, 32);
                b1 = static_cast<int16_t>(b1 >> 6);

                b_out = packus8_cpu(b1);
                g_out = packus8_cpu(g1);
                r_out = packus8_cpu(r1);
            } else {
                int32_t yy = y8 - coeff.y_offset;
                int32_t uu = u8 - coeff.chroma_offset;
                int32_t vv = v8 - coeff.chroma_offset;

                int32_t yy_scaled = (yy * coeff.ymult) >> 7;
                int32_t r1 = (yy_scaled                          + coeff.r_vmult * vv + 64) >> 7;
                int32_t g1 = (yy_scaled * 2 - coeff.g_umult * uu - coeff.g_vmult * vv + 128) >> 8;
                int32_t b1 = (yy_scaled     + 2 * coeff.b_umult * uu                  + 64) >> 7;

                b_out = packus8_cpu(b1);
                g_out = packus8_cpu(g1);
                r_out = packus8_cpu(r1);
            }

            out_row[4 * x + 0] = b_out;
            out_row[4 * x + 1] = g_out;
            out_row[4 * x + 2] = r_out;
            out_row[4 * x + 3] = 255;
        }
    }
}

void RunSingleTest(
    const std::string& name,
    int width, int height,
    ColorMatrix matrix,
    ColorRange range,
    RowOrientation orientation,
    const std::vector<int16_t>& host_y,
    const std::vector<int16_t>& host_v,
    const std::vector<int16_t>& host_u)
{
    size_t y_pitch_host = width * sizeof(int16_t);
    size_t v_pitch_host = (width / 2) * sizeof(int16_t);
    size_t u_pitch_host = (width / 2) * sizeof(int16_t);
    size_t bgra_pitch_host = width * 4;

    std::vector<uint8_t> ref_bgra(bgra_pitch_host * height, 0);
    ConvertYuv422ToBgra8Cpu(
        host_y.data(), y_pitch_host,
        host_v.data(), v_pitch_host,
        host_u.data(), u_pitch_host,
        ref_bgra.data(), bgra_pitch_host,
        width, height,
        matrix, range, orientation
    );

    // Allocate GPU buffers
    int16_t *d_y = nullptr, *d_v = nullptr, *d_u = nullptr;
    uint8_t *d_bgra = nullptr;
    size_t d_y_pitch = 0, d_v_pitch = 0, d_u_pitch = 0, d_bgra_pitch = 0;

    cudaMallocPitch(&d_y, &d_y_pitch, width * sizeof(int16_t), height);
    cudaMallocPitch(&d_v, &d_v_pitch, (width / 2) * sizeof(int16_t), height);
    cudaMallocPitch(&d_u, &d_u_pitch, (width / 2) * sizeof(int16_t), height);
    cudaMallocPitch(&d_bgra, &d_bgra_pitch, width * 4, height);

    cudaMemcpy2D(d_y, d_y_pitch, host_y.data(), y_pitch_host, width * sizeof(int16_t), height, cudaMemcpyHostToDevice);
    cudaMemcpy2D(d_v, d_v_pitch, host_v.data(), v_pitch_host, (width / 2) * sizeof(int16_t), height, cudaMemcpyHostToDevice);
    cudaMemcpy2D(d_u, d_u_pitch, host_u.data(), u_pitch_host, (width / 2) * sizeof(int16_t), height, cudaMemcpyHostToDevice);

    bool ok = LaunchYuv422ToBgra8(
        d_y, d_y_pitch,
        d_v, d_v_pitch,
        d_u, d_u_pitch,
        d_bgra, d_bgra_pitch,
        width, height,
        matrix, range, orientation,
        nullptr
    );

    if (!ok) {
        throw std::runtime_error("LaunchYuv422ToBgra8 returned false for " + name);
    }
    cudaDeviceSynchronize();

    std::vector<uint8_t> gpu_bgra(bgra_pitch_host * height, 0);
    cudaMemcpy2D(gpu_bgra.data(), bgra_pitch_host, d_bgra, d_bgra_pitch, width * 4, height, cudaMemcpyDeviceToHost);

    cudaFree(d_y);
    cudaFree(d_v);
    cudaFree(d_u);
    cudaFree(d_bgra);

    // Verify bit-exact equality (Max Absolute Error = 0)
    int max_err = 0;
    int diff_count = 0;
    for (size_t i = 0; i < ref_bgra.size(); ++i) {
        int diff = std::abs(int(gpu_bgra[i]) - int(ref_bgra[i]));
        if (diff > max_err) max_err = diff;
        if (diff > 0) diff_count++;
    }

    if (max_err != 0) {
        throw std::runtime_error("FAILURE in " + name + ": max_err = " + std::to_string(max_err) +
                                 ", diff_bytes = " + std::to_string(diff_count));
    }

    std::cout << "  [PASS] " << name << " (0 diff, max_err=0)" << std::endl;
}

} // anonymous namespace

int main() {
    try {
        std::cout << "=== HyperCine CUDA Color Conversion Unit Tests ===" << std::endl;

        const int w = 320;
        const int h = 180;
        const size_t y_count = w * h;
        const size_t uv_count = (w / 2) * h;

        std::vector<ColorMatrix> matrices = {ColorMatrix::BT709, ColorMatrix::BT601};
        std::vector<ColorRange> ranges = {ColorRange::CG, ColorRange::VS};
        std::vector<RowOrientation> orientations = {RowOrientation::BottomUp, RowOrientation::TopDown};

        // 1. Neutral Gray (Y=512, U=512, V=512 -> y8=128, u8=128, v8=128)
        for (auto m : matrices) {
            for (auto r : ranges) {
                for (auto o : orientations) {
                    std::string desc = "Neutral Gray (matrix=" + std::to_string(int(m)) +
                                       " range=" + std::to_string(int(r)) +
                                       " orient=" + std::to_string(int(o)) + ")";
                    std::vector<int16_t> y_val(y_count, 512);
                    std::vector<int16_t> v_val(uv_count, 512);
                    std::vector<int16_t> u_val(uv_count, 512);
                    RunSingleTest(desc, w, h, m, r, o, y_val, v_val, u_val);
                }
            }
        }

        // 2. Pure Black (Y=64, U=512, V=512) and (Y=0, U=512, V=512)
        {
            std::vector<int16_t> y_val(y_count, 64);
            std::vector<int16_t> v_val(uv_count, 512);
            std::vector<int16_t> u_val(uv_count, 512);
            RunSingleTest("Pure Black (Y=64)", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);

            std::fill(y_val.begin(), y_val.end(), 0);
            RunSingleTest("Zero Black (Y=0)", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
        }

        // 3. Pure White (Y=940, U=512, V=512) and (Y=1023, U=512, V=512)
        {
            std::vector<int16_t> y_val(y_count, 940);
            std::vector<int16_t> v_val(uv_count, 512);
            std::vector<int16_t> u_val(uv_count, 512);
            RunSingleTest("Studio White (Y=940)", w, h, ColorMatrix::BT709, ColorRange::VS, RowOrientation::BottomUp, y_val, v_val, u_val);

            std::fill(y_val.begin(), y_val.end(), 1023);
            RunSingleTest("Peak White (Y=1023)", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
        }

        // 4. Saturated Chroma Extrema (Red, Green, Blue)
        {
            // Red-like extrema: high V, low U
            std::vector<int16_t> y_val(y_count, 300);
            std::vector<int16_t> v_val(uv_count, 1000);
            std::vector<int16_t> u_val(uv_count, 350);
            RunSingleTest("Red Extrema", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
            RunSingleTest("Red Extrema BT601", w, h, ColorMatrix::BT601, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);

            // Green-like extrema: low V, low U
            std::fill(y_val.begin(), y_val.end(), 600);
            std::fill(v_val.begin(), v_val.end(), 100);
            std::fill(u_val.begin(), u_val.end(), 200);
            RunSingleTest("Green Extrema", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
            RunSingleTest("Green Extrema BT601", w, h, ColorMatrix::BT601, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);

            // Blue-like extrema: low V, high U
            std::fill(y_val.begin(), y_val.end(), 150);
            std::fill(v_val.begin(), v_val.end(), 300);
            std::fill(u_val.begin(), u_val.end(), 900);
            RunSingleTest("Blue Extrema", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
            RunSingleTest("Blue Extrema BT601", w, h, ColorMatrix::BT601, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
        }

        // 5. Alternating Chroma (High-Frequency Checkerboard)
        {
            std::vector<int16_t> y_val(y_count, 512);
            std::vector<int16_t> v_val(uv_count);
            std::vector<int16_t> u_val(uv_count);
            for (size_t i = 0; i < uv_count; ++i) {
                v_val[i] = (i % 2 == 0) ? 200 : 800;
                u_val[i] = (i % 2 == 0) ? 800 : 200;
            }
            RunSingleTest("Alternating Chroma", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
        }

        // 6. Luma & Chroma Ramps
        {
            std::vector<int16_t> y_val(y_count);
            std::vector<int16_t> v_val(uv_count);
            std::vector<int16_t> u_val(uv_count);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    y_val[y * w + x] = static_cast<int16_t>((x * 1024) / w);
                }
            }
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w / 2; ++x) {
                    u_val[y * (w / 2) + x] = static_cast<int16_t>((x * 1024) / (w / 2));
                    v_val[y * (w / 2) + x] = static_cast<int16_t>((y * 1024) / h);
                }
            }
            RunSingleTest("Luma & Chroma Ramp 709 CG", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
            RunSingleTest("Luma & Chroma Ramp 601 VS", w, h, ColorMatrix::BT601, ColorRange::VS, RowOrientation::BottomUp, y_val, v_val, u_val);
        }

        // 7. Clipping below and above legal range & Extreme signed-16 values
        {
            std::vector<int16_t> y_val(y_count);
            std::vector<int16_t> v_val(uv_count);
            std::vector<int16_t> u_val(uv_count);

            for (size_t i = 0; i < y_count; ++i) {
                y_val[i] = (i % 4 == 0) ? -1000 : (i % 4 == 1) ? 2048 : (i % 4 == 2) ? -32768 : 32767;
            }
            for (size_t i = 0; i < uv_count; ++i) {
                v_val[i] = (i % 4 == 0) ? -2000 : (i % 4 == 1) ? 3000 : (i % 4 == 2) ? 32767 : -32768;
                u_val[i] = (i % 4 == 0) ? 3000 : (i % 4 == 1) ? -2000 : (i % 4 == 2) ? -32768 : 32767;
            }
            RunSingleTest("Legal Range & Extreme Clamping (709 CG)", w, h, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
            RunSingleTest("Legal Range & Extreme Clamping (601 VS)", w, h, ColorMatrix::BT601, ColorRange::VS, RowOrientation::BottomUp, y_val, v_val, u_val);
        }

        // 8. Odd edge positions and boundary pixel pairs across multiple resolutions
        std::vector<std::pair<int, int>> resolutions = {
            {128, 64},
            {192, 108},
            {1920, 1080},
            {3840, 2160}
        };

        for (auto& res : resolutions) {
            int rw = res.first;
            int rh = res.second;
            size_t ry_count = rw * rh;
            size_t ruv_count = (rw / 2) * rh;

            std::vector<int16_t> y_val(ry_count, 512);
            std::vector<int16_t> v_val(ruv_count, 512);
            std::vector<int16_t> u_val(ruv_count, 512);

            // Set extrema on first row, last row, first column, last column, and border column
            for (int x = 0; x < rw; ++x) {
                y_val[0 * rw + x] = 1000;              // First row
                y_val[(rh - 1) * rw + x] = 100;        // Last row
            }
            for (int y = 0; y < rh; ++y) {
                y_val[y * rw + 0] = 900;               // First column
                y_val[y * rw + (rw - 1)] = 200;        // Last column
                y_val[y * rw + (rw - 2)] = 850;        // Penultimate column
            }

            std::string rname = "Resolution Boundary Check " + std::to_string(rw) + "x" + std::to_string(rh);
            RunSingleTest(rname, rw, rh, ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, y_val, v_val, u_val);
        }

        std::cout << "\nSUCCESS: All standalone CUDA color conversion unit tests passed with 0 diff!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "COLOR CONVERSION TEST ERROR: " << e.what() << std::endl;
        return 1;
    }
}
