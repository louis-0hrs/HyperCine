// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include <iostream>
extern "C" {
#include "config.h"
#include "codec.h"
#include "image.h"
}
int main() {
    IMAGE image{}; image.num_bands=4;
    image.band_valid_flags=image.band_started_flags=0x0e;
    // Exercise the ACTUAL frozen macro, not a copied formula. A green
    // diagnostic regression records its blind spot; it does not clear release.
    if(!BANDS_ALL_VALID(&image) || !BANDS_ALL_STARTED(&image) || (image.band_valid_flags&1)!=0) return 1;
    image.band_valid_flags=image.band_started_flags=0x0f;
    if(!BANDS_ALL_VALID(&image) || (image.band_valid_flags&1)==0) return 1;
    image.band_valid_flags=0x07;
    if(BANDS_ALL_VALID(&image)) return 1;
    std::cout << "Frozen completion macro equality accepts 0x0e without lowpass; release remains unresolved\n";
    return 0;
}
