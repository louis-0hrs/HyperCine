// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "decode_fieldplus.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace HyperCine {
namespace {

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
constexpr uint16_t TAG_FRAME_WIDTH = 20;
constexpr uint16_t TAG_FRAME_HEIGHT = 21;
constexpr uint16_t TAG_FRAME_FORMAT = 22;
constexpr uint16_t TAG_FRAME_TRAILER = 24;
constexpr uint16_t TAG_LOWPASS_WIDTH = 27;
constexpr uint16_t TAG_LOWPASS_HEIGHT = 28;
constexpr uint16_t TAG_WAVELET_TYPE = 37;
constexpr uint16_t TAG_WAVELET_NUMBER = 38;
constexpr uint16_t TAG_WAVELET_LEVEL = 39;
constexpr uint16_t TAG_HIGHPASS_WIDTH = 41;
constexpr uint16_t TAG_HIGHPASS_HEIGHT = 42;
constexpr uint16_t TAG_BAND_NUMBER = 48;
constexpr uint16_t TAG_BAND_WIDTH = 49;
constexpr uint16_t TAG_BAND_HEIGHT = 50;
constexpr uint16_t TAG_BAND_SUBBAND = 51;
constexpr uint16_t TAG_BAND_ENCODING = 52;
constexpr uint16_t TAG_BAND_QUANTIZATION = 53;
constexpr uint16_t TAG_BAND_HEADER = 55;
constexpr uint16_t TAG_CHANNEL = 62;
constexpr uint16_t TAG_SAMPLE_FLAGS = 68;
constexpr uint16_t TAG_PRECISION = 70;
constexpr uint16_t TAG_BAND_CODING_FLAGS = 72;
constexpr uint16_t TAG_PEAK_LEVEL = 74;
constexpr uint16_t TAG_PEAK_TABLE_OFFSET_L = 75;
constexpr uint16_t TAG_PEAK_TABLE_OFFSET_H = 76;
constexpr uint16_t TAG_ENCODED_FORMAT = 84;
constexpr uint16_t TAG_SUBBAND_SIZE = 0x2000;
constexpr uint16_t TAG_CHUNK = 0x4000;
constexpr uint16_t TAG_CUSTOM_CHUNK24BIT = 0x6000;
constexpr uint16_t TAG_PEAK_TABLE = 0x4001;
constexpr uint16_t MARKER_LOWPASS_START = 0x0F0F;
constexpr uint32_t HIGHPASS_TRAILER_WORD = 0x00003800;

uint16_t ReadBE16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t AbsoluteTag(uint16_t raw) {
    const int signed_value = raw < 0x8000u ? int(raw) : int(raw) - 0x10000;
    return static_cast<uint16_t>(signed_value < 0 ? -signed_value : signed_value);
}

struct BandMap {
    int wavelet;
    int band;
    FieldPlusWaveletRole role;
    FieldPlusTemporalRole temporal;
    FieldPlusFrameAssociation frames;
};

constexpr BandMap kBandMap[kFieldPlusLogicalBandsPerChannel] = {
    {5, 0, FieldPlusWaveletRole::TemporalLowSpatial2, FieldPlusTemporalRole::GopLow, FieldPlusFrameAssociation::BothFrames},
    {5, 1, FieldPlusWaveletRole::TemporalLowSpatial2, FieldPlusTemporalRole::GopLow, FieldPlusFrameAssociation::BothFrames},
    {5, 2, FieldPlusWaveletRole::TemporalLowSpatial2, FieldPlusTemporalRole::GopLow, FieldPlusFrameAssociation::BothFrames},
    {5, 3, FieldPlusWaveletRole::TemporalLowSpatial2, FieldPlusTemporalRole::GopLow, FieldPlusFrameAssociation::BothFrames},
    {4, 1, FieldPlusWaveletRole::TemporalLowSpatial1, FieldPlusTemporalRole::GopLow, FieldPlusFrameAssociation::BothFrames},
    {4, 2, FieldPlusWaveletRole::TemporalLowSpatial1, FieldPlusTemporalRole::GopLow, FieldPlusFrameAssociation::BothFrames},
    {4, 3, FieldPlusWaveletRole::TemporalLowSpatial1, FieldPlusTemporalRole::GopLow, FieldPlusFrameAssociation::BothFrames},
    {3, 0, FieldPlusWaveletRole::TemporalHighSpatial, FieldPlusTemporalRole::GopHigh, FieldPlusFrameAssociation::BothFrames},
    {3, 1, FieldPlusWaveletRole::TemporalHighSpatial, FieldPlusTemporalRole::GopHigh, FieldPlusFrameAssociation::BothFrames},
    {3, 2, FieldPlusWaveletRole::TemporalHighSpatial, FieldPlusTemporalRole::GopHigh, FieldPlusFrameAssociation::BothFrames},
    {3, 3, FieldPlusWaveletRole::TemporalHighSpatial, FieldPlusTemporalRole::GopHigh, FieldPlusFrameAssociation::BothFrames},
    {1, 1, FieldPlusWaveletRole::Frame1Base, FieldPlusTemporalRole::Frame1Detail, FieldPlusFrameAssociation::Frame1},
    {1, 2, FieldPlusWaveletRole::Frame1Base, FieldPlusTemporalRole::Frame1Detail, FieldPlusFrameAssociation::Frame1},
    {1, 3, FieldPlusWaveletRole::Frame1Base, FieldPlusTemporalRole::Frame1Detail, FieldPlusFrameAssociation::Frame1},
    {0, 1, FieldPlusWaveletRole::Frame0Base, FieldPlusTemporalRole::Frame0Detail, FieldPlusFrameAssociation::Frame0},
    {0, 2, FieldPlusWaveletRole::Frame0Base, FieldPlusTemporalRole::Frame0Detail, FieldPlusFrameAssociation::Frame0},
    {0, 3, FieldPlusWaveletRole::Frame0Base, FieldPlusTemporalRole::Frame0Detail, FieldPlusFrameAssociation::Frame0}
};

int BandLevel(int logical_subband) {
    return logical_subband < 4 ? 2 : logical_subband < 7 ? 1 : 0;
}

bool Fail(const char* message, std::string* error, DecodeError* status,
          DecodeError code = DecodeError::InvalidSample) {
    if (error) *error = message;
    if (status) *status = code;
    return false;
}

bool ParsePeak(const uint8_t* data, size_t size, size_t peak_base,
               uint32_t peak_relative_offset, int peak_level,
               size_t& offset, int& count) {
    if (peak_base > size || peak_relative_offset > size - peak_base) return false;
    const size_t peak_data = peak_base + peak_relative_offset;
    if (peak_data < 4 || peak_data > size) return false;
    const int16_t raw = static_cast<int16_t>(ReadBE16(data + peak_data - 4));
    const uint16_t tag = static_cast<uint16_t>(raw < 0 ? -int(raw) : int(raw));
    if (tag != TAG_PEAK_TABLE) return false;
    const size_t bytes = size_t(ReadBE16(data + peak_data - 2)) * 4;
    if (!bytes || bytes > size - peak_data || peak_level <= 0 || bytes / 2 > size_t((std::numeric_limits<int>::max)())) return false;
    offset = peak_data;
    count = int(bytes / 2);
    return true;
}

} // namespace

bool ParseProgressiveFieldPlusSampleValidated(const uint8_t* sample, size_t size,
    int pixel_format_channeloffset, FieldPlusSampleDescriptor& out,
    std::string* error, DecodeError* status) {
    out = {};
    if (status) *status = DecodeError::InvalidSample;
    if (!sample || size < 32) return Fail("compressed sample is null or too small", error, status);

    int frame_width = 0, frame_height = 0, precision = 0, encoded_format = 1;
    int channels = 0, frames = 0, transform_type = -1, wavelets = 0, subbands = 0, spatial = 0;
    int first_wavelet = -1, wavelet_type = -1, wavelet_number = -1, wavelet_level = -1;
    int current_channel = 0, channel_chunks = 0, physical_chunks = 0, bands = 0;
    int lowpass_width = 0, lowpass_height = 0, band_width = 0, band_height = 0;
    int band_quantization = 1, band_encoding = 3, active_codebook = 0, peak_level = 0;
    int band_number = -1, band_subband = -1;
    uint32_t peak_relative_offset = 0;
    size_t peak_base = 0;
    bool has_peak_table = false, difference_coding = false, progressive_flag_seen = false;
    bool progressive = false, trailer_seen = false;
    std::array<int, kFieldPlusChannelCount> channel_counts{};
    std::array<int, kFieldPlusChannelCount> control_counts{};
    size_t pos = 0;

    while (pos + 4 <= size) {
        const size_t token_offset = pos;
        const uint16_t tag = AbsoluteTag(ReadBE16(sample + pos));
        const uint16_t value = ReadBE16(sample + pos + 2);
        pos += 4;

        if (tag == TAG_INDEX) {
            const size_t bytes = size_t(value) * 4;
            if (bytes > size - pos) return Fail("TAG_INDEX size exceeds sample bounds", error, status);
            pos += bytes;
            continue;
        }

        if ((tag & 0xff00u) == TAG_SUBBAND_SIZE) {
            const size_t chunk_bytes = ((size_t(tag & 0xffu) << 16) | value) * 4;
            if (chunk_bytes < 4 || chunk_bytes > size - pos || pos + 4 > size)
                return Fail("TAG_SUBBAND_SIZE exceeds sample bounds", error, status);
            const uint16_t inner_tag = AbsoluteTag(ReadBE16(sample + pos));
            const uint16_t inner_value = ReadBE16(sample + pos + 2);
            pos += 4;
            const bool control = inner_tag == TAG_BAND_HEADER &&
                (physical_chunks % kFieldPlusChunksPerChannel) == 11 && channel_chunks == 11;
            const int logical_subband = control ? 255 : channel_chunks < 11 ? channel_chunks : channel_chunks - 1;
            const int expected_subband = control ? 255 : logical_subband;
            if (physical_chunks >= kFieldPlusPhysicalChunkCount || current_channel < 0 || current_channel >= kFieldPlusChannelCount)
                return Fail("too many physical FIELDPLUS chunks", error, status);
            if (channel_chunks >= kFieldPlusChunksPerChannel || expected_subband != logical_subband)
                return Fail("invalid physical FIELDPLUS chunk order", error, status);
            if (channel_chunks == 11 && !control)
                return Fail("missing FIELDPLUS control chunk at physical index 11", error, status);
            if (channel_chunks != 11 && logical_subband > 16)
                return Fail("invalid FIELDPLUS logical subband index", error, status);

            auto& chunk = out.chunks[size_t(physical_chunks)];
            chunk.kind = control ? FieldPlusChunkKind::TemporalHighControl : FieldPlusChunkKind::Coefficients;
            chunk.physical_index = physical_chunks;
            chunk.channel = current_channel;
            chunk.channel_chunk_index = channel_chunks;
            chunk.logical_subband = logical_subband;
            chunk.chunk_offset = token_offset;
            chunk.chunk_bytes = chunk_bytes;
            chunk.codebook = active_codebook;
            chunk.difference_coding = difference_coding;
            chunk.has_peaks = has_peak_table;
            chunk.peak_level = peak_level;
            chunk.width = band_width;
            chunk.height = band_height;

            if (control) {
                if (chunk_bytes != 8 || band_subband != 255 || wavelet_number != 3 || band_number != 1 ||
                    wavelet_type != 4 || wavelet_level < 0 ||
                    current_channel != physical_chunks / kFieldPlusChunksPerChannel)
                    return Fail("invalid FIELDPLUS temporal-high control chunk", error, status);
                if (pos + 4 > size || ReadLE32(sample + pos) != HIGHPASS_TRAILER_WORD)
                    return Fail("invalid FIELDPLUS temporal-high control trailer", error, status);
                chunk.payload_offset = pos;
                chunk.payload_bytes = 0;
                chunk.wavelet_owner = 2;
                chunk.band_owner = 1;
                chunk.wavelet_role = FieldPlusWaveletRole::GopTemporal;
                chunk.temporal_role = FieldPlusTemporalRole::GopHigh;
                chunk.frame_association = FieldPlusFrameAssociation::BothFrames;
                out.control_chunk_indices[size_t(current_channel)] = physical_chunks;
                ++control_counts[size_t(current_channel)];
                pos += chunk_bytes - 4;
            } else {
                if (channel_chunks == 11 || bands >= kFieldPlusBandCount || logical_subband < 0 || logical_subband >= kFieldPlusLogicalBandsPerChannel)
                    return Fail("invalid FIELDPLUS coefficient chunk index", error, status);
                if (logical_subband > 0 && band_subband != logical_subband) {
                    std::ostringstream message;
                    message << "FIELDPLUS logical subband tag/order mismatch at physical chunk " << physical_chunks
                            << " at byte " << token_offset << " (inner tag " << inner_tag << ")"
                            << ": tag=" << band_subband << ", expected=" << logical_subband;
                    if (error) *error = message.str();
                    if (status) *status = DecodeError::InvalidSample;
                    return false;
                }
                const auto& map = kBandMap[logical_subband];
                if (logical_subband > 0 &&
                    (wavelet_number != map.wavelet + 1 || band_number != map.band ||
                     wavelet_type != (map.wavelet <= 1 ? 5 : 3)))
                    return Fail("FIELDPLUS wavelet owner metadata does not match the frozen subband map", error, status);
                chunk.wavelet_owner = map.wavelet;
                chunk.band_owner = map.band;
                chunk.wavelet_role = map.role;
                chunk.temporal_role = map.temporal;
                chunk.frame_association = map.frames;

                auto& band = out.bands[size_t(bands)];
                band.logical_subband = logical_subband;
                band.physical_chunk = physical_chunks;
                band.wavelet_owner = map.wavelet;
                band.band_owner = map.band;
                band.wavelet_role = map.role;
                band.temporal_role = map.temporal;
                band.frame_association = map.frames;
                auto& d = band.entropy;
                d.channel = current_channel;
                d.level = BandLevel(logical_subband);
                d.band = map.band;
                d.subband = logical_subband;
                d.precision = precision;
                d.quantization = band_quantization;
                d.codebook_id = active_codebook;
                d.difference_coding = difference_coding;
                d.has_peaks = has_peak_table;
                d.peak_level = peak_level;
                d.prescale = map.wavelet == 5 ? 2 : 0;
                d.channel_offset = logical_subband == 0 ? pixel_format_channeloffset : 0;
                d.companding = active_codebook == 1 ? CompandingPolicy::Cubic :
                    active_codebook == 2 ? CompandingPolicy::PiecewiseLinear : CompandingPolicy::None;

                if (inner_tag == TAG_MARKER && inner_value == MARKER_LOWPASS_START) {
                    if (logical_subband != 0 || lowpass_width <= 0 || lowpass_height <= 0)
                        return Fail("invalid FIELDPLUS lowpass chunk", error, status);
                    const size_t bytes = size_t(lowpass_width) * size_t(lowpass_height) * sizeof(int16_t);
                    if (bytes > chunk_bytes - 4 || bytes > size - pos)
                        return Fail("FIELDPLUS lowpass dimensions exceed chunk bounds", error, status);
                    d.method = BandEncodingMethod::LowPass16S;
                    d.width = lowpass_width;
                    d.height = lowpass_height;
                    d.payload_offset = pos;
                    d.payload_bytes = static_cast<uint32_t>(bytes);
                    d.safe_upper_bound = static_cast<uint32_t>(chunk_bytes - 4);
                    chunk.payload_offset = pos;
                    chunk.payload_bytes = bytes;
                    chunk.width = d.width;
                    chunk.height = d.height;
                    pos += chunk_bytes - 4;
                } else if (inner_tag == TAG_BAND_HEADER) {
                    if (band_width <= 0 || band_height <= 0 || band_quantization <= 0 ||
                        active_codebook < 0 || active_codebook > 2) {
                        return Fail("invalid FIELDPLUS highpass metadata", error, status);
                    }
                    if (band_encoding == 4) {
                        // The progressive FIELDPLUS temporal-high LL band is stored as
                        // big-endian signed 16-bit coefficients. The chunk also carries
                        // a four-byte alignment word and the normal BAND_TRAILER tag.
                        const size_t raw_bytes = size_t(band_width) * size_t(band_height) * sizeof(int16_t);
                        if (logical_subband != 7 || map.wavelet != 3 || map.band != 0 ||
                            band_quantization != 1 || chunk_bytes != raw_bytes + 12 ||
                            raw_bytes > (std::numeric_limits<uint32_t>::max)() ||
                            raw_bytes > size - pos) {
                            return Fail("invalid raw16 FIELDPLUS temporal-high band", error, status);
                        }
                        const size_t trailer_pos = pos + chunk_bytes - 8;
                        if (trailer_pos + 4 > size || ReadLE32(sample + trailer_pos) != HIGHPASS_TRAILER_WORD)
                            return Fail("FIELDPLUS raw16 highpass trailer mismatch", error, status);
                        d.method = BandEncodingMethod::Raw16;
                        d.width = band_width;
                        d.height = band_height;
                        d.payload_offset = pos;
                        d.payload_bytes = static_cast<uint32_t>(raw_bytes);
                        d.safe_upper_bound = static_cast<uint32_t>(raw_bytes);
                        chunk.payload_offset = pos;
                        chunk.payload_bytes = raw_bytes;
                        chunk.width = band_width;
                        chunk.height = band_height;
                        pos += chunk_bytes - 4;
                    } else if (band_encoding == 3 && chunk_bytes >= 8) {
                        if (logical_subband == 7)
                            return Fail("FIELDPLUS temporal-high LL must use raw16 encoding", error, status);
                        const size_t payload_bytes = chunk_bytes - 8;
                        if (payload_bytes > (std::numeric_limits<uint32_t>::max)() || pos + payload_bytes + 4 > size)
                            return Fail("FIELDPLUS entropy payload exceeds sample bounds", error, status);
                        const size_t trailer_pos = pos + payload_bytes;
                        if (ReadLE32(sample + trailer_pos) != HIGHPASS_TRAILER_WORD)
                            return Fail("FIELDPLUS highpass trailer mismatch", error, status);
                        d.method = BandEncodingMethod::RunLengthVLC;
                        d.width = band_width;
                        d.height = band_height;
                        d.payload_offset = pos;
                        d.payload_bytes = static_cast<uint32_t>(payload_bytes);
                        d.safe_upper_bound = static_cast<uint32_t>(payload_bytes);
                        chunk.payload_offset = pos;
                        chunk.payload_bytes = payload_bytes;
                        chunk.width = band_width;
                        chunk.height = band_height;
                        if (has_peak_table && peak_level > 0) {
                            size_t peak_offset = 0;
                            int peak_count = 0;
                            if (!ParsePeak(sample, size, peak_base, peak_relative_offset, peak_level, peak_offset, peak_count))
                                return Fail("invalid FIELDPLUS Peak table", error, status);
                            d.peak_offset = peak_offset;
                            d.peak_count = peak_count;
                            chunk.peak_offset = peak_offset;
                            chunk.peak_count = peak_count;
                        }
                        pos += chunk_bytes - 4;
                    } else {
                        std::ostringstream message;
                        message << "unsupported/malformed FIELDPLUS highpass at physical chunk " << physical_chunks
                                << ": encoding=" << band_encoding << ", bytes=" << chunk_bytes << ", width=" << band_width;
                        if (error) *error = message.str();
                        return Fail("unsupported or malformed FIELDPLUS highpass chunk", nullptr, status);
                    }
                } else {
                    return Fail("unexpected FIELDPLUS subband header", error, status);
                }

                const size_t row_bytes = size_t(d.width) * sizeof(int16_t);
                d.pitch_bytes = (row_bytes + 15u) & ~size_t(15u);
                d.pitch_samples = d.pitch_bytes / sizeof(int16_t);
                d.coefficient_count = size_t(d.height) * d.pitch_samples;
                ++bands;
            }

            ++channel_counts[size_t(current_channel)];
            ++channel_chunks;
            ++physical_chunks;
            active_codebook = 0;
            difference_coding = false;
            has_peak_table = false;
            peak_level = 0;
            peak_relative_offset = 0;
            peak_base = 0;
            if (control) channel_chunks = 12;
            continue;
        }

        if (tag >= TAG_CHUNK && tag < TAG_CUSTOM_CHUNK24BIT) {
            const size_t bytes = size_t(value) * 4;
            if (bytes > size - pos) return Fail("custom chunk size exceeds sample bounds", error, status);
            pos += bytes;
            continue;
        }

        switch (tag) {
        case TAG_TRANSFORM_TYPE: transform_type = value; break;
        case TAG_NUM_FRAMES: frames = value; break;
        case TAG_NUM_CHANNELS: channels = value; break;
        case TAG_NUM_WAVELETS: wavelets = value; break;
        case TAG_NUM_SUBBANDS: subbands = value; break;
        case TAG_NUM_SPATIAL: spatial = value; break;
        case TAG_FIRST_WAVELET: first_wavelet = value; break;
        case TAG_FRAME_WIDTH: frame_width = value; break;
        case TAG_FRAME_HEIGHT: frame_height = value; break;
        case TAG_FRAME_FORMAT: if (value != 2) return Fail("unsupported FIELDPLUS frame format", error, status); break;
        case TAG_PRECISION: precision = value; break;
        case TAG_ENCODED_FORMAT: encoded_format = value; break;
        case TAG_SAMPLE_FLAGS: progressive = (value & 1u) != 0; progressive_flag_seen = true; break;
        case TAG_CHANNEL:
            if (value >= kFieldPlusChannelCount || value != current_channel + 1 || channel_chunks != kFieldPlusChunksPerChannel)
                return Fail("invalid FIELDPLUS channel ordering", error, status);
            current_channel = value;
            channel_chunks = 0;
            break;
        case TAG_LOWPASS_WIDTH: lowpass_width = value; break;
        case TAG_LOWPASS_HEIGHT: lowpass_height = value; break;
        case TAG_WAVELET_TYPE: wavelet_type = value; break;
        case TAG_WAVELET_NUMBER: wavelet_number = value; break;
        case TAG_WAVELET_LEVEL: wavelet_level = value; break;
        case TAG_BAND_NUMBER: band_number = value; break;
        case TAG_HIGHPASS_WIDTH:
        case TAG_BAND_WIDTH: band_width = value; break;
        case TAG_HIGHPASS_HEIGHT:
        case TAG_BAND_HEIGHT: band_height = value; break;
        case TAG_BAND_SUBBAND: band_subband = value; break;
        case TAG_BAND_ENCODING: band_encoding = value; break;
        case TAG_BAND_QUANTIZATION: band_quantization = value; break;
        case TAG_BAND_CODING_FLAGS:
            if (value & ~0x001fu) return Fail("invalid FIELDPLUS band coding flags", error, status);
            active_codebook = value & 0x0f;
            difference_coding = ((value >> 4) & 1u) != 0;
            break;
        case TAG_PEAK_TABLE_OFFSET_L: peak_base = pos; peak_relative_offset = (peak_relative_offset & 0xffff0000u) | value; break;
        case TAG_PEAK_TABLE_OFFSET_H: peak_relative_offset = (peak_relative_offset & 0x0000ffffu) | (uint32_t(value) << 16); break;
        case TAG_PEAK_LEVEL: peak_level = value; has_peak_table = true; break;
        case TAG_FRAME_TRAILER:
        case TAG_GROUP_TRAILER: trailer_seen = true; break;
        default: break;
        }
    }

    if (pos != size) return Fail("trailing truncated FIELDPLUS tag", error, status);
    if (transform_type != 2 || frames != 2)
        return Fail("unsupported temporal/field transform", error, status, DecodeError::UnsupportedEncodingMode);
    if (!progressive_flag_seen || !progressive)
        return Fail("interlaced FIELDPLUS is not supported", error, status, DecodeError::UnsupportedEncodingMode);
    if (channels != kFieldPlusChannelCount || precision != 10 || encoded_format != 1 ||
        wavelets != 6 || subbands != kFieldPlusLogicalBandsPerChannel || spatial != 3 || first_wavelet != 3 ||
        frame_width < 64 || frame_height < 64 || frame_width > 7680 || frame_height > 4320 ||
        frame_width % 16 || frame_height % 8 || !trailer_seen) {
        std::ostringstream message;
        message << "unsupported or inconsistent progressive FIELDPLUS profile: transform=" << transform_type
                << " frames=" << frames << " channels=" << channels << " precision=" << precision
                << " format=" << encoded_format << " wavelets=" << wavelets << " subbands=" << subbands
                << " spatial=" << spatial << " first_wavelet=" << first_wavelet << " dimensions="
                << frame_width << 'x' << frame_height << " trailer=" << trailer_seen;
        if (error) *error = message.str();
        return Fail("unsupported or inconsistent progressive FIELDPLUS profile", nullptr, status,
            DecodeError::UnsupportedEncodingMode);
    }
    if (physical_chunks != kFieldPlusPhysicalChunkCount || bands != kFieldPlusBandCount)
        return Fail("FIELDPLUS chunk count is not 54 physical / 51 coefficient chunks", error, status);
    for (int ch = 0; ch < kFieldPlusChannelCount; ++ch) {
        if (channel_counts[size_t(ch)] != kFieldPlusChunksPerChannel || control_counts[size_t(ch)] != 1 ||
            out.control_chunk_indices[size_t(ch)] != ch * kFieldPlusChunksPerChannel + 11)
            return Fail("FIELDPLUS channel is missing its unique control chunk", error, status);
    }

    const int luma_channel_width = frame_width;
    for (int i = 0; i < kFieldPlusBandCount; ++i) {
        const auto& b = out.bands[size_t(i)];
        const int ch = i / kFieldPlusLogicalBandsPerChannel;
        const int sb = i % kFieldPlusLogicalBandsPerChannel;
        const int divisor = sb < 4 ? 8 : sb < 11 ? 4 : 2;
        const int channel_width = ch == 0 ? luma_channel_width : luma_channel_width / 2;
        const int expected_width = channel_width / divisor;
        const int expected_height = frame_height / divisor;
        const auto& d = b.entropy;
        if (b.logical_subband != sb || d.channel != ch || d.width != expected_width || d.height != expected_height ||
            d.precision != 10 || d.quantization <= 0 || d.codebook_id < 0 || d.codebook_id > 2 ||
            b.physical_chunk != ch * kFieldPlusChunksPerChannel + (sb < 11 ? sb : sb + 1))
            return Fail("invalid or unsupported FIELDPLUS band descriptor", error, status);
        out.chunks[size_t(b.physical_chunk)].width = d.width;
        out.chunks[size_t(b.physical_chunk)].height = d.height;
    }

    out.width = frame_width;
    out.height = frame_height;
    out.coded_precision = precision;
    out.channel_count = channels;
    out.frame_count = frames;
    out.transform_type = transform_type;
    out.wavelet_count = wavelets;
    out.logical_subbands_per_channel = subbands;
    out.progressive = progressive;
    if (status) *status = DecodeError::None;
    if (error) error->clear();
    return true;
}

} // namespace HyperCine
