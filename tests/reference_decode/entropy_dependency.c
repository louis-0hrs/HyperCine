// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Manual Windows diagnostic only. Header guards retain this replacement at
// exactly the three queue predicates in entropy_threading.c; no global change.
#include "config.h"
#include "decoder.h"
#include "image.h"
bool HyperCineDependencyReady(DECODER*, TRANSFORM*, int, int, IMAGE*);
#undef BANDS_ALL_VALID
#define BANDS_ALL_VALID(w) HyperCineDependencyReady(decoder, transform, channel, index, (w))
#include "../../upstream/Codec/entropy_threading.c"
