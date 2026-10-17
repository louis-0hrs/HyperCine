# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Opt-in v1 adapter for benchmark_codec JSON. No collection or GPU work."""
import argparse
import json
import math
import re
from pathlib import Path


def number(value, name, integer=False, minimum=0):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or (isinstance(value, float) and not math.isfinite(value)) or value < minimum
            or (integer and not isinstance(value, int))):
        raise ValueError("Invalid numeric field: " + name)
    return value


def memory_samples(snapshots):
    """Global cudaMemGetInfo snapshots, never process allocation peaks.

    Input is (stage, free_bytes, total_bytes). Negative deltas are meaningful
    when other device users release memory. No sample is a guaranteed peak.
    """
    if len(snapshots) > 256:
        raise ValueError("Too many memory snapshots")
    allowed = {"start", "context_init", "session_allocated", "before_first_frame",
               "after_first_frame", "periodic", "last_frame", "teardown", "failure"}
    samples = []
    for stage, free, total in snapshots:
        if stage not in allowed:
            raise ValueError("Unknown stage")
        number(free, "free_bytes", integer=True)
        number(total, "total_bytes", integer=True, minimum=1)
        if free > total:
            raise ValueError("Free memory exceeds total memory")
        used = total - free
        baseline = samples[0]["global_used_bytes"] if samples else used
        samples.append({"stage": stage, "free_bytes": free, "total_bytes": total,
                        "global_used_bytes": used,
                        "global_used_delta_bytes": used - baseline})
    return samples


def failure_record(api_code, symbolic_name, stage):
    """Preserve CUDA error identity; only code 2 denotes allocation failure."""
    number(api_code, "api_code", integer=True, minimum=1)
    if not re.fullmatch(r"cudaError[A-Za-z0-9]+", symbolic_name):
        raise ValueError("Invalid CUDA symbolic name")
    if stage not in {"context_init", "allocation", "h2d", "kernel", "d2h", "teardown"}:
        raise ValueError("Unknown failure stage")
    return {"api": "CUDA", "code": api_code, "symbolic_name": symbolic_name,
            "stage": stage, "is_oom": api_code == 2}


def adapt_cpu(source, index=0, operation="encode"):
    """Whitelist measurements only; do not copy arbitrary system metadata.

    Legacy success status is UNKNOWN: old benchmark JSON does not carry an
    explicit success/error contract. Legacy source SHA and warmup are unknown.
    """
    if operation not in {"encode", "decode"}:
        raise ValueError("Unknown operation")
    if not isinstance(source, dict):
        raise ValueError("Expected benchmark object")
    expected = {"pixel_format": "CFHD_PIXEL_FORMAT_BGRA",
                "encoded_format": "CFHD_ENCODED_FORMAT_YUV_422",
                "quality": "CFHD_ENCODING_QUALITY_FILM3"}
    if source.get("configuration") != expected:
        raise ValueError("Unsupported benchmark profile")
    if not isinstance(index, int) or isinstance(index, bool) or index < 0:
        raise ValueError("Invalid benchmark index")
    row = source["benchmarks"][index]
    workload = {key: number(row[key], key, integer=True, minimum=1)
                for key in ("width", "height", "frames")}
    workload.update({"operation": operation, "mode": "CpuReference",
                     "pixel_format": "BGRA8", "quality": "FilmScan3",
                     "gop": 1, "warmup_frames": None})
    perf = {key: number(row[operation][key], key) for key in
            ("fps", "megapixels_per_sec", "latency_avg_ms", "latency_min_ms", "latency_max_ms")}
    if not perf["latency_min_ms"] <= perf["latency_avg_ms"] <= perf["latency_max_ms"]:
        raise ValueError("Invalid latency ordering")
    perf.update({"latency_p50_ms": None, "latency_p95_ms": None, "latency_p99_ms": None})
    metric = row.get("peak_memory_metric", "unavailable")
    if metric not in {"windows_peak_working_set", "linux_vmhwm", "unavailable"}:
        raise ValueError("Unsupported host memory metric")
    peak = (number(row["peak_memory_bytes"], "peak_memory_bytes", integer=True)
            if metric != "unavailable" else None)
    unavailable = {
        "provenance.source_sha": "legacy_cpu_json_does_not_record_source_sha",
        "provenance.tree_dirty": "legacy_cpu_json_does_not_record_tree_state",
        "device.gpu": "cpu_only_adapter",
        "workload.warmup_frames": "legacy_cpu_json_does_not_record_warmup",
        "memory.process_gpu_peak_bytes": "no_allocator_tracking",
        "memory.system_available_bytes": "not_collected",
        "performance.latency_p50_ms": "legacy_cpu_json_has_no_frame_samples",
        "performance.latency_p95_ms": "legacy_cpu_json_has_no_frame_samples",
        "performance.latency_p99_ms": "legacy_cpu_json_has_no_frame_samples",
        "failure": "legacy_cpu_json_has_no_failure_contract",
    }
    if peak is None:
        unavailable["memory.host_peak_bytes"] = "legacy_counter_unavailable"
    return {"schema_version": 1, "status": "UNKNOWN",
            "provenance": {"adapter": "benchmark_codec", "source_sha": None, "tree_dirty": None},
            "device": {"backend": "CPU", "gpu": None}, "workload": workload,
            "memory": {"host_peak_bytes": peak, "host_peak_metric": metric,
                       "host_peak_scope": "process_lifetime_at_end_of_encode_and_decode",
                       "process_gpu_peak_bytes": None, "system_available_bytes": None},
            "performance": perf, "samples": [], "failure": None,
            "unavailable_metrics": unavailable}


class DiagnosticParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's default error includes rejected values and option names.
        # They may be paths or other private data supplied on the command line.
        self.print_usage()
        self.exit(2, "Diagnostic conversion failed: invalid arguments; use --help.\n")


def main():
    parser = DiagnosticParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--index", type=int, default=0)
    parser.add_argument("--operation", choices=("encode", "decode"), default="encode")
    args = parser.parse_args()
    try:
        if args.index < 0:
            raise ValueError("Negative benchmark index")
        result = adapt_cpu(json.loads(args.input.read_text(encoding="utf-8")), args.index, args.operation)
        # Exclusive creation protects both the source and existing reports.
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, allow_nan=False)
            stream.write("\n")
    except (ValueError, KeyError, IndexError, TypeError, OSError):
        # Do not echo raw JSON, arbitrary keys or host-specific file paths.
        parser.exit(2, "Diagnostic conversion failed: invalid input or inaccessible output.\n")


if __name__ == "__main__":
    main()
