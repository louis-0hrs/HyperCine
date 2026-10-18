// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Deterministic checks for the issue #36 stage F1 export helpers. These do not
// need the SDK: they cover SHA-256, FNV-1a and the volatile-metadata masking that
// makes a normalized packet hash reproducible.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../tools/format_corpus_common.h"

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void AppendTag(std::vector<uint8_t>* packet, const char* tag, uint32_t payload_size,
               const std::vector<uint8_t>& payload) {
    packet->insert(packet->end(), tag, tag + 4);
    const uint32_t type_size = payload_size;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&type_size);
    packet->insert(packet->end(), bytes, bytes + sizeof(type_size));
    packet->insert(packet->end(), payload.begin(), payload.end());
}

void TestSha256Vectors() {
    const std::string abc = "abc";
    Check(hypercine_format::Sha256Hex(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256(\"abc\")");
    Check(hypercine_format::Sha256Hex(nullptr, 0) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256(empty)");
}

void TestMaskVolatileMetadata() {
    std::vector<uint8_t> packet = {0x01, 0x02, 0x03};
    AppendTag(&packet, "GUID", 16, std::vector<uint8_t>(16, 0xAB));
    packet.push_back(0x7F);
    AppendTag(&packet, "DATE", 8, std::vector<uint8_t>(8, 0xCD));
    packet.push_back(0x7E);
    const std::vector<uint8_t> original = packet;

    const std::vector<hypercine_format::MaskRange> ranges =
        hypercine_format::MaskVolatileMetadata(&packet);
    Check(ranges.size() == 2, "two volatile tags found");
    Check(packet.size() == original.size(), "masking keeps the packet length");
    Check(packet[3] == 'G' && packet[4] == 'U' && packet[5] == 'I' && packet[6] == 'D',
          "GUID tag bytes stay untouched");
    Check(packet[7] == 16, "GUID payload size stays untouched");
    for (size_t i = 11; i < 27; ++i) Check(packet[i] == 0, "GUID payload zeroed");
    Check(packet[27] == 0x7F, "bytes between tags stay untouched");
    Check(packet[32] == 8, "DATE payload size stays untouched");
    for (size_t i = 36; i < 44; ++i) Check(packet[i] == 0, "DATE payload zeroed");
    Check(packet[44] == 0x7E, "trailing byte stays untouched");

    std::vector<uint8_t> repeated = original;
    hypercine_format::MaskVolatileMetadata(&repeated);
    Check(repeated == packet, "masking is idempotent");
}

void TestMaskRejectsTruncatedPayload() {
    std::vector<uint8_t> packet = {'D', 'A', 'T', 'E'};
    const uint32_t type_size = 32;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&type_size);
    packet.insert(packet.end(), bytes, bytes + sizeof(type_size));
    packet.insert(packet.end(), {0x11, 0x22, 0x33});
    const std::vector<uint8_t> original = packet;
    const std::vector<hypercine_format::MaskRange> ranges =
        hypercine_format::MaskVolatileMetadata(&packet);
    Check(ranges.empty(), "truncated payload is not masked");
    Check(packet == original, "truncated payload bytes stay untouched");
}

void TestNullAndEmptyInput() {
    Check(hypercine_format::MaskVolatileMetadata(nullptr).empty(), "null input is safe");
    std::vector<uint8_t> empty;
    Check(hypercine_format::MaskVolatileMetadata(&empty).empty(), "empty input is safe");
}

void TestFnv() {
    const std::string abc = "abc";
    Check(hypercine_format::Fnv1a64(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()) ==
              0xE71FA2190541574Bull,
          "FNV-1a 64 of \"abc\"");
}

void TestJsonEscape() {
    Check(hypercine_format::EscapeJson("a\"b\\c") == "a\\\"b\\\\c", "quotes and backslashes escaped");
}

}  // namespace

int main() {
    TestSha256Vectors();
    TestMaskVolatileMetadata();
    TestMaskRejectsTruncatedPayload();
    TestNullAndEmptyInput();
    TestFnv();
    TestJsonEscape();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "format corpus export helper checks PASS\n";
    return 0;
}
