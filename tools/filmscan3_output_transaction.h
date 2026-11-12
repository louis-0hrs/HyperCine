// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

// CLI-only filesystem ownership. A successful run is published with one native
// no-replace directory rename. Failed/interrupted runs remain in a uniquely
// owned .hypercine-incomplete-* staging directory; there is deliberately no
// recursive cleanup of paths another process could have replaced or populated.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif
#endif

namespace hypercine_cli {
namespace fs = std::filesystem;

inline fs::path Absolute(const fs::path& path) {
    std::error_code error;
    auto result = fs::absolute(path, error);
    if (error) throw std::runtime_error("cannot resolve filesystem path");
    return result.lexically_normal();
}
inline fs::path Canonical(const fs::path& path) {
    std::error_code error;
    auto result = fs::canonical(path, error);
    if (error) throw std::runtime_error("path must have an existing readable parent");
    return result;
}

class Handle {
public:
#ifdef _WIN32
    using Value = HANDLE;
    static Value Invalid() noexcept { return INVALID_HANDLE_VALUE; }
#else
    using Value = int;
    static Value Invalid() noexcept { return -1; }
#endif
    explicit Handle(Value value = Invalid()) noexcept : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = Invalid(); }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { Reset(); value_ = other.value_; other.value_ = Invalid(); }
        return *this;
    }
    Value get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != Invalid(); }
    void CloseChecked() {
        const auto handle = value_; value_ = Invalid();
#ifdef _WIN32
        if (!CloseHandle(handle)) throw std::runtime_error("output close failed; run was not published");
#else
        if (close(handle) != 0) throw std::runtime_error("output close failed; run was not published");
#endif
    }
private:
    void Reset() noexcept {
        if (!valid()) return;
#ifdef _WIN32
        CloseHandle(value_);
#else
        close(value_);
#endif
        value_ = Invalid();
    }
    Value value_;
};

inline std::size_t Read(Handle::Value handle, void* data, std::size_t size) {
#ifdef _WIN32
    DWORD count = 0;
    if (!ReadFile(handle, data, static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD)), &count, nullptr))
        throw std::runtime_error("input read failed");
    return count;
#else
    ssize_t count;
    do { count = read(handle, data, std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))); }
    while (count < 0 && errno == EINTR);
    if (count < 0) throw std::runtime_error("input read failed");
    return static_cast<std::size_t>(count);
#endif
}

// Retain real input handles until publication. Early validation never creates
// output. A source truncated during encoding is detected again while reading.
class Input {
public:
    Input(const fs::path& path, std::uintmax_t expected_bytes) {
        const auto canonical = Canonical(path);
        parent_ = canonical.parent_path();
#ifdef _WIN32
        file_ = Handle(CreateFileW(canonical.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!file_.valid() || GetFileType(file_.get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(file_.get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("input must be a regular file");
        const auto size = (static_cast<std::uintmax_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
        file_ = Handle(open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat info{};
        if (!file_.valid() || fstat(file_.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
            throw std::runtime_error("input must be a regular file");
        const auto size = static_cast<std::uintmax_t>(info.st_size);
#endif
        if (size != expected_bytes)
            throw std::runtime_error("input or explicit metadata has an unexpected byte count");
    }
    const fs::path& parent() const noexcept { return parent_; }
    void ReadExact(void* data, std::size_t size) {
        auto* next = static_cast<unsigned char*>(data);
        while (size) {
            const auto count = Read(file_.get(), next, size);
            if (!count) throw std::runtime_error("input or explicit metadata was truncated");
            next += count; size -= count;
        }
    }
    void RequireEnd() {
        unsigned char extra;
        if (Read(file_.get(), &extra, 1) != 0)
            throw std::runtime_error("input or explicit metadata has excess bytes");
    }
private:
    Handle file_;
    fs::path parent_;
};

class OutputTransaction {
public:
    OutputTransaction(const fs::path& destination, const std::vector<fs::path>& retained_parents)
        : requested_parent_(Absolute(destination).parent_path()),
          parent_path_(Canonical(requested_parent_)),
          final_name_(Absolute(destination).filename()) {
        if (final_name_.empty() || final_name_ == "." || final_name_ == "..")
            throw std::runtime_error("output must name a new run directory");
#ifdef _WIN32
        // Reject stream and device-name spellings rather than relying on Win32
        // normalization to turn them into a different destination.
        const auto leaf = final_name_.native();
        if (leaf.find(L':') != std::wstring::npos || leaf.back() == L'.' || leaf.back() == L' ')
            throw std::runtime_error("output has an ambiguous directory name");
#endif
        // Compare physical ancestors, including case aliases on case-insensitive
        // filesystems. Merely comparing canonical strings is insufficient there.
        for (auto ancestor = parent_path_; !ancestor.empty();) {
            for (const auto& retained : retained_parents) {
                std::error_code error;
                const bool same = fs::equivalent(ancestor, retained, error);
                if (error) throw std::runtime_error("cannot verify retained input directory");
                if (same) throw std::runtime_error("output must be outside retained input and metadata directories");
            }
            const auto next = ancestor.parent_path();
            if (next == ancestor) break;
            ancestor = next;
        }
        OpenParent();
        if (DestinationExists()) throw std::runtime_error("output directory already exists; choose a new run directory");
        static std::atomic<unsigned long> serial{0};
#ifdef _WIN32
        const auto process = GetCurrentProcessId();
#else
        const auto process = getpid();
#endif
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            stage_name_ = ".hypercine-incomplete-" + std::to_string(process) + "-" + std::to_string(serial.fetch_add(1));
#ifdef _WIN32
            const auto stage_path = parent_path_ / stage_name_;
            if (!CreateDirectoryW(stage_path.c_str(), nullptr)) {
                if (GetLastError() == ERROR_ALREADY_EXISTS) continue;
                throw std::runtime_error("cannot create exclusive output staging directory");
            }
            stage_ = OpenDirectory(stage_path, true);
#else
            if (mkdirat(parent_.get(), stage_name_.c_str(), 0700) != 0) {
                if (errno == EEXIST) continue;
                throw std::runtime_error("cannot create exclusive output staging directory");
            }
            stage_ = Handle(openat(parent_.get(), stage_name_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (!stage_.valid()) throw std::runtime_error("output staging directory changed during creation");
#endif
            return;
        }
        throw std::runtime_error("cannot reserve a unique output staging directory");
    }
    OutputTransaction(const OutputTransaction&) = delete;
    OutputTransaction& operator=(const OutputTransaction&) = delete;

    void WriteNew(const std::string& filename, const void* data, std::size_t size) {
        if (filename.empty() || filename.find_first_of("/\\:") != std::string::npos || filename == "." || filename == "..")
            throw std::runtime_error("invalid output leaf name");
#ifdef _WIN32
        Handle file(CreateFileW((parent_path_ / stage_name_ / filename).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
#else
        Handle file(openat(stage_.get(), filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
#endif
        if (!file.valid()) throw std::runtime_error("cannot create exclusive output file");
        const auto* next = static_cast<const unsigned char*>(data);
        while (size) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(file.get(), next, static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD)), &count, nullptr) || !count)
                throw std::runtime_error("output write failed; run was not published");
#else
            ssize_t count;
            do { count = write(file.get(), next, std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))); }
            while (count < 0 && errno == EINTR);
            if (count <= 0) throw std::runtime_error("output write failed; run was not published");
#endif
            next += count; size -= static_cast<std::size_t>(count);
        }
#ifdef _WIN32
        if (!FlushFileBuffers(file.get())) throw std::runtime_error("output flush failed; run was not published");
#else
        if (fsync(file.get()) != 0) throw std::runtime_error("output flush failed; run was not published");
#endif
        file.CloseChecked();
    }

    void Publish() {
        // A replaced textual parent never redirects the writes through the
        // retained handle. Refuse publication if that requested name moved.
        if (!RequestedParentStillMatches())
            throw std::runtime_error("output parent changed; run was not published");
#ifndef _WIN32
        if (!StageNameStillMatches(stage_name_))
            throw std::runtime_error("output staging name changed; run was not published");
#endif
#ifdef _WIN32
        const auto name = final_name_.native();
        const auto bytes = offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t);
        // operator new supplies the structure's alignment; placement construction
        // starts its lifetime before the variable-length filename is copied.
        std::unique_ptr<void, void(*)(void*)> storage(::operator new(bytes), [](void* memory) { ::operator delete(memory); });
        auto* info = new (storage.get()) FILE_RENAME_INFO{};
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = parent_.get();
        info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, name.data(), info->FileNameLength);
        if (!SetFileInformationByHandle(stage_.get(), FileRenameInfo, info, static_cast<DWORD>(bytes)))
            throw std::runtime_error("exclusive output publication failed; choose a new run directory");
#elif defined(__linux__)
        if (syscall(SYS_renameat2, parent_.get(), stage_name_.c_str(), parent_.get(), final_name_.c_str(), RENAME_NOREPLACE) != 0)
            throw std::runtime_error("exclusive output publication failed; choose a new run directory");
#elif defined(__APPLE__)
        if (renameatx_np(parent_.get(), stage_name_.c_str(), parent_.get(), final_name_.c_str(), RENAME_EXCL) != 0)
            throw std::runtime_error("exclusive output publication failed; choose a new run directory");
#else
        // A check followed by ordinary rename can replace an empty destination.
        // Platforms without an atomic no-replace primitive must fail closed.
        throw std::runtime_error("atomic exclusive directory publication is unavailable on this platform");
#endif
#ifndef _WIN32
        // A process with mutation access to the same parent can also replace a
        // staging name. Never report success for a foreign directory or symlink.
        // No cleanup follows a mismatch: an unowned replacement is left intact.
        if (!StageNameStillMatches(final_name_))
            throw std::runtime_error("output identity changed during publication");
#endif
    }

private:
    bool RequestedParentStillMatches() const {
#ifdef _WIN32
        Handle current(CreateFileW(requested_parent_.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        // ReFS can require the full 128-bit ID. Legacy 64-bit file indexes are
        // not an adequate identity check on every Windows filesystem. If this
        // query is unavailable, refuse publication rather than weaken the check.
        FILE_ID_INFO actual{}, retained{};
        return current.valid() &&
            GetFileInformationByHandleEx(current.get(), FileIdInfo, &actual, sizeof(actual)) &&
            GetFileInformationByHandleEx(parent_.get(), FileIdInfo, &retained, sizeof(retained)) &&
            actual.VolumeSerialNumber == retained.VolumeSerialNumber &&
            std::memcmp(actual.FileId.Identifier, retained.FileId.Identifier, sizeof(actual.FileId.Identifier)) == 0;
#else
        struct stat actual{}, retained{};
        return stat(requested_parent_.c_str(), &actual) == 0 && fstat(parent_.get(), &retained) == 0 &&
            S_ISDIR(actual.st_mode) && actual.st_dev == retained.st_dev && actual.st_ino == retained.st_ino;
#endif
    }
#ifndef _WIN32
    bool StageNameStillMatches(const fs::path& name) const {
        struct stat actual{}, retained{};
        return fstatat(parent_.get(), name.c_str(), &actual, AT_SYMLINK_NOFOLLOW) == 0 &&
            fstat(stage_.get(), &retained) == 0 && S_ISDIR(actual.st_mode) &&
            actual.st_dev == retained.st_dev && actual.st_ino == retained.st_ino;
    }
#endif
#ifdef _WIN32
    static Handle OpenDirectory(const fs::path& path, bool rename = false) {
        Handle result(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | (rename ? DELETE : 0),
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!result.valid() || !GetFileInformationByHandle(result.get(), &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("output parent must be a stable real directory");
        return result;
    }
#endif
    void OpenParent() {
#ifdef _WIN32
        fs::path current = parent_path_.root_path();
        ancestors_.push_back(OpenDirectory(current));
        for (const auto& component : parent_path_.relative_path()) {
            current /= component;
            ancestors_.push_back(OpenDirectory(current));
        }
        parent_ = OpenDirectory(parent_path_);
#else
        parent_ = Handle(open(parent_path_.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!parent_.valid()) throw std::runtime_error("cannot open output root");
        for (const auto& component : parent_path_.relative_path()) {
            Handle next(openat(parent_.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (!next.valid()) throw std::runtime_error("output parent changed or contains a link");
            parent_ = std::move(next);
        }
#endif
    }
    bool DestinationExists() const {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW((parent_path_ / final_name_).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) return true;
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return false;
        throw std::runtime_error("cannot verify output destination");
#else
        struct stat info{};
        if (fstatat(parent_.get(), final_name_.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) return true;
        if (errno == ENOENT) return false;
        throw std::runtime_error("cannot verify output destination");
#endif
    }
    fs::path requested_parent_, parent_path_, final_name_;
    std::string stage_name_;
    Handle parent_, stage_;
#ifdef _WIN32
    std::vector<Handle> ancestors_;
#endif
};
} // namespace hypercine_cli
