/* Copyright (c) 2026 HyperCine Contributors.
 * Dual-licensed under the MIT License or Apache License, Version 2.0.
 * See LICENSE-MIT and LICENSE-APACHE for details.
 */
/* The main header must be first: prior includes used to hide missing size_t. */
#include "hypercine/hypercine.h"
#include <string.h>

int main(void) {
    const char* (*version)(void) = HyperCine_GetVersionString;
    return strcmp(version(), HYPERCINE_VERSION_STRING) != 0;
}
