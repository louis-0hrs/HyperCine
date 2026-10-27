# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "diagnostic_summary.py"
SPEC = importlib.util.spec_from_file_location("diagnostic_summary", SCRIPT)
summary = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summary)


def fixture():
    perf = dict(fps=40, megapixels_per_sec=82.944, latency_avg_ms=25,
                latency_min_ms=20, latency_max_ms=30)
    return {"system": {"host": "SYNTHETIC_PRIVATE_MARKER"},
        "configuration": {"pixel_format": "CFHD_PIXEL_FORMAT_BGRA",
                          "encoded_format": "CFHD_ENCODED_FORMAT_YUV_422",
                          "quality": "CFHD_ENCODING_QUALITY_FILM3"}, "benchmarks": [
        dict(width=1920, height=1080, frames=50, encode=perf, decode=copy.copy(perf),
             peak_memory_bytes=4096, peak_memory_metric="windows_peak_working_set")]}


class DiagnosticsTest(unittest.TestCase):
    def test_cpu_contract(self):
        result = summary.adapt_cpu(fixture())
        self.assertEqual(set(result), {"schema_version", "status", "provenance", "device",
                         "workload", "memory", "performance", "samples", "failure", "unavailable_metrics"})
        self.assertEqual(result["status"], "UNKNOWN")
        self.assertEqual(result["memory"]["host_peak_bytes"], 4096)
        self.assertEqual(result["performance"]["fps"], 40)
        self.assertIsNone(result["device"]["gpu"])
        self.assertNotIn("SYNTHETIC_PRIVATE_MARKER", json.dumps(result))

    def test_unknown_counters_are_null(self):
        data = fixture()
        data["benchmarks"][0]["peak_memory_metric"] = "unavailable"
        result = summary.adapt_cpu(data)
        self.assertIsNone(result["memory"]["host_peak_bytes"])
        self.assertIn("memory.host_peak_bytes", result["unavailable_metrics"])
        self.assertIsNone(result["performance"]["latency_p99_ms"])

    def test_linux_metric(self):
        data = fixture()
        data["benchmarks"][0]["peak_memory_metric"] = "linux_vmhwm"
        self.assertEqual(summary.adapt_cpu(data)["memory"]["host_peak_metric"], "linux_vmhwm")

    def test_decode_selection(self):
        data = fixture()
        data["benchmarks"][0]["decode"]["fps"] = 60
        self.assertEqual(summary.adapt_cpu(data, operation="decode")["performance"]["fps"], 60)

    def test_invalid_numbers(self):
        for bad in (-1, float("nan"), float("inf"), True, "1"):
            data = fixture()
            data["benchmarks"][0]["encode"]["fps"] = bad
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                summary.adapt_cpu(data)

    def test_reject_unsupported_profile_and_latency_order(self):
        with self.assertRaises(ValueError):
            summary.adapt_cpu([])
        data = fixture()
        data["configuration"]["quality"] = "UNKNOWN"
        with self.assertRaises(ValueError):
            summary.adapt_cpu(data)
        data = fixture()
        data["benchmarks"][0]["encode"]["latency_avg_ms"] = 100
        with self.assertRaises(ValueError):
            summary.adapt_cpu(data)

    def test_snapshots_preserve_negative_delta_and_teardown(self):
        samples = summary.memory_samples([("start", 800, 1000), ("session_allocated", 600, 1000),
                                          ("periodic", 900, 1000), ("teardown", 850, 1000)])
        self.assertEqual([s["global_used_delta_bytes"] for s in samples], [0, 200, -100, -50])
        self.assertEqual(samples[-1]["stage"], "teardown")
        self.assertNotIn("peak_bytes", samples[1])

    def test_snapshot_validation(self):
        for snapshots in ([("start", 1001, 1000)], [("start", -1, 1000)],
                          [("arbitrary", 0, 1000)], [("start", 0, 1000)] * 257):
            with self.subTest(snapshots=snapshots[:1]), self.assertRaises(ValueError):
                summary.memory_samples(snapshots)
        self.assertEqual(summary.memory_samples([]), [])

    def test_non_oom_error_preserved(self):
        failure = summary.failure_record(209, "cudaErrorNoKernelImageForDevice", "kernel")
        self.assertEqual(failure["code"], 209)
        self.assertFalse(failure["is_oom"])
        self.assertEqual(failure["symbolic_name"], "cudaErrorNoKernelImageForDevice")
        self.assertTrue(summary.failure_record(2, "cudaErrorMemoryAllocation", "allocation")["is_oom"])

    def test_cli_argument_errors_do_not_echo_values(self):
        marker = "SYNTHETIC_PRIVATE_ARGUMENT"
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input.json"
            target = Path(directory) / "output.json"
            source.write_text(json.dumps(fixture()), encoding="utf-8")
            base = [sys.executable, str(SCRIPT), str(source), str(target)]
            for arguments in (("--index", marker), ("--operation", marker),
                              ("--" + marker,), (marker,)):
                with self.subTest(arguments=arguments):
                    result = subprocess.run(base + list(arguments), capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2)
                    self.assertNotIn(marker, result.stdout + result.stderr)
                    self.assertNotIn(directory, result.stdout + result.stderr)
                    self.assertFalse(target.exists())

    def test_cli_exclusive_output_and_safe_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / "input.json", Path(directory) / "output.json"
            source.write_text(json.dumps(fixture()), encoding="utf-8")
            command = [sys.executable, str(SCRIPT), str(source), str(target)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            before = target.read_bytes()
            self.assertEqual(json.loads(before)["schema_version"], 1)
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn(directory, result.stderr)
            self.assertEqual(target.read_bytes(), before)
            self.assertEqual(subprocess.run(command + ["--index", "-1"], capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()
