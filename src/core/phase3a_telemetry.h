// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE3A_TELEMETRY_H
#define HYPERCINE_PHASE3A_TELEMETRY_H

#include "hypercine/phase3a.h"
#include <algorithm>
#include <optional>
#include <sstream>

namespace hypercine { namespace detail {

// One optional per independently queried counter. Zero is a valid measurement.
struct NvmlSample {
    std::optional<int> gpu_util, mem_util, gclk, mclk, temp;
    std::optional<double> power_w;
    std::optional<uint64_t> throttle;
};

struct TelemetryMetric {
    int count = 0;
    double sum = 0, min = 0, max = 0, first = 0, last = 0;
    template<class T> void Add(const std::optional<T>& value) {
        if (!value) return;
        const double v = static_cast<double>(*value);
        if (count == 0) first = min = max = v;
        min = std::min(min, v);
        max = std::max(max, v);
        last = v;
        sum += v;
        ++count;
    }
    double Mean() const { return count ? sum / count : 0.0; }
};

// Constant storage: aggregate every poll; never retain a growing sample vector.
class NvmlAccumulator {
public:
    void Add(const NvmlSample& s) {
        ++attempts_;
        gpu_.Add(s.gpu_util); mem_.Add(s.mem_util);
        gclk_.Add(s.gclk); mclk_.Add(s.mclk); temp_.Add(s.temp);
        power_.Add(s.power_w);
        if (s.throttle) { ++throttle_count_; throttle_ |= *s.throttle; }
    }
    Phase3ANvmlTelemetry Finish(bool provider_available) const {
        Phase3ANvmlTelemetry out;
        out.nvml_available = provider_available;
        out.sample_count = attempts_;
        out.utilization_sample_count = gpu_.count;
        out.graphics_clock_sample_count = gclk_.count;
        out.mem_clock_sample_count = mclk_.count;
        out.temperature_sample_count = temp_.count;
        out.power_sample_count = power_.count;
        out.throttle_sample_count = throttle_count_;
        out.hardware_gpu_utilization_pct_mean = gpu_.Mean();
        out.hardware_gpu_utilization_pct_max = gpu_.max;
        out.hardware_mem_utilization_pct_mean = mem_.Mean();
        out.hardware_mem_utilization_pct_max = mem_.max;
        out.graphics_clock_mhz_mean = gclk_.Mean();
        out.graphics_clock_mhz_min = static_cast<int>(gclk_.min);
        out.graphics_clock_mhz_max = static_cast<int>(gclk_.max);
        out.mem_clock_mhz_mean = mclk_.Mean();
        out.mem_clock_mhz_min = static_cast<int>(mclk_.min);
        out.mem_clock_mhz_max = static_cast<int>(mclk_.max);
        out.temperature_c_start = static_cast<int>(temp_.first);
        out.temperature_c_end = static_cast<int>(temp_.last);
        out.temperature_c_max = static_cast<int>(temp_.max);
        out.power_w_mean = power_.Mean(); out.power_w_max = power_.max;
        out.throttle_reasons_bitmask = throttle_;
        return out;
    }
private:
    int attempts_ = 0, throttle_count_ = 0;
    uint64_t throttle_ = 0;
    TelemetryMetric gpu_, mem_, gclk_, mclk_, temp_, power_;
};

template<class T> std::string TelemetryJson(T value, int valid_count) {
    if (!valid_count) return "null";
    std::ostringstream out;
    out << value;
    return out.str();
}
inline std::string TelemetryTextJson(const std::string& value, int valid_count) {
    // Only fixed internal throttle flag names are passed here.
    return valid_count ? "\"" + value + "\"" : "null";
}

inline std::string NvmlCounterStatusJson(const Phase3ANvmlTelemetry& t) {
    std::ostringstream out;
    out << "{\"sampling_interval_ms\":" << t.sampling_interval_ms;
    const auto field = [&](const char* name, int count) {
        out << ",\"" << name << "\":{\"valid_sample_count\":" << count
            << ",\"unavailable_reason\":";
        if (count) out << "null";
        else out << (t.nvml_available ? "\"no_successful_samples\"" : "\"provider_unavailable\"");
        out << "}";
    };
    field("utilization", t.utilization_sample_count);
    field("graphics_clock", t.graphics_clock_sample_count);
    field("mem_clock", t.mem_clock_sample_count);
    field("temperature", t.temperature_sample_count);
    field("power", t.power_sample_count);
    field("throttle", t.throttle_sample_count);
    out << "}";
    return out.str();
}

}} // namespace hypercine::detail
#endif
