// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CFHDEncoder.h"
#include "CFHDMetadata.h"
extern "C" {
#include "config.h"
#include "codec.h"
#include "encoder.h"
#include "wavelet.h"
#include "image.h"
#include "frame.h"
#include "bitstream.h"
void EncodeQuantizedBand(ENCODER*, BITSTREAM*, IMAGE*, int, int, int, int);
}
#include "VideoBuffers.h"
#include "SampleMetadata.h"
// The existing SDK reference tests inspect the same prepared object. No SDK
// source, struct layout, selector or encoder implementation is replaced here.
#define private public
#define protected public
#include "SampleEncoder.h"
#undef private
#undef protected

#include "hypercine/entropy_parallel.h"
#include "hypercine/entropy_stage.h"
#include "test_vectors.h"

namespace {
using HyperCineTest::AlignedVector;
using namespace hypercine;
int negatives = 0;
int positives = 0;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct EncoderHandle {
    CFHD_EncoderRef ref = nullptr;
    ~EncoderHandle() { if (ref) CFHD_CloseEncoder(ref); }
};

struct Prepared {
    static constexpr int width = 256, height = 256, pitch = width * 4;
    EncoderHandle handle;
    CSampleEncoder* sample = nullptr;
    ENCODER* encoder = nullptr;
    TRANSFORM** transforms = nullptr;
    FRAME* frame = nullptr;
    int channels = 0;
    AlignedVector<uint8_t> source_bgra;
    std::vector<IMAGE*> images;
    std::vector<IMAGE*> planes;
    std::vector<std::pair<const uint8_t*, std::size_t>> buffers;
    std::vector<BandRef> refs;
    Prepared() : source_bgra(pitch * height) {
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const auto i = static_cast<std::size_t>(y * pitch + x * 4);
            source_bgra[i] = static_cast<uint8_t>(x + y);
            source_bgra[i + 1] = static_cast<uint8_t>(3 * x + y);
            source_bgra[i + 2] = static_cast<uint8_t>(x ^ y);
            source_bgra[i + 3] = 255;
        }
        Require(CFHD_OpenEncoder(&handle.ref, nullptr) == CFHD_ERROR_OKAY, "SDK open failed");
        Require(CFHD_PrepareToEncode(handle.ref, width, height, CFHD_PIXEL_FORMAT_BGRA,
                    CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE,
                    CFHD_ENCODING_QUALITY_FILMSCAN3) == CFHD_ERROR_OKAY, "SDK prepare failed");
        Require(CFHD_EncodeSample(handle.ref, source_bgra.data(), pitch) == CFHD_ERROR_OKAY, "SDK encode failed");
        sample = reinterpret_cast<CSampleEncoder*>(handle.ref);
        encoder = sample->m_encoder;
        transforms = sample->m_transformArray;
        frame = encoder->frame;
        channels = sample->m_channelCount;
        Require(channels == 3, "unexpected reference channel count");
        Require(RefreshUpstreamPyramid(encoder, transforms, channels, source_bgra.data(), pitch,
                    sample->m_scratchBuffer, sample->m_scratchBufferSize), "SDK pyramid refresh failed");
        for (int c = 0; c < channels; ++c) {
            planes.push_back(frame->channel[c]);
            const auto* plane = frame->channel[c];
            buffers.emplace_back(reinterpret_cast<const uint8_t*>(plane->band[0]),
                                 static_cast<std::size_t>(plane->pitch) * plane->height);
            for (int k = 0; k < transforms[c]->num_wavelets; ++k) {
                IMAGE* image = transforms[c]->wavelet[k];
                images.push_back(image);
                for (int band = 0; band < image->num_bands; ++band)
                    buffers.emplace_back(reinterpret_cast<const uint8_t*>(image->band[band]),
                                         static_cast<std::size_t>(image->pitch) * image->height);
            }
        }
        buffers.emplace_back(source_bgra.data(), source_bgra.size());
        buffers.emplace_back(static_cast<const uint8_t*>(sample->m_scratchBuffer), sample->m_scratchBufferSize);
        refs = EnumerateBands(transforms, channels);
        Require(refs.size() == 30, "prepared reference needs 30 bands");
    }
    IMAGE* Image(const BandRef& ref) { return transforms[ref.channel]->wavelet[ref.wavelet_index]; }
};

template<class T> std::vector<unsigned char> Bytes(const T& object) {
    const auto* begin = reinterpret_cast<const unsigned char*>(&object);
    return {begin, begin + sizeof(object)};
}

// Keep the fixture's known object list independent of the deliberately damaged
// metadata. Rejected calls must leave both encoder and wavelet objects untouched.
struct Snapshot {
    std::vector<unsigned char> encoder, frame;
    std::vector<std::vector<unsigned char>> transforms, images, planes;
    explicit Snapshot(const Prepared& fixture) : encoder(Bytes(*fixture.encoder)), frame(Bytes(*fixture.frame)) {
        for (int c = 0; c < fixture.channels; ++c)
            if (fixture.transforms[c]) transforms.push_back(Bytes(*fixture.transforms[c]));
        for (const auto* image : fixture.images) images.push_back(Bytes(*image));
        for (const auto* plane : fixture.planes) planes.push_back(Bytes(*plane));
    }
    void Check(const Prepared& fixture, const std::string& label) const {
        const Snapshot after(fixture);
        Require(encoder == after.encoder && frame == after.frame && transforms == after.transforms &&
                images == after.images && planes == after.planes,
                label + ": rejected request mutated SDK state");
    }
};

struct RefreshSnapshot {
    Snapshot objects;
    std::vector<std::vector<uint8_t>> contents;
    explicit RefreshSnapshot(const Prepared& fixture) : objects(fixture) {
        // These views were captured from the valid SDK allocation once, not
        // reconstructed from the deliberately corrupted fields under test.
        for (const auto& buffer : fixture.buffers)
            contents.emplace_back(buffer.first, buffer.first + buffer.second);
    }
    void Check(const Prepared& fixture, const std::string& label) const {
        objects.Check(fixture, label);
        for (std::size_t i = 0; i < fixture.buffers.size(); ++i)
            Require(std::memcmp(fixture.buffers[i].first, contents[i].data(), contents[i].size()) == 0,
                    label + ": rejected refresh wrote frame/wavelet/input/scratch bytes");
    }
};

ExternalCoeff Coeff(const BandRef& ref, const void* data) {
    ExternalCoeff coeff;
    coeff.channel = ref.channel; coeff.level = ref.level; coeff.band = ref.band;
    coeff.subband = ref.subband; coeff.lowpass = ref.lowpass;
    coeff.width = ref.width; coeff.height = ref.height; coeff.data = data;
    return coeff;
}

void RejectBoth(Prepared& fixture, const ExternalCoeff& coeff, int channels, const std::string& label,
                TRANSFORM** transforms) {
    const Snapshot before(fixture);
    EncodedPiece piece; piece.ok = true; piece.payload = {0xaa};
    EntropySymbols symbols; symbols.ok = true; symbols.payload = {0xbb};
    Require(!EncodeOneExternalBand(fixture.encoder, transforms, channels, coeff, &piece), label + ": encoded invalid band");
    Require(!piece.ok && piece.payload.empty(), label + ": retained a successful fragment");
    Require(!MatchUpstreamHighpass(fixture.encoder, transforms, channels, coeff, &symbols), label + ": matched invalid band");
    Require(!symbols.ok && symbols.payload.empty(), label + ": retained a successful match");
    before.Check(fixture, label);
    ++negatives;
}

void RejectBoth(Prepared& fixture, const ExternalCoeff& coeff, const std::string& label) {
    RejectBoth(fixture, coeff, fixture.channels, label, fixture.transforms);
}

void RejectTable(Prepared& fixture, int subband, const std::string& label) {
    const Snapshot before(fixture);
    EntropyTables tables;
    tables.run_length = 3; tables.run_bits = {1, 2, 3};
    Require(!LoadEntropyTables(fixture.encoder, subband, &tables), label + ": loaded invalid table");
    Require(tables.run_length == 0 && tables.run_bits.empty(), label + ": retained stale table data");
    before.Check(fixture, label);
    ++negatives;
}

template<class T, class F> void WithValue(T& field, T replacement, F test) {
    const T saved = field;
    field = replacement;
    try { test(); } catch (...) { field = saved; throw; }
    field = saved;
}

std::vector<uint8_t> DirectSdk(Prepared& fixture, IMAGE* image, const ExternalCoeff& coeff) {
    IMAGE copy = *image;
    const int stride = image->pitch / static_cast<int>(sizeof(PIXEL));
    AlignedVector<PIXEL> padded(static_cast<std::size_t>(stride) * coeff.height, 0);
    const auto* input = static_cast<const int16_t*>(coeff.data);
    for (int y = 0; y < coeff.height; ++y)
        std::copy_n(input + static_cast<std::size_t>(y) * coeff.width, coeff.width,
                    padded.data() + static_cast<std::size_t>(y) * stride);
    copy.band[coeff.band] = padded.data();
    copy.pixel_type[coeff.band] = PIXEL_TYPE_16S;
    std::vector<uint8_t> output(static_cast<std::size_t>(coeff.width) * coeff.height * 8u + 65536u);
    BITSTREAM stream{};
    InitBitstreamBuffer(&stream, output.data(), output.size(), BITSTREAM_ACCESS_WRITE);
    if (coeff.lowpass) {
        EncodeLowPassBand(fixture.encoder, &stream, &copy, coeff.channel, coeff.subband);
    } else {
        int quantization = copy.quantization[coeff.band];
        if (quantization <= 0) quantization = copy.quant[coeff.band];
        if (quantization <= 0) quantization = 1;
        EncodeQuantizedBand(fixture.encoder, &stream, &copy, coeff.band, coeff.subband,
                            BAND_ENCODING_RUNLENGTHS, quantization);
    }
    Require(stream.error == 0 && stream.nWordsUsed > 0 &&
            static_cast<std::size_t>(stream.nWordsUsed) <= output.size(), "direct SDK fragment failed");
    output.resize(static_cast<std::size_t>(stream.nWordsUsed));
    return output;
}

void PositiveBands(Prepared& fixture) {
    for (const auto& ref : fixture.refs) {
        IMAGE* image = fixture.Image(ref);
        AlignedVector<int16_t> tight(static_cast<std::size_t>(ref.width) * ref.height);
        const int stride = image->pitch / static_cast<int>(sizeof(PIXEL));
        for (int y = 0; y < ref.height; ++y)
            std::copy_n(image->band[ref.band] + static_cast<std::size_t>(y) * stride, ref.width,
                        tight.data() + static_cast<std::size_t>(y) * ref.width);
        auto coeff = Coeff(ref, tight.data());
        const auto direct = DirectSdk(fixture, image, coeff);
        EncodedPiece piece;
        Require(EncodeOneExternalBand(fixture.encoder, fixture.transforms, fixture.channels, coeff, &piece), "valid band rejected");
        Require(piece.ok && piece.payload == direct, "wrapper differs from direct frozen SDK encoding");
        if (!ref.lowpass) {
            EntropySymbols symbols;
            Require(MatchUpstreamHighpass(fixture.encoder, fixture.transforms, fixture.channels, coeff, &symbols), "valid highpass match rejected: " + symbols.error);
            Require(symbols.byte_match && symbols.payload == direct, "staged highpass differs from direct SDK");
        }
        ++positives;
    }
    // Real array extent one is valid; the wrappers must not infer three channels.
    const auto ref = fixture.refs[1];
    AlignedVector<int16_t> narrow(static_cast<std::size_t>(ref.width) * ref.height);
    AlignedVector<int8_t> compact(narrow.size());
    for (std::size_t i = 0; i < narrow.size(); ++i) compact[i] = narrow[i] = static_cast<int>(i % 31) - 15;
    auto coeff = Coeff(ref, narrow.data());
    TRANSFORM* one[] = {fixture.transforms[0]};
    EncodedPiece wide, small;
    Require(EncodeOneExternalBand(fixture.encoder, one, 1, coeff, &wide), "one-channel extent rejected");
    coeff.elem_bytes = 1; coeff.data = compact.data(); coeff.value_min = -15; coeff.value_max = 15;
    Require(EncodeOneExternalBand(fixture.encoder, one, 1, coeff, &small), "valid int8 band rejected");
    Require(small.used_int8 && small.payload == wide.payload, "int8 payload changed");
    positives += 2;
    // A legal peaks override retains the SDK fallback for both transport types.
    WithValue(fixture.encoder->q.codebookflags[ref.subband], CBFLAG_PEAKCODE, [&] {
        coeff.elem_bytes = 2; coeff.data = narrow.data();
        const auto direct = DirectSdk(fixture, fixture.Image(ref), coeff);
        Require(EncodeOneExternalBand(fixture.encoder, one, 1, coeff, &wide), "valid peaks int16 rejected");
        coeff.elem_bytes = 1; coeff.data = compact.data();
        Require(EncodeOneExternalBand(fixture.encoder, one, 1, coeff, &small), "valid peaks int8 rejected");
        Require(wide.peaks_fallback && small.peaks_fallback && !small.used_int8 &&
                wide.payload == direct && small.payload == direct, "peaks fallback payload changed");
        positives += 2;
    });
}

void NegativeBands(Prepared& fixture) {
    const auto ref = fixture.refs[1];
    AlignedVector<int16_t> data(static_cast<std::size_t>(ref.width) * ref.height, 3);
    const auto good = Coeff(ref, data.data());
    for (int channel : {-1, fixture.channels, INT_MIN, INT_MAX}) {
        auto bad = good; bad.channel = channel; RejectBoth(fixture, bad, "channel");
    }
    for (int count : {-1, 0, TRANSFORM_MAX_CHANNELS + 1, INT_MAX})
        RejectBoth(fixture, good, count, "channel extent", fixture.transforms);
    TRANSFORM* one[] = {fixture.transforms[0]};
    auto outside = good; outside.channel = 1;
    RejectBoth(fixture, outside, 1, "actual one-entry array", one);
    RejectBoth(fixture, good, fixture.channels, "null transform array", nullptr);
    for (int value : {-1, IMAGE_NUM_BANDS, INT_MAX}) {
        auto bad = good; bad.band = value; RejectBoth(fixture, bad, "band");
    }
    for (int value : {-1, 0, fixture.transforms[0]->num_levels + 1, TRANSFORM_MAX_LEVELS + 1, INT_MAX}) {
        auto bad = good; bad.level = value; RejectBoth(fixture, bad, "level");
    }
    for (int value : {-1, INT_MIN, static_cast<int>(fixture.encoder->num_subbands), INT_MAX}) {
        auto bad = good; bad.subband = value; RejectBoth(fixture, bad, "subband");
        RejectTable(fixture, value, "table subband");
    }
    auto bad = good; bad.subband++; RejectBoth(fixture, bad, "subband belongs to another band");
    bad = good; bad.lowpass = true; RejectBoth(fixture, bad, "highpass marked lowpass");
    bad = good; bad.band = LL_BAND; RejectBoth(fixture, bad, "lowpass marked highpass");
    bad = good; bad.width++; RejectBoth(fixture, bad, "width mismatch");
    bad = good; bad.height = 0; RejectBoth(fixture, bad, "height zero");
    bad = good; bad.elem_bytes = 4; RejectBoth(fixture, bad, "element size");
    bad = good; bad.data = nullptr; RejectBoth(fixture, bad, "null coefficients");
    bad = good; bad.elem_bytes = 1; bad.value_min = -129; RejectBoth(fixture, bad, "int8 range");
    bad = Coeff(fixture.refs[0], data.data()); bad.level--; RejectBoth(fixture, bad, "lowpass below top wavelet");
    bad = Coeff(fixture.refs[0], data.data()); bad.subband = 1; RejectBoth(fixture, bad, "lowpass subband nonzero");
    bad = Coeff(fixture.refs[0], data.data()); bad.elem_bytes = 1; RejectBoth(fixture, bad, "int8 lowpass");

    auto* transform = fixture.transforms[0];
    auto* image = fixture.Image(ref);
    for (int value : {-1, 0, transform->num_wavelets - 1, TRANSFORM_MAX_WAVELETS + 1, INT_MAX}) {
        WithValue(transform->num_wavelets, value, [&] {
            RejectBoth(fixture, good, "wavelet count");
            Require(EnumerateBands(fixture.transforms, fixture.channels).empty(), "enumeration accepted invalid wavelet count");
        });
    }
    for (int value : {0, transform->num_levels + 1, TRANSFORM_MAX_LEVELS + 1})
        WithValue(transform->num_levels, value, [&] { RejectBoth(fixture, good, "level count"); });
    for (int value : {-1, INT_MIN, 0, 1, 3, TRANSFORM_MAX_LEVELS, INT_MAX})
        WithValue(transform->num_spatial, value, [&] { RejectBoth(fixture, good, "spatial count/relation"); });
    for (int value : {0, IMAGE_NUM_BANDS + 1})
        WithValue(image->num_bands, value, [&] { RejectBoth(fixture, good, "band count"); });
    WithValue(image->num_bands, 2, [&] {
        auto outside_band = good; outside_band.band = HH_BAND; outside_band.subband = 3;
        RejectBoth(fixture, outside_band, "band outside declared image count");
    });
    WithValue(transform->type, TRANSFORM_TYPE_FIELDPLUS, [&] { RejectBoth(fixture, good, "temporal transform unsupported"); });
    WithValue(transform->num_frames, 2, [&] { RejectBoth(fixture, good, "temporal frame count unsupported"); });
    WithValue(fixture.encoder->gop_length, static_cast<uint8_t>(2), [&] { RejectBoth(fixture, good, "GOP2 unsupported"); });
    WithValue(image->level, transform->wavelet[0]->level, [&] { RejectBoth(fixture, good, "duplicate wavelet level"); });
    WithValue(transform->wavelet[0], static_cast<IMAGE*>(nullptr), [&] { RejectBoth(fixture, good, "null wavelet"); });
    WithValue(fixture.transforms[0], static_cast<TRANSFORM*>(nullptr), [&] { RejectBoth(fixture, good, "null transform"); });
    for (int value : {-1, image->width * 2 - 2, image->pitch + 1, INT_MAX - 1})
        WithValue(image->pitch, value, [&] { RejectBoth(fixture, good, "invalid pitch or size overflow"); });
    WithValue(image->height, INT_MAX, [&] {
        auto huge = good; huge.height = INT_MAX; RejectBoth(fixture, huge, "height overflow");
    });
    for (int value : {0, CODEC_MAX_SUBBANDS + 1}) {
        WithValue(fixture.encoder->num_subbands, static_cast<uint8_t>(value), [&] {
            RejectBoth(fixture, good, "encoder subband count"); RejectTable(fixture, 0, "encoder table count");
        });
    }
    WithValue(fixture.encoder->num_subbands, static_cast<uint8_t>(fixture.encoder->num_subbands + 1), [&] {
        RejectBoth(fixture, good, "transform subband count mismatch");
    });
    for (int codebook : {CODEC_NUM_CODESETS, 15}) {
        WithValue(fixture.encoder->q.codebookflags[good.subband], codebook, [&] {
            RejectBoth(fixture, good, "codebook override"); RejectTable(fixture, good.subband, "table codebook override");
        });
    }
    EntropyTables table;
    Require(LoadEntropyTables(fixture.encoder, good.subband, &table), "valid table rejected after negatives");
    const int codebook = table.codebook;
    WithValue(fixture.encoder->codebook_runbook[codebook], static_cast<RLCBOOK*>(nullptr), [&] {
        RejectBoth(fixture, good, "missing runbook"); RejectTable(fixture, good.subband, "missing runbook table");
    });
    WithValue(fixture.encoder->valuebook[codebook], static_cast<VALBOOK*>(nullptr), [&] {
        RejectBoth(fixture, good, "missing valuebook"); RejectTable(fixture, good.subband, "missing valuebook table");
    });
    WithValue(fixture.encoder->band_end_size[codebook], 32, [&] {
        RejectBoth(fixture, good, "invalid band-end size"); RejectTable(fixture, good.subband, "invalid band-end table");
    });
    const int run_length = fixture.encoder->codebook_runbook[codebook]->length;
    for (int length : {1, run_length - 1, run_length + 1, INT_MAX}) {
        WithValue(fixture.encoder->codebook_runbook[codebook]->length, length, [&] {
            RejectBoth(fixture, good, "runbook extent"); RejectTable(fixture, good.subband, "runbook extent table");
        });
    }
    for (int length : {VALUE_TABLE_LENGTH - 1, VALUE_TABLE_LENGTH + 1, INT_MAX}) {
        WithValue(fixture.encoder->valuebook[codebook]->length, length, [&] {
            RejectBoth(fixture, good, "valuebook extent"); RejectTable(fixture, good.subband, "valuebook extent table");
        });
    }
    WithValue(fixture.encoder->valuebook[codebook]->size, VALUE_TABLE_SIZE + 1, [&] {
        RejectBoth(fixture, good, "valuebook size"); RejectTable(fixture, good.subband, "valuebook size table");
    });
}

void RejectRefresh(Prepared& fixture, TRANSFORM** transforms, int channels, const std::string& label) {
    const RefreshSnapshot before(fixture);
    Require(!RefreshUpstreamPyramid(fixture.encoder, transforms, channels,
                fixture.source_bgra.data(), Prepared::pitch, fixture.sample->m_scratchBuffer,
                fixture.sample->m_scratchBufferSize), label + ": accepted invalid refresh");
    before.Check(fixture, label);
    ++negatives;
}

void NegativeRefresh(Prepared& fixture) {
    const auto original_source = fixture.source_bgra;
    // A premature conversion must visibly change the retained plane contents,
    // even if the implementation later rejects the transform metadata.
    for (std::size_t i = 0; i < fixture.source_bgra.size(); ++i)
        if (i % 4 != 3) fixture.source_bgra[i] ^= 0x55;
    const auto reject = [&](const std::string& label) {
        RejectRefresh(fixture, fixture.transforms, fixture.channels, label);
    };
    for (int channels : {-1, 0, 1, 2, 4, INT_MAX})
        RejectRefresh(fixture, fixture.transforms, channels, "refresh transform extent");
    TRANSFORM* one[] = {fixture.transforms[0]};
    RejectRefresh(fixture, one, 1, "refresh actual one-entry transform array");
    for (int channels : {-1, 0, 1, 2, 4, INT_MAX})
        WithValue(fixture.frame->num_channels, channels, [&] { reject("refresh frame channel count"); });
    for (int format : {FRAME_FORMAT_NONE, FRAME_FORMAT_GRAY, FRAME_FORMAT_RGB, FRAME_FORMAT_RGBA})
        WithValue(fixture.frame->format, format, [&] { reject("refresh frame format"); });
    WithValue(fixture.encoder->frame, static_cast<FRAME*>(nullptr), [&] { reject("refresh null frame"); });
    for (int c = 0; c < fixture.channels; ++c) {
        WithValue(fixture.frame->channel[c], static_cast<IMAGE*>(nullptr), [&] { reject("refresh null plane"); });
        WithValue(fixture.planes[c]->band[0], static_cast<PIXEL*>(nullptr), [&] { reject("refresh null plane storage"); });
        WithValue(fixture.transforms[c]->num_spatial, 1, [&] { reject("refresh transform spatial mismatch"); });
    }
    for (int spatial : {0, 1, 3, TRANSFORM_MAX_LEVELS, 255})
        WithValue(fixture.encoder->num_spatial, static_cast<uint8_t>(spatial), [&] { reject("refresh encoder spatial mismatch"); });
    for (int spatial : {INT_MIN, -1, TRANSFORM_MAX_LEVELS, INT_MAX})
        WithValue(fixture.transforms[0]->num_spatial, spatial, [&] { reject("refresh spatial count outside bounds"); });

    // Preserve the recorded source view rather than replacing its allocation.
    std::copy(original_source.begin(), original_source.end(), fixture.source_bgra.begin());
    Require(RefreshUpstreamPyramid(fixture.encoder, fixture.transforms, fixture.channels,
                fixture.source_bgra.data(), Prepared::pitch, fixture.sample->m_scratchBuffer,
                fixture.sample->m_scratchBufferSize), "valid refresh rejected after negatives");
    ++positives;
}
}

int main() {
    try {
        Prepared fixture;
        PositiveBands(fixture);
        NegativeBands(fixture);
        NegativeRefresh(fixture);
        // Rejection must not poison the prepared SDK encoder or its inputs.
        PositiveBands(fixture);
        std::cout << "SDK entropy bounds: " << negatives << " negative controls, " << positives
                  << " positive comparisons; CPU-only original SDK, no CUDA qualification\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SDK entropy bounds: " << error.what() << '\n';
        return 1;
    }
}
