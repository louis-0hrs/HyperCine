// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/decoder.h"
#include <cuda_runtime_api.h>
namespace HyperCine {
inline DecodeConsumerStream CudaDecodeConsumer(cudaStream_t stream) {
    return {reinterpret_cast<void*>(stream)};
}
inline cudaEvent_t CudaDecodeReadyEvent(const DeviceOutput& output) {
    return reinterpret_cast<cudaEvent_t>(output.native_ready_event);
}
} // namespace HyperCine
