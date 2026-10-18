// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// RCA #159 probe: measure whether decoder thread count exceeds the number of
// CPUs the process may actually run on, and fingerprint each decoded frame.
//
// Motivation: Linux runs of the pristine upstream decoder described their arms
// as "1/2/4/8 logical processors" and pinned them with an affinity tool. The
// frozen SDK sizes its worker pools from sysconf(_SC_NPROCESSORS_ONLN), which
// ignores the affinity mask, so those arms did not limit the decoder at all.
// This probe reports the two quantities separately:
//
//   * affinity_cpus  - CPUs the process is allowed to run on (sched_getaffinity)
//   * decoder_threads - threads the process actually had during the decode
//                       (peak of /proc/self/status Threads)
//
// and it decodes a caller-supplied CFHD packet so the output can be compared by
// SHA-256 against a known-good reference. One call per fresh process keeps the
// measurement free of cross-frame state.
//
// Usage:
//   hypercine_decoder_thread_probe <calls> <cpu_max> <0|1> <packet.cfhd>
//
//     calls    - decode calls per process (use 1 for fresh-process sweeps)
//     cpu_max  - value for metadata tag TAG_CPU_MAX; 0 leaves it unset.
//                This is the only knob that actually limits the SDK pools.
//     encode   - 1 to encode a generated gradient and use it, 0 to decode the
//                packet given as the fourth argument
//
// Environment:
//   OUTDIR   - if set, each decoded frame is written there as dec_cNN.bgra
//
// Each line of stdout is one JSON object. The first describes the input and the
// process affinity; every following line is one decode call.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include <sched.h>

#include "CFHDDecoder.h"
#include "CFHDEncoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

// Minimal SHA-256 so the probe has no external dependency.
struct Sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t buflen;

    Sha256() { reset(); }

    void reset() {
        static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                       0xa54ff53a, 0x510e527f, 0x9b05688c,
                                       0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h, iv, sizeof(h));
        len = 0;
        buflen = 0;
    }

    static uint32_t rot(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
                   (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rot(e, 6) ^ rot(e, 11) ^ rot(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rot(a, 2) ^ rot(a, 13) ^ rot(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len += n;
        while (n) {
            size_t take = 64 - buflen;
            if (take > n) take = n;
            std::memcpy(buf + buflen, p, take);
            buflen += take;
            p += take;
            n -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) update(&zero, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(lb, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

std::string sha(const void* data, size_t n) {
    Sha256 s;
    s.update(data, n);
    return s.hex();
}

// Peak thread count is read after the decode returns; the pools are created
// during the decode and are not joined until the decoder is closed.
int process_threads() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    int n = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "Threads:", 8) == 0) std::sscanf(line + 8, "%d", &n);
    }
    std::fclose(f);
    return n;
}

int affinity_cpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return -1;
    return CPU_COUNT(&set);
}

void fill_gradient(std::vector<uint8_t>& img) {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            size_t i = (size_t(y) * kWidth + x) * 4;
            img[i + 0] = (uint8_t)((x * 255) / kWidth);
            img[i + 1] = (uint8_t)((y * 255) / kHeight);
            img[i + 2] = (uint8_t)(((x / 2 + y / 2) * 255) / (kWidth + kHeight));
            img[i + 3] = 255;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int calls = (argc > 1) ? std::atoi(argv[1]) : 5;
    const int cpu_max = (argc > 2) ? std::atoi(argv[2]) : 0;
    const bool do_encode = (argc > 3) ? std::atoi(argv[3]) != 0 : true;
    const char* packet_path = (argc > 4) ? argv[4] : nullptr;

    std::vector<uint8_t> image((size_t)kWidth * kHeight * 4);
    fill_gradient(image);

    std::vector<uint8_t> packet;
    if (do_encode) {
        CFHD_EncoderRef enc = nullptr;
        if (CFHD_OpenEncoder(&enc, nullptr) != CFHD_ERROR_OKAY || !enc) {
            std::fprintf(stderr, "open encoder failed\n");
            return 2;
        }
        if (CFHD_PrepareToEncode(enc, kWidth, kHeight, CFHD_PIXEL_FORMAT_BGRA,
                                 CFHD_ENCODED_FORMAT_RGB_444,
                                 CFHD_ENCODING_FLAGS_NONE,
                                 CFHD_ENCODING_QUALITY_FILMSCAN3) != CFHD_ERROR_OKAY) {
            std::fprintf(stderr, "prepare encode failed\n");
            return 2;
        }
        if (CFHD_EncodeSample(enc, image.data(), kWidth * 4) != CFHD_ERROR_OKAY) {
            std::fprintf(stderr, "encode failed\n");
            return 2;
        }
        void* data = nullptr;
        size_t size = 0;
        if (CFHD_GetSampleData(enc, &data, &size) != CFHD_ERROR_OKAY || !data || !size) {
            std::fprintf(stderr, "get sample failed\n");
            return 2;
        }
        packet.assign((uint8_t*)data, (uint8_t*)data + size);
        CFHD_CloseEncoder(enc);
        if (packet_path) {
            FILE* f = std::fopen(packet_path, "wb");
            if (f) { std::fwrite(packet.data(), 1, packet.size(), f); std::fclose(f); }
        }
    } else {
        FILE* f = std::fopen(packet_path, "rb");
        if (!f) { std::fprintf(stderr, "packet not found\n"); return 2; }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        packet.resize((size_t)size);
        const bool ok = std::fread(packet.data(), 1, packet.size(), f) == (size_t)size;
        std::fclose(f);
        if (!ok) return 2;
    }

    std::printf(
        "{\"packet_bytes\":%zu,\"packet_sha256\":\"%s\",\"affinity_cpus\":%d,"
        "\"cpu_max\":%d}\n",
        packet.size(), sha(packet.data(), packet.size()).c_str(),
        affinity_cpus(), cpu_max);

    for (int i = 0; i < calls; ++i) {
        const auto t0 = std::chrono::steady_clock::now();

        CFHD_DecoderRef dec = nullptr;
        CFHD_Error err = CFHD_OpenDecoder(&dec, nullptr);
        if (err != CFHD_ERROR_OKAY || !dec) {
            std::printf("{\"call\":%d,\"status\":\"open_failed\",\"code\":%d}\n", i, int(err));
            continue;
        }

        int w = 0, h = 0;
        CFHD_PixelFormat fmt = CFHD_PIXEL_FORMAT_UNKNOWN;
        err = CFHD_PrepareToDecode(dec, 0, 0, CFHD_PIXEL_FORMAT_BGRA,
                                   CFHD_DECODED_RESOLUTION_FULL,
                                   CFHD_DECODING_FLAGS_NONE, packet.data(),
                                   packet.size(), &w, &h, &fmt);
        if (err != CFHD_ERROR_OKAY) {
            std::printf("{\"call\":%d,\"status\":\"prepare_failed\",\"code\":%d}\n", i, int(err));
            CFHD_CloseDecoder(dec);
            continue;
        }

        int32_t pitch = 0;
        CFHD_GetImagePitch(w, fmt, &pitch);
        if (pitch <= 0) pitch = w * 4;

        // Sentinel tail so a write past the nominal image size is visible.
        std::vector<uint8_t> out((size_t)pitch * h + 64, 0xCD);

        CFHD_MetadataRef meta = nullptr;
        if (CFHD_OpenMetadata(&meta) == CFHD_ERROR_OKAY && meta) {
            CFHD_InitSampleMetadata(meta, METADATATYPE_ORIGINAL, packet.data(),
                                    (uint32_t)packet.size());
            unsigned int path = PROCESSING_ALL_ON;
            CFHD_SetActiveMetadata(dec, meta, TAG_PROCESS_PATH, METADATATYPE_UINT32,
                                   &path, sizeof(path));
            if (cpu_max > 0) {
                uint32_t limit = (uint32_t)cpu_max;
                CFHD_SetActiveMetadata(dec, meta, TAG_CPU_MAX, METADATATYPE_HIDDEN,
                                       &limit, sizeof(limit));
            }
        }

        err = CFHD_DecodeSample(dec, packet.data(), packet.size(), out.data(), pitch);
        if (meta) CFHD_CloseMetadata(meta);

        const int threads = process_threads();
        CFHD_CloseDecoder(dec);

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (const char* dir = std::getenv("OUTDIR")) {
            char name[512];
            std::snprintf(name, sizeof(name), "%s/dec_c%02d.bgra", dir, i);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fwrite(out.data(), 1, (size_t)pitch * h, f);
                std::fclose(f);
            }
        }

        std::printf(
            "{\"call\":%d,\"status\":\"%s\",\"code\":%d,\"width\":%d,\"height\":%d,"
            "\"pitch\":%d,\"frame_sha256\":\"%s\",\"elapsed_ms\":%.3f,"
            "\"decoder_threads\":%d,\"worker_threads\":%d,\"affinity_cpus\":%d,"
            "\"cpu_max\":%d,\"oversubscribed\":%s}\n",
            i, err == CFHD_ERROR_OKAY ? "ok" : "decode_failed", int(err), w, h, pitch,
            sha(out.data(), out.size()).c_str(), ms, threads, threads - 1, affinity_cpus(),
            cpu_max, (threads - 1) > affinity_cpus() ? "true" : "false");
        std::fflush(stdout);
    }
    return 0;
}
