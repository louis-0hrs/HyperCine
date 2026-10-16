// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Forced into the non-upstream C++ translation units on Linux via -include.
// upstream/Common/CFHDAllocator.h declares size_t-based function pointer
// typedefs without including <stddef.h>. Windows/MSVC and older libstdc++
// releases happened to provide size_t transitively; GCC 14 does not, so every
// consumer that reaches CFHDAllocator.h through CFHDTypes.h fails to parse.
// upstream/ stays pristine: the definition is supplied here instead.
#include <stddef.h>
