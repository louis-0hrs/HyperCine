#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Prove the RCA-160 ledger audit can fail, one mutation per claim.

Each case copies the recorded evidence into a scratch root, applies one change to
the JSON, and requires the audit to reject it **for the stated reason**. A bare
non-zero exit would also be produced by a missing file or a JSON syntax error, so
every case asserts the message.
"""
from copy import deepcopy
import json
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

TOOL = Path(__file__).resolve().parent / "rca160_ledger_audit.py"
REPO = TOOL.parents[1]
TREE = "docs/evidence"

CASES = []


def case(name, files, mutate, expected):
    CASES.append({"name": name, "files": files, "mutate": mutate, "expected": expected})


def _all_channels(records):
    return records["sdk-rca-all-channels-2026-10-28"]


def _legacy(records):
    return records["sdk-rca-2026-10-28"]


def drop_one_call(records):
    _all_channels(records)["calls.json"].pop()


def duplicate_call_name(records):
    calls = _all_channels(records)["calls.json"]
    calls[1]["name"] = calls[0]["name"]


def wrong_arm_identity(records):
    _all_channels(records)["calls.json"][0]["cpu"] = 8


def drop_selected_channel_difference(records):
    """The labelled channel no longer shows the difference it is named for."""
    for summary in _all_channels(records)["summary.json"]:
        if summary["name"] == "half-induceU-0":
            for event in summary["comparisons"]:
                if event["ch"] == 2 and event["phase"] == 1:
                    event["first_difference"] = None


def clean_arm_still_corrupt(records):
    """A publish-all call keeps a coefficient difference, i.e. no real recovery."""
    for summary in _all_channels(records)["summary.json"]:
        if summary["name"] == "half-publishAll-0":
            summary["comparisons"][0]["first_difference"] = {
                "x": 0, "y": 0, "expected": 2700, "actual": 0,
                "coefficient": 0, "byte_offset": 0}


def serial_control_corrupt(records):
    for summary in _all_channels(records)["summary.json"]:
        if summary["name"] == "full-serial-0":
            summary["pixel_equal"] = False


def change_first_difference_value(records):
    for summary in _all_channels(records)["summary.json"]:
        for event in summary["comparisons"]:
            if (event["ch"] == 1 and event["phase"] == 0
                    and event["first_difference"]):
                event["first_difference"]["expected"] = 1064


def break_producer_channel(records):
    """A consumer fed by a publication of a different channel."""
    for summary in _all_channels(records)["summary.json"]:
        if summary["name"] != "full-induceAll-0":
            continue
        events = summary["comparisons"]
        for event in events:
            if event["phase"] == 0 and event["index"] == 1 and event["producer"] >= 0:
                other = next(e for e in events
                             if e["phase"] == 2 and e["ch"] != event["ch"])
                event["producer"] = other["event"]
                return


def break_producer_index(records):
    """A consumer fed by a publication of the wrong level, same channel."""
    for summary in _all_channels(records)["summary.json"]:
        if summary["name"] != "full-induceAll-1":
            continue
        events = summary["comparisons"]
        for event in events:
            if event["phase"] == 0 and event["index"] == 1 and event["producer"] >= 0:
                # A same-channel publication of a different level, so the case is
                # rejected for the level and not for "not a publication".
                wrong = next(e for e in events
                             if e["phase"] == 2 and e["ch"] == event["ch"]
                             and e["index"] != event["index"] + 1)
                event["producer"] = wrong["event"]
                return


def guard_violation(records):
    _all_channels(records)["calls.json"][3]["guards_ok"] = False


def wait_timeout(records):
    _all_channels(records)["calls.json"][4]["waits_ok"] = False


def wrong_packet(records):
    _all_channels(records)["plan.json"]["packet_sha256"] = "0" * 64


def pixel_digest_mismatch(records):
    """A call claiming equal pixels without carrying the serial digest."""
    summaries = _all_channels(records)["summary.json"]
    serial = next(s for s in summaries if s["name"] == "full-serial-0")
    tampered = next(s for s in summaries if s["name"] == "full-publishAll-0")
    tampered["pixel_sha256"] = "f" * 64
    assert serial["pixel_sha256"] != tampered["pixel_sha256"]


def legacy_row_count_wrong(records):
    row = _legacy(records)["summary.json"][0]
    row["differing_outputs"] = 3


def legacy_bytes_without_count(records):
    """A row carries changed bytes it does not count."""
    row = _legacy(records)["summary.json"][3]
    row["differing_outputs"] = 0


def legacy_late_producer_wrong(records):
    _legacy(records)["summary.json"][4]["late_producer"] = 7


def legacy_wait_count_wrong(records):
    _legacy(records)["summary.json"][5]["signalled_waits"] = 0


case("a call record is missing", ["sdk-rca-all-channels-2026-10-28/calls.json"],
     drop_one_call, "records for")
case("a call name is duplicated", ["sdk-rca-all-channels-2026-10-28/calls.json"],
     duplicate_call_name, "inventory does not match")
case("a call does not match its arm", ["sdk-rca-all-channels-2026-10-28/calls.json"],
     wrong_arm_identity, "does not match arm")
case("the labelled channel shows no difference",
     ["sdk-rca-all-channels-2026-10-28/summary.json"],
     drop_selected_channel_difference, "no phase-1 difference")
case("a recovered arm is still corrupt",
     ["sdk-rca-all-channels-2026-10-28/summary.json"],
     clean_arm_still_corrupt, "publish-all arm shows a coefficient difference")
case("the serial control is corrupt",
     ["sdk-rca-all-channels-2026-10-28/summary.json"],
     serial_control_corrupt, "serial control")
case("a first difference value was altered",
     ["sdk-rca-all-channels-2026-10-28/summary.json"],
     change_first_difference_value, "first difference")
case("a consumer is fed by another channel",
     ["sdk-rca-all-channels-2026-10-28/summary.json"],
     break_producer_channel, "is fed by a channel")
case("a consumer is fed by the wrong level",
     ["sdk-rca-all-channels-2026-10-28/summary.json"],
     break_producer_index, "must be")
case("an output guard was violated", ["sdk-rca-all-channels-2026-10-28/calls.json"],
     guard_violation, "output guard violated")
case("an instrumented wait did not complete",
     ["sdk-rca-all-channels-2026-10-28/calls.json"],
     wait_timeout, "did not complete")
case("the packet digest changed", ["sdk-rca-all-channels-2026-10-28/plan.json"],
     wrong_packet, "packet SHA-256")
case("an equal-pixel call lost its serial digest",
     ["sdk-rca-all-channels-2026-10-28/summary.json"],
     pixel_digest_mismatch, "does not carry the serial digest")
case("a 96-call row miscounts its differences",
     ["sdk-rca-2026-10-28/summary.json"],
     legacy_row_count_wrong, "but")
case("a 96-call row carries uncounted bytes",
     ["sdk-rca-2026-10-28/summary.json"],
     legacy_bytes_without_count, "carry bytes")
case("the 96-call late-producer total is wrong",
     ["sdk-rca-2026-10-28/summary.json"],
     legacy_late_producer_wrong, "late-producer")
case("the 96-call wait total is wrong",
     ["sdk-rca-2026-10-28/summary.json"],
     legacy_wait_count_wrong, "signalled waits")


def build_scratch(root):
    """Copy only the evidence the audit reads, preserving the relative layout."""
    for name in ("sdk-rca-all-channels-2026-10-28", "sdk-rca-2026-10-28"):
        destination = root / TREE / name
        destination.mkdir(parents=True)
        for source in sorted((REPO / TREE / name).glob("*.json")):
            shutil.copyfile(source, destination / source.name)


def load(records, root, relative):
    path = root / TREE / relative
    return json.loads(path.read_text(encoding="utf-8-sig"))


def run_tool(root):
    environment = dict(os.environ)
    environment["RCA160_EVIDENCE"] = str(root)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run([sys.executable, str(TOOL)], capture_output=True,
                            text=True, env=environment, timeout=180)
    return result.returncode, result.stdout, result.stderr


def run():
    failures = []
    root = Path(tempfile.mkdtemp(prefix="rca160-ledger-selftest-"))
    try:
        pristine = root / "pristine"
        pristine.mkdir()
        build_scratch(pristine)
        code, stdout, stderr = run_tool(pristine)
        if code != 0:
            failures.append("pristine audit failed rc=%d: %s"
                            % (code, (stderr or stdout).strip()[:400]))
            print("PRISTINE-FAILED")
        else:
            print("pristine  rc=0  (positive control accepted)")

        for index, entry in enumerate(CASES, start=1):
            case_root = root / ("mutation-%02d" % index)
            case_root.mkdir()
            build_scratch(case_root)

            records = {}
            for name in ("sdk-rca-all-channels-2026-10-28", "sdk-rca-2026-10-28"):
                records[name] = {path.name: load(records, case_root, name + "/" + path.name)
                                 for path in (case_root / TREE / name).glob("*.json")}
            before = deepcopy(records)
            entry["mutate"](records)
            if records == before:
                failures.append("%s: mutation changed nothing" % entry["name"])
                print("MUTATION-NOT-APPLIED %s" % entry["name"])
                continue
            for name, files in records.items():
                for filename, payload in files.items():
                    if payload != before[name][filename]:
                        (case_root / TREE / name / filename).write_text(
                            json.dumps(payload, indent=1) + "\n", encoding="utf-8")

            code, stdout, stderr = run_tool(case_root)
            combined = stderr + stdout
            if code == 0:
                failures.append("%s: audit still passed" % entry["name"])
                print("SURVIVED  %s" % entry["name"])
            elif entry["expected"] not in combined:
                failures.append("%s: rejected without saying %r (got %r)"
                                % (entry["name"], entry["expected"], combined.strip()[:300]))
                print("WRONG-REASON %s -> %s" % (entry["name"], combined.strip()[:170]))
            else:
                print("killed    %s" % entry["name"])

        restore = root / "restored"
        restore.mkdir()
        build_scratch(restore)
        code, stdout, stderr = run_tool(restore)
        if code != 0:
            failures.append("restored audit failed rc=%d" % code)
            print("RESTORE-FAILED")
        else:
            print("restored  rc=0  (fresh copy, audit green)")
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print()
    if failures:
        print("RCA160-LEDGER-SELFTEST: FAIL (%d)" % len(failures))
        for failure in failures:
            print("  - %s" % failure)
        return 1
    print("RCA160-LEDGER-SELFTEST: PASS (%d mutations killed, reason asserted, "
          "positive control and restore green)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(run())
