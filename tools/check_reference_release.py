# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Run the bounded Phase 2 reference lane and emit a fail-closed inventory.

Python is a validation-tool dependency only. No independent pixel oracle is
qualified by this runner; successful CTest execution alone cannot grant release.
"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


REQUIRED = ("test_reference_oracle", "test_phase2a", "test_phase2b",
            "test_phase2c", "test_phase2d")


def inventory(xml_text, ctest_exit):
    root = ET.fromstring(xml_text)
    records = {}
    for node in root.iter("testcase"):
        name = node.get("name")
        if name in records:
            raise ValueError("duplicate test result: " + str(name))
        if not name:
            raise ValueError("unnamed test result")
        if node.find("failure") is not None or node.find("error") is not None:
            state, code, reason = "FAIL", 1, "CTest failure"
        elif node.find("skipped") is not None:
            state, code, reason = "BLOCKED", 77, "CTest skipped; no qualification"
        elif node.get("status") != "run":
            state, code, reason = "BLOCKED", 77, "test did not run"
        else:
            state, code, reason = "PASS", 0, "CTest executed successfully"
        records[name] = dict(test=name, state=state, gate_exit=code, reason=reason)
    for name in REQUIRED:
        if name not in records:
            records[name] = dict(test=name, state="BLOCKED", gate_exit=77,
                                 reason="required test absent (including CUDA-off builds)")
    rows = sorted(records.values(), key=lambda row: row["test"])
    failed = any(row["state"] == "FAIL" for row in rows)
    blocked = any(row["state"] == "BLOCKED" for row in rows)
    # CTest may fail before producing a complete result file. Never hide that.
    code = 1 if failed or ctest_exit != 0 else 77 if blocked else 0
    return dict(schema_version=1, scope="reference-oracle lane only",
                ctest_exit=ctest_exit, gate_exit=code,
                release_gate="PASS" if code == 0 else "FAIL" if code == 1 else "BLOCKED",
                tests=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    # A fresh report prevents a failed invocation from consuming stale evidence.
    with tempfile.TemporaryDirectory(prefix="hypercine_reference_gate_") as temp:
        report = Path(temp) / "ctest.xml"
        try:
            result = subprocess.run([
                "ctest", "--test-dir", str(args.build), "-C", args.config,
                "--output-on-failure", "--no-tests=error", "-L", "^reference-oracle$",
                "--output-junit", str(report)], check=False)
            data = inventory(report.read_text(encoding="utf-8"), result.returncode)
        except (OSError, ValueError, ET.ParseError) as error:
            data = dict(schema_version=1, scope="reference-oracle lane only",
                        release_gate="FAIL", gate_exit=1,
                        reason="missing/invalid CTest evidence: " + str(error), tests=[])
    args.output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print("reference release gate:", data["release_gate"])
    return data["gate_exit"]


if __name__ == "__main__":
    raise SystemExit(main())
