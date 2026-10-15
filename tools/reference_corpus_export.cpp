// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/phase3a.h"

#include <Windows.h>
#include <bcrypt.h>
#include <cuda_runtime.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct FileRecord { std::string path; std::string sha; size_t bytes = 0; };
struct FrameRecord { std::string json; };
struct RunRecord { int width; int height; int requested; int matched; int exported; bool ok; std::string status; std::string error; };
std::vector<FileRecord> g_files;
std::vector<FrameRecord> g_frames;
std::vector<RunRecord> g_runs;
fs::path g_out;
bool g_save_input_files = true;
std::string g_run_label = "sequence";
hypercine::EncodingQuality g_quality = hypercine::EncodingQuality::FilmScan3;

std::string Escape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << static_cast<char>(c);
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if (c < 0x20) o << "?";
        else o << static_cast<char>(c);
    }
    return o.str();
}

std::string Hex(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD obj_size = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_size), sizeof(obj_size), &got, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0); return {};
    }
    std::vector<uint8_t> obj(obj_size);
    std::array<uint8_t, 32> digest{};
    if (BCryptCreateHash(alg, &hash, obj.data(), obj_size, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); return {};
    }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (uint8_t b : digest) out << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
}

std::string HashFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    return Hex(bytes.data(), bytes.size());
}

std::string DisplayDriverVersion() {
    FILE* pipe = _popen("nvidia-smi --query-gpu=driver_version --format=csv,noheader", "r");
    if (!pipe) return "unavailable";
    char buffer[128] = {};
    const bool read = std::fgets(buffer, sizeof(buffer), pipe) != nullptr;
    _pclose(pipe);
    if (!read) return "unavailable";
    std::string value(buffer);
    const size_t end = value.find_last_not_of("0123456789."); if (end != std::string::npos) value.erase(end + 1);
    return value;
}

std::string WindowsVersion() {
    OSVERSIONINFOEXW info{}; info.dwOSVersionInfoSize = sizeof(info);
    using RtlGetVersionFn = LONG (WINAPI *)(OSVERSIONINFOEXW*);
    const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!fn || fn(&info) != 0) return "unavailable";
    return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
}

bool Save(const fs::path& relative, const std::vector<uint8_t>& data, std::string* error) {
    const fs::path path = g_out / relative;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) { *error = "create directory failed: " + ec.message(); return false; }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || (!data.empty() && !f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))) {
        *error = "write failed: " + path.string(); return false;
    }
    g_files.push_back({relative.generic_string(), Hex(data.data(), data.size()), data.size()});
    return !g_files.back().sha.empty();
}

std::string Array36(const int* v) {
    std::ostringstream o; o << '[';
    for (int i=0;i<36;++i) { if (i) o << ','; o << v[i]; }
    o << ']'; return o.str();
}

bool SaveFrame(const hypercine::Phase3AReferenceFrameArtifact& a, std::string* error) {
    std::ostringstream base; base << "frames/" << g_run_label << "/frame_" << std::setw(4) << std::setfill('0') << a.frame_index;
    const fs::path dir(base.str());
    if ((g_save_input_files && !Save(dir / "input.bgra", a.input_bgra, error)) ||
        !Save(dir / "sdk.cfhd", a.upstream_cfhd, error) ||
        !Save(dir / "cuda.cfhd", a.cuda_cfhd, error) ||
        !Save(dir / "sdk.normalized.cfhd", a.normalized_upstream_cfhd, error) ||
        !Save(dir / "cuda.normalized.cfhd", a.normalized_cuda_cfhd, error)) return false;

    std::ostringstream j;
    j << "{\"frame_index\":" << a.frame_index << ",\"frame_order\":" << a.frame_index << ",\"pattern\":\"" << Escape(a.pattern_name)
      << "\",\"width\":" << a.width << ",\"height\":" << a.height << ",\"pitch\":" << a.pitch
      << ",\"input_sha256\":\"" << a.input_sha256 << "\",\"input_byte_length\":" << a.input_bgra.size()
      << ",\"sdk_sha256\":\"" << a.upstream_sha256 << "\",\"sdk_byte_length\":" << a.upstream_cfhd.size()
      << ",\"cuda_sha256\":\"" << a.cuda_sha256 << "\",\"cuda_byte_length\":" << a.cuda_cfhd.size()
      << ",\"sdk_normalized_sha256\":\"" << a.normalized_upstream_sha256
      << "\",\"cuda_normalized_sha256\":\"" << a.normalized_cuda_sha256
      << "\",\"normalized_equal\":" << (a.normalized_upstream_cfhd == a.normalized_cuda_cfhd ? "true" : "false")
      << ",\"first_unmasked_difference\":";
    if (a.first_unmasked_difference == static_cast<size_t>(-1)) j << "null"; else j << a.first_unmasked_difference;
    j << ",\"fs_rate_limiter\":" << a.fs_rate_limiter << ",\"previous_sample_bytes\":" << a.previous_sample_bytes
      << ",\"quant\":" << Array36(a.quant) << ",\"scale\":" << Array36(a.scale) << ",\"metadata_mask_ranges\":[";
    for (size_t i=0;i<a.metadata_mask_ranges.size();++i) {
        const auto& r=a.metadata_mask_ranges[i]; if(i) j << ',';
        j << "{\"tag\":\"" << Escape(r.tag_name) << "\",\"offset\":" << r.byte_offset << ",\"length\":" << r.byte_length << '}';
    }
    j << "],\"bands\":[";
    for (size_t i=0;i<a.bands.size();++i) {
        const auto& b=a.bands[i]; if(i) j << ',';
        j << "{\"channel\":" << b.channel << ",\"wavelet\":" << b.wavelet << ",\"band\":" << b.band
          << ",\"subband\":" << b.subband << ",\"width\":" << b.width << ",\"height\":" << b.height
          << ",\"quant\":" << b.quant << ",\"scale\":" << b.scale << ",\"sdk_coeff_fnv64\":\"" << b.sdk_coeff_fnv64 << "\",\"cuda_coeff_fnv64\":\"" << b.cuda_coeff_fnv64
          << "\",\"sdk_payload_sha256\":\"" << b.sdk_payload_sha256 << "\",\"cuda_payload_sha256\":\"" << b.cuda_payload_sha256 << "\"}";
    }
    j << "]}";
    g_frames.push_back({j.str()});
    return true;
}

bool Run(int width, int height, int frames) {
    const auto cb = [](const hypercine::Phase3AReferenceFrameArtifact& a, std::string* err) { return SaveFrame(a, err); };
    const auto report = hypercine::VerifyPhase3ASequence120(width, height, frames, 0x1337BEEFu, cb, g_quality);
    const std::string status = report.ok ? "complete" : (report.frames.empty() ? "unsupported" : (report.error.find("equivalence mismatch") != std::string::npos ? "mismatch" : "failed"));
    g_runs.push_back({width, height, frames, report.matched_frames, static_cast<int>(report.frames.size()), report.ok, status, report.error});
    return report.ok;
}

bool WriteManifest() {
    cudaDeviceProp prop{}; int device=0, runtime=0, driver=0;
    cudaGetDevice(&device); cudaGetDeviceProperties(&prop, device); cudaRuntimeGetVersion(&runtime); cudaDriverGetVersion(&driver);
    std::ostringstream j;
    j << "{\n\"schema_version\":1,\n\"generator\":{\"name\":\"hypercine_reference_export\",\"source_commit\":\"" << HYPERCINE_SOURCE_COMMIT
      << "\",\"working_tree_dirty\":" << HYPERCINE_SOURCE_DIRTY << ",\"pattern_generator\":\"GeneratePhase3AFrame\",\"seed\":\"0x1337BEEF\"},\n"
      << "\"source_files\":[{\"path\":\"include/hypercine/phase3a.h\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "include/hypercine/phase3a.h") << "\"},{\"path\":\"src/core/phase3a_encoder.cpp\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "src/core/phase3a_encoder.cpp") << "\"},{\"path\":\"src/cuda/hybrid_kernels.cu\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "src/cuda/hybrid_kernels.cu") << "\"},{\"path\":\"tools/reference_corpus_export.cpp\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "tools/reference_corpus_export.cpp") << "\"},{\"path\":\"src/cuda/entropy_gpu.cu\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "src/cuda/entropy_gpu.cu") << "\"},{\"path\":\"src/cuda/wavelet_cuda_optimized.cu\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "src/cuda/wavelet_cuda_optimized.cu") << "\"},{\"path\":\"CMakeLists.txt\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "CMakeLists.txt") << "\"},{\"path\":\"tests/CMakeLists.txt\",\"sha256\":\"" << HashFile(fs::path(HYPERCINE_SOURCE_ROOT) / "tests/CMakeLists.txt") << "\"}],\n"
      << "\"sdk\":{\"implementation\":\"pristine GoPro CFHD_EncodeSample\",\"upstream_frozen_ref\":\"11574d0295771edccadd17af14af74a539f924c7\"},\n"
      << "\"cuda\":{\"device\":\"" << Escape(prop.name) << "\",\"compute_capability\":\"" << prop.major << '.' << prop.minor
      << "\",\"runtime_version\":" << runtime << ",\"driver_api_version\":" << driver << ",\"display_driver_version\":\"" << Escape(DisplayDriverVersion()) << "\"},\n"
      << "\"host\":{\"os\":\"Windows\",\"os_version\":\"" << WindowsVersion() << "\",\"compiler\":\"MSVC\",\"compiler_version\":" << _MSC_VER << ",\"compiler_full_version\":" << _MSC_FULL_VER << "},\n"
      << "\"profile\":{\"input_format\":\"BGRA8\",\"encoded_format\":\"YUV422\",\"range\":\"CFHD upstream default for generated BGRA8 source values\",\"row_orientation\":\"top-down\",\"pitch_bytes\":\"width*4\",\"color_conversion\":\"Rec.709 default; YUV_601 flag not set\",\"chroma_convention\":\"CFHD YUV 4:2:2 default\",\"encoder_flags\":\"CFHD_ENCODING_FLAGS_NONE\",\"quality\":\"" << hypercine::EncodingQualityName(g_quality) << "\",\"quality_sdk_enum\":" << static_cast<int>(g_quality) << ",\"gop\":1,\"precision\":10,\"metadata\":\"SDK metadata preserved; normalization zeros only parsed GUID/DATE/TIME/TIMC value bytes\"},\n"
      << "\"runs\":[";
    for(size_t i=0;i<g_runs.size();++i) { const auto& r=g_runs[i]; if(i) j << ',';
        j << "{\"width\":"<<r.width<<",\"height\":"<<r.height<<",\"requested_frames\":"<<r.requested<<",\"matched_frames\":"<<r.matched<<",\"exported_frames\":"<<r.exported
          <<",\"status\":\""<<r.status<<"\",\"error\":\""<<Escape(r.error)<<"\"}"; }
    j << "],\n\"frames\":[";
    for(size_t i=0;i<g_frames.size();++i) { if(i) j << ','; j << g_frames[i].json; }
    j << "],\n\"artifacts\":[";
    for(size_t i=0;i<g_files.size();++i) { const auto& f=g_files[i]; if(i) j << ',';
        j << "{\"path\":\""<<Escape(f.path)<<"\",\"sha256\":\""<<f.sha<<"\",\"byte_length\":"<<f.bytes<<'}'; }
    j << "]\n}\n";
    std::ofstream out(g_out / "manifest.json", std::ios::binary | std::ios::trunc);
    out << j.str(); return out.good();
}

bool Verify(const fs::path& root) {
    std::ifstream in(root / "manifest.json", std::ios::binary);
    if (!in) { std::cerr << "manifest.json not found\n"; return false; }
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    if (!std::regex_search(text, std::regex(R"re(\"schema_version\"\s*:\s*1)re"))) { std::cerr << "unsupported schema_version\n"; return false; }
    const std::regex item(R"re(\{\s*"path"\s*:\s*"([^"]+)"\s*,\s*"sha256"\s*:\s*"([0-9a-f]{64})"\s*,\s*"byte_length"\s*:\s*([0-9]+)\s*\})re");
    size_t n=0;
    for (std::sregex_iterator it(text.begin(), text.end(), item), end; it!=end; ++it) {
        const fs::path p=root / (*it)[1].str(); std::ifstream f(p, std::ios::binary);
        if(!f) { std::cerr<<"missing: "<<p.string()<<"\n"; return false; }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        const auto expected_size=static_cast<size_t>(std::stoull((*it)[3].str()));
        const std::string actual=Hex(bytes.data(),bytes.size());
        if(bytes.size()!=expected_size || actual!=(*it)[2].str()) { std::cerr<<"hash/size mismatch: "<<p.string()<<"\n"; return false; }
        ++n;
    }
    std::cout<<"Verified "<<n<<" corpus files against SHA-256 and byte lengths.\n";
    return n>0;
}

void Usage() {
    std::cout << "hypercine_reference_export [--quality low|medium|high|filmscan1|filmscan2|filmscan3] --tier-a --out-dir DIR\n"
                 "hypercine_reference_export [--quality low|medium|high|filmscan1|filmscan2|filmscan3] --sequence WIDTH HEIGHT FRAMES --out-dir DIR\n"
                 "hypercine_reference_export --verify DIR\n";
}

bool ParseQuality(const std::string& name, hypercine::EncodingQuality* quality) {
    if (name == "low") *quality = hypercine::EncodingQuality::Low;
    else if (name == "medium") *quality = hypercine::EncodingQuality::Medium;
    else if (name == "high") *quality = hypercine::EncodingQuality::High;
    else if (name == "filmscan1") *quality = hypercine::EncodingQuality::FilmScan1;
    else if (name == "filmscan2") *quality = hypercine::EncodingQuality::FilmScan2;
    else if (name == "filmscan3") *quality = hypercine::EncodingQuality::FilmScan3;
    else return false;
    return true;
}
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--verify") return Verify(argv[2]) ? 0 : 1;
    int width=0, height=0, frames=0;
    bool tier_a=false, sequence=false;
    fs::path out_dir;
    for (int i=1; i<argc; ++i) {
        const std::string arg=argv[i];
        if (arg=="--tier-a") tier_a=true;
        else if (arg=="--sequence" && i+3<argc) { sequence=true; width=std::stoi(argv[++i]); height=std::stoi(argv[++i]); frames=std::stoi(argv[++i]); }
        else if (arg=="--quality" && i+1<argc) { if (!ParseQuality(argv[++i], &g_quality)) { std::cerr << "unsupported quality profile\n"; return 2; } }
        else if (arg=="--out-dir" && i+1<argc) out_dir=argv[++i];
        else if (arg=="--omit-input-files") g_save_input_files=false;
        else { Usage(); return 2; }
    }
    if ((tier_a==sequence) || out_dir.empty() || (sequence && (width<=0 || height<=0 || frames<=0))) { Usage(); return 2; }
    g_out=fs::absolute(out_dir); std::error_code ec; fs::create_directories(g_out,ec);
    if(ec) { std::cerr<<ec.message()<<'\n'; return 1; }
    bool ok=false;
    if(tier_a) {
        g_run_label=std::string(hypercine::EncodingQualityName(g_quality))+"-tier-a-128";
        const bool tier_a_128_ok=Run(128,128,40);
        g_run_label=std::string(hypercine::EncodingQualityName(g_quality))+"-tier-a-1080";
        const bool hd=Run(1920,1080,16);
        ok=hd;
        std::cout << "Tier A 128x128: " << (tier_a_128_ok?"PASS":(g_runs[0].status=="mismatch"?"MISMATCH (retained)":"UNSUPPORTED/FAILED")) << "; 1080p: " << (hd?"PASS":"FAILED") << '\n';
    } else {
        g_run_label=std::string(hypercine::EncodingQualityName(g_quality))+"-sequence-"+std::to_string(width)+"x"+std::to_string(height);
        ok=Run(width,height,frames);
        std::cout << "Sequence " << width << 'x' << height << " frames=" << frames << ": " << (ok?"PASS":"FAILED/MISMATCH") << '\n';
    }
    if(!WriteManifest()) { std::cerr<<"failed to write manifest\n"; return 1; }
    return ok ? 0 : 1;
}
