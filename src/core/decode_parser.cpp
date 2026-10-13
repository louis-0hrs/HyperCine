// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_entropy_cuda.h"
#include "decoder_backend.h"
#include <cstring>
#include <sstream>

namespace HyperCine {

namespace {

inline uint16_t ReadBE16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

inline uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// CineForm tag definitions
constexpr uint16_t TAG_ZERO = 0;
constexpr uint16_t TAG_SAMPLE = 1;
constexpr uint16_t TAG_INDEX = 2;
constexpr uint16_t TAG_MARKER = 4;
constexpr uint16_t TAG_TRANSFORM_TYPE = 10;
constexpr uint16_t TAG_NUM_FRAMES = 11;
constexpr uint16_t TAG_NUM_CHANNELS = 12;
constexpr uint16_t TAG_NUM_WAVELETS = 13;
constexpr uint16_t TAG_NUM_SUBBANDS = 14;
constexpr uint16_t TAG_NUM_SPATIAL = 15;
constexpr uint16_t TAG_FIRST_WAVELET = 16;
constexpr uint16_t TAG_GROUP_TRAILER = 18;
constexpr uint16_t TAG_FRAME_TYPE = 19;
constexpr uint16_t TAG_FRAME_WIDTH = 20;
constexpr uint16_t TAG_FRAME_HEIGHT = 21;
constexpr uint16_t TAG_FRAME_FORMAT = 22;
constexpr uint16_t TAG_FRAME_TRAILER = 24;
constexpr uint16_t TAG_LOWPASS_SUBBAND = 25;
constexpr uint16_t TAG_NUM_LEVELS = 26;
constexpr uint16_t TAG_LOWPASS_WIDTH = 27;
constexpr uint16_t TAG_LOWPASS_HEIGHT = 28;
constexpr uint16_t TAG_PIXEL_OFFSET = 33;
constexpr uint16_t TAG_QUANTIZATION = 34;
constexpr uint16_t TAG_PIXEL_DEPTH = 35;
constexpr uint16_t TAG_LOWPASS_TRAILER = 36;
constexpr uint16_t TAG_WAVELET_TYPE = 37;
constexpr uint16_t TAG_WAVELET_NUMBER = 38;
constexpr uint16_t TAG_WAVELET_LEVEL = 39;
constexpr uint16_t TAG_NUM_BANDS = 40;
constexpr uint16_t TAG_HIGHPASS_WIDTH = 41;
constexpr uint16_t TAG_HIGHPASS_HEIGHT = 42;
constexpr uint16_t TAG_BAND_NUMBER = 48;
constexpr uint16_t TAG_BAND_WIDTH = 49;
constexpr uint16_t TAG_BAND_HEIGHT = 50;
constexpr uint16_t TAG_BAND_SUBBAND = 51;
constexpr uint16_t TAG_BAND_ENCODING = 52;
constexpr uint16_t TAG_BAND_QUANTIZATION = 53;
constexpr uint16_t TAG_BAND_SCALE = 54;
constexpr uint16_t TAG_BAND_HEADER = 55;
constexpr uint16_t TAG_BAND_TRAILER = 56;
constexpr uint16_t TAG_CHANNEL = 62;
constexpr uint16_t TAG_SAMPLE_FLAGS = 68;
constexpr uint16_t TAG_PRECISION = 70;
constexpr uint16_t TAG_ENCODED_FORMAT = 84;
constexpr uint16_t TAG_BAND_CODING_FLAGS = 72;
constexpr uint16_t TAG_PEAK_LEVEL = 74;
constexpr uint16_t TAG_PEAK_TABLE_OFFSET_L = 75;
constexpr uint16_t TAG_PEAK_TABLE_OFFSET_H = 76;
constexpr uint16_t TAG_PEAK_TABLE = 0x4001;
constexpr uint16_t TAG_METADATA = 0x4002;
constexpr uint16_t TAG_SUBBAND_SIZE = 0x2000;
constexpr uint16_t TAG_LEVEL_SIZE = 0x2100;
constexpr uint16_t TAG_SAMPLE_SIZE = 0x2200;
constexpr uint16_t TAG_UNCOMPRESS = 0x2300;
constexpr uint16_t TAG_CHUNK = 0x4000;
constexpr uint16_t TAG_CUSTOM_CHUNK24BIT = 0x6000;
constexpr uint16_t TAG_METADATA_LARGE = 0x6000;

constexpr uint16_t MARKER_LOWPASS_START = 0x0F0F;
constexpr uint32_t HIGHPASS_TRAILER_WORD = 0x00003800;

} // anonymous namespace

static bool ParseSample(
    const uint8_t* compressed_data,
    size_t compressed_size,
    int pixel_format_channeloffset,
    std::vector<GpuEntropyBandDescriptor>& out_bands,
    std::string* out_error,
    bool strict,
    DecodeError* status = nullptr,
    FrameReconstruction* mode = nullptr
) {
    out_bands.clear();
    if (status) *status = DecodeError::InvalidSample;
    if (mode) *mode = FrameReconstruction::Progressive;

    if (!compressed_data || compressed_size < 32) {
        if (out_error) *out_error = "compressed sample is null or too small";
        return false;
    }

    size_t pos = 0;

    int frame_width = 0;
    int frame_height = 0;
    int frame_channels = 3;
    int precision = 10;
    int encoded_format = 1; // Upstream's default is YUV 4:2:2.
    bool is_progressive = !strict;
    bool has_frame_or_group_trailer = false;

    int current_channel = 0;
    int current_level = 2;
    int current_band = 0;
    int current_subband = 0;

    int lowpass_width = 0;
    int lowpass_height = 0;
    int highpass_width = 0;
    int highpass_height = 0;

    int band_width = 0;
    int band_height = 0;
    int band_quant = 1;
    int band_encoding = 3; // BAND_ENCODING_RUNLENGTHS = 3

    int active_codebook = 0;
    bool diff_coding = false;

    bool has_peak_table = false;
    uint32_t peak_offset = 0;
    size_t peak_base = 0;
    int peak_level = 0;

    while (pos + 4 <= compressed_size) {
        uint16_t raw_tag = ReadBE16(compressed_data + pos);
        uint16_t val = ReadBE16(compressed_data + pos + 2);
        pos += 4;

        int16_t stag = static_cast<int16_t>(raw_tag);
        uint16_t tag = (stag < 0) ? static_cast<uint16_t>(-stag) : static_cast<uint16_t>(stag);

        // Check for chunk tags
        if (tag == TAG_INDEX) {
            size_t n = static_cast<size_t>(val) * 4;
            if (pos + n > compressed_size) {
                if (out_error) *out_error = "TAG_INDEX size exceeds sample bounds";
                return false;
            }
            pos += n;
            continue;
        }

        // Subband size chunk tag
        if ((tag & 0xff00) == TAG_SUBBAND_SIZE) {
            size_t chunk_bytes = ((static_cast<size_t>(tag & 0xff) << 16) | val) * 4;
            if (chunk_bytes < 4 || chunk_bytes > compressed_size - pos) {
                if (out_error) *out_error = "TAG_SUBBAND_SIZE exceeds sample bounds";
                return false;
            }

            // Immediately following TAG_SUBBAND_SIZE is the tag for the subband header
            if (pos + 4 > compressed_size) {
                if (out_error) *out_error = "truncated subband header";
                return false;
            }

            uint16_t sub_raw_tag = ReadBE16(compressed_data + pos);
            uint16_t sub_val = ReadBE16(compressed_data + pos + 2);
            pos += 4;

            int16_t sub_stag = static_cast<int16_t>(sub_raw_tag);
            uint16_t sub_tag = (sub_stag < 0) ? static_cast<uint16_t>(-sub_stag) : static_cast<uint16_t>(sub_stag);

            if (sub_tag == TAG_MARKER && sub_val == MARKER_LOWPASS_START) {
                // Lowpass band (deepest LL, subband 0)
                GpuEntropyBandDescriptor desc{};
                desc.payload_offset = pos;
                desc.channel = current_channel;
                desc.level = 2; // Deepest LL is Level 2
                desc.band = 0;  // LL band
                desc.subband = 0;
                desc.width = lowpass_width;
                desc.height = lowpass_height;
                desc.pitch_bytes = (static_cast<size_t>(lowpass_width) * sizeof(int16_t) + 15) & ~size_t(15);
                desc.pitch_samples = desc.pitch_bytes / sizeof(int16_t);
                desc.coefficient_count = static_cast<size_t>(lowpass_height) * desc.pitch_samples;

                size_t expected_ll_bytes = static_cast<size_t>(lowpass_width) * lowpass_height * sizeof(int16_t);
                if (strict && expected_ll_bytes > chunk_bytes - 4) {
                    if (out_error) *out_error = "lowpass dimensions exceed declared chunk";
                    return false;
                }
                desc.payload_bytes = static_cast<uint32_t>(expected_ll_bytes);
                desc.safe_upper_bound = static_cast<uint32_t>(chunk_bytes - 4);
                desc.method = BandEncodingMethod::LowPass16S;
                desc.precision = precision;
                desc.prescale = 0;
                desc.channel_offset = pixel_format_channeloffset;
                desc.quantization = 1;

                if (pos + desc.payload_bytes > compressed_size) {
                    if (out_error) *out_error = "lowpass payload extends beyond sample buffer";
                    return false;
                }

                out_bands.push_back(desc);
                current_subband = 1;
                pos += (chunk_bytes - 4);
            }
            else if (sub_tag == TAG_BAND_HEADER) {
                if (strict && band_encoding != 3) {
                    if (out_error) *out_error = "unsupported highpass encoding";
                    return false;
                }
                // Highpass VLC band (subband 1..9)
                if (chunk_bytes < 8) {
                    if (out_error) *out_error = "subband chunk size too small for highpass band";
                    return false;
                }

                uint32_t vlc_payload_bytes = static_cast<uint32_t>(chunk_bytes - 8);
                size_t trailer_pos = pos + vlc_payload_bytes;

                if (trailer_pos + 4 > compressed_size) {
                    if (out_error) *out_error = "highpass payload trailer exceeds sample buffer";
                    return false;
                }

                uint32_t trailer_word = ReadLE32(compressed_data + trailer_pos);
                if (trailer_word != HIGHPASS_TRAILER_WORD) {
                    if (out_error) {
                        std::ostringstream ss;
                        ss << "highpass band " << current_subband << " trailer mismatch: 0x"
                           << std::hex << trailer_word << " expected 0x" << HIGHPASS_TRAILER_WORD;
                        *out_error = ss.str();
                    }
                    return false;
                }

                // Standard CineForm subband mapping for 3-level spatial wavelet:
                // Subbands 0..3: Level 2 (bands 0, 1, 2, 3)
                // Subbands 4..6: Level 1 (bands 1, 2, 3)
                // Subbands 7..9: Level 0 (bands 1, 2, 3)
                static const int kSubbandWaveletIndex[10] = {2, 2, 2, 2, 1, 1, 1, 0, 0, 0};
                static const int kSubbandBandIndex[10]    = {0, 1, 2, 3, 1, 2, 3, 1, 2, 3};

                int sb = current_subband;
                int band_lvl = (sb >= 0 && sb < 10) ? kSubbandWaveletIndex[sb] : current_level;
                int band_idx = (sb >= 0 && sb < 10) ? kSubbandBandIndex[sb] : current_band;

                GpuEntropyBandDescriptor desc{};
                desc.payload_offset = pos;
                desc.payload_bytes = vlc_payload_bytes;
                desc.safe_upper_bound = vlc_payload_bytes;
                desc.trailer_word = HIGHPASS_TRAILER_WORD;

                desc.channel = current_channel;
                desc.level = band_lvl;
                desc.band = band_idx;
                desc.subband = current_subband;
                desc.width = band_width;
                desc.height = band_height;
                desc.pitch_bytes = (static_cast<size_t>(band_width) * sizeof(int16_t) + 15) & ~size_t(15);
                desc.pitch_samples = desc.pitch_bytes / sizeof(int16_t);
                desc.coefficient_count = static_cast<size_t>(band_height) * desc.pitch_samples;

                desc.method = BandEncodingMethod::RunLengthVLC;
                desc.codebook_id = active_codebook;
                desc.initial_fsm_state = 0;
                desc.quantization = band_quant;

                if (active_codebook == 1) {
                    desc.companding = CompandingPolicy::Cubic;
                } else if (active_codebook == 2) {
                    desc.companding = CompandingPolicy::PiecewiseLinear;
                } else {
                    desc.companding = CompandingPolicy::None;
                }

                desc.precision = precision;
                desc.prescale = (band_lvl == 1) ? 2 : 0;
                desc.channel_offset = 0;
                desc.difference_coding = diff_coding;

                desc.has_peaks = has_peak_table;
                desc.peak_offset = 0;
                desc.peak_level = peak_level;
                if (has_peak_table && peak_level > 0) {
                    if (peak_base > compressed_size || peak_offset > compressed_size - peak_base) {
                        if (out_error) *out_error = "peak table offset outside sample";
                        return false;
                    }
                    const size_t peak_data = peak_base + peak_offset;
                    const auto peak_raw = peak_data >= 4 && peak_data <= compressed_size ? int16_t(ReadBE16(compressed_data + peak_data - 4)) : int16_t(0);
                    const auto peak_tag = uint16_t(peak_raw < 0 ? -int(peak_raw) : int(peak_raw));
                    if (peak_data < 4 || peak_data > compressed_size || peak_tag != TAG_PEAK_TABLE) {
                        if (out_error) *out_error = "invalid peak table header";
                        return false;
                    }
                    const size_t peak_bytes = size_t(ReadBE16(compressed_data + peak_data - 2)) * 4;
                    if (!peak_bytes || peak_bytes > compressed_size - peak_data) {
                        if (out_error) *out_error = "peak table size outside sample";
                        return false;
                    }
                    desc.peak_offset = peak_data;
                    desc.peak_count = int(peak_bytes / 2);
                }

                out_bands.push_back(desc);
                current_subband++;

                // Reset per-band transient state
                active_codebook = 0;
                diff_coding = false;
                has_peak_table = false;
                peak_level = 0;

                pos += (chunk_bytes - 4);
            }
            else {
                // Non-standard subband header - skip
                pos += (chunk_bytes - 4);
            }
            continue;
        }

        // Custom chunks with size
        if (tag >= TAG_CHUNK && tag < TAG_CUSTOM_CHUNK24BIT) {
            size_t n = static_cast<size_t>(val) * 4;
            if (pos + n > compressed_size) {
                if (out_error) *out_error = "custom chunk size exceeds sample bounds";
                return false;
            }
            pos += n;
            continue;
        }

        // Parse individual tags
        switch (tag) {
        case TAG_TRANSFORM_TYPE:
        case TAG_NUM_FRAMES:
            if (strict && ((tag == TAG_TRANSFORM_TYPE && val != 0) || (tag == TAG_NUM_FRAMES && val != 1))) {
                if (status) *status = DecodeError::UnsupportedEncodingMode;
                if (out_error) *out_error = "unsupported temporal/field transform";
                return false;
            }
            break;
        case TAG_NUM_CHANNELS:
            frame_channels = val;
            break;
        case TAG_FRAME_FORMAT:
            if (strict && val != 2) {
                if (out_error) *out_error = "unsupported frame format";
                return false;
            }
            break;
        case TAG_SAMPLE_FLAGS:
            // Bit 0 clear is GOP-1 interlaced. Acceptance is decided after the
            // YUV422 precision-10 contract, so FIELDPLUS still dies on tag 10/11.
            is_progressive = (val & 0x0001) != 0;
            break;

        case TAG_FRAME_WIDTH:
            frame_width = val;
            break;

        case TAG_FRAME_HEIGHT:
            frame_height = val;
            break;

        case TAG_PRECISION:
            precision = val;
            break;

        case TAG_ENCODED_FORMAT:
            encoded_format = val;
            break;

        case TAG_CHANNEL:
            current_channel = val;
            current_subband = 0;
            break;

        case TAG_LOWPASS_WIDTH:
            lowpass_width = val;
            break;

        case TAG_LOWPASS_HEIGHT:
            lowpass_height = val;
            break;

        case TAG_WAVELET_LEVEL:
            current_level = val;
            break;

        case TAG_BAND_NUMBER:
            current_band = val;
            break;

        case TAG_BAND_SUBBAND:
            current_subband = val;
            break;

        case TAG_HIGHPASS_WIDTH:
        case TAG_BAND_WIDTH:
            band_width = val;
            break;

        case TAG_HIGHPASS_HEIGHT:
        case TAG_BAND_HEIGHT:
            band_height = val;
            break;

        case TAG_BAND_QUANTIZATION:
            band_quant = val;
            break;

        case TAG_BAND_ENCODING:
            band_encoding = val;
            break;

        case TAG_BAND_CODING_FLAGS:
            if (strict && (val & ~0x001fu)) {
                if (out_error) *out_error = "invalid band coding flags";
                return false;
            }
            active_codebook = val & 0x0f;
            diff_coding = ((val >> 4) & 1) != 0;
            break;

        case TAG_PEAK_TABLE_OFFSET_L:
            peak_base = pos;
            peak_offset = (peak_offset & ~0xffffu) | static_cast<uint32_t>(val);
            break;

        case TAG_PEAK_TABLE_OFFSET_H:
            peak_offset = (peak_offset & 0xffffu) | (static_cast<uint32_t>(val) << 16);
            break;

        case TAG_PEAK_LEVEL:
            peak_level = val;
            has_peak_table = true;
            break;

        case TAG_FRAME_TRAILER:
        case TAG_GROUP_TRAILER:
            has_frame_or_group_trailer = true;
            // End of frame/group reached
            break;

        default:
            break;
        }
    }

    if (out_bands.size() != 30) {
        if (out_error) {
            std::ostringstream ss;
            ss << "expected 30 bands for YUV422 progressive frame, but parsed " << out_bands.size();
            *out_error = ss.str();
        }
        return false;
    }

    if (strict) {
        auto reject = [&] (const char* message) {
            if (out_error) *out_error = message;
            return false;
        };
        // A complete entropy payload is insufficient for an interrupted GOP-1
        // interlaced frame: upstream rejects a missing frame/group trailer. Preserve
        // the established progressive parser policy while closing this new path.
        if (!is_progressive && !has_frame_or_group_trailer)
            return reject("interrupted interlaced frame: missing frame/group trailer");
        if (!is_progressive && encoded_format != 1)
            return reject("interlaced encoded format is not YUV 4:2:2");
        if (pos != compressed_size || frame_channels != 3 || precision != 10 ||
            frame_width < 64 || frame_height < 64 || frame_width > 7680 || frame_height > 4320 ||
            (frame_width % 16) || (frame_height % 8))
            return reject("unsupported or inconsistent frame geometry/precision");
        static constexpr int levels[10] = {2,2,2,2,1,1,1,0,0,0};
        static constexpr int bands[10] = {0,1,2,3,1,2,3,1,2,3};
        for (size_t i = 0; i < out_bands.size(); ++i) {
            const auto& d = out_bands[i];
            const int ch = int(i / 10), sb = int(i % 10);
            const int divisor = 1 << (levels[sb] + 1);
            const int w = (ch == 0 ? frame_width : frame_width / 2) / divisor;
            if (d.channel != ch || d.subband != sb || d.level != levels[sb] || d.band != bands[sb] ||
                d.width != w || d.height != frame_height / divisor ||
                d.precision != 10 || d.quantization <= 0 || d.codebook_id < 0 || d.codebook_id > 2 ||
                !d.payload_bytes || d.payload_offset > compressed_size ||
                d.payload_bytes > compressed_size - d.payload_offset)
                return reject("invalid or unsupported band descriptor");
        }
        // The contract above is 30-band 10-bit YUV 4:2:2. A missing or clear
        // progressive flag on that contract is GOP-1 interlaced. FIELDPLUS
        // already returned UnsupportedEncodingMode. RGB/RGBA/Bayer fail the
        // band or precision checks and stay rejected.
        if (mode) *mode = is_progressive ? FrameReconstruction::Progressive
                                         : FrameReconstruction::InterlacedGop1;
    }
    if (status) *status = DecodeError::None;
    return true;
}

bool ParseCineFormSample(const uint8_t* p, size_t n, int offset,
    std::vector<GpuEntropyBandDescriptor>& bands, std::string* error) {
    return ParseSample(p, n, offset, bands, error, false);
}
bool ParseCineFormSampleValidated(const uint8_t* p, size_t n, int offset,
    std::vector<GpuEntropyBandDescriptor>& bands, std::string* error, DecodeError* status,
    FrameReconstruction* mode) {
    return ParseSample(p, n, offset, bands, error, true, status, mode);
}

} // namespace HyperCine
