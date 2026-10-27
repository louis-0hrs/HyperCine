// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Test-only Windows affinity launcher and topology probe. The launcher creates
// the reference decoder worker suspended, applies and verifies the requested
// process affinity before the worker (and therefore any SDK decoder) can run,
// enforces a bounded wall clock, and records the observed masks. It links no
// codec, GPU or production code.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr DWORD kAffinityFailureExit = 3;
constexpr DWORD kTimeoutExit = 4;
constexpr DWORD kLauncherFailureExit = 5;
constexpr DWORD kChildTimeoutMs = 20000;

std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::nouppercase << value;
    return out.str();
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
        }
    }
    return out;
}

std::string CpuBrand() {
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    const unsigned max_extended = static_cast<unsigned>(regs[0]);
    if (max_extended < 0x80000004) return "UNKNOWN";
    char brand[49] = {};
    for (unsigned leaf = 0x80000002; leaf <= 0x80000004; ++leaf) {
        __cpuid(regs, static_cast<int>(leaf));
        std::memcpy(brand + (leaf - 0x80000002) * 16, regs, 16);
    }
    std::string text(brand);
    const auto first = text.find_first_not_of(' ');
    const auto last = text.find_last_not_of(' ');
    if (first == std::string::npos) return "UNKNOWN";
    return text.substr(first, last - first + 1);
}

struct CoreRecord {
    unsigned group = 0;
    uint64_t mask = 0;
    bool smt = false;
    unsigned efficiency_class = 0;
};

struct Topology {
    bool available = false;
    DWORD processor_groups = 0;
    DWORD active_logical_processors = 0;
    uint64_t system_mask = 0;
    uint64_t process_mask = 0;
    std::vector<CoreRecord> cores;
    DWORD packages = 0;
    std::string source = "GetLogicalProcessorInformationEx";
    std::string error;
};

bool ReadLegacyTopology(Topology& topology) {
    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);
    if (length == 0 || length % sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) != 0) {
        return false;
    }
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> entries(
        length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(entries.data(), &length)) return false;
    std::vector<CoreRecord> cores;
    DWORD packages = 0;
    for (const auto& entry : entries) {
        if (entry.Relationship == RelationProcessorCore) {
            CoreRecord core;
            core.group = 0;
            core.mask = static_cast<uint64_t>(entry.ProcessorMask);
            core.smt = (entry.ProcessorCore.Flags & LTP_PC_SMT) != 0;
            cores.push_back(core);
        } else if (entry.Relationship == RelationProcessorPackage) {
            ++packages;
        }
    }
    if (cores.empty()) return false;
    topology.cores = std::move(cores);
    topology.packages = packages;
    topology.source = "GetLogicalProcessorInformation";
    return true;
}

Topology ReadTopology() {
    Topology topology;
    topology.processor_groups = GetActiveProcessorGroupCount();
    topology.active_logical_processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &topology.process_mask,
                                &topology.system_mask)) {
        topology.error = "GetProcessAffinityMask";
        return topology;
    }

    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        topology.error = "RelationProcessorCore length";
        return topology;
    }
    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &length)) {
        topology.error = "RelationProcessorCore";
        return topology;
    }
    const auto* cursor = reinterpret_cast<const unsigned char*>(buffer.data());
    const auto* end = cursor + length;
    while (cursor + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= end) {
        const auto* item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            cursor);
        if (item->Relationship == RelationProcessorCore) {
            for (WORD index = 0; index < item->Processor.GroupCount; ++index) {
                const GROUP_AFFINITY& affinity = item->Processor.GroupMask[index];
                CoreRecord core;
                core.group = affinity.Group;
                core.mask = static_cast<uint64_t>(affinity.Mask);
                core.smt = (item->Processor.Flags & LTP_PC_SMT) != 0;
                core.efficiency_class = item->Processor.EfficiencyClass;
                topology.cores.push_back(core);
            }
        }
        if (item->Size == 0 || cursor + item->Size > end) break;
        cursor += item->Size;
    }

    DWORD package_length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorPackage, nullptr, &package_length);
    if (package_length) {
        std::vector<unsigned char> packages(package_length);
        if (GetLogicalProcessorInformationEx(RelationProcessorPackage,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(packages.data()),
                &package_length)) {
            const auto* package_cursor = packages.data();
            const auto* package_end = package_cursor + package_length;
            while (package_cursor + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <=
                   package_end) {
                const auto* item =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        package_cursor);
                if (item->Relationship == RelationProcessorPackage) ++topology.packages;
                if (item->Size == 0 || package_cursor + item->Size > package_end) break;
                package_cursor += item->Size;
            }
        }
    }

    uint64_t covered = 0;
    for (const CoreRecord& core : topology.cores) covered |= core.mask;
    if (topology.cores.empty() || covered != topology.system_mask ||
        topology.packages == 0) {
        Topology legacy;
        if (ReadLegacyTopology(legacy)) {
            if (topology.cores.empty() || covered != topology.system_mask) {
                topology.cores = std::move(legacy.cores);
                topology.source = legacy.source;
            }
            if (topology.packages == 0) topology.packages = legacy.packages;
        }
    }

    topology.available = !topology.cores.empty();
    if (!topology.available) topology.error = "no processor cores";
    return topology;
}

std::string TopologyJson(const Topology& topology) {
    std::ostringstream out;
    out << '{';
    out << "\"schema_version\":1";
    out << ",\"topology_available\":" << (topology.available ? "true" : "false");
    out << ",\"processor_groups\":" << topology.processor_groups;
    out << ",\"active_logical_processors\":" << topology.active_logical_processors;
    out << ",\"system_affinity_mask\":\"" << Hex(topology.system_mask) << '"';
    out << ",\"process_affinity_mask\":\"" << Hex(topology.process_mask) << '"';
    out << ",\"core_count\":" << topology.cores.size();
    out << ",\"smt_core_count\":"
        << std::count_if(topology.cores.begin(), topology.cores.end(),
                         [](const CoreRecord& core) { return core.smt; });
    out << ",\"packages\":" << topology.packages;
    out << ",\"topology_source\":\"" << JsonEscape(topology.source) << '"';
    out << ",\"cpu_brand\":\"" << JsonEscape(CpuBrand()) << '"';
    out << ",\"cores\":[";
    for (size_t index = 0; index < topology.cores.size(); ++index) {
        if (index) out << ',';
        const CoreRecord& core = topology.cores[index];
        out << "{\"group\":" << core.group << ",\"mask\":\"" << Hex(core.mask)
            << "\",\"smt\":" << (core.smt ? "true" : "false")
            << ",\"efficiency_class\":" << core.efficiency_class << '}';
    }
    out << ']';
    if (!topology.error.empty())
        out << ",\"error\":\"" << JsonEscape(topology.error) << '"';
    out << '}';
    return out.str();
}

std::wstring QuoteArgument(const std::wstring& argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out += c;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string MaskText(uint64_t mask) { return Hex(mask); }

struct Meta {
    std::string requested;
    uint64_t requested_mask = 0;
    uint64_t effective_mask = 0;
    uint64_t system_mask = 0;
    uint64_t process_mask = 0;
    bool affinity_ok = false;
    bool child_started = false;
    bool timeout = false;
    DWORD exit_code = 0;
    uint64_t wall_ns = 0;
    std::string invalid_reason;
};

std::string MetaJson(const Meta& meta, const Topology& topology) {
    std::ostringstream out;
    out << '{';
    out << "\"schema_version\":1";
    out << ",\"requested_mask\":\"" << JsonEscape(meta.requested) << '"';
    out << ",\"requested_mask_hex\":\"" << MaskText(meta.requested_mask) << '"';
    out << ",\"effective_mask\":\"" << MaskText(meta.effective_mask) << '"';
    out << ",\"system_mask\":\"" << MaskText(meta.system_mask) << '"';
    out << ",\"parent_process_mask\":\"" << MaskText(meta.process_mask) << '"';
    out << ",\"affinity_ok\":" << (meta.affinity_ok ? "true" : "false");
    out << ",\"child_started\":" << (meta.child_started ? "true" : "false");
    out << ",\"timeout\":" << (meta.timeout ? "true" : "false");
    out << ",\"exit_code\":" << meta.exit_code;
    out << ",\"wall_ns\":" << meta.wall_ns;
    out << ",\"active_logical_processors\":" << topology.active_logical_processors;
    out << ",\"processor_groups\":" << topology.processor_groups;
    out << ",\"core_count\":" << topology.cores.size();
    if (!meta.invalid_reason.empty())
        out << ",\"invalid_reason\":\"" << JsonEscape(meta.invalid_reason) << '"';
    out << '}';
    return out.str();
}

bool ParseMask(const std::wstring& text, uint64_t& mask) {
    std::wstring value = text;
    if (value.rfind(L"0x", 0) == 0 || value.rfind(L"0X", 0) == 0) value = value.substr(2);
    if (value.empty() || value.size() > 16) return false;
    try {
        size_t consumed = 0;
        mask = std::stoull(value, &consumed, 16);
        return consumed == value.size();
    } catch (...) {
        return false;
    }
}

int RunTopology() {
    const Topology topology = ReadTopology();
    std::cout << TopologyJson(topology) << '\n';
    return topology.available ? 0 : kLauncherFailureExit;
}

int RunChild(const std::wstring& mask_text, const std::filesystem::path& meta_path,
             const std::vector<std::wstring>& child_arguments) {
    const Topology topology = ReadTopology();
    Meta meta;
    meta.requested = "unrestricted";
    meta.process_mask = topology.process_mask;
    meta.system_mask = topology.system_mask;

    bool unrestricted = mask_text == L"unrestricted";
    uint64_t requested = 0;
    if (!unrestricted) {
        if (!ParseMask(mask_text, requested)) {
            meta.invalid_reason = "MASK_PARSE";
            WriteText(meta_path, MetaJson(meta, topology));
            std::cerr << "INVALID_AFFINITY_LAUNCH: mask parse\n";
            return kAffinityFailureExit;
        }
        meta.requested = MaskText(requested);
        meta.requested_mask = requested;
    }
    meta.requested_mask = requested;
    if (unrestricted) meta.requested_mask = topology.system_mask;

    if (topology.processor_groups != 1) {
        meta.invalid_reason = "MULTIPLE_PROCESSOR_GROUPS_UNSUPPORTED";
        WriteText(meta_path, MetaJson(meta, topology));
        std::cerr << "INVALID_AFFINITY_LAUNCH: processor groups\n";
        return kAffinityFailureExit;
    }
    if (!unrestricted && (requested == 0 || (requested & ~topology.system_mask) != 0)) {
        meta.invalid_reason = "MASK_NOT_WITHIN_SYSTEM";
        WriteText(meta_path, MetaJson(meta, topology));
        std::cerr << "INVALID_AFFINITY_LAUNCH: mask outside system affinity\n";
        return kAffinityFailureExit;
    }

    std::wstring command_line;
    for (size_t index = 0; index < child_arguments.size(); ++index) {
        if (index) command_line += L' ';
        command_line += QuoteArgument(child_arguments[index]);
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(child_arguments.front().c_str(), mutable_command.data(), nullptr,
                        nullptr, TRUE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &startup, &process)) {
        meta.invalid_reason = "CREATE_PROCESS";
        WriteText(meta_path, MetaJson(meta, topology));
        std::cerr << "INVALID_AFFINITY_LAUNCH: CreateProcess\n";
        return kLauncherFailureExit;
    }
    meta.child_started = true;

    const auto launch_begin = std::chrono::steady_clock::now();
    bool applied = true;
    if (!unrestricted) {
        applied = SetProcessAffinityMask(process.hProcess, static_cast<DWORD_PTR>(requested))
                  != FALSE;
    }
    uint64_t effective_process = 0;
    uint64_t effective_system = 0;
    const bool observed = GetProcessAffinityMask(process.hProcess, &effective_process,
                                                 &effective_system) != FALSE;
    meta.effective_mask = observed ? static_cast<uint64_t>(effective_process)
                                   : topology.process_mask;
    meta.system_mask = observed ? static_cast<uint64_t>(effective_system)
                                : topology.system_mask;
    const uint64_t desired = unrestricted ? meta.system_mask : requested;
    meta.affinity_ok = applied && observed && meta.effective_mask == desired &&
                       (meta.effective_mask & ~meta.system_mask) == 0;

    if (!meta.affinity_ok) {
        meta.invalid_reason = "AFFINITY_NOT_APPLIED";
        meta.exit_code = kAffinityFailureExit;
        TerminateProcess(process.hProcess, kAffinityFailureExit);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        WriteText(meta_path, MetaJson(meta, topology));
        std::cerr << "INVALID_AFFINITY_LAUNCH: affinity verification\n";
        return kAffinityFailureExit;
    }

    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, kChildTimeoutMs);
    meta.wall_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - launch_begin)
            .count());
    if (wait == WAIT_TIMEOUT) {
        meta.timeout = true;
        meta.invalid_reason = "CHILD_TIMEOUT";
        TerminateProcess(process.hProcess, kTimeoutExit);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code = kLauncherFailureExit;
    GetExitCodeProcess(process.hProcess, &exit_code);
    meta.exit_code = exit_code;
    CloseHandle(process.hProcess);
    WriteText(meta_path, MetaJson(meta, topology));
    return static_cast<int>(exit_code);
}

void PrintUsage() {
    std::cerr << "usage:\n"
              << "  affinity_launcher --topology\n"
              << "  affinity_launcher --mask <0xhex|unrestricted> --meta <path> -- <exe> [args...]\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--topology") return RunTopology();
    int index = 1;
    std::wstring mask;
    std::filesystem::path meta_path;
    bool saw_separator = false;
    std::vector<std::wstring> child;
    while (index < argc) {
        const std::wstring argument = argv[index];
        if (argument == L"--") {
            saw_separator = true;
            ++index;
            break;
        }
        if (argument == L"--mask" && index + 1 < argc) {
            mask = argv[++index];
            ++index;
            continue;
        }
        if (argument == L"--meta" && index + 1 < argc) {
            meta_path = std::filesystem::path(argv[++index]);
            ++index;
            continue;
        }
        PrintUsage();
        return kLauncherFailureExit;
    }
    if (!saw_separator) {
        PrintUsage();
        return kLauncherFailureExit;
    }
    for (; index < argc; ++index) child.emplace_back(argv[index]);
    if (mask.empty() || meta_path.empty() || child.empty()) {
        PrintUsage();
        return kLauncherFailureExit;
    }
    return RunChild(mask, meta_path, child);
}
