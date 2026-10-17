// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Isolated diagnostic compile of the pristine translation unit. Redirect only
// its inverse-lowpass CALL boundary; the original wavelet implementation runs.
// No frozen source file or production target is changed.
#define TransformInverseSpatialQuantLowpass HyperCineObserveInverseLowpass
#include "../../upstream/Codec/decoder.c"
