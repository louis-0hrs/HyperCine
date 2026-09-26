// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Forced into the threaded upstream library with /FI.
// upstream/Codec/encoder.h names ENCODER in the threaded structs before the
// typedef. C accepts that use. C++ does not. This forward declaration lets the
// same pristine sources compile with _THREADED_ENCODER=1.
typedef struct encoder ENCODER;
