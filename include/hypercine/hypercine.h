// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_H
#define HYPERCINE_H

/**
 * @file hypercine.h
 * @brief Main header for HyperCine - Modern GPU-accelerated CineForm codec
 *
 * Dual licensed under MIT OR Apache-2.0, preserving upstream GoPro CineForm SDK licensing.
 */

#define HYPERCINE_VERSION_MAJOR 0
#define HYPERCINE_VERSION_MINOR 1
#define HYPERCINE_VERSION_PATCH 0
#define HYPERCINE_VERSION_STRING "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/* Upstream reference headers provide full codec and format definitions */
#include "CFHDTypes.h"
#include "CFHDError.h"

/**
 * @brief Returns the version string for HyperCine.
 */
inline const char* HyperCine_GetVersionString(void) {
    return HYPERCINE_VERSION_STRING;
}

#ifdef __cplusplus
}
#endif

#endif /* HYPERCINE_H */
