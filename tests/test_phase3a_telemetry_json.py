#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Parse output from the actual Phase3A writer with synthetic NVML providers."""
import json
import subprocess
import sys


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise AssertionError("duplicate JSON field: " + key)
        result[key] = value
    return result


cases = json.loads(subprocess.check_output([sys.argv[1]], text=True),
                   object_pairs_hook=unique_pairs)
assert len(cases) == 4
for case in cases:
    assert case["phase3a"]["p99_latency_ms"] == 9.125
    assert case["phase3a"]["p95_latency_ms"] == 7
    assert case["canonical"]["latency"]["p99_ms"] == 9.125
    assert case["canonical"]["telemetry"]["power_w_mean"] == case["phase3a"]["nvml_telemetry"]["power_w_mean"]
live, partial, failed, missing = [c["phase3a"]["nvml_telemetry"] for c in cases]
assert live["hardware_gpu_utilization_pct_mean"] == 0
assert live["throttle_reasons_str"] == "None"
assert partial["power_w_mean"] is None
assert partial["graphics_clock_mhz_mean"] == 1200
assert partial["counter_status"]["power"]["unavailable_reason"] == "no_successful_samples"
assert failed["available"] is True
assert missing["available"] is False
for case in (failed, missing):
    for key in ("hardware_gpu_utilization_pct_mean", "graphics_clock_mhz_mean",
                "mem_clock_mhz_mean", "temperature_c_start", "temperature_c_end",
                "power_w_mean", "throttle_reasons_bitmask", "throttle_reasons_str"):
        assert case[key] is None, key
assert missing["counter_status"]["power"]["unavailable_reason"] == "provider_unavailable"
print("8 real-writer JSON cases PASS; unique keys, p99, valid zero, partial failure, missing provider")
