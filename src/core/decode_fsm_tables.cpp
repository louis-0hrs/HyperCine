// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_entropy_cuda.h"
#include "CFHDDecoder.h"
#include "decoder.h"
#include "ISampleDecoder.h"
#include "SampleDecoder.h"
#include <vector>
#include <stdexcept>
#include <iostream>

namespace HyperCine {

namespace {

// Research member accessor to obtain internal DECODER* from CSampleDecoder without modifying upstream
struct DecoderMemberTag {
    using type = DECODER* CSampleDecoder::*;
    friend type DecoderMember(DecoderMemberTag);
};
template<class Tag, typename Tag::type Member> struct ResearchMemberAccess {
    friend typename Tag::type DecoderMember(Tag) { return Member; }
};
template struct ResearchMemberAccess<DecoderMemberTag, &CSampleDecoder::m_decoder>;

inline DECODER* GetNativeDecoder(void* handle) {
    if (!handle) return nullptr;
    return static_cast<CSampleDecoder*>(handle)->*DecoderMember(DecoderMemberTag{});
}

} // anonymous namespace

bool ExtractCineFormBaseFsmTables(std::vector<GpuFsmEntry>& out_tables) {
    // Total entries: cs9 (136 * 16 = 2176) + cs17 (518 * 16 = 8288) + cs18 (518 * 16 = 8288) = 18752
    constexpr size_t TOTAL_ENTRIES = 2176 + 8288 + 8288;
    out_tables.clear();
    out_tables.resize(TOTAL_ENTRIES);

    void* raw_decoder = nullptr;
    CFHD_Error err = CFHD_OpenDecoder(&raw_decoder, nullptr);
    if (err != CFHD_ERROR_OKAY || !raw_decoder) {
        return false;
    }

    int actual_w = 0, actual_h = 0;
    CFHD_PixelFormat actual_fmt = CFHD_PIXEL_FORMAT_BGRA;
    err = CFHD_PrepareToDecode(
        raw_decoder,
        1920, 1080,
        CFHD_PIXEL_FORMAT_BGRA,
        CFHD_DECODED_RESOLUTION_FULL,
        CFHD_DECODING_FLAGS_NONE,
        nullptr, 0,
        &actual_w, &actual_h, &actual_fmt
    );

    if (err != CFHD_ERROR_OKAY) {
        CFHD_CloseDecoder(raw_decoder);
        return false;
    }

    DECODER* d = GetNativeDecoder(raw_decoder);
    if (!d) {
        CFHD_CloseDecoder(raw_decoder);
        return false;
    }

    size_t offsets[3] = {0, 2176, 2176 + 8288};
    int expected_states[3] = {136, 518, 518};

    for (int c = 0; c < 3; ++c) {
        int num_states = d->fsm[c].table.num_states;
        if (num_states <= 0 || num_states > expected_states[c]) {
            CFHD_CloseDecoder(raw_decoder);
            return false;
        }

        size_t base_offset = offsets[c];
        for (int s = 0; s < num_states; ++s) {
            for (int j = 0; j < 16; ++j) {
                const auto& entry = d->fsm[c].table.entries[s][j];
                int16_t v0 = entry.value0;
                int16_t v1 = entry.value1;

                if (d->fsm[c].InitizedRestore == 1) {
                    int count = 2 * (s * 16 + j);
                    v0 = d->fsm[c].restoreFSM[count];
                    v1 = d->fsm[c].restoreFSM[count + 1];
                }

                size_t out_idx = base_offset + s * 16 + j;
                out_tables[out_idx].value0 = v0;
                out_tables[out_idx].value1 = v1;
                out_tables[out_idx].pre_post_skip = entry.pre_post_skip;
                out_tables[out_idx].next_state = entry.next_state;
            }
        }
    }

    CFHD_CloseDecoder(raw_decoder);
    return true;
}

} // namespace HyperCine
