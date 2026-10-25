# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Run the bounded Phase 2 reference lane and emit a fail-closed inventory.

Python is a validation-tool dependency only. No independent pixel oracle is
qualified by this runner; successful CTest execution alone cannot grant release.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET


REQUIRED = ("test_reference_oracle", "test_phase2a", "test_phase2b",
            "test_phase2c", "test_phase2d")
# Only these tests gate pixel quality through the original SDK. They must be
# registered with SKIP_RETURN_CODE=77 so a blocked gate is never reported as a
# passing test; test_reference_oracle is SDK-free and only needs the lane label.
PHASE2 = ("test_phase2a", "test_phase2b", "test_phase2c", "test_phase2d")
LANE_LABEL = "reference-oracle"
ORACLE_SKIP = "SKIP_RETURN_CODE=77"

# The allocator's documented capacity diagnostic (include/hypercine/entropy_gpu.h).
# Only this exact contract may relabel a strict failure as device-capacity-bound;
# the row stays FAIL so a limited device is never reported as a passing gate.
CAPACITY_BOUND = re.compile(
    r"UNSUPPORTED_RESOURCE_BOUND: [^\n]*? allocation failed: "
    r"requested (\d+) B; "
    r"(?:free (\d+) B of (\d+) B|device memory query failed); "
    r"CUDA (cudaError[A-Za-z]+) \(")


def _as_labels(value):
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    return [str(item) for item in value]


def _as_skip_code(value):
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1  # Malformed property: never treat it as the 77 contract.


def load_registration(json_text):
    """Parse `ctest --show-only=json-v1` registration for this exact build."""
    root = json.loads(json_text)
    declared = {}
    for node in root.get("tests", []):
        name = node.get("name")
        if not name:
            raise ValueError("unnamed registered test")
        if name in declared:
            raise ValueError("duplicate registered test: " + name)
        properties = {prop.get("name"): prop.get("value")
                      for prop in node.get("properties", [])}
        declared[name] = dict(labels=_as_labels(properties.get("LABELS")),
                              skip_return_code=_as_skip_code(
                                  properties.get("SKIP_RETURN_CODE")))
    return declared


def _skip_text(node):
    return " ".join(part for part in ((node.get("message") or ""),
                                      (node.text or "").strip(),
                                      (node.get("value") or "")) if part)


def _evidence_text(node):
    """Collect a testcase's failure and captured output text."""
    parts = []
    for child in node:
        if child.tag in ("failure", "error", "system-out", "system-err"):
            parts.append(_skip_text(child))
    return "\n".join(part for part in parts if part)


# HyperCine's tests fail through `Fail()`: `FAIL: <reason>` on stderr, then
# exit 1. The diagnostic that ended the run is therefore the last such line;
# everything printed before it was handled and did not terminate the test.
FATAL_LINE = re.compile(r'^FAIL: ', re.MULTILINE)


def fatal_diagnostic(text):
    """Return the text of the diagnostic that actually terminated the test.

    Returns None when the output carries no `FAIL:` line. Callers then fall
    back to the whole text, because a run whose failure is reported through a
    different channel must not silently lose its classification.
    """
    starts = [match.start() for match in FATAL_LINE.finditer(text)]
    if not starts:
        return None
    start = starts[-1]
    end = text.find('\n', start)
    return text[start:] if end < 0 else text[start:end]


def capacity_bound(text):
    """Read the exact allocator capacity contract from the fatal diagnostic.

    The marker is bound to the line that ended the run, not to the whole
    captured output. A test may print a capacity message and then survive it:
    on a device where a smaller configuration fits, that message is context,
    and a later unrelated failure (a coefficient mismatch, say) must not be
    relabelled as device-capacity-bound. Searching the whole output made those
    two cases indistinguishable.

    A malformed or partial message stays an ordinary strict failure, so an
    unverified device-capacity claim cannot be manufactured from test output.
    """
    fatal = fatal_diagnostic(text)
    match = CAPACITY_BOUND.search(text if fatal is None else fatal)
    if match is None:
        return None
    requested, free_bytes, total_bytes, status = match.groups()
    return dict(requested_bytes=int(requested),
                free_bytes=None if free_bytes is None else int(free_bytes),
                total_bytes=None if total_bytes is None else int(total_bytes),
                cuda_status=status)


def _registration_problem(name, entry):
    if LANE_LABEL not in entry["labels"]:
        return "registered without the " + LANE_LABEL + " label"
    if name in PHASE2 and entry["skip_return_code"] != 77:
        return "registered without SKIP_RETURN_CODE=77"
    return None


def _oracle_state():
    return dict(state="BLOCKED", qualified=False,
                reason="no reviewed independent pixel-oracle adapter implemented")


def error_inventory(reason, ctest_exit=None):
    """Emit the full schema-3 key set when no trustworthy evidence exists.

    Consumers must see the same shape as a completed run, with empty counts and
    incomplete coverage, rather than a truncated document with missing keys.
    """
    return dict(schema_version=3, scope="reference-oracle lane only",
                ctest_exit=ctest_exit, gate_exit=1, reference_lane="FAIL",
                independent_pixel_oracle=_oracle_state(),
                release_gate="FAIL",
                counts={state: 0 for state in ("PASS", "FAIL", "BLOCKED")},
                coverage=dict(required=len(REQUIRED), required_registered=[],
                              lane_registered=[], results=[], unregistered=[],
                              registration_missing=[], registration_mismatch=[],
                              complete=False),
                reason=reason, tests=[])


def inventory(xml_text, ctest_exit, registration):
    """Classify one JUnit run against the registration of the same build."""
    root = ET.fromstring(xml_text)
    results = {}
    for node in root.iter("testcase"):
        name = node.get("name")
        if not name:
            raise ValueError("unnamed test result")
        if name in results:
            raise ValueError("duplicate test result: " + name)
        results[name] = node

    rows = []
    extra = sorted((set(registration) | set(results)) - set(REQUIRED))
    for name in list(REQUIRED) + extra:
        required = name in REQUIRED
        entry = registration.get(name)
        if entry is None:
            if required:
                rows.append(dict(test=name, required=True, state="BLOCKED", gate_exit=77,
                                 classification="REGISTRATION_MISSING",
                                 reason="required test is not registered on this build"))
            else:
                rows.append(dict(test=name, required=False, state="FAIL", gate_exit=1,
                                 classification="UNREGISTERED_RESULT",
                                 reason="result has no registration on this build"))
            continue
        problem = _registration_problem(name, entry)
        if problem is not None:
            rows.append(dict(test=name, required=required, state="BLOCKED", gate_exit=77,
                             classification="REGISTRATION_MISMATCH",
                             reason=problem + "; execution is not coverage"))
            continue
        node = results.get(name)
        if node is None:
            rows.append(dict(test=name, required=required, state="BLOCKED", gate_exit=77,
                             classification="NOT_RUN",
                             reason="registered with the lane label but no result was produced"))
            continue
        if node.find("failure") is not None or node.find("error") is not None:
            capacity = capacity_bound(_evidence_text(node))
            if capacity is None:
                rows.append(dict(test=name, required=required, state="FAIL",
                                 gate_exit=1, classification="STRICT_FAILURE",
                                 reason="CTest failure"))
            else:
                rows.append(dict(test=name, required=required, state="FAIL",
                                 gate_exit=1,
                                 classification="STRICT_FAILURE_CAPACITY_BOUND",
                                 capacity=capacity,
                                 reason="device capacity exhausted before the strict "
                                        "gate could run; still a hard failure"))
        elif node.find("skipped") is not None:
            skipped = node.find("skipped")
            if ORACLE_SKIP in _skip_text(skipped):
                classification = "REFERENCE_ORACLE_BLOCKED"
                reason = "original-SDK pixel gate blocked via " + ORACLE_SKIP
            else:
                classification = "UNEXPECTED_SKIP"
                reason = "skipped for an unrecognized reason; not oracle evidence"
            rows.append(dict(test=name, required=required, state="BLOCKED", gate_exit=77,
                             classification=classification, reason=reason))
        elif node.get("status") != "run":
            rows.append(dict(test=name, required=required, state="BLOCKED", gate_exit=77,
                             classification="NOT_RUN",
                             reason="test did not run"))
        else:
            # Executed successfully, but that is provenance, not quality proof.
            rows.append(dict(test=name, required=required, state="PASS", gate_exit=0,
                             classification="PASS_EXECUTION_ONLY",
                             reason="CTest executed successfully"))

    rows.sort(key=lambda row: row["test"])
    unregistered = sorted(set(results) - set(registration))
    failed = any(row["state"] == "FAIL" for row in rows)
    blocked = any(row["state"] == "BLOCKED" for row in rows)
    missing = [row["test"] for row in rows
               if row["classification"] == "REGISTRATION_MISSING"]
    mismatched = [row["test"] for row in rows
                  if row["classification"] in ("REGISTRATION_MISMATCH",
                                                "UNREGISTERED_RESULT")]
    complete = not missing and not mismatched and set(results) == set(registration)
    # CTest may fail before producing a complete result file. Never hide that.
    lane_code = 1 if failed or ctest_exit != 0 else 77 if blocked or not complete else 0
    # There is currently no reviewed independent pixel-oracle adapter. Passing
    # test processes is execution evidence, not positive oracle qualification.
    # Keep this closed until an actual reviewed implementation supplies bound
    # same-packet quality evidence; do not add a CLI flag or trust a manifest
    # that merely asserts qualification.
    oracle = _oracle_state()
    code = 1 if lane_code == 1 else 77
    counts = {state: sum(row["state"] == state for row in rows)
              for state in ("PASS", "FAIL", "BLOCKED")}
    coverage = dict(required=len(REQUIRED),
                    required_registered=[name for name in REQUIRED if name in registration],
                    lane_registered=sorted(registration), results=sorted(results),
                    unregistered=unregistered, registration_missing=missing,
                    registration_mismatch=mismatched, complete=complete)
    return dict(schema_version=3, scope="reference-oracle lane only",
                ctest_exit=ctest_exit, gate_exit=code,
                reference_lane="PASS" if lane_code == 0 else "FAIL" if lane_code == 1 else "BLOCKED",
                independent_pixel_oracle=oracle,
                release_gate="PASS" if code == 0 else "FAIL" if code == 1 else "BLOCKED",
                counts=counts, coverage=coverage, tests=rows)


def _run(command):
    return subprocess.run(command, check=False, capture_output=True, text=True)


def _diagnostic(result, limit=2000):
    """Return CTest's exit code plus both streams; failures often land on stdout."""
    parts = ["exit " + str(result.returncode)]
    for stream in ("stdout", "stderr"):
        text = (getattr(result, stream) or "").strip()
        if text:
            parts.append(stream + ": " + text[-limit:])
    return "; ".join(parts)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--output", required=True, type=Path)
    # Use the CTest that configured this build; a different PATH CTest could
    # change registration or JUnit behavior on another host.
    parser.add_argument("--ctest", default=os.environ.get("CTEST_COMMAND") or "ctest")
    args = parser.parse_args(argv)
    lane = ["-L", "^" + LANE_LABEL + "$"]
    ctest_exit = None
    # A fresh report prevents a failed invocation from consuming stale evidence.
    with tempfile.TemporaryDirectory(prefix="hypercine_reference_gate_") as temp:
        report = Path(temp) / "ctest.xml"
        try:
            shown = _run([args.ctest, "--test-dir", str(args.build), "-C", args.config]
                         + lane + ["--show-only=json-v1"])
            if shown.returncode != 0:
                raise ValueError("ctest --show-only failed; " + _diagnostic(shown))
            registration = load_registration(shown.stdout)
            result = _run([args.ctest, "--test-dir", str(args.build), "-C", args.config,
                           "--output-on-failure", "--no-tests=error"] + lane
                          + ["--output-junit", str(report)])
            ctest_exit = result.returncode
            try:
                xml_text = report.read_text(encoding="utf-8")
            except OSError:
                raise ValueError("ctest produced no JUnit report; "
                                 + _diagnostic(result))
            data = inventory(xml_text, result.returncode, registration)
        except OSError as error:
            data = error_inventory("failed to run " + args.ctest + ": " + str(error),
                                   ctest_exit)
        except (ValueError, json.JSONDecodeError, ET.ParseError) as error:
            data = error_inventory("missing/invalid CTest evidence: " + str(error),
                                   ctest_exit)
    args.output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print("reference release gate:", data["release_gate"])
    return data["gate_exit"]


if __name__ == "__main__":
    raise SystemExit(main())
