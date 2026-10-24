// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Host portability helpers for the benchmark tools. Measurement code stays in
// the tools; this header only covers compiler metadata, process pipes, and
// fail-closed artifact writes (issues #226 and #33).
#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>

namespace hypercine::bench {

inline void WriteCompilerField(std::ostream& out, const char* indent = "    ") {
#if defined(HYPERCINE_BENCH_COMPILER)
    out << indent << "\"compiler\": \"" << HYPERCINE_BENCH_COMPILER << "\",\n";
#elif defined(_MSC_VER)
    out << indent << "\"compiler\": \"MSVC " << _MSC_VER << "\",\n";
#elif defined(__clang__)
    out << indent << "\"compiler\": \"Clang\",\n";
#elif defined(__GNUC__)
    out << indent << "\"compiler\": \"GCC\",\n";
#else
    out << indent << "\"compiler\": \"Unknown\",\n";
#endif
}

inline const char* BuildTypeName(const char* fallback) {
#if defined(HYPERCINE_BENCH_BUILD_TYPE)
    return HYPERCINE_BENCH_BUILD_TYPE;
#else
    return fallback;
#endif
}

inline FILE* OpenReadPipe(const char* command) {
#if defined(_WIN32)
    return _popen(command, "r");
#else
    return popen(command, "r");
#endif
}

inline int CloseReadPipe(FILE* pipe) {
#if defined(_WIN32)
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

// Creates the parent of an artifact path. An empty parent (a bare filename in
// the working directory) is success. Failure is reported on stderr and is not
// a successful benchmark run.
inline bool CreateParentDirectory(const std::filesystem::path& file_path) {
    const std::filesystem::path parent = file_path.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        std::cerr << "Failed to create output directory " << parent.string()
                  << ": " << ec.message() << std::endl;
        return false;
    }
    return true;
}

// Writes the entire body and closes the stream. Returns false when the file
// was not fully written. Callers must not print a success banner in that case.
inline bool WriteCompleteFile(const std::filesystem::path& file_path, const std::string& body) {
    if (!CreateParentDirectory(file_path)) {
        return false;
    }
    std::ofstream out(file_path, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to write " << file_path.string() << std::endl;
        return false;
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.close();
    if (!out) {
        std::cerr << "Failed to finish writing " << file_path.string() << std::endl;
        return false;
    }
    return true;
}

}  // namespace hypercine::bench
