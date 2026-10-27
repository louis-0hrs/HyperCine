// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../core/filmscan3_backend.h"
#include "filmscan3_cuda_stats.h"
#include "hypercine/wavelet.h"
#include <atomic>
#include <cuda_runtime.h>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hypercine { namespace standalone { namespace detail {
// Accounting only: a monotonic total of successful spatial launches across every
// session in this process. It is never read to decide whether a launch should fail,
// so it cannot make one session observe another session's state.
std::atomic<std::uint64_t> g_cuda_spatial_dispatches_total{0};
namespace {

[[noreturn]] void ThrowCuda(cudaError_t error, const char* operation) {
    std::ostringstream message;
    message << operation << ": " << cudaGetErrorString(error);
    throw std::runtime_error(message.str());
}
void CheckCuda(cudaError_t error, const char* operation) {
    if (error != cudaSuccess) ThrowCuda(error, operation);
}
void CheckWavelet(HyperCineStatus status, const char* operation) {
    if (status != HYPERCINE_STATUS_SUCCESS) {
        std::ostringstream message;
        message << operation << " returned status " << static_cast<int>(status);
        throw std::runtime_error(message.str());
    }
}

class DeviceBuffer {
public:
    explicit DeviceBuffer(std::size_t elements) {
        CheckCuda(cudaMalloc(reinterpret_cast<void**>(&pointer_), elements * sizeof(int16_t)), "cudaMalloc");
    }
    ~DeviceBuffer() { if (pointer_) cudaFree(pointer_); }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    int16_t* get() const noexcept { return pointer_; }
private:
    int16_t* pointer_ = nullptr;
};

class Stream {
public:
    Stream() { CheckCuda(cudaStreamCreate(&stream_), "cudaStreamCreate"); }
    ~Stream() { if (stream_) cudaStreamDestroy(stream_); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    operator cudaStream_t() const noexcept { return stream_; }
private:
    cudaStream_t stream_ = nullptr;
};

class Cuda final : public Frontend {
public:
    Cuda(FilmScan3Geometry geometry, CudaTestMode mode)
        : geometry_(geometry), mode_(mode) {
        ValidateFilmScan3Geometry(geometry_);
        if (!HyperCine_IsCUDAAvailable()) throw std::runtime_error("CUDA device unavailable");
        CheckCuda(cudaGetDeviceProperties(&properties_, 0), "cudaGetDeviceProperties");
    }
    FilmScan3Geometry Geometry() const noexcept override { return geometry_; }
    // Test seam accessors; see filmscan3_cuda_stats.h. Instance-scoped by design.
    // Public only because the free seam functions reach them; this class is an
    // internal implementation detail of the CUDA frontend, not a public API.
    std::uint64_t DispatchCount() const noexcept {
        return dispatches_.load(std::memory_order_relaxed);
    }
    void ArmFailureAfter(std::uint64_t value) noexcept {
        fail_after_.store(value, std::memory_order_relaxed);
    }
    // `fail_after_ == 0` means disarmed. Reads only this session's counters.
    bool ShouldFailThisSession() const noexcept {
        const std::uint64_t fail_after = fail_after_.load(std::memory_order_relaxed);
        if (!fail_after) return false;
        return dispatches_.load(std::memory_order_relaxed) >= fail_after;
    }
    void NoteDispatch() noexcept {
        dispatches_.fetch_add(1, std::memory_order_relaxed);
        g_cuda_spatial_dispatches_total.fetch_add(1, std::memory_order_relaxed);
    }
private:
    Planes Convert(const uint8_t* data, std::size_t bytes, int width, int height,
                   std::size_t stride) override {
        // Color conversion remains the existing scalar FilmScan3 implementation.
        return ConvertFilmScan3Bgra(data, bytes, width, height, stride);
    }
    WaveletLevel Forward(const Plane& input, int prescale) override {
        ValidateWaveletInput(input, prescale);
        // The middle level has FilmScan3's special prescale-2 rounding semantics
        // and intentionally remains on the existing CPU path.
        if (prescale == 2) return ForwardLevel(input, prescale);

        WaveletLevel output{{Plane(input.width/2,input.height/2),
                             Plane(input.width/2,input.height/2),
                             Plane(input.width/2,input.height/2),
                             Plane(input.width/2,input.height/2)}};
        Stream stream;
        DeviceBuffer source(static_cast<std::size_t>(input.stride) * input.height);
        DeviceBuffer ll(static_cast<std::size_t>(output.bands[0].stride) * output.bands[0].height);
        DeviceBuffer lh(static_cast<std::size_t>(output.bands[1].stride) * output.bands[1].height);
        DeviceBuffer hl(static_cast<std::size_t>(output.bands[2].stride) * output.bands[2].height);
        DeviceBuffer hh(static_cast<std::size_t>(output.bands[3].stride) * output.bands[3].height);

        const auto input_width_bytes=static_cast<std::size_t>(input.width)*sizeof(int16_t);
        CheckCuda(cudaMemcpy2DAsync(source.get(),static_cast<std::size_t>(input.stride)*sizeof(int16_t),
                    input.pixels.data(),static_cast<std::size_t>(input.stride)*sizeof(int16_t),
                    input_width_bytes,input.height,cudaMemcpyHostToDevice,stream),"CUDA FilmScan3 upload");
        constexpr unsigned char poison=0x5a;
        for (auto* pointer : {ll.get(),lh.get(),hl.get(),hh.get()})
            CheckCuda(cudaMemsetAsync(pointer,poison,
                      static_cast<std::size_t>(output.bands[0].stride)*output.bands[0].height*sizeof(int16_t),
                      stream),"CUDA FilmScan3 poison");

        if (mode_ != CudaTestMode::SkipKernel) {
            if (ShouldFailThisSession()) {
                // Real, deterministic CUDA failure (allocation of size_t max).
                // Measured on this driver: the next cudaMalloc still succeeds,
                // but cudaPeekAtLastError stays at out-of-memory until cleared.
                // Clear it here so the failed frame cannot leak that status.
                void* sink = nullptr;
                const cudaError_t err = cudaMalloc(&sink, static_cast<std::size_t>(-1));
                if (err == cudaSuccess) {
                    if (sink) cudaFree(sink);
                    throw std::runtime_error("CUDA FilmScan3 mid-frame failure: injection did not fail");
                }
                cudaGetLastError();
                std::ostringstream message;
                message << "CUDA FilmScan3 mid-frame failure: " << cudaGetErrorString(err)
                        << " code=" << static_cast<int>(err);
                throw std::runtime_error(message.str());
            }
            CheckWavelet(HyperCine_ForwardSpatial2D_CUDA(
                HYPERCINE_BACKEND_CUDA_REFERENCE,
                source.get(),input.width,input.height,input.stride,
                ll.get(),output.bands[0].stride,
                lh.get(),output.bands[1].stride,
                hl.get(),output.bands[2].stride,
                hh.get(),output.bands[3].stride,stream),"CUDA FilmScan3 spatial wavelet");
            NoteDispatch();
        }

        int16_t* device_bands[]={ll.get(),lh.get(),hl.get(),hh.get()};
        for (int band=0;band<4;++band) {
            auto& host=output.bands[band];
            CheckCuda(cudaMemcpy2DAsync(host.pixels.data(),static_cast<std::size_t>(host.stride)*sizeof(int16_t),
                        device_bands[band],static_cast<std::size_t>(host.stride)*sizeof(int16_t),
                        static_cast<std::size_t>(host.width)*sizeof(int16_t),host.height,
                        cudaMemcpyDeviceToHost,stream),"CUDA FilmScan3 readback");
        }
        CheckCuda(cudaStreamSynchronize(stream),"CUDA FilmScan3 synchronize");
        if (mode_ == CudaTestMode::AlterOutput) output.bands[3].pixels[0] = static_cast<int16_t>(output.bands[3].pixels[0] ^ 0x4000);
        return output;
    }
    std::string DeviceName() const override {
        return std::string(properties_.name) + " CUDA spatial wavelet";
    }
    FilmScan3Geometry geometry_;
    CudaTestMode mode_;
    cudaDeviceProp properties_{};
    // Fault injection is per session, not per process. A test that arms one session
    // must not be able to trip a concurrent unrelated session's launch, which is
    // exactly what a shared counter did.
    mutable std::atomic<std::uint64_t> dispatches_{0};
    mutable std::atomic<std::uint64_t> fail_after_{0};
};
} // namespace

namespace {
// The test-facing functions below are documented in filmscan3_cuda_stats.h. They
// reach the per-session state through the concrete type, so the public Frontend
// interface stays free of test seams.
Cuda* AsCuda(const Frontend& frontend) {
    return dynamic_cast<Cuda*>(const_cast<Frontend*>(&frontend));
}
const Cuda* AsCudaConst(const Frontend& frontend) {
    return dynamic_cast<const Cuda*>(&frontend);
}
}  // namespace

std::uint64_t CudaSpatialDispatchCount(const Frontend& frontend) noexcept {
    const Cuda* cuda = AsCudaConst(frontend);
    return cuda ? cuda->DispatchCount() : 0;
}
std::uint64_t ProcessCudaSpatialDispatchTotal() noexcept {
    return g_cuda_spatial_dispatches_total.load(std::memory_order_relaxed);
}
void ArmCudaMidframeFailureAfter(Frontend& frontend, std::uint64_t successful_dispatches) {
    Cuda* cuda = AsCuda(frontend);
    if (!cuda) throw std::invalid_argument("mid-frame injection requires a CUDA frontend");
    cuda->ArmFailureAfter(successful_dispatches);
}
bool CudaSpatialLaunchShouldFail(const Frontend& frontend) noexcept {
    const Cuda* cuda = AsCudaConst(frontend);
    return cuda ? cuda->ShouldFailThisSession() : false;
}

std::shared_ptr<Frontend> CudaFrontend(FilmScan3Geometry geometry, CudaTestMode mode) {
    return std::make_shared<Cuda>(geometry,mode);
}

}}} // namespace hypercine::standalone::detail
