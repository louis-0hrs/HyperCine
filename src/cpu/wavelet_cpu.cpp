// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/wavelet.h"
#include <vector>
#include <cstring>
#include <cstdlib>

namespace {

// C++17 signed shifts are implementation-defined; use exact floor division.
inline int32_t FloorDivide8(int32_t value) { return value>=0 ? value/8 : -((-value+7)/8); }

inline int16_t Saturate16S(int32_t val) {
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return static_cast<int16_t>(val);
}

} // anonymous namespace

HyperCineStatus HyperCine_ForwardHorizontal26_CPU(
    const int16_t* src, int width, int height, int src_stride,
    int16_t* lowpass, int low_stride,
    int16_t* highpass, int high_stride
) {
    if (!src || !lowpass || !highpass) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width < 6 || (width % 2) != 0 || height <= 0) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (src_stride < width || low_stride < (width / 2) || high_stride < (width / 2)) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    const int half_w = width / 2;

    for (int y = 0; y < height; ++y) {
        const int16_t* in_row = src + y * src_stride;
        int16_t* low_row = lowpass + y * low_stride;
        int16_t* high_row = highpass + y * high_stride;

        for (int c = 0; c < half_w; ++c) {
            const int x = c * 2;

            // Lowpass: sum of adjacent pair
            int32_t low_sum = static_cast<int32_t>(in_row[x]) + static_cast<int32_t>(in_row[x + 1]);
            low_row[c] = Saturate16S(low_sum);

            // Highpass: 2-6 biorthogonal wavelet filter
            if (c == 0) {
                // Left border filter: [5, -11, 4, 4, -1, -1] / 8
                int32_t sum = 5 * static_cast<int32_t>(in_row[0])
                            - 11 * static_cast<int32_t>(in_row[1])
                            + 4 * static_cast<int32_t>(in_row[2])
                            + 4 * static_cast<int32_t>(in_row[3])
                            - 1 * static_cast<int32_t>(in_row[4])
                            - 1 * static_cast<int32_t>(in_row[5]);
                sum = FloorDivide8(sum + 4);
                high_row[0] = Saturate16S(sum);
            } else if (c < half_w - 1) {
                // Interior filter: [-1, -1, 8, -8, 1, 1] / 8
                int32_t hp_sum = - static_cast<int32_t>(in_row[x - 2])
                                 - static_cast<int32_t>(in_row[x - 1])
                                 + static_cast<int32_t>(in_row[x + 2])
                                 + static_cast<int32_t>(in_row[x + 3]);
                hp_sum = FloorDivide8(hp_sum + 4);
                int32_t sum = hp_sum + static_cast<int32_t>(in_row[x]) - static_cast<int32_t>(in_row[x + 1]);
                high_row[c] = Saturate16S(sum);
            } else {
                // Right border filter: [1, 1, -4, -4, 11, -5] / 8
                int32_t sum = 11 * static_cast<int32_t>(in_row[x])
                            - 5 * static_cast<int32_t>(in_row[x + 1])
                            - 4 * static_cast<int32_t>(in_row[x - 1])
                            - 4 * static_cast<int32_t>(in_row[x - 2])
                            + 1 * static_cast<int32_t>(in_row[x - 3])
                            + 1 * static_cast<int32_t>(in_row[x - 4]);
                sum = FloorDivide8(sum + 4);
                high_row[c] = Saturate16S(sum);
            }
        }
    }

    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus HyperCine_ForwardVertical26_CPU(
    const int16_t* src, int width, int height, int src_stride,
    int16_t* lowpass, int low_stride,
    int16_t* highpass, int high_stride
) {
    if (!src || !lowpass || !highpass) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (height < 6 || (height % 2) != 0 || width <= 0) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (src_stride < width || low_stride < width || high_stride < width) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    const int half_h = height / 2;

    for (int r = 0; r < half_h; ++r) {
        const int y = r * 2;
        int16_t* low_row = lowpass + r * low_stride;
        int16_t* high_row = highpass + r * high_stride;

        for (int x = 0; x < width; ++x) {
            // Lowpass: sum of adjacent pair in column
            int32_t low_sum = static_cast<int32_t>(src[y * src_stride + x])
                            + static_cast<int32_t>(src[(y + 1) * src_stride + x]);
            low_row[x] = Saturate16S(low_sum);

            // Highpass
            if (r == 0) {
                // Top border filter
                int32_t sum = 5 * static_cast<int32_t>(src[0 * src_stride + x])
                            - 11 * static_cast<int32_t>(src[1 * src_stride + x])
                            + 4 * static_cast<int32_t>(src[2 * src_stride + x])
                            + 4 * static_cast<int32_t>(src[3 * src_stride + x])
                            - 1 * static_cast<int32_t>(src[4 * src_stride + x])
                            - 1 * static_cast<int32_t>(src[5 * src_stride + x]);
                sum = FloorDivide8(sum + 4);
                high_row[x] = Saturate16S(sum);
            } else if (r < half_h - 1) {
                // Interior filter
                int32_t hp_sum = - static_cast<int32_t>(src[(y - 2) * src_stride + x])
                                 - static_cast<int32_t>(src[(y - 1) * src_stride + x])
                                 + static_cast<int32_t>(src[(y + 2) * src_stride + x])
                                 + static_cast<int32_t>(src[(y + 3) * src_stride + x]);
                hp_sum = FloorDivide8(hp_sum + 4);
                int32_t sum = hp_sum + static_cast<int32_t>(src[y * src_stride + x])
                                     - static_cast<int32_t>(src[(y + 1) * src_stride + x]);
                high_row[x] = Saturate16S(sum);
            } else {
                // Bottom border filter
                int32_t sum = 11 * static_cast<int32_t>(src[y * src_stride + x])
                            - 5 * static_cast<int32_t>(src[(y + 1) * src_stride + x])
                            - 4 * static_cast<int32_t>(src[(y - 1) * src_stride + x])
                            - 4 * static_cast<int32_t>(src[(y - 2) * src_stride + x])
                            + 1 * static_cast<int32_t>(src[(y - 3) * src_stride + x])
                            + 1 * static_cast<int32_t>(src[(y - 4) * src_stride + x]);
                sum = FloorDivide8(sum + 4);
                high_row[x] = Saturate16S(sum);
            }
        }
    }

    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus HyperCine_ForwardSpatial2D_CPU(
    const int16_t* src, int width, int height, int src_stride,
    int16_t* ll, int ll_stride,
    int16_t* lh, int lh_stride,
    int16_t* hl, int hl_stride,
    int16_t* hh, int hh_stride
) {
    if (!src || !ll || !lh || !hl || !hh) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width < 6 || (width % 2) != 0 || height < 6 || (height % 2) != 0) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    const int half_w = width / 2;
    const int half_h = height / 2;

    // Intermediate horizontal lowpass and highpass buffers
    std::vector<int16_t> horiz_low(static_cast<size_t>(half_w) * height);
    std::vector<int16_t> horiz_high(static_cast<size_t>(half_w) * height);

    HyperCineStatus status = HyperCine_ForwardHorizontal26_CPU(
        src, width, height, src_stride,
        horiz_low.data(), half_w,
        horiz_high.data(), half_w
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Vertical pass on horizontal lowpass: LL and HL
    status = HyperCine_ForwardVertical26_CPU(
        horiz_low.data(), half_w, height, half_w,
        ll, ll_stride,
        hl, hl_stride
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Vertical pass on horizontal highpass: LH and HH
    status = HyperCine_ForwardVertical26_CPU(
        horiz_high.data(), half_w, height, half_w,
        lh, lh_stride,
        hh, hh_stride
    );
    return status;
}

HyperCineStatus HyperCine_InverseVertical26_CPU(
    const int16_t* lowpass, int low_stride,
    const int16_t* highpass, int high_stride,
    int width, int height,
    int16_t* dst, int dst_stride
) {
    if (!lowpass || !highpass || !dst) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width <= 0 || height < 3) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (low_stride < width || high_stride < width || dst_stride < width) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    for (int r = 0; r < height; ++r) {
        int16_t* even_row = dst + (2 * r) * dst_stride;
        int16_t* odd_row = dst + (2 * r + 1) * dst_stride;

        for (int x = 0; x < width; ++x) {
            int32_t even = 0;
            int32_t odd = 0;

            if (r == 0) {
                // Top border inverse
                even = (11 * static_cast<int32_t>(lowpass[0 * low_stride + x])
                       - 4 * static_cast<int32_t>(lowpass[1 * low_stride + x])
                       + 1 * static_cast<int32_t>(lowpass[2 * low_stride + x])
                       + 4) >> 3;
                even += static_cast<int32_t>(highpass[0 * high_stride + x]);

                odd = (5 * static_cast<int32_t>(lowpass[0 * low_stride + x])
                      + 4 * static_cast<int32_t>(lowpass[1 * low_stride + x])
                      - 1 * static_cast<int32_t>(lowpass[2 * low_stride + x])
                      + 4) >> 3;
                odd -= static_cast<int32_t>(highpass[0 * high_stride + x]);
            } else if (r < height - 1) {
                // Interior inverse
                even = (static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
                       - static_cast<int32_t>(lowpass[(r + 1) * low_stride + x])
                       + 4) >> 3;
                even += static_cast<int32_t>(lowpass[r * low_stride + x])
                      + static_cast<int32_t>(highpass[r * high_stride + x]);

                odd = (- static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
                       + static_cast<int32_t>(lowpass[(r + 1) * low_stride + x])
                       + 4) >> 3;
                odd += static_cast<int32_t>(lowpass[r * low_stride + x])
                     - static_cast<int32_t>(highpass[r * high_stride + x]);
            } else {
                // Bottom border inverse
                even = (5 * static_cast<int32_t>(lowpass[r * low_stride + x])
                       + 4 * static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
                       - 1 * static_cast<int32_t>(lowpass[(r - 2) * low_stride + x])
                       + 4) >> 3;
                even += static_cast<int32_t>(highpass[r * high_stride + x]);

                odd = (11 * static_cast<int32_t>(lowpass[r * low_stride + x])
                      - 4 * static_cast<int32_t>(lowpass[(r - 1) * low_stride + x])
                      + 1 * static_cast<int32_t>(lowpass[(r - 2) * low_stride + x])
                      + 4) >> 3;
                odd -= static_cast<int32_t>(highpass[r * high_stride + x]);
            }

            even_row[x] = Saturate16S(even >> 1);
            odd_row[x] = Saturate16S(odd >> 1);
        }
    }

    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus HyperCine_InverseHorizontal26_CPU(
    const int16_t* lowpass, int low_stride,
    const int16_t* highpass, int high_stride,
    int width, int height,
    int16_t* dst, int dst_stride
) {
    if (!lowpass || !highpass || !dst) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width < 3 || height <= 0) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (low_stride < width || high_stride < width || dst_stride < (2 * width)) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    for (int y = 0; y < height; ++y) {
        const int16_t* low_row = lowpass + y * low_stride;
        const int16_t* high_row = highpass + y * high_stride;
        int16_t* out_row = dst + y * dst_stride;

        for (int c = 0; c < width; ++c) {
            int32_t even = 0;
            int32_t odd = 0;

            if (c == 0) {
                // Left border inverse
                even = (11 * static_cast<int32_t>(low_row[0])
                       - 4 * static_cast<int32_t>(low_row[1])
                       + 1 * static_cast<int32_t>(low_row[2])
                       + 4) >> 3;
                even += static_cast<int32_t>(high_row[0]);

                odd = (5 * static_cast<int32_t>(low_row[0])
                      + 4 * static_cast<int32_t>(low_row[1])
                      - 1 * static_cast<int32_t>(low_row[2])
                      + 4) >> 3;
                odd -= static_cast<int32_t>(high_row[0]);
            } else if (c < width - 1) {
                // Interior inverse
                even = (static_cast<int32_t>(low_row[c - 1])
                       - static_cast<int32_t>(low_row[c + 1])
                       + 4) >> 3;
                even += static_cast<int32_t>(low_row[c])
                      + static_cast<int32_t>(high_row[c]);

                odd = (- static_cast<int32_t>(low_row[c - 1])
                       + static_cast<int32_t>(low_row[c + 1])
                       + 4) >> 3;
                odd += static_cast<int32_t>(low_row[c])
                     - static_cast<int32_t>(high_row[c]);
            } else {
                // Right border inverse
                even = (5 * static_cast<int32_t>(low_row[c])
                       + 4 * static_cast<int32_t>(low_row[c - 1])
                       - 1 * static_cast<int32_t>(low_row[c - 2])
                       + 4) >> 3;
                even += static_cast<int32_t>(high_row[c]);

                odd = (11 * static_cast<int32_t>(low_row[c])
                      - 4 * static_cast<int32_t>(low_row[c - 1])
                      + 1 * static_cast<int32_t>(low_row[c - 2])
                      + 4) >> 3;
                odd -= static_cast<int32_t>(high_row[c]);
            }

            out_row[2 * c] = Saturate16S(even >> 1);
            out_row[2 * c + 1] = Saturate16S(odd >> 1);
        }
    }

    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus HyperCine_InverseSpatial2D_CPU(
    const int16_t* ll, int ll_stride,
    const int16_t* lh, int lh_stride,
    const int16_t* hl, int hl_stride,
    const int16_t* hh, int hh_stride,
    int subband_width, int subband_height,
    int16_t* dst, int dst_stride
) {
    if (!ll || !lh || !hl || !hh || !dst) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (subband_width < 3 || subband_height < 3) return HYPERCINE_STATUS_INVALID_ARGUMENT;

    const int recon_h = subband_height * 2;
    const int recon_w = subband_width * 2;
    if (dst_stride < recon_w) return HYPERCINE_STATUS_INVALID_ARGUMENT;

    // Intermediate vertical inverse results: horiz_low (from LL and HL) and horiz_high (from LH and HH)
    std::vector<int16_t> horiz_low(static_cast<size_t>(subband_width) * recon_h);
    std::vector<int16_t> horiz_high(static_cast<size_t>(subband_width) * recon_h);

    // Reconstruct vertical lowpass
    HyperCineStatus status = HyperCine_InverseVertical26_CPU(
        ll, ll_stride,
        hl, hl_stride,
        subband_width, subband_height,
        horiz_low.data(), subband_width
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Reconstruct vertical highpass
    status = HyperCine_InverseVertical26_CPU(
        lh, lh_stride,
        hh, hh_stride,
        subband_width, subband_height,
        horiz_high.data(), subband_width
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Reconstruct final image from horiz_low and horiz_high
    return HyperCine_InverseHorizontal26_CPU(
        horiz_low.data(), subband_width,
        horiz_high.data(), subband_width,
        subband_width, recon_h,
        dst, dst_stride
    );
}
