// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void DecodeProfileReset(void);
void DecodeProfileEnter(int stage);
void DecodeProfileLeave(void);
double DecodeProfileMilliseconds(int stage);
unsigned long long DecodeProfileCalls(int stage);
#ifdef __cplusplus
}
#endif
