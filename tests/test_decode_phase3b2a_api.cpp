// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decoder.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    Require(bool(in), "open progressive GOP-1 fixture");
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    Require(n > 0, "fixture size");
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    Require(bool(in), "read fixture");
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: test_decode_phase3b2a_api <repo-root>");
        const auto sample = ReadFile(std::filesystem::path(argv[1]) /
            "tests/fixtures/phase3b0/YUV422-p10-ch3-spatial-prog-f1-bands30-fmt-1.cfhd");
        HyperCine::Decoder decoder;
        Require(decoder.Initialize() == HyperCine::DecodeError::None, "initialize decoder");
        Require(decoder.Capabilities().progressive_fieldplus_gop2,
            "capabilities advertise progressive FIELDPLUS GOP-2 on CUDA");

        HyperCine::DecodeRequest request{};
        Require(request.frame_index == 0, "one-frame default index remains zero");
        request.frame_index = 1;
        HyperCine::DecodeError error = HyperCine::DecodeError::None;
        const auto ticket = decoder.QueueDecode(sample.data(), sample.size(), request, &error);
        Require(bool(ticket), "invalid GOP-1 frame index gets a releasable failed ticket");
        const auto status = decoder.WaitForDecode(ticket);
        Require(status.state == HyperCine::DecodeState::Failed && status.error == HyperCine::DecodeError::InvalidRequest,
            "GOP-1 rejects frame index one");
        Require(error == HyperCine::DecodeError::InvalidRequest, "QueueDecode reports invalid GOP-1 frame index");
        Require(decoder.ReleaseOutput(ticket), "release rejected frame-index ticket");

        request.frame_index = -1;
        const auto invalid = decoder.QueueDecode(sample.data(), sample.size(), request, &error);
        Require(!invalid && error == HyperCine::DecodeError::InvalidRequest,
            "negative frame index rejected before ticket allocation");

        const auto interlaced = ReadFile(std::filesystem::path(argv[1]) /
            "tests/fixtures/phase3b0/YUV422-p10-ch3-fieldplus-field-f2-bands54-fmt-1.cfhd");
        request.frame_index = 0;
        const auto rejected_fieldplus = decoder.QueueDecode(interlaced.data(), interlaced.size(), request, &error);
        Require(bool(rejected_fieldplus), "interlaced FIELDPLUS gets a releasable failed ticket");
        const auto fieldplus_status = decoder.WaitForDecode(rejected_fieldplus);
        Require(fieldplus_status.state == HyperCine::DecodeState::Failed &&
            fieldplus_status.error == HyperCine::DecodeError::UnsupportedEncodingMode,
            "interlaced FIELDPLUS is rejected by production dispatch");
        Require(error == HyperCine::DecodeError::UnsupportedEncodingMode,
            "QueueDecode reports unsupported interlaced FIELDPLUS");
        Require(decoder.ReleaseOutput(rejected_fieldplus), "release rejected interlaced FIELDPLUS ticket");

        const auto oversized = decoder.QueueDecode(sample.data(), 32u * 1024u * 1024u + 1u, request, &error);
        Require(bool(oversized) && error == HyperCine::DecodeError::SampleTooLarge,
            "oversized sample rejection");
        Require(decoder.ReleaseOutput(oversized), "release oversized-sample ticket");
        decoder.Release();
        std::cout << "DecodeRequest frame-index validation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
