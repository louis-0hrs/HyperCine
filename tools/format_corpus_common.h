// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Shared helpers for the issue #36 format-corpus tools. Header-only so the
// export tool and its unit test use the same hashing and normalization code.

#pragma once

#ifndef HYPERCINE_FORMAT_CORPUS_COMMON_H
#define HYPERCINE_FORMAT_CORPUS_COMMON_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hypercine_format {

inline std::string Sha256Hex(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD got = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &got, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::vector<uint8_t> object(object_size);
    std::array<uint8_t, 32> digest{};
    const bool ok = BCryptCreateHash(alg, &hash, object.data(), object_size, nullptr, 0, 0) >= 0 &&
                    BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0 &&
                    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : digest) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

inline std::string Sha256Hex(const std::vector<uint8_t>& bytes) {
    return Sha256Hex(bytes.data(), bytes.size());
}

inline uint64_t Fnv1a64(const uint8_t* data, size_t size) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

inline std::string FnvText(uint64_t hash) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

// The pristine encoder stamps a fresh GUID and wall-clock DATE/TIME/TIMC
// metadata into every packet (docs/UPSTREAM.md quirks 8 and 11). Their value
// bytes are zeroed here so the normalized packet hashes reproduce across runs.
struct MaskRange {
    std::string tag;
    size_t offset = 0;
    size_t length = 0;
};

inline std::vector<MaskRange> MaskVolatileMetadata(std::vector<uint8_t>* sample) {
    static const char* kVolatileTags[] = {"GUID", "DATE", "TIME", "TIMC"};
    std::vector<MaskRange> ranges;
    if (!sample) return ranges;
    const size_t size = sample->size();
    for (const char* tag : kVolatileTags) {
        for (size_t i = 0; i + 8 <= size; ++i) {
            if (std::memcmp(sample->data() + i, tag, 4) != 0) continue;
            uint32_t type_size = 0;
            std::memcpy(&type_size, sample->data() + i + 4, sizeof(type_size));
            const uint32_t payload = type_size & 0x00FFFFFFu;
            if (payload == 0 || payload > 64 || i + 8 + payload > size) continue;
            std::memset(sample->data() + i + 8, 0, payload);
            ranges.push_back({tag, i + 8, payload});
            i += 7 + payload;
        }
    }
    return ranges;
}

inline std::string EscapeJson(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 0x20) out << '?';
        else out << static_cast<char>(c);
    }
    return out.str();
}

}  // namespace hypercine_format

#endif  // HYPERCINE_FORMAT_CORPUS_COMMON_H
