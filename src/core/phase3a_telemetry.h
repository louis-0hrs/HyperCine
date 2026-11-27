// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_PHASE3A_TELEMETRY_H
#define HYPERCINE_PHASE3A_TELEMETRY_H

#include "hypercine/phase3a.h"
#include <algorithm>
#include <optional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace hypercine { namespace detail {

inline void SetMeasuredVramUsage(Phase3AResult& result,
                                 std::optional<size_t> before,
                                 std::optional<size_t> after) {
    result.measured_global_vram_delta_bytes.reset();
    result.vram_used_bytes = 0;
    if (!before || !after) return;
    const size_t magnitude = *after >= *before ? *after - *before : *before - *after;
    if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return;
    const int64_t delta = static_cast<int64_t>(magnitude);
    result.measured_global_vram_delta_bytes = *after >= *before ? delta : -delta;
    if (*after > *before) result.vram_used_bytes = magnitude;
}

// Cross-trial aggregation of the signed measured deltas. A missing measurement
// (failed query or unrepresentable difference) is excluded from the denominator;
// a measured zero is a real observation and is counted; a negative global delta
// stays negative and must never be clamped or turned into an estimate.
class VramDeltaSummary {
public:
    void Add(const std::optional<int64_t>& delta) {
        if (!delta) return;
        const double value = static_cast<double>(*delta);
        if (count_ == 0) { min_ = max_ = value; }
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
        sum_ += value;
        values_.push_back(value);
        ++count_;
    }
    int Count() const { return count_; }
    double Sum() const { return sum_; }
    double Mean() const { return count_ ? sum_ / count_ : 0.0; }
    double Min() const { return count_ ? min_ : 0.0; }
    double Max() const { return count_ ? max_ : 0.0; }
    // Only successful measurements, in trial order; the caller decides how to
    // reduce them (the canonical summary publishes the median).
    const std::vector<double>& Values() const { return values_; }
private:
    int count_ = 0;
    double sum_ = 0.0, min_ = 0.0, max_ = 0.0;
    std::vector<double> values_;
};

// Local-only device identity matching between the active compute device and the
// provider's own enumeration. ``identity`` is the provider's stable local handle
// text (compared, never emitted). A missing, empty or ambiguous match returns
// nullopt so the caller reports the provider as unavailable rather than sampling
// an unrelated physical device. A duplicate identity is treated as ambiguity, not
// as "first match wins".
struct NvmlDeviceCandidate {
    unsigned int index = 0;
    std::string identity;
};

inline std::optional<unsigned int> SelectNvmlDeviceIndex(
        const std::string& active_identity,
        const std::vector<NvmlDeviceCandidate>& candidates) {
    if (active_identity.empty()) return std::nullopt;
    std::optional<unsigned int> match;
    for (const auto& candidate : candidates) {
        if (candidate.identity.empty() || candidate.identity != active_identity) continue;
        if (match) return std::nullopt;
        match = candidate.index;
    }
    return match;
}

inline void WriteVramUsageJson(std::ostream& out, const Phase3AResult& result) {
    out << "\"vram_used_bytes\": ";
    if (result.measured_global_vram_delta_bytes) out << result.vram_used_bytes;
    else out << "null";
    out << ", \"measured_global_vram_delta_bytes\": ";
    if (result.measured_global_vram_delta_bytes) out << *result.measured_global_vram_delta_bytes;
    else out << "null";
    out << ", \"vram_measurement_scope\": \"global_device_usage_delta\", "
        << "\"total_estimated_vram_bytes\": " << result.slot_selection.total_estimated_vram_bytes;
}

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
