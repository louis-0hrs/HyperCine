// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Deterministic GLSL -> SPIR-V embedder used at build time when
// HYPERCINE_ENABLE_VULKAN=ON.
//
// Rationale for dlopen(libshaderc) instead of linking a shader-compiler
// frontend at configure time: HyperCine must remain buildable on hosts that
// ship only the shaderc runtime library (Debian/Ubuntu package `libshaderc1`)
// without the `glslang-tools` / `spirv-tools` command-line packages and without
// root. Using the C API through dlopen keeps the SDK dependency at one small
// header-free boundary and produces byte-reproducible SPIR-V for a fixed
// shaderc version (the version string and library hash are emitted alongside).
//
// Usage:
//   vulkan_spirv_embed --out <header.h> --manifest <manifest.txt> [--shaderc <path>]
//                      <stage:path.comp> ...
//
// The generated header exposes, per shader:
//   static const uint32_t <name>_spv[]; static const unsigned <name>_spv_words;

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// --- minimal shaderc C API surface (values from shaderc/shaderc.h) ---
enum shaderc_compilation_status {
    shaderc_compilation_status_success = 0,
    shaderc_compilation_status_invalid_stage = 1,
    shaderc_compilation_status_compilation_error = 2,
};
enum shaderc_shader_kind {
    shaderc_glsl_vertex = 0, shaderc_glsl_fragment = 1, shaderc_glsl_compute = 2,
};
enum shaderc_source_language { shaderc_source_language_glsl = 0, shaderc_source_language_hlsl = 1 };
enum shaderc_target_env { shaderc_target_env_vulkan = 0 };
enum shaderc_env_version { shaderc_env_version_vulkan_1_0 = 0 };

struct shaderc_compiler;
struct shaderc_compile_options;
struct shaderc_compilation_result;

typedef void* (*fn_compiler_initialize)();
typedef void  (*fn_compiler_release)(void*);
typedef void* (*fn_options_initialize)();
typedef void  (*fn_options_release)(void*);
typedef void  (*fn_options_set_source_language)(void*, int);
typedef void  (*fn_options_set_target_env)(void*, int, unsigned);
typedef void* (*fn_compile_into_spv)(void*, const char*, size_t, int, const char*, const char*, void*);
typedef int   (*fn_result_status)(void*);
typedef size_t(*fn_result_length)(void*);
typedef const char* (*fn_result_error)(void*);
typedef const void* (*fn_result_bytes)(void*);
typedef void  (*fn_result_release)(void*);

struct ShadercApi {
    void* handle = nullptr;
    fn_compiler_initialize compiler_initialize = nullptr;
    fn_compiler_release compiler_release = nullptr;
    fn_options_initialize options_initialize = nullptr;
    fn_options_release options_release = nullptr;
    fn_options_set_source_language options_set_source_language = nullptr;
    fn_options_set_target_env options_set_target_env = nullptr;
    fn_compile_into_spv compile_into_spv = nullptr;
    fn_result_status result_status = nullptr;
    fn_result_length result_length = nullptr;
    fn_result_error result_error = nullptr;
    fn_result_bytes result_bytes = nullptr;
    fn_result_release result_release = nullptr;
};

void* open_library(const std::string& path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* resolve(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string basename_id(const std::string& path) {
    std::string s = path;
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const auto dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    for (char& c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
    }
    return s;
}

// FNV-1a 64 over the SPIR-V bytes: cheap, dependency-free stability fingerprint.
uint64_t fnv1a64(const uint8_t* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

int main(int argc, char** argv) {
    std::string out_path, manifest_path, shaderc_path;
    std::vector<std::string> shader_paths;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--manifest" && i + 1 < argc) manifest_path = argv[++i];
        else if (a == "--shaderc" && i + 1 < argc) shaderc_path = argv[++i];
        else shader_paths.push_back(a);
    }
    if (out_path.empty() || shader_paths.empty()) {
        std::fprintf(stderr, "usage: vulkan_spirv_embed --out <header> [--manifest <txt>] [--shaderc <lib>] <shader.comp>...\n");
        return 2;
    }

    try {
        if (shaderc_path.empty()) {
#if defined(_WIN32)
            shaderc_path = "shaderc_shared.dll";
#else
            const char* candidates[] = {
                "libshaderc.so.1", "libshaderc.so",
                "/usr/lib/x86_64-linux-gnu/libshaderc.so.1",
            };
            for (const char* c : candidates) {
                if (open_library(c)) { shaderc_path = c; break; }
            }
#endif
        }

        ShadercApi api;
        api.handle = open_library(shaderc_path);
        if (!api.handle) throw std::runtime_error("cannot load shaderc library: " + shaderc_path);

        auto bind = [&](const char* name) -> void* {
            void* p = resolve(api.handle, name);
            if (!p) throw std::runtime_error(std::string("shaderc missing symbol: ") + name);
            return p;
        };
        api.compiler_initialize = reinterpret_cast<fn_compiler_initialize>(bind("shaderc_compiler_initialize"));
        api.compiler_release = reinterpret_cast<fn_compiler_release>(bind("shaderc_compiler_release"));
        api.options_initialize = reinterpret_cast<fn_options_initialize>(bind("shaderc_compile_options_initialize"));
        api.options_release = reinterpret_cast<fn_options_release>(bind("shaderc_compile_options_release"));
        api.options_set_source_language = reinterpret_cast<fn_options_set_source_language>(bind("shaderc_compile_options_set_source_language"));
        api.options_set_target_env = reinterpret_cast<fn_options_set_target_env>(bind("shaderc_compile_options_set_target_env"));
        api.compile_into_spv = reinterpret_cast<fn_compile_into_spv>(bind("shaderc_compile_into_spv"));
        api.result_status = reinterpret_cast<fn_result_status>(bind("shaderc_result_get_compilation_status"));
        api.result_length = reinterpret_cast<fn_result_length>(bind("shaderc_result_get_length"));
        api.result_error = reinterpret_cast<fn_result_error>(bind("shaderc_result_get_error_message"));
        api.result_bytes = reinterpret_cast<fn_result_bytes>(bind("shaderc_result_get_bytes"));
        api.result_release = reinterpret_cast<fn_result_release>(bind("shaderc_result_release"));

        void* compiler = api.compiler_initialize();
        if (!compiler) throw std::runtime_error("shaderc_compiler_initialize failed");

        std::ostringstream header;
        header << "// Generated by vulkan_spirv_embed. DO NOT EDIT.\n"
               << "// Deterministic SPIR-V for the HyperCine Vulkan compute slice.\n"
               << "// shaderc library: " << shaderc_path << "\n"
               << "#pragma once\n#include <cstdint>\n\n";

        std::ostringstream manifest;
        manifest << "# HyperCine Vulkan shader build fingerprint\n";

        for (const std::string& path : shader_paths) {
            const std::string source = read_file(path);
            void* options = api.options_initialize();
            api.options_set_source_language(options, shaderc_source_language_glsl);
            api.options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
            void* result = api.compile_into_spv(compiler, source.data(), source.size(),
                                                shaderc_glsl_compute, path.c_str(), "main", options);
            if (!result) throw std::runtime_error("shaderc compile returned null for " + path);
            const int status = api.result_status(result);
            if (status != shaderc_compilation_status_success) {
                std::string msg = api.result_error(result) ? api.result_error(result) : "(no message)";
                api.result_release(result);
                api.options_release(options);
                throw std::runtime_error("shader compilation failed for " + path + ":\n" + msg);
            }
            const size_t n = api.result_length(result);
            const void* bytes = api.result_bytes(result);
            if (!bytes || n == 0 || (n % 4) != 0) {
                api.result_release(result);
                api.options_release(options);
                throw std::runtime_error("invalid SPIR-V size for " + path);
            }
            const auto* words = static_cast<const uint32_t*>(bytes);
            const size_t word_count = n / 4;
            const uint64_t fp = fnv1a64(static_cast<const uint8_t*>(bytes), n);
            const std::string id = basename_id(path);

            header << "// source: " << path << ", words: " << word_count
                   << ", fnv1a64: " << std::hex << fp << std::dec << "\n";
            header << "static const uint32_t " << id << "_spv[] = {\n";
            for (size_t i = 0; i < word_count; ++i) {
                if (i % 8 == 0) header << "    ";
                char buf[16];
                std::snprintf(buf, sizeof(buf), "0x%08xu", words[i]);
                header << buf;
                if (i + 1 < word_count) header << ",";
                if (i % 8 == 7 || i + 1 == word_count) header << "\n";
            }
            header << "};\n";
            header << "static const unsigned " << id << "_spv_words = "
                   << word_count << "u;\n";
            header << "static const unsigned long long " << id << "_spv_fnv1a64 = "
                   << "0x" << std::hex << fp << std::dec << "ull;\n\n";

            char fp_hex[32];
            std::snprintf(fp_hex, sizeof(fp_hex), "%016llx", static_cast<unsigned long long>(fp));
            manifest << "shader " << id << " source " << path
                     << " words " << word_count
                     << " fnv1a64 " << fp_hex << "\n";

            api.result_release(result);
            api.options_release(options);
        }

        api.compiler_release(compiler);

        std::ofstream out(out_path, std::ios::binary);
        if (!out) throw std::runtime_error("cannot write " + out_path);
        out << header.str();
        out.close();

        if (!manifest_path.empty()) {
            std::ofstream mf(manifest_path, std::ios::binary);
            if (!mf) throw std::runtime_error("cannot write " + manifest_path);
            mf << manifest.str();
        }
        std::fprintf(stdout, "vulkan_spirv_embed: wrote %s (%zu shaders)\n", out_path.c_str(), shader_paths.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vulkan_spirv_embed: %s\n", e.what());
        return 1;
    }
}
