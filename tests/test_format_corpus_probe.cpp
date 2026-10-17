// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Exercise fault records without asking the frozen SDK to fail or allocate.
#define main format_corpus_probe_main
#include "../tools/format_corpus_probe.cpp"
#undef main

int main() {
    int failures = 0;
    int checks = 0;
    for (const bool decode : {false, true}) {
        for (const int error : {static_cast<int>(CFHD_ERROR_BADFORMAT),
                               static_cast<int>(CFHD_ERROR_OUTOFMEMORY),
                               static_cast<int>(CFHD_ERROR_INTERNAL), -1}) {
            CaseResult item;
            item.source_bytes = 16;
            item.encode.opened = true;
            item.encode.prepare_error = CFHD_ERROR_OKAY;
            item.encode.encode_error = CFHD_ERROR_OKAY;
            item.encode.sample_error = CFHD_ERROR_OKAY;
            item.sample.assign(16, 0);
            if (decode) {
                item.decode.stage = "prepare";
                item.decode.thrown = true;
                item.decode.thrown_error = error;
            } else {
                item.encode.stage = "prepare";
                item.encode.thrown = true;
                item.encode.thrown_error = error;
            }
            const std::string expected = error == CFHD_ERROR_BADFORMAT
                ? (decode ? "DECODE_UNSUPPORTED" : "SDK_UNSUPPORTED")
                : (decode ? "DECODE_FAILED" : "ENCODE_FAILED");
            ++checks;
            if (VerdictFor(item) != expected) {
                std::cerr << (decode ? "decode" : "encode") << " prepare error "
                          << error << " misclassified\n";
                ++failures;
            }
        }
    }
    std::cout << checks << " fault classification checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
