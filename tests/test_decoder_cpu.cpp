// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/decoder.h"
#include <iostream>
int main() {
    using namespace HyperCine;
    Decoder d;
    DecodeError e{};
    if (d.QueueDecode(nullptr, 0, {}, &e) || e != DecodeError::NotInitialized) return 1;
    if (d.Initialize() != DecodeError::BackendUnavailable || d.IsInitialized() || d.Capabilities().device_backend_available) return 1;
    DeviceOutput out;
    if (d.GetDeviceOutput({}, &out) || out.plane_count || d.ReleaseOutput({}) || d.StreamWait({}, {})) return 1;
    DecoderConfig invalid; invalid.slot_count = 5;
    Decoder bad(invalid);
    if (bad.Initialize() != DecodeError::InvalidConfig) return 1;
    std::cout << "CPU-only production header, unavailable backend and invalid config PASS\n";
}
