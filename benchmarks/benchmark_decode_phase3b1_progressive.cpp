// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../tests/decoder_support.h"
#include <fstream>
using namespace ProductionDecodeTest;
int AcStatus() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS power{}; Require(GetSystemPowerStatus(&power) != 0, "read AC power status");
    Require(power.ACLineStatus == 1, "same-session benchmark requires AC power"); return int(power.ACLineStatus);
#else
    return -1;
#endif
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "output JSON path required");
        std::ofstream out(argv[1]); out << "{\"license\":\"MIT OR Apache-2.0\",\"runs\":["; bool comma = false;
        for (auto geometry : {std::pair<int,int>{1920,1080}, {3840,2160}}) {
            auto samples = Generate(geometry.first, geometry.second, 3);
            DecoderConfig cfg; cfg.maximum_width = geometry.first; cfg.maximum_height = geometry.second; cfg.slot_count = 2;
            Decoder d(cfg); Require(d.Initialize() == DecodeError::None, "progressive benchmark init");
            const auto memory = d.MemoryStats();
            for (int p : {0, 2}) for (auto strategy : {DiagnosticDecodeStrategy::Lightweight, DiagnosticDecodeStrategy::Hybrid, DiagnosticDecodeStrategy::Automatic}) {
                DecodeRequest req; req.diagnostic_strategy = strategy; auto& s = samples[p];
                for (int r = 0; r < 20; ++r) { auto t = Begin(d, s, req); d.ReleaseOutput(t); }
                for (int repeat = 0; repeat < 3; ++repeat) {
                    const int ac_before = AcStatus();
                    double total = 0, minimum = 1e9, maximum = 0; constexpr int n = 100;
                    for (int r = 0; r < n; ++r) {
                        const auto start = Clock::now(); auto t = Begin(d, s, req);
                        const double ms = std::chrono::duration<double,std::milli>(Clock::now() - start).count(); d.ReleaseOutput(t);
                        total += ms; minimum = (std::min)(minimum, ms); maximum = (std::max)(maximum, ms);
                    }
                    const int ac_after = AcStatus();
                    if (comma) out << ','; comma = true;
                    out << "{\"width\":" << s.width << ",\"height\":" << s.height << ",\"pattern\":\"" << (p ? "noise" : "flat")
                        << "\",\"strategy\":" << int(strategy) << ",\"repeat\":" << repeat << ",\"sample_hash\":\"" << Hash(s.compressed.data(), s.compressed.size())
                        << "\",\"avg_ms\":" << total / n << ",\"min_ms\":" << minimum << ",\"max_ms\":" << maximum << ",\"fps\":" << n * 1000.0 / total
                        << ",\"megapixels_per_second\":" << double(s.width) * s.height * n / (total * 1000)
                        << ",\"peak_working_set_bytes\":" << WorkingSet(true) << ",\"device_bytes\":" << memory.device_bytes << ",\"pinned_bytes\":" << memory.pinned_host_bytes
                        << ",\"pageable_bytes\":" << memory.pageable_capacity_bytes << ",\"cuda_allocations\":" << memory.cuda_allocation_calls
                        << ",\"ac_line_status_before\":" << ac_before << ",\"ac_line_status_after\":" << ac_after << '}';
                    std::cout << s.width << ' ' << (p ? "noise" : "flat") << ' ' << int(strategy) << ' ' << repeat << ' ' << total / n << " ms\n" << std::flush;
                }
            }
        }
        out << "]}\n"; Require(bool(out), "benchmark evidence write"); return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
