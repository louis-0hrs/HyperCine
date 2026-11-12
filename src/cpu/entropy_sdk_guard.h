// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include "hypercine/entropy_parallel.h"

#include <climits>
#include <cstdint>
#include <iterator>

extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "image.h"
#include "wavelet.h"
#include "vlc.h"

int SetCodingFlags(ENCODER*, int, int*, int*);
}

namespace hypercine { namespace entropy_detail {

// Frozen codebooks.c allocates newcodes::entries[NEW_CODEBOOK_LENGTH]. The
// macro is local to that C file by default; retain its documented build override
// and fail closed if a differently built SDK presents another table extent.
#ifdef NEW_CODEBOOK_LENGTH
constexpr int kRunCodebookLength = NEW_CODEBOOK_LENGTH;
#else
constexpr int kRunCodebookLength = 3072;
#endif

// These wrappers consume live, prepared SDK objects. The caller owns the actual
// extent/lifetime of the pointer array and coefficient storage; a pointer alone
// cannot establish either. Validate metadata before indexing nested arrays.
inline bool ValidChannelCount(int count) noexcept {
    return count > 0 && count <= TRANSFORM_MAX_CHANNELS;
}

inline bool ValidWaveletGeometry(const IMAGE* image) noexcept {
    if (!image || image->width <= 0 || image->height <= 0 || image->pitch <= 0 ||
        image->pitch % static_cast<int>(sizeof(PIXEL)) != 0) return false;
    const auto width = static_cast<std::uint64_t>(image->width);
    const auto height = static_cast<std::uint64_t>(image->height);
    const auto pitch = static_cast<std::uint64_t>(image->pitch) / sizeof(PIXEL);
    // SDK buffer sizes, scan positions and staged bit offsets use signed int.
    return pitch >= width && width * height <= (INT_MAX - 65536u) / 8u &&
           pitch * height <= static_cast<std::uint64_t>(INT_MAX / 32 - 1);
}

inline bool ValidTransform(const TRANSFORM* transform) noexcept {
    if (!transform || transform->type != TRANSFORM_TYPE_SPATIAL || transform->num_frames != 1 ||
        transform->num_spatial < 0 || transform->num_spatial >= TRANSFORM_MAX_LEVELS ||
        transform->num_spatial >= static_cast<int>(std::size(transform->wavelet)) ||
        transform->num_spatial >= static_cast<int>(std::size(transform->prescale)) ||
        transform->num_wavelets <= 0 ||
        transform->num_wavelets > static_cast<int>(std::size(transform->wavelet)) ||
        transform->num_levels <= 0 || transform->num_levels > TRANSFORM_MAX_LEVELS) return false;
    // AllocateTransform's spatial/GOP1 relation. Bound the signed value before
    // adding one, including before SDK SubbandCount later multiplies it by three.
    const int levels = transform->num_spatial + 1;
    if (transform->num_levels != levels || transform->num_wavelets != levels) return false;
    for (int k = 0; k < transform->num_wavelets; ++k) {
        const IMAGE* image = transform->wavelet[k];
        if (!ValidWaveletGeometry(image) || image->num_bands <= 0 ||
            image->num_bands > static_cast<int>(std::size(image->band)) ||
            image->level <= 0 || image->level > transform->num_levels) return false;
        // ExternalCoeff names a level, not an array index. Ambiguous levels
        // must not select different images in the two wrappers.
        for (int previous = 0; previous < k; ++previous) {
            if (transform->wavelet[previous]->level == image->level) return false;
        }
    }
    return true;
}

inline bool ValidTransforms(TRANSFORM* const* transforms, int channel_count) noexcept {
    if (!transforms || !ValidChannelCount(channel_count)) return false;
    for (int channel = 0; channel < channel_count; ++channel) {
        if (!ValidTransform(transforms[channel])) return false;
    }
    return true;
}

inline bool ValidSubband(const ENCODER* encoder, int subband) noexcept {
    return encoder && encoder->num_subbands > 0 && encoder->num_subbands <= CODEC_MAX_SUBBANDS &&
           subband >= 0 && subband < encoder->num_subbands;
}

struct CodingSelection {
    int codebook = 0;
    int peaks = 0;
    int flags = 0;
};

inline bool SelectCoding(ENCODER* encoder, int subband, CodingSelection* selected) noexcept {
    if (!selected || !ValidSubband(encoder, subband)) return false;
    CodingSelection result;
    // Frozen SetCodingFlags checks only the upper bound of codebookflags[].
    result.flags = SetCodingFlags(encoder, subband, &result.codebook, &result.peaks);
    if (result.codebook < 0 || result.codebook >= CODEC_NUM_CODESETS) return false;
    const auto* runs = encoder->codebook_runbook[result.codebook];
    const auto* values = encoder->valuebook[result.codebook];
    if (!runs || !values || runs->length != kRunCodebookLength ||
        values->length != VALUE_TABLE_LENGTH || values->size != VALUE_TABLE_SIZE ||
        encoder->band_end_size[result.codebook] <= 0 || encoder->band_end_size[result.codebook] >= 32) return false;
    *selected = result;
    return true;
}

inline IMAGE* ResolveExternalBand(ENCODER* encoder, TRANSFORM* const* transforms,
                                  int channel_count, const ExternalCoeff& coeff) noexcept {
    if (!encoder || encoder->gop_length != 1 || !ValidChannelCount(channel_count) ||
        coeff.channel < 0 || coeff.channel >= channel_count ||
        !ValidSubband(encoder, coeff.subband) || !coeff.data || coeff.width <= 0 || coeff.height <= 0 ||
        coeff.band < 0 || coeff.band >= IMAGE_NUM_BANDS || coeff.level <= 0 ||
        coeff.level > TRANSFORM_MAX_LEVELS || (coeff.elem_bytes != 1 && coeff.elem_bytes != 2) ||
        (coeff.elem_bytes == 1 && (coeff.lowpass || coeff.value_min < -128 || coeff.value_max > 127 ||
                                 coeff.value_min > coeff.value_max)) ||
        !ValidTransforms(transforms, channel_count)) return nullptr;

    TRANSFORM* transform = transforms[coeff.channel];
    IMAGE* selected = nullptr;
    int expected_subband = 1;
    for (int k = transform->num_wavelets - 1; k >= 0; --k) {
        IMAGE* image = transform->wavelet[k];
        if (image->level == coeff.level) {
            if (coeff.band >= image->num_bands || image->width != coeff.width || image->height != coeff.height)
                return nullptr;
            if (coeff.lowpass) {
                if (k != transform->num_wavelets - 1 || coeff.band != LL_BAND || coeff.subband != 0)
                    return nullptr;
            } else if (coeff.band == LL_BAND || coeff.subband != expected_subband + coeff.band - 1) {
                return nullptr;
            }
            selected = image;
        }
        expected_subband += image->num_bands - 1;
    }
    return expected_subband == encoder->num_subbands ? selected : nullptr;
}

}}  // namespace hypercine::entropy_detail
