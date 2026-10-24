// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Host portability helpers for the benchmark tools. Measurement code stays in
// the tools; this header only covers compiler metadata, process pipes, and
// fail-closed artifact writes (issues #226 and #33).
//
// A closed stream is not a measurement record. Character devices accept every
// write and keep nothing (POSIX /dev/null, Windows NUL). Callers may claim
// success only when the path is a regular file both before the open and again
// after the close.
#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

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

// Nothing there yet may become a regular file. An existing regular file may be
// overwritten. A device, pipe, socket, directory, or a path that cannot be
// inspected must not be reported as a saved artifact.
enum class OutputTargetKind {
    Missing,
    RegularFile,
    NotRegular,
    Uninspectable
};

inline OutputTargetKind InspectOutputTarget(const std::string& path) {
#if defined(_WIN32)
    HANDLE handle = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return OutputTargetKind::Missing;
        }
        return OutputTargetKind::Uninspectable;
    }
    const DWORD type = GetFileType(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL haveInfo = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (type != FILE_TYPE_DISK) {
        return OutputTargetKind::NotRegular;
    }
    if (!haveInfo) {
        return OutputTargetKind::Uninspectable;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return OutputTargetKind::NotRegular;
    }
    return OutputTargetKind::RegularFile;
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return (errno == ENOENT || errno == ENOTDIR) ? OutputTargetKind::Missing
                                                     : OutputTargetKind::Uninspectable;
    }
    return S_ISREG(info.st_mode) ? OutputTargetKind::RegularFile
                                 : OutputTargetKind::NotRegular;
#endif
}

inline const char* DescribeRejectedTarget(OutputTargetKind kind) {
    return kind == OutputTargetKind::NotRegular
               ? "target is not a regular file (device, pipe, socket or directory)"
               : "target path cannot be inspected as a regular file";
}

// Missing and RegularFile may be opened. Anything else is refused before the
// stream exists, so a later successful close cannot be mistaken for an artifact.
inline bool OutputTargetAllowsWrite(const std::filesystem::path& file_path) {
    const OutputTargetKind kind = InspectOutputTarget(file_path.string());
    if (kind == OutputTargetKind::NotRegular || kind == OutputTargetKind::Uninspectable) {
        std::cerr << "Refusing to write " << file_path.string() << ": "
                  << DescribeRejectedTarget(kind) << std::endl;
        return false;
    }
    return true;
}

// After close, the path has to name a regular file. Bytes accepted by a device
// do not count.
inline bool OutputTargetIsRegularFile(const std::filesystem::path& file_path) {
    if (InspectOutputTarget(file_path.string()) != OutputTargetKind::RegularFile) {
        std::cerr << "Failed to write " << file_path.string()
                  << ": did not produce a regular file" << std::endl;
        return false;
    }
    return true;
}

// Writes the entire body and closes the stream. Returns false unless the path
// was a regular file afterwards. Callers must not print a success banner in
// that case.
inline bool WriteCompleteFile(const std::filesystem::path& file_path, const std::string& body) {
    if (!OutputTargetAllowsWrite(file_path)) {
        return false;
    }
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
    return OutputTargetIsRegularFile(file_path);
}

}  // namespace hypercine::bench
