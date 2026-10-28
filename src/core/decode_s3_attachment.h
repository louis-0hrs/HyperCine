// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "decode_task_graph.h"

namespace hypercine::detail {
// Synthetic S3 attachment only. Four graph-owned dequantized wavelet5 bands
// feed one same-channel wavelet4 lowpass. No parser or frame decoder.
// Readiness is expressed as four graph edges; no worker waits for a producer.
TaskSpec MakeSpatialLowpassTask(Generation generation, std::uint32_t channel,
                               std::uint32_t width, std::uint32_t height,
                               std::size_t input_pitch);
} // namespace hypercine::detail
