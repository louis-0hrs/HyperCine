#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Validate a real successful inputs-only reference report without echoing its contents."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

COMPILER = re.compile(r"(?:MSVC|GNU|AppleClang|Clang|IntelLLVM|Intel|NVHPC) [0-9]{1,6}(?:[.][0-9]{1,6}){0,3}\Z")
CMAKE = re.compile(r"cmake version [0-9]{1,6}(?:[.][0-9]{1,6}){1,3}\Z")
HEX = re.compile(r"[0-9a-f]{64}\Z")
EXPECTED_COMMAND = [
    "python3", "tests/validate_windows_reference.py",
    "--probe", "${HYPERCINE_PROBE}", "--inputs-only",
    "--output", "${HYPERCINE_REPORT}"
]


def require(condition):
    if not condition:
        raise ValueError("invalid report artifact")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check_report(report, raw, report_path, probe_path, root):
    require(isinstance(report, dict) and type(report.get("schema_version")) is int
            and report["schema_version"] == 1)
    require("command" not in report and report.get("command_template") == EXPECTED_COMMAND)
    # Raw executable/destination paths cannot be placed in public evidence.
    for name in (str(report_path), str(probe_path)):
        require(bool(name) and name.encode("utf-8") not in raw)
    label = report.get("compiler")
    require(type(label) is str and (label == "NOT_RECORDED" or COMPILER.fullmatch(label)))
    require(type(report.get("cmake")) is str and CMAKE.fullmatch(report["cmake"]))
    require(probe_path.is_file() and type(report.get("probe_sha256")) is str)
    require(HEX.fullmatch(report["probe_sha256"]))
    require(digest(probe_path.read_bytes()) == report["probe_sha256"])
    sources = report.get("source_files")
    require(isinstance(sources, list) and len(sources) >= 5)
    for item in sources:
        require(type(item) is dict and type(item.get("path")) is str and
                type(item.get("sha256")) is str and HEX.fullmatch(item["sha256"]))
        rel = Path(item["path"])
        require(not rel.is_absolute() and ".." not in rel.parts)
        file = (root / rel).resolve()
        require(root.resolve() in file.parents and file.is_file())
        require(digest(file.read_bytes()) == item["sha256"])
    runs = report.get("runs")
    require(isinstance(runs, list) and len(runs) == 2)
    for run in runs:
        require(isinstance(run, dict) and run.get("input_gate") == "PASS")
        rows = run.get("frames")
        require(isinstance(rows, list) and bool(rows))
        require(type(run.get("input_frames_verified")) is int
                and run["input_frames_verified"] == len(rows))
        require(all(isinstance(row, dict) and row.get("input") == "PASS" for row in rows))
    require(report.get("production_gate") == "BLOCKED")


def negative_controls(report, raw, report_path, probe_path, root):
    # Known false-PASS mutations must all be rejected by the same real validator.
    candidates = []
    for field, value in (
        ("command", ["python3", "SECRET_EXAMPLE_RAW_LOCAL_COMMAND"]),
        ("compiler", "GNU 14.2.0 (/synthetic/SECRET_EXAMPLE_COMPILER_PATH)"),
        ("cmake", "cmake version 3.31.6 SECRET_EXAMPLE_PATH"),
        ("probe_sha256", "0" * 64),
        ("command_template", ["python3", "/synthetic/SECRET_EXAMPLE_COMMAND_PATH"])
    ):
        mutant = dict(report)
        mutant[field] = value
        candidates.append(mutant)
    mutant = dict(report)
    mutant["runs"] = []
    candidates.append(mutant)
    for item in candidates:
        try:
            check_report(item, json.dumps(item).encode("utf-8"), report_path, probe_path, root)
        except (ValueError, KeyError, TypeError, OSError):
            continue
        raise ValueError("negative control accepted")
    return len(candidates)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()
    raw = args.report.read_bytes()
    require(len(raw) <= 2 * 1024 * 1024)
    report = json.loads(raw)
    root = Path(__file__).resolve().parents[1]
    check_report(report, raw, args.report, args.probe, root)
    count = negative_controls(report, raw, args.report, args.probe, root)
    print("Real reference report and %d synthetic refusal controls PASS" % count)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, TypeError, json.JSONDecodeError):
        print("FAIL: public report artifact validation rejected", file=sys.stderr)
        sys.exit(1)
