#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Parse output from the actual Phase3A writer with synthetic NVML providers.

Every check raises explicitly. ``assert`` is not used anywhere in this file: it
disappears under ``python -O``, which would turn the whole contract into a silent
no-op. The same reason is why the checker is exercised against malformed and
incorrect writer output in :func:`negative_controls` - a verifier that only ever
sees correct input cannot prove it is still looking.
"""
import copy
import json
import subprocess
import sys


class Failure(RuntimeError):
    """Raised when the writer output or the verifier's own controls fail."""


def require(condition, message):
    if not condition:
        raise Failure(message)


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Failure("duplicate JSON field: " + key)
        result[key] = value
    return result


def parse(text):
    return json.loads(text, object_pairs_hook=unique_pairs)


MEMORY_KEYS = {
    "vram_used_bytes",
    "measured_global_vram_delta_bytes",
    "vram_measurement_scope",
    "total_estimated_vram_bytes",
}

TELEMETRY_MINIMUM_KEYS = {
    "available",
    "sample_count",
    "hardware_gpu_utilization_pct_mean",
    "graphics_clock_mhz_mean",
    "mem_clock_mhz_mean",
    "temperature_c_start",
    "temperature_c_end",
    "power_w_mean",
    "throttle_reasons_bitmask",
    "throttle_reasons_str",
    "counter_status",
}

# 50, 0, -50 and one failed query, in writer order.
EXPECTED_DELTAS = (50, 0, -50, None)
EXPECTED_LEGACY_POSITIVE = (50, 0, 0, None)
EXPECTED_ESTIMATE_BYTES = 9999


def verify_memory(memory, delta, legacy_positive, where):
    """One memory record, as emitted by both real writers."""
    missing = MEMORY_KEYS - set(memory)
    require(not missing, where + ": missing memory keys " + repr(sorted(missing)))
    require(memory["measured_global_vram_delta_bytes"] == delta,
            where + ": measured signed delta")
    require(memory["vram_used_bytes"] == legacy_positive,
            where + ": legacy compatibility value")
    require(memory["total_estimated_vram_bytes"] == EXPECTED_ESTIMATE_BYTES,
            where + ": estimate kept distinct")
    require(memory["vram_measurement_scope"] == "global_device_usage_delta",
            where + ": measurement scope")
    if delta is None:
        require(memory["vram_used_bytes"] is None,
                where + ": unavailable measurement must be null, never an estimate")
        require(memory["vram_used_bytes"] != memory["total_estimated_vram_bytes"],
                where + ": estimate must not be relabelled as measured")
    elif delta < 0:
        require(memory["vram_used_bytes"] == 0,
                where + ": negative global delta has no positive legacy part")


def verify_telemetry(live, partial, failed, missing):
    missing_keys = TELEMETRY_MINIMUM_KEYS - set(live)
    require(not missing_keys, "live telemetry: missing keys " + repr(sorted(missing_keys)))
    require(live["available"] is True, "live provider must be available")
    require(live["hardware_gpu_utilization_pct_mean"] == 0,
            "a real zero utilisation is a measurement, not unavailability")
    require(live["throttle_reasons_str"] == "None", "zero throttle mask renders as None")

    require(partial["power_w_mean"] is None, "unsupported counter stays null")
    require(partial["graphics_clock_mhz_mean"] == 1200,
            "independent counter failure must not erase the clocks")
    require(partial["counter_status"]["power"]["unavailable_reason"] == "no_successful_samples",
            "per-counter unavailability reason")
    require(failed["available"] is True, "initialized provider with failing counters")
    require(missing["available"] is False, "absent provider is not available")
    for case in (failed, missing):
        for key in ("hardware_gpu_utilization_pct_mean", "graphics_clock_mhz_mean",
                    "mem_clock_mhz_mean", "temperature_c_start", "temperature_c_end",
                    "power_w_mean", "throttle_reasons_bitmask", "throttle_reasons_str"):
            require(case[key] is None, "unavailable counter must be null: " + key)
        require(case["counter_status"]["throttle"]["unavailable_reason"] ==
                "no_successful_samples" if case is failed else "provider_unavailable",
                "counter status must distinguish no-sample from no-provider")


def verify_cases(cases):
    require(isinstance(cases, list), "writer output must be a JSON array")
    require(len(cases) == len(EXPECTED_DELTAS),
            "expected 4 writer cases, got " + str(len(cases)))
    for index, case in enumerate(cases):
        where = "case " + str(index)
        require("phase3a" in case and "canonical" in case, where + ": both writers present")
        phase3a = case["phase3a"]
        canonical = case["canonical"]
        require(phase3a["p99_latency_ms"] == 9.125, where + ": detailed p99")
        require(phase3a["p95_latency_ms"] == 7, where + ": detailed p95")
        require(canonical["latency"]["p99_ms"] == 9.125, where + ": canonical p99")
        require(canonical["telemetry"]["power_w_mean"] ==
                phase3a["nvml_telemetry"]["power_w_mean"],
                where + ": both writers must publish the same power reading")
        delta = EXPECTED_DELTAS[index]
        legacy = EXPECTED_LEGACY_POSITIVE[index]
        verify_memory(phase3a["memory_bytes"], delta, legacy, where + " detailed")
        verify_memory(canonical["memory_bytes"], delta, legacy, where + " canonical")
    live, partial, failed, missing = [case["phase3a"]["nvml_telemetry"] for case in cases]
    verify_telemetry(live, partial, failed, missing)


def expect_rejected(mutated, description, raw=False):
    """The verifier must reject this input; accepting it means the gate is dead."""
    try:
        if raw:
            parse(mutated)
        else:
            verify_cases(mutated)
    except Failure:
        return
    raise Failure("verifier accepted malformed output: " + description)


def negative_controls(cases):
    duplicate_key = ('[{"phase3a": {"memory_bytes": {"vram_used_bytes": 1, '
                     '"vram_used_bytes": 2}}}]')
    expect_rejected(duplicate_key, "duplicate JSON key", raw=True)

    dropped_case = copy.deepcopy(cases)
    dropped_case.pop()
    expect_rejected(dropped_case, "missing writer case")

    flipped = copy.deepcopy(cases)
    flipped[2]["phase3a"]["memory_bytes"]["measured_global_vram_delta_bytes"] = 50
    expect_rejected(flipped, "negative delta reported as positive")

    estimate_as_measured = copy.deepcopy(cases)
    for writer in ("phase3a", "canonical"):
        estimate_as_measured[3][writer]["memory_bytes"]["vram_used_bytes"] = \
            EXPECTED_ESTIMATE_BYTES
    expect_rejected(estimate_as_measured, "estimate substituted for a failed measurement")

    removed_field = copy.deepcopy(cases)
    removed_field[0]["phase3a"]["memory_bytes"].pop("measured_global_vram_delta_bytes")
    expect_rejected(removed_field, "removed nullable field")

    silent_provider = copy.deepcopy(cases)
    silent_provider[3]["phase3a"]["nvml_telemetry"]["available"] = True
    expect_rejected(silent_provider, "absent provider reported as available")

    zero_as_missing = copy.deepcopy(cases)
    zero_as_missing[0]["phase3a"]["nvml_telemetry"]["hardware_gpu_utilization_pct_mean"] = None
    expect_rejected(zero_as_missing, "measured zero reported as unavailable")

    erased_clocks = copy.deepcopy(cases)
    erased_clocks[1]["phase3a"]["nvml_telemetry"]["graphics_clock_mhz_mean"] = None
    expect_rejected(erased_clocks, "independent counter failure erases a valid counter")


def main():
    require(len(sys.argv) == 2, "usage: test_phase3a_telemetry_json.py <telemetry-binary>")
    output = subprocess.check_output([sys.argv[1]], text=True)
    cases = parse(output)
    verify_cases(cases)
    print("4 real-writer cases PASS: unique keys, p99, measured zero, partial failure, "
          "missing provider")
    print("8 real-writer memory records PASS: signed/zero/null deltas kept distinct from "
          "the unchanged slot estimate")
    negative_controls(cases)
    print("8 negative controls PASS: duplicate, missing, sign-flipped, estimate-as-measured, "
          "removed field, silent provider, zero-as-missing, erased counter")
    return 0


if __name__ == "__main__":
    sys.exit(main())
