// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Tiny SEH boundary so the Phase 3B-0 probe can record an encoder or
// decoder access violation and continue with the next sample.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
int HyperCineSehCall(void (*fn)(void*), void* arg, unsigned long* code) {
    __try {
        fn(arg);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (code) *code = GetExceptionCode();
        return 1;
    }
}
#else
int HyperCineSehCall(void (*fn)(void*), void* arg, unsigned long* code) {
    (void)code;
    fn(arg);
    return 0;
}
#endif
