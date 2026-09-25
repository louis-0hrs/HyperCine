// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/wavelet.h"
#include "wavelet_cuda_internal.h"
#include "wavelet_cuda_common.cuh"
#include <cuda_runtime.h>
#include <string.h>

namespace {

struct ScopedDeviceBuffer {
    int16_t* ptr = nullptr;
    cudaStream_t stream = nullptr;
    bool is_async = false;

    ScopedDeviceBuffer(size_t elements, cudaStream_t s) : stream(s) {
        size_t bytes = elements * sizeof(int16_t);
        cudaError_t err = cudaMallocAsync(&ptr, bytes, s);
        if (err == cudaSuccess) {
            is_async = true;
        } else {
            cudaGetLastError(); // clear error
            err = cudaMalloc(&ptr, bytes);
            is_async = false;
        }
    }

    ~ScopedDeviceBuffer() {
        if (ptr) {
            if (is_async) {
                cudaFreeAsync(ptr, stream);
            } else {
                cudaFree(ptr);
            }
        }
    }

    int16_t* get() const { return ptr; }
    bool valid() const { return ptr != nullptr; }
};

} // anonymous namespace

bool HyperCine_IsCUDAAvailable(void) {
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    return (err == cudaSuccess && device_count > 0);
}

HyperCineStatus HyperCine_GetCUDADeviceInfo(HyperCineCUDADeviceInfo* info) {
    if (!info) return HYPERCINE_STATUS_INVALID_ARGUMENT;

    int device = 0;
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, device));

    strncpy(info->device_name, prop.name, sizeof(info->device_name) - 1);
    info->device_name[sizeof(info->device_name) - 1] = '\0';
    info->major = prop.major;
    info->minor = prop.minor;
    info->multiprocessor_count = prop.multiProcessorCount;
    info->total_global_mem = prop.totalGlobalMem;

    int driver_version = 0, runtime_version = 0;
    cudaDriverGetVersion(&driver_version);
    cudaRuntimeGetVersion(&runtime_version);
    info->cuda_driver_version = driver_version;
    info->cuda_runtime_version = runtime_version;

    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus HyperCine_ForwardHorizontal26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    void* cuda_stream
) {
    if (!d_src || !d_low || !d_high) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width < 6 || (width % 2) != 0 || height <= 0) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (src_stride < width || low_stride < (width / 2) || high_stride < (width / 2)) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);

    if (backend == HYPERCINE_BACKEND_CUDA_OPTIMIZED) {
        return ForwardHorizontal26_Optimized(d_src, width, height, src_stride, d_low, low_stride, d_high, high_stride, stream);
    } else {
        return ForwardHorizontal26_Reference(d_src, width, height, src_stride, d_low, low_stride, d_high, high_stride, stream);
    }
}

HyperCineStatus HyperCine_ForwardVertical26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    void* cuda_stream
) {
    if (!d_src || !d_low || !d_high) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (height < 6 || (height % 2) != 0 || width <= 0) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (src_stride < width || low_stride < width || high_stride < width) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);

    if (backend == HYPERCINE_BACKEND_CUDA_OPTIMIZED) {
        return ForwardVertical26_Optimized(d_src, width, height, src_stride, d_low, low_stride, d_high, high_stride, stream);
    } else {
        return ForwardVertical26_Reference(d_src, width, height, src_stride, d_low, low_stride, d_high, high_stride, stream);
    }
}

HyperCineStatus HyperCine_ForwardSpatial2D_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_ll, int ll_stride,
    int16_t* d_lh, int lh_stride,
    int16_t* d_hl, int hl_stride,
    int16_t* d_hh, int hh_stride,
    void* cuda_stream
) {
    if (!d_src || !d_ll || !d_lh || !d_hl || !d_hh) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width < 6 || (width % 2) != 0 || height < 6 || (height % 2) != 0) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);
    const int half_w = width / 2;
    const size_t intermediate_elements = static_cast<size_t>(half_w) * height;

    ScopedDeviceBuffer d_horiz_low(intermediate_elements, stream);
    ScopedDeviceBuffer d_horiz_high(intermediate_elements, stream);
    if (!d_horiz_low.valid() || !d_horiz_high.valid()) {
        return HYPERCINE_STATUS_OUT_OF_MEMORY;
    }

    // Horizontal Forward
    HyperCineStatus status = HyperCine_ForwardHorizontal26_CUDA(
        backend, d_src, width, height, src_stride,
        d_horiz_low.get(), half_w,
        d_horiz_high.get(), half_w,
        stream
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Vertical Forward on horizontal lowpass: LL and HL
    status = HyperCine_ForwardVertical26_CUDA(
        backend, d_horiz_low.get(), half_w, height, half_w,
        d_ll, ll_stride,
        d_hl, hl_stride,
        stream
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Vertical Forward on horizontal highpass: LH and HH
    return HyperCine_ForwardVertical26_CUDA(
        backend, d_horiz_high.get(), half_w, height, half_w,
        d_lh, lh_stride,
        d_hh, hh_stride,
        stream
    );
}


HyperCineStatus HyperCine_ForwardSpatialQuant2D_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_ll, int ll_stride,
    int16_t* d_lh, int lh_stride,
    int16_t* d_hl, int hl_stride,
    int16_t* d_hh, int hh_stride,
    int quant_ll, int quant_lh, int quant_hl, int quant_hh,
    int prequant_midpoint,
    void* cuda_stream
) {
    if (!d_src || !d_ll || !d_lh || !d_hl || !d_hh) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width < 6 || (width % 2) != 0 || height < 6 || (height % 2) != 0) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);
    const int half_w = width / 2;
    const size_t intermediate_elements = static_cast<size_t>(half_w) * height;

    ScopedDeviceBuffer d_horiz_low(intermediate_elements, stream);
    ScopedDeviceBuffer d_horiz_high(intermediate_elements, stream);
    if (!d_horiz_low.valid() || !d_horiz_high.valid()) {
        return HYPERCINE_STATUS_OUT_OF_MEMORY;
    }

    HyperCineStatus status = HyperCine_ForwardHorizontal26_CUDA(
        backend, d_src, width, height, src_stride,
        d_horiz_low.get(), half_w,
        d_horiz_high.get(), half_w,
        stream
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    return ForwardVerticalQuant26_Reference(
        d_horiz_low.get(), half_w,
        d_horiz_high.get(), half_w,
        half_w, height,
        d_ll, ll_stride,
        d_lh, lh_stride,
        d_hl, hl_stride,
        d_hh, hh_stride,
        quant_ll, quant_lh, quant_hl, quant_hh, prequant_midpoint,
        stream
    );
}

HyperCineStatus HyperCine_ForwardPyramid_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int num_levels,
    const HyperCinePyramidLevel* levels,
    int prequant_midpoint,
    void* cuda_stream
) {
    if (num_levels < 1 || num_levels > 3 || !levels) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    const int16_t* current_src = d_src;
    int current_width = width;
    int current_height = height;
    int current_stride = src_stride;

    for (int i = 0; i < num_levels; ++i) {
        const auto& lvl = levels[i];
        if (lvl.width != current_width || lvl.height != current_height) {
            return HYPERCINE_STATUS_INVALID_ARGUMENT;
        }

        HyperCineStatus status = HyperCine_ForwardSpatialQuant2D_CUDA(
            backend,
            current_src, current_width, current_height, current_stride,
            lvl.d_ll, lvl.stride_ll,
            lvl.d_lh, lvl.stride_lh,
            lvl.d_hl, lvl.stride_hl,
            lvl.d_hh, lvl.stride_hh,
            lvl.quant_ll, lvl.quant_lh, lvl.quant_hl, lvl.quant_hh,
            prequant_midpoint,
            cuda_stream
        );
        if (status != HYPERCINE_STATUS_SUCCESS) return status;

        current_src = lvl.d_ll;
        current_width /= 2;
        current_height /= 2;
        current_stride = lvl.stride_ll;
    }
    return HYPERCINE_STATUS_SUCCESS;
}

HyperCineStatus HyperCine_InverseVertical26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
) {
    if (!d_low || !d_high || !d_dst) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width <= 0 || height < 3) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (low_stride < width || high_stride < width || dst_stride < width) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);

    if (backend == HYPERCINE_BACKEND_CUDA_OPTIMIZED) {
        return InverseVertical26_Optimized(d_low, low_stride, d_high, high_stride, width, height, d_dst, dst_stride, stream);
    } else {
        return InverseVertical26_Reference(d_low, low_stride, d_high, high_stride, width, height, d_dst, dst_stride, stream);
    }
}

HyperCineStatus HyperCine_InverseHorizontal26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
) {
    if (!d_low || !d_high || !d_dst) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (width < 3 || height <= 0) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (low_stride < width || high_stride < width || dst_stride < (2 * width)) {
        return HYPERCINE_STATUS_INVALID_ARGUMENT;
    }

    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);

    if (backend == HYPERCINE_BACKEND_CUDA_OPTIMIZED) {
        return InverseHorizontal26_Optimized(d_low, low_stride, d_high, high_stride, width, height, d_dst, dst_stride, stream);
    } else {
        return InverseHorizontal26_Reference(d_low, low_stride, d_high, high_stride, width, height, d_dst, dst_stride, stream);
    }
}

HyperCineStatus HyperCine_InverseSpatial2D_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_ll, int ll_stride,
    const int16_t* d_lh, int lh_stride,
    const int16_t* d_hl, int hl_stride,
    const int16_t* d_hh, int hh_stride,
    int subband_width, int subband_height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
) {
    if (!d_ll || !d_lh || !d_hl || !d_hh || !d_dst) return HYPERCINE_STATUS_INVALID_ARGUMENT;
    if (subband_width < 3 || subband_height < 3) return HYPERCINE_STATUS_INVALID_ARGUMENT;

    const int recon_h = subband_height * 2;
    const int recon_w = subband_width * 2;
    if (dst_stride < recon_w) return HYPERCINE_STATUS_INVALID_ARGUMENT;

    cudaStream_t stream = static_cast<cudaStream_t>(cuda_stream);
    const size_t intermediate_elements = static_cast<size_t>(subband_width) * recon_h;

    ScopedDeviceBuffer d_horiz_low(intermediate_elements, stream);
    ScopedDeviceBuffer d_horiz_high(intermediate_elements, stream);
    if (!d_horiz_low.valid() || !d_horiz_high.valid()) {
        return HYPERCINE_STATUS_OUT_OF_MEMORY;
    }

    // Inverse Vertical on LL and HL -> horiz_low
    HyperCineStatus status = HyperCine_InverseVertical26_CUDA(
        backend, d_ll, ll_stride, d_hl, hl_stride,
        subband_width, subband_height,
        d_horiz_low.get(), subband_width,
        stream
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Inverse Vertical on LH and HH -> horiz_high
    status = HyperCine_InverseVertical26_CUDA(
        backend, d_lh, lh_stride, d_hh, hh_stride,
        subband_width, subband_height,
        d_horiz_high.get(), subband_width,
        stream
    );
    if (status != HYPERCINE_STATUS_SUCCESS) return status;

    // Inverse Horizontal on horiz_low and horiz_high -> d_dst
    return HyperCine_InverseHorizontal26_CUDA(
        backend, d_horiz_low.get(), subband_width,
        d_horiz_high.get(), subband_width,
        subband_width, recon_h,
        d_dst, dst_stride,
        stream
    );
}
