// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "wavelet_cuda_common.cuh"

// Block dimensions for optimized kernels
#define OPT_HORIZ_THREADS_X 128
#define OPT_HORIZ_THREADS_Y 2
#define OPT_VERT_THREADS_X 32
#define OPT_VERT_THREADS_Y 16

/* ========================================================================= */
/* Optimized Forward Horizontal Kernel with Shared Memory Tiling             */
/* ========================================================================= */

__global__ void ForwardHorizontal26_OptKernel(
    const int16_t* __restrict__ src, int width, int height, int src_stride,
    int16_t* __restrict__ lowpass, int low_stride,
    int16_t* __restrict__ highpass, int high_stride
) {
    // Shared memory layout: each row has OPT_HORIZ_THREADS_X * 2 pixels + 4 halo pixels
    // halo: 2 pixels left, 2 pixels right
    const int TILE_IN_WIDTH = OPT_HORIZ_THREADS_X * 2;
    __shared__ int16_t s_row[OPT_HORIZ_THREADS_Y][TILE_IN_WIDTH + 4];

    const int tx = threadIdx.x;
    const int ty = threadIdx.y;
    const int bx = blockIdx.x;
    const int by = blockIdx.y;

    const int y = by * OPT_HORIZ_THREADS_Y + ty;
    const int half_w = width / 2;
    const int block_in_start = bx * TILE_IN_WIDTH;

    if (y < height) {
        const int16_t* in_row = src + y * src_stride;

        // Cooperative loading of tile + halo
        // Each thread loads 2 input elements
        int in_x0 = block_in_start + tx * 2;
        int in_x1 = in_x0 + 1;

        s_row[ty][tx * 2 + 2] = (in_x0 < width) ? in_row[in_x0] : 0;
        s_row[ty][tx * 2 + 3] = (in_x1 < width) ? in_row[in_x1] : 0;

        // Left halo (2 elements) loaded by thread 0
        if (tx == 0) {
            s_row[ty][0] = (block_in_start >= 2) ? in_row[block_in_start - 2] : 0;
            s_row[ty][1] = (block_in_start >= 1) ? in_row[block_in_start - 1] : 0;
        }
        // Right halo (2 elements) loaded by thread 1
        if (tx == 1) {
            int right_idx0 = block_in_start + TILE_IN_WIDTH;
            int right_idx1 = right_idx0 + 1;
            s_row[ty][TILE_IN_WIDTH + 2] = (right_idx0 < width) ? in_row[right_idx0] : 0;
            s_row[ty][TILE_IN_WIDTH + 3] = (right_idx1 < width) ? in_row[right_idx1] : 0;
        }
    }

    __syncthreads();

    if (y >= height) return;

    const int c = bx * OPT_HORIZ_THREADS_X + tx;
    if (c >= half_w) return;

    const int sx = tx * 2 + 2; // Index in shared memory corresponding to in_row[c * 2]

    // Lowpass: sum of adjacent pair
    int32_t low_sum = static_cast<int32_t>(s_row[ty][sx]) + static_cast<int32_t>(s_row[ty][sx + 1]);
    lowpass[y * low_stride + c] = CUDASaturate16S(low_sum);

    // Highpass: 2-6 biorthogonal wavelet filter
    if (c == 0) {
        // Global left border filter
        int32_t sum = 5 * static_cast<int32_t>(s_row[ty][sx + 0])
                    - 11 * static_cast<int32_t>(s_row[ty][sx + 1])
                    + 4 * static_cast<int32_t>(s_row[ty][sx + 2])
                    + 4 * static_cast<int32_t>(s_row[ty][sx + 3])
                    - 1 * static_cast<int32_t>(s_row[ty][sx + 4])
                    - 1 * static_cast<int32_t>(s_row[ty][sx + 5]);
        sum = (sum + 4) >> 3;
        highpass[y * high_stride + 0] = CUDASaturate16S(sum);
    } else if (c < half_w - 1) {
        // Interior filter
        int32_t hp_sum = - static_cast<int32_t>(s_row[ty][sx - 2])
                         - static_cast<int32_t>(s_row[ty][sx - 1])
                         + static_cast<int32_t>(s_row[ty][sx + 2])
                         + static_cast<int32_t>(s_row[ty][sx + 3]);
        hp_sum = (hp_sum + 4) >> 3;
        int32_t sum = hp_sum + static_cast<int32_t>(s_row[ty][sx]) - static_cast<int32_t>(s_row[ty][sx + 1]);
        highpass[y * high_stride + c] = CUDASaturate16S(sum);
    } else {
        // Global right border filter
        int32_t sum = 11 * static_cast<int32_t>(s_row[ty][sx + 0])
                    - 5 * static_cast<int32_t>(s_row[ty][sx + 1])
                    - 4 * static_cast<int32_t>(s_row[ty][sx - 1])
                    - 4 * static_cast<int32_t>(s_row[ty][sx - 2])
                    + 1 * static_cast<int32_t>(s_row[ty][sx - 3])
                    + 1 * static_cast<int32_t>(s_row[ty][sx - 4]);
        sum = (sum + 4) >> 3;
        highpass[y * high_stride + c] = CUDASaturate16S(sum);
    }
}

/* ========================================================================= */
/* Optimized Forward Vertical Kernel with 2D Shared Memory Tiling            */
/* ========================================================================= */

__global__ void ForwardVertical26_OptKernel(
    const int16_t* __restrict__ src, int width, int height, int src_stride,
    int16_t* __restrict__ lowpass, int low_stride,
    int16_t* __restrict__ highpass, int high_stride
) {
    // 32 columns x (16 output rows * 2 input rows + 4 halo rows) = 32 x 36
    // Padding width by +1 to 33 elements to eliminate shared memory bank conflicts
    const int TILE_IN_H = OPT_VERT_THREADS_Y * 2; // 32
    __shared__ int16_t s_col[TILE_IN_H + 4][OPT_VERT_THREADS_X + 1];

    const int tx = threadIdx.x;
    const int ty = threadIdx.y;
    const int bx = blockIdx.x;
    const int by = blockIdx.y;

    const int x = bx * OPT_VERT_THREADS_X + tx;
    const int half_h = height / 2;
    const int block_in_start_y = by * TILE_IN_H;

    // Load main 32 rows into shared memory
    // ty in [0, 15], so each thread tx loads 2 rows
    if (x < width) {
        int y0 = block_in_start_y + ty * 2;
        int y1 = y0 + 1;
        s_col[ty * 2 + 2][tx] = (y0 < height) ? src[y0 * src_stride + x] : 0;
        s_col[ty * 2 + 3][tx] = (y1 < height) ? src[y1 * src_stride + x] : 0;

        // Top halo (2 rows) loaded by ty == 0
        if (ty == 0) {
            s_col[0][tx] = (block_in_start_y >= 2) ? src[(block_in_start_y - 2) * src_stride + x] : 0;
            s_col[1][tx] = (block_in_start_y >= 1) ? src[(block_in_start_y - 1) * src_stride + x] : 0;
        }
        // Bottom halo (2 rows) loaded by ty == 1
        if (ty == 1) {
            int bot_y0 = block_in_start_y + TILE_IN_H;
            int bot_y1 = bot_y0 + 1;
            s_col[TILE_IN_H + 2][tx] = (bot_y0 < height) ? src[bot_y0 * src_stride + x] : 0;
            s_col[TILE_IN_H + 3][tx] = (bot_y1 < height) ? src[bot_y1 * src_stride + x] : 0;
        }
    }

    __syncthreads();

    if (x >= width) return;

    const int r = by * OPT_VERT_THREADS_Y + ty;
    if (r >= half_h) return;

    const int sy = ty * 2 + 2;

    // Lowpass: sum of adjacent pair in column
    int32_t low_sum = static_cast<int32_t>(s_col[sy][tx]) + static_cast<int32_t>(s_col[sy + 1][tx]);
    lowpass[r * low_stride + x] = CUDASaturate16S(low_sum);

    // Highpass
    if (r == 0) {
        // Global top border filter
        int32_t sum = 5 * static_cast<int32_t>(s_col[sy + 0][tx])
                    - 11 * static_cast<int32_t>(s_col[sy + 1][tx])
                    + 4 * static_cast<int32_t>(s_col[sy + 2][tx])
                    + 4 * static_cast<int32_t>(s_col[sy + 3][tx])
                    - 1 * static_cast<int32_t>(s_col[sy + 4][tx])
                    - 1 * static_cast<int32_t>(s_col[sy + 5][tx]);
        sum = (sum + 4) >> 3;
        highpass[r * high_stride + x] = CUDASaturate16S(sum);
    } else if (r < half_h - 1) {
        // Interior filter
        int32_t hp_sum = - static_cast<int32_t>(s_col[sy - 2][tx])
                         - static_cast<int32_t>(s_col[sy - 1][tx])
                         + static_cast<int32_t>(s_col[sy + 2][tx])
                         + static_cast<int32_t>(s_col[sy + 3][tx]);
        hp_sum = (hp_sum + 4) >> 3;
        int32_t sum = hp_sum + static_cast<int32_t>(s_col[sy][tx]) - static_cast<int32_t>(s_col[sy + 1][tx]);
        highpass[r * high_stride + x] = CUDASaturate16S(sum);
    } else {
        // Global bottom border filter
        int32_t sum = 11 * static_cast<int32_t>(s_col[sy + 0][tx])
                    - 5 * static_cast<int32_t>(s_col[sy + 1][tx])
                    - 4 * static_cast<int32_t>(s_col[sy - 1][tx])
                    - 4 * static_cast<int32_t>(s_col[sy - 2][tx])
                    + 1 * static_cast<int32_t>(s_col[sy - 3][tx])
                    + 1 * static_cast<int32_t>(s_col[sy - 4][tx]);
        sum = (sum + 4) >> 3;
        highpass[r * high_stride + x] = CUDASaturate16S(sum);
    }
}

/* ========================================================================= */
/* Optimized Inverse Vertical Kernel                                         */
/* ========================================================================= */

__global__ void InverseVertical26_OptKernel(
    const int16_t* __restrict__ lowpass, int low_stride,
    const int16_t* __restrict__ highpass, int high_stride,
    int width, int height,
    int16_t* __restrict__ dst, int dst_stride
) {
    const int TILE_H = OPT_VERT_THREADS_Y; // 16
    __shared__ int16_t s_low[TILE_H + 4][OPT_VERT_THREADS_X + 1];

    const int tx = threadIdx.x;
    const int ty = threadIdx.y;
    const int bx = blockIdx.x;
    const int by = blockIdx.y;

    const int x = bx * OPT_VERT_THREADS_X + tx;
    const int block_start_r = by * TILE_H;

    if (x < width) {
        int r = block_start_r + ty;
        s_low[ty + 2][tx] = (r < height) ? lowpass[r * low_stride + x] : 0;

        if (ty == 0) {
            s_low[0][tx] = (block_start_r >= 2) ? lowpass[(block_start_r - 2) * low_stride + x] : 0;
            s_low[1][tx] = (block_start_r >= 1) ? lowpass[(block_start_r - 1) * low_stride + x] : 0;
        }
        if (ty == 1) {
            int bot_r0 = block_start_r + TILE_H;
            int bot_r1 = bot_r0 + 1;
            s_low[TILE_H + 2][tx] = (bot_r0 < height) ? lowpass[bot_r0 * low_stride + x] : 0;
            s_low[TILE_H + 3][tx] = (bot_r1 < height) ? lowpass[bot_r1 * low_stride + x] : 0;
        }
    }

    __syncthreads();

    if (x >= width) return;

    const int r = block_start_r + ty;
    if (r >= height) return;

    const int sr = ty + 2;
    const int32_t hp_val = static_cast<int32_t>(highpass[r * high_stride + x]);

    int32_t even = 0;
    int32_t odd = 0;

    if (r == 0) {
        even = (11 * static_cast<int32_t>(s_low[sr + 0][tx])
               - 4 * static_cast<int32_t>(s_low[sr + 1][tx])
               + 1 * static_cast<int32_t>(s_low[sr + 2][tx])
               + 4) >> 3;
        even += hp_val;

        odd = (5 * static_cast<int32_t>(s_low[sr + 0][tx])
              + 4 * static_cast<int32_t>(s_low[sr + 1][tx])
              - 1 * static_cast<int32_t>(s_low[sr + 2][tx])
              + 4) >> 3;
        odd -= hp_val;
    } else if (r < height - 1) {
        even = (static_cast<int32_t>(s_low[sr - 1][tx])
               - static_cast<int32_t>(s_low[sr + 1][tx])
               + 4) >> 3;
        even += static_cast<int32_t>(s_low[sr][tx]) + hp_val;

        odd = (- static_cast<int32_t>(s_low[sr - 1][tx])
               + static_cast<int32_t>(s_low[sr + 1][tx])
               + 4) >> 3;
        odd += static_cast<int32_t>(s_low[sr][tx]) - hp_val;
    } else {
        even = (5 * static_cast<int32_t>(s_low[sr][tx])
               + 4 * static_cast<int32_t>(s_low[sr - 1][tx])
               - 1 * static_cast<int32_t>(s_low[sr - 2][tx])
               + 4) >> 3;
        even += hp_val;

        odd = (11 * static_cast<int32_t>(s_low[sr][tx])
              - 4 * static_cast<int32_t>(s_low[sr - 1][tx])
              + 1 * static_cast<int32_t>(s_low[sr - 2][tx])
              + 4) >> 3;
        odd -= hp_val;
    }

    dst[(2 * r) * dst_stride + x] = CUDASaturate16S(even >> 1);
    dst[(2 * r + 1) * dst_stride + x] = CUDASaturate16S(odd >> 1);
}

/* ========================================================================= */
/* Optimized Inverse Horizontal Kernel                                       */
/* ========================================================================= */

__global__ void InverseHorizontal26_OptKernel(
    const int16_t* __restrict__ lowpass, int low_stride,
    const int16_t* __restrict__ highpass, int high_stride,
    int width, int height,
    int16_t* __restrict__ dst, int dst_stride
) {
    const int TILE_W = OPT_HORIZ_THREADS_X; // 128
    __shared__ int16_t s_low[OPT_HORIZ_THREADS_Y][TILE_W + 4];

    const int tx = threadIdx.x;
    const int ty = threadIdx.y;
    const int bx = blockIdx.x;
    const int by = blockIdx.y;

    const int y = by * OPT_HORIZ_THREADS_Y + ty;
    const int block_start_c = bx * TILE_W;

    if (y < height) {
        const int16_t* low_row = lowpass + y * low_stride;
        int c = block_start_c + tx;
        s_low[ty][tx + 2] = (c < width) ? low_row[c] : 0;

        if (tx == 0) {
            s_low[ty][0] = (block_start_c >= 2) ? low_row[block_start_c - 2] : 0;
            s_low[ty][1] = (block_start_c >= 1) ? low_row[block_start_c - 1] : 0;
        }
        if (tx == 1) {
            int right_c0 = block_start_c + TILE_W;
            int right_c1 = right_c0 + 1;
            s_low[ty][TILE_W + 2] = (right_c0 < width) ? low_row[right_c0] : 0;
            s_low[ty][TILE_W + 3] = (right_c1 < width) ? low_row[right_c1] : 0;
        }
    }

    __syncthreads();

    if (y >= height) return;

    const int c = block_start_c + tx;
    if (c >= width) return;

    const int sc = tx + 2;
    const int32_t hp_val = static_cast<int32_t>(highpass[y * high_stride + c]);

    int32_t even = 0;
    int32_t odd = 0;

    if (c == 0) {
        even = (11 * static_cast<int32_t>(s_low[ty][sc + 0])
               - 4 * static_cast<int32_t>(s_low[ty][sc + 1])
               + 1 * static_cast<int32_t>(s_low[ty][sc + 2])
               + 4) >> 3;
        even += hp_val;

        odd = (5 * static_cast<int32_t>(s_low[ty][sc + 0])
              + 4 * static_cast<int32_t>(s_low[ty][sc + 1])
              - 1 * static_cast<int32_t>(s_low[ty][sc + 2])
              + 4) >> 3;
        odd -= hp_val;
    } else if (c < width - 1) {
        even = (static_cast<int32_t>(s_low[ty][sc - 1])
               - static_cast<int32_t>(s_low[ty][sc + 1])
               + 4) >> 3;
        even += static_cast<int32_t>(s_low[ty][sc]) + hp_val;

        odd = (- static_cast<int32_t>(s_low[ty][sc - 1])
               + static_cast<int32_t>(s_low[ty][sc + 1])
               + 4) >> 3;
        odd += static_cast<int32_t>(s_low[ty][sc]) - hp_val;
    } else {
        even = (5 * static_cast<int32_t>(s_low[ty][sc])
               + 4 * static_cast<int32_t>(s_low[ty][sc - 1])
               - 1 * static_cast<int32_t>(s_low[ty][sc - 2])
               + 4) >> 3;
        even += hp_val;

        odd = (11 * static_cast<int32_t>(s_low[ty][sc])
              - 4 * static_cast<int32_t>(s_low[ty][sc - 1])
              + 1 * static_cast<int32_t>(s_low[ty][sc - 2])
              + 4) >> 3;
        odd -= hp_val;
    }

    dst[y * dst_stride + 2 * c] = CUDASaturate16S(even >> 1);
    dst[y * dst_stride + 2 * c + 1] = CUDASaturate16S(odd >> 1);
}

HyperCineStatus ForwardHorizontal26_Optimized(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
) {
    const int half_w = width / 2;
    dim3 block(OPT_HORIZ_THREADS_X, OPT_HORIZ_THREADS_Y);
    dim3 grid((half_w + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    ForwardHorizontal26_OptKernel<<<grid, block, 0, stream>>>(
        d_src, width, height, src_stride,
        d_low, low_stride,
        d_high, high_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus ForwardVertical26_Optimized(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
) {
    const int half_h = height / 2;
    dim3 block(OPT_VERT_THREADS_X, OPT_VERT_THREADS_Y);
    dim3 grid((width + block.x - 1) / block.x, (half_h + block.y - 1) / block.y);

    ForwardVertical26_OptKernel<<<grid, block, 0, stream>>>(
        d_src, width, height, src_stride,
        d_low, low_stride,
        d_high, high_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus InverseVertical26_Optimized(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
) {
    dim3 block(OPT_VERT_THREADS_X, OPT_VERT_THREADS_Y);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    InverseVertical26_OptKernel<<<grid, block, 0, stream>>>(
        d_low, low_stride,
        d_high, high_stride,
        width, height,
        d_dst, dst_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus InverseHorizontal26_Optimized(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
) {
    dim3 block(OPT_HORIZ_THREADS_X, OPT_HORIZ_THREADS_Y);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    InverseHorizontal26_OptKernel<<<grid, block, 0, stream>>>(
        d_low, low_stride,
        d_high, high_stride,
        width, height,
        d_dst, dst_stride
    );
    CUDA_CHECK_KERNEL();
    return HYPERCINE_STATUS_SUCCESS;
}
