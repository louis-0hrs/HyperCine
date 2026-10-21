// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// RCA-195 Milestone-1 completion harness. TEST-ONLY; it is built against a
// private out-of-repo SDK copy carrying the rca195 probe, never against the
// frozen tree in this repository.
//
// One decode per invocation, so every record belongs to exactly one call. The
// printed JSON carries what the review asks to see on the same call: the queue
// state recorded immediately before the reset, the public SDK return, and the
// output digest.
//
// argv: <sample.cfhd> <cpu_max> <FULL|HALF|QUARTER> <outdir> <tag>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <unistd.h>

#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "CFHDMetadataTags.h"

static bool read_file(const char *p, std::vector<uint8_t> &v) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize((size_t)n);
    bool ok = fread(v.data(), 1, v.size(), f) == v.size();
    fclose(f);
    return ok;
}

// Minimal SHA-256, so a digest is reproducible without extra dependencies.
static std::string sha256(const uint8_t *data, size_t len) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::vector<uint8_t> buf(data, data + len);
    buf.push_back(0x80);
    while (buf.size() % 64 != 56) buf.push_back(0);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 7; i >= 0; --i) buf.push_back((uint8_t)(bits >> (i * 8)));
    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (buf[off+i*4]<<24)|(buf[off+i*4+1]<<16)|(buf[off+i*4+2]<<8)|buf[off+i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ((w[i-15]>>7)|(w[i-15]<<25)) ^ ((w[i-15]>>18)|(w[i-15]<<14)) ^ (w[i-15]>>3);
            uint32_t s1 = ((w[i-2]>>17)|(w[i-2]<<15)) ^ ((w[i-2]>>19)|(w[i-2]<<13)) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ((e>>6)|(e<<26)) ^ ((e>>11)|(e<<21)) ^ ((e>>25)|(e<<7));
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ((a>>2)|(a<<30)) ^ ((a>>13)|(a<<19)) ^ ((a>>22)|(a<<10));
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) snprintf(out + i*8, 9, "%08x", h[i]);
    out[64] = 0;
    return std::string(out);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s sample cpu_max FULL|HALF|QUARTER outdir tag\n", argv[0]);
        return 2;
    }
    const char *sample_path = argv[1];
    int cpu_max = atoi(argv[2]);
    const char *resarg = argv[3];
    const char *outdir = argv[4];
    const char *tag = argv[5];

    CFHD_DecodedResolution res = CFHD_DECODED_RESOLUTION_FULL;
    if (!strcmp(resarg, "HALF")) res = CFHD_DECODED_RESOLUTION_HALF;
    else if (!strcmp(resarg, "QUARTER")) res = CFHD_DECODED_RESOLUTION_QUARTER;

    std::vector<uint8_t> sample;
    if (!read_file(sample_path, sample)) { fprintf(stderr, "sample not found\n"); return 2; }

    char logpath[800];
    snprintf(logpath, sizeof(logpath), "%s/probe_%s.log", outdir, tag);
    unlink(logpath);
    setenv("RCA195_PROBE_LOG", logpath, 1);

    auto t0 = std::chrono::steady_clock::now();
    CFHD_DecoderRef dec = nullptr;
    CFHD_Error e = CFHD_OpenDecoder(&dec, nullptr);
    if (e != CFHD_ERROR_OKAY || !dec) {
        printf("{\"tag\":\"%s\",\"status\":\"open_failed\",\"code\":%d}\n", tag, (int)e);
        return 0;
    }

    int aw = 0, ah = 0;
    CFHD_PixelFormat fmt = CFHD_PIXEL_FORMAT_UNKNOWN;
    e = CFHD_PrepareToDecode(dec, 0, 0, CFHD_PIXEL_FORMAT_BGRA, res,
                             CFHD_DECODING_FLAGS_NONE, sample.data(), sample.size(),
                             &aw, &ah, &fmt);
    if (e != CFHD_ERROR_OKAY) {
        printf("{\"tag\":\"%s\",\"status\":\"prepare_failed\",\"code\":%d}\n", tag, (int)e);
        CFHD_CloseDecoder(dec);
        return 0;
    }

    int32_t pitch = 0;
    CFHD_GetImagePitch(aw, fmt, &pitch);
    if (pitch <= 0) pitch = aw * 4;

    /* The guard region must exist: a buffer sized exactly pitch*height leaves
     * the check below with an empty range, so `guard_ok` would be vacuously 1
     * in every call. Allocate real slack and report its size. */
    const size_t GUARD_BYTES = 64 * 1024;
    const size_t active_bytes = (size_t)pitch * (size_t)ah;
    std::vector<uint8_t> out(active_bytes + GUARD_BYTES, 0xCD);

    CFHD_MetadataRef meta = nullptr;
    if (CFHD_OpenMetadata(&meta) == CFHD_ERROR_OKAY && meta) {
        CFHD_InitSampleMetadata(meta, METADATATYPE_ORIGINAL, sample.data(), (uint32_t)sample.size());
        unsigned int pf = PROCESSING_ALL_ON;
        CFHD_SetActiveMetadata(dec, meta, TAG_PROCESS_PATH, METADATATYPE_UINT32, &pf, sizeof(pf));
        if (cpu_max > 0) {
            uint32_t cm = (uint32_t)cpu_max;
            CFHD_SetActiveMetadata(dec, meta, TAG_CPU_MAX, METADATATYPE_HIDDEN, &cm, sizeof(cm));
        }
    }

    e = CFHD_DecodeSample(dec, sample.data(), sample.size(), out.data(), pitch);
    if (meta) CFHD_CloseMetadata(meta);
    CFHD_CloseDecoder(dec);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Guard band: bytes beyond the active image must be untouched.
    int guard_ok = 1;
    size_t guard_first = out.size();
    for (size_t g = active_bytes; g < out.size(); ++g)
        if (out[g] != 0xCD) { guard_ok = 0; guard_first = g; break; }

    std::string digest = sha256(out.data(), active_bytes);
    char fn[900];
    snprintf(fn, sizeof(fn), "%s/frame_%s.bgra", outdir, tag);
    if (FILE *of = fopen(fn, "wb")) { fwrite(out.data(), 1, active_bytes, of); fclose(of); }

    printf("{\"tag\":\"%s\",\"res\":\"%s\",\"cpu_max\":%d,\"w\":%d,\"h\":%d,\"pitch\":%d,"
           "\"out_bytes\":%zu,\"guard_bytes\":%zu,\"guard_first_dirty\":%zu,"
           "\"sdk_error\":%d,\"sdk_ok\":%d,\"guard_ok\":%d,"
           "\"sha256\":\"%s\",\"ms\":%.3f,\"log\":\"%s\"}\n",
           tag, resarg, cpu_max, aw, ah, (int)pitch, active_bytes, GUARD_BYTES,
           guard_ok ? 0 : guard_first,
           (int)e, e == CFHD_ERROR_OKAY ? 1 : 0, guard_ok,
           digest.c_str(), ms, logpath);
    return 0;
}
