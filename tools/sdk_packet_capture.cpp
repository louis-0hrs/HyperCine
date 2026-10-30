// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Only original public SDK calls: no HyperCine encoder or quality calculator.
#include <cstddef>
#include "CFHDEncoder.h"
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
namespace fs = std::filesystem;
void* Allocate(size_t bytes) {
#ifdef _WIN32
    return _aligned_malloc(bytes, 64);
#else
    void* ptr = nullptr;
    return posix_memalign(&ptr, 64, bytes) == 0 ? ptr : nullptr;
#endif
}
void Release(void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
struct Encoder {
    CFHD_EncoderRef ref = nullptr;
    ~Encoder() { if (ref) CFHD_CloseEncoder(ref); }
};
int main(int argc, char** argv) {
    try {
        if (argc < 4) throw std::runtime_error("usage: sdk_packet_capture QUALITY OUTPUT INPUT.bgra ...");
        const int quality = std::stoi(argv[1]);
        if (quality < 1 || quality > 6) throw std::runtime_error("quality outside 1..6");
        const fs::path out(argv[2]);
        if (fs::exists(out)) throw std::runtime_error("output must not exist");
        fs::create_directories(out);
        Encoder enc;
        if (CFHD_OpenEncoder(&enc.ref, nullptr) != CFHD_ERROR_OKAY ||
            CFHD_PrepareToEncode(enc.ref, 1920, 1080, CFHD_PIXEL_FORMAT_BGRA,
                CFHD_ENCODED_FORMAT_YUV_422, CFHD_ENCODING_FLAGS_NONE,
                static_cast<CFHD_EncodingQuality>(quality)) != CFHD_ERROR_OKAY)
            throw std::runtime_error("original SDK initialization failed");
        constexpr size_t bytes = 1920 * 1080 * 4;
        std::unique_ptr<void, decltype(&Release)> input(Allocate(bytes), &Release);
        if (!input) throw std::bad_alloc();
        std::ofstream trace(out / "trace.tsv");
        for (int f = 3; f < argc; ++f) {
            std::ifstream src(argv[f], std::ios::binary);
            src.read(static_cast<char*>(input.get()), bytes);
            if (!src || src.peek() != std::char_traits<char>::eof())
                throw std::runtime_error("input must contain exactly 1080p BGRA8 bytes");
            if (CFHD_EncodeSample(enc.ref, input.get(), 1920 * 4) != CFHD_ERROR_OKAY)
                throw std::runtime_error("original SDK encode failed");
            void* packet = nullptr; size_t size = 0;
            if (CFHD_GetSampleData(enc.ref, &packet, &size) != CFHD_ERROR_OKAY || !packet || !size)
                throw std::runtime_error("original SDK packet retrieval failed");
            const auto name = "frame_" + std::to_string(f - 3) + ".cfhd";
            std::ofstream dst(out / name, std::ios::binary);
            dst.write(static_cast<const char*>(packet), size);
            if (!dst) throw std::runtime_error("packet write failed");
            trace << f - 3 << '	' << size << '	' << name << '\n';
            if (!trace) throw std::runtime_error("trace write failed");
        }
        std::cout << "original SDK captured " << argc - 3 << " packets\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
