#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Re-derive the RCA-160 all-channel ledger from its raw records.

This is the ledger half of the independent Linux acceptance. It does not call
the submission's own verifier and it does not replay the decoder; it recomputes
every published count from the per-call records and refuses to report a pass for
a ledger it cannot close.

What is recomputed rather than trusted:

* arm membership and the FULL/HALF x seven-arm inventory;
* the per-channel corruption matrix, per call and per arm;
* the recovery claim (publish-all and reinduce arms), per channel;
* the producer-identity witness that ties each consumer input to its own
  same-channel publisher;
* the API/guard/wait outcome of every call.

What is *not* claimed here: a decoder replay, a production rate, or any
statement about a packet this ledger does not contain. The recorded ledgers were
produced on another host under instrumentation that perturbs scheduling.
"""
import argparse
import json
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[1]
ROOT = Path(__import__("os").environ.get("RCA160_EVIDENCE", REPO)).resolve()

ALL_CHANNELS = ROOT / "docs/evidence/sdk-rca-all-channels-2026-10-28"
LEGACY = ROOT / "docs/evidence/sdk-rca-2026-10-28"

PACKET_SHA = "fc046231223e537bb221524b2d6f9c0d9f24a71ea23f79deb140f44f7bd063c8"
PACKET_SIZE = 900396

# (name, cpu_max, mode, induce-mask); mask bits are 1=Y, 2=V, 4=U.
ARMS = (("serial", 1, 0, 0), ("induceY", 8, 1, 1), ("induceV", 8, 1, 2),
        ("induceU", 8, 1, 4), ("induceAll", 8, 1, 7), ("publishAll", 8, 2, 7),
        ("reinduceAll", 8, 1, 7))
RESOLUTIONS = ("full", "half")
REPETITIONS = 2
CHANNELS = (0, 1, 2)

# Phases as recorded by the probe: 0 = consumer input snapshot, 1 = consumer
# output snapshot, 2 = publication snapshot.
PHASE_INPUT, PHASE_OUTPUT, PHASE_PUBLISH = 0, 1, 2

# The published first-difference values, by channel, for the phase-0 and
# phase-1 snapshots. Recomputed from the records, then compared with these.
EXPECTED_FIRST_INPUT = {0: (2700, 0), 1: (1063, 0), 2: (709, 0)}
EXPECTED_FIRST_OUTPUT = {0: (2696, -10), 1: (1066, 6), 2: (708, 0)}
IDENTITY_INPUT_SHAPE = {0: (480, 270), 1: (240, 270), 2: (240, 270)}


class ContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def read_json(path):
    text = path.read_text(encoding="utf-8-sig")
    return json.loads(text, object_pairs_hook=unique_object)


def unique_object(pairs):
    out = {}
    for key, value in pairs:
        require(key not in out, "duplicate JSON key %r in %s" % (key, "a record"))
        out[key] = value
    return out


def sha(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


# --------------------------------------------------------------------------
# inventory
# --------------------------------------------------------------------------

def expected_names():
    return {"%s-%s-%d" % (resolution, arm[0], repetition)
            for resolution in RESOLUTIONS for arm in ARMS
            for repetition in range(REPETITIONS)}


def check_inventory(plan, calls, summaries, report):
    require(plan["packet_sha256"] == PACKET_SHA,
            "plan: packet SHA-256 is not the recorded capture")
    require(plan["packet_size"] == PACKET_SIZE,
            "plan: packet size is %r, expected %d" % (plan["packet_size"], PACKET_SIZE))
    require(plan["repetitions"] == REPETITIONS,
            "plan: repetitions is %r, expected %d"
            % (plan["repetitions"], REPETITIONS))
    require([list(arm) for arm in ARMS] == plan["arms"],
            "plan: the seven arms are not the pre-registered set")
    for name, digest in plan["source_sha256"].items():
        require(sha(digest), "plan: source digest for %s is not a SHA-256" % name)

    names = expected_names()
    require(len(calls) == len(names),
            "calls: %d records for %d expected cells" % (len(calls), len(names)))
    require({c["name"] for c in calls} == names,
            "calls: inventory does not match the pre-registered matrix")
    require({s["name"] for s in summaries} == names,
            "summary: inventory does not match the pre-registered matrix")
    require(len({c["name"] for c in calls}) == len(calls),
            "calls: duplicate call names")

    by_name = {c["name"]: c for c in calls}
    for summary in summaries:
        call = by_name[summary["name"]]
        require(summary["pixel_sha256"] == call.get("pixel_sha256", summary["pixel_sha256"]),
                "summary/call: pixel digest disagreement for %s" % summary["name"])

    report["inventory"] = {
        "calls": len(calls),
        "cells": len(names),
        "arms": [arm[0] for arm in ARMS],
        "resolutions": list(RESOLUTIONS),
        "packet_sha256": plan["packet_sha256"],
    }


def check_arm_identity(calls):
    """Every call must carry the CPU/mode/mask its arm name promises."""
    for call in calls:
        resolution, arm, repetition = call["name"].split("-")
        spec = next((a for a in ARMS if a[0] == arm), None)
        require(spec is not None, "call %s: unknown arm %r" % (call["name"], arm))
        require(call["resolution"] == resolution,
                "call %s: resolution recorded as %r" % (call["name"], call["resolution"]))
        require((call["cpu"], call["mode"], call["mask"]) == spec[1:],
                "call %s: cpu/mode/mask %r does not match arm %r"
                % (call["name"], (call["cpu"], call["mode"], call["mask"]), arm))
        require(int(repetition) in range(REPETITIONS),
                "call %s: repetition index out of range" % call["name"])


def check_outcomes(calls, summaries):
    """SDK success, guard retention and wait completion on every call."""
    by_name = {s["name"]: s for s in summaries}
    for call in calls:
        summary = by_name[call["name"]]
        require(call["exit"] == 0, "call %s: exit %r" % (call["name"], call["exit"]))
        require(call["guards_ok"] is True, "call %s: output guard violated" % call["name"])
        require(call["waits_ok"] is True, "call %s: an instrumented wait did not complete"
                % call["name"])
        require(summary["exit"] == 0 and summary["guards_ok"] is True
                and summary["waits_ok"] is True,
                "summary %s disagrees with its call record" % summary["name"])


# --------------------------------------------------------------------------
# per-channel corruption matrix
# --------------------------------------------------------------------------

def differing_channels(comparisons, phase):
    """Channels whose snapshot at this phase differs from the serial reference."""
    return sorted({event["ch"] for event in comparisons
                   if event["phase"] == phase and event["first_difference"]})


def check_matrix(calls, summaries, report):
    """Recompute which channel each arm actually corrupts.

    A corrupt call must show the difference on the phase-0 (consumer input) or
    phase-1 (consumer output) snapshot of exactly the arm's selected channel,
    and an untargeted channel must never appear as a phase-1 difference in a
    single-channel arm -- that is what makes the arm's label meaningful.
    """
    by_name = {s["name"]: s for s in summaries}
    matrix = {}
    problems = []

    for call in calls:
        name = call["name"]
        resolution, arm, _ = name.split("-")
        summary = by_name[name]
        spec = next(a for a in ARMS if a[0] == arm)
        mask = spec[3]

        differences = [(event["ch"], event["phase"], event["first_difference"])
                       for event in summary["comparisons"] if event["first_difference"]]
        touched_phase1 = sorted({ch for ch, phase, _ in differences if phase == PHASE_OUTPUT})
        touched_phase0 = sorted({ch for ch, phase, _ in differences if phase == PHASE_INPUT})

        if arm == "serial":
            if differences:
                problems.append("%s: serial control shows a difference" % name)
            if summary["pixel_equal"] is not True:
                problems.append("%s: serial control pixels differ" % name)
        elif arm == "publishAll":
            if differences:
                problems.append("%s: publish-all arm shows a coefficient difference" % name)
            if summary["pixel_equal"] is not True:
                problems.append("%s: publish-all arm pixels differ" % name)
        else:
            selected = [ch for ch in CHANNELS if mask & (1 << ch)]
            for channel in selected:
                if channel not in touched_phase0:
                    problems.append("%s: selected channel %d has no phase-0 difference"
                                    % (name, channel))
                if channel not in touched_phase1:
                    problems.append("%s: selected channel %d has no phase-1 difference"
                                    % (name, channel))
            if summary["pixel_equal"] is not False:
                problems.append("%s: an inducing arm reports equal pixels" % name)
            if arm in ("induceY", "induceV", "induceU"):
                unexpected = [ch for ch in touched_phase1 if ch not in selected]
                if unexpected:
                    # Untargeted channels may show a phase-0 difference whose
                    # output catches up; a phase-1 difference would contradict
                    # the single-channel claim.
                    problems.append("%s: untargeted channel(s) %s differ at the consumer "
                                    "output" % (name, unexpected))

        matrix.setdefault((resolution, arm), []).append(
            {"name": name, "phase0": touched_phase0, "phase1": touched_phase1,
             "pixel_equal": summary["pixel_equal"]})

    require(not problems, "ledger matrix: " + "; ".join(problems))
    report["matrix"] = {"%s/%s" % key: value for key, value in sorted(matrix.items())}
    return matrix


def check_first_differences(summaries, report):
    """The published first differing coefficient must be reproduced exactly."""
    observed = {}
    for summary in summaries:
        for event in summary["comparisons"]:
            difference = event["first_difference"]
            if not difference:
                continue
            key = (event["ch"], event["phase"])
            observed.setdefault(key, set()).add(
                (difference["expected"], difference["actual"],
                 difference["coefficient"], difference["byte_offset"]))

    for channel, (expected, actual) in EXPECTED_FIRST_INPUT.items():
        require((channel, PHASE_INPUT) in observed,
                "first difference: no phase-0 record for channel %d" % channel)
        require(observed[(channel, PHASE_INPUT)] == {(expected, actual, 0, 0)},
                "first difference: channel %d phase 0 is %r, expected one of %r"
                % (channel, sorted(observed[(channel, PHASE_INPUT)]), (expected, actual, 0, 0)))

    for channel, (expected, actual) in EXPECTED_FIRST_OUTPUT.items():
        require((channel, PHASE_OUTPUT) in observed,
                "first difference: no phase-1 record for channel %d" % channel)
        require(observed[(channel, PHASE_OUTPUT)] == {(expected, actual, 0, 0)},
                "first difference: channel %d phase 1 is %r, expected one of %r"
                % (channel, sorted(observed[(channel, PHASE_OUTPUT)]), (expected, actual, 0, 0)))

    report["first_difference"] = {
        str(channel): {
            "phase_0_expected": EXPECTED_FIRST_INPUT[channel][0],
            "phase_0_actual": EXPECTED_FIRST_INPUT[channel][1],
            "phase_1_expected": EXPECTED_FIRST_OUTPUT[channel][0],
            "phase_1_actual": EXPECTED_FIRST_OUTPUT[channel][1],
            "coefficient": 0,
            "byte_offset": 0,
        } for channel in CHANNELS
    }


# --------------------------------------------------------------------------
# producer identity and recovery
# --------------------------------------------------------------------------

def check_producer_identity(summaries, report):
    """Each consumer input must name the publication event of its own channel.

    This is the substitution witness the report leans on: without it, "the
    consumer read early" is an observation about flags rather than about a
    specific buffer. The record carries the producing event index per consumer
    snapshot, so it can be checked instead of assumed.
    """
    checked = 0
    for summary in summaries:
        events = summary["comparisons"]
        published = {}
        for event in events:
            if event["phase"] == PHASE_PUBLISH and event["producer"] >= 0:
                published[event["ch"]] = event["event"]
        for event in events:
            if event["phase"] != PHASE_INPUT or event["index"] != 1:
                continue
            if event["producer"] < 0:
                continue
            producer = event["producer"]
            require(producer in range(len(events)),
                    "%s: producer index %d out of range" % (summary["name"], producer))
            source = events[producer]
            require(source["phase"] == PHASE_PUBLISH,
                    "%s: producer event %d is phase %d, not a publication"
                    % (summary["name"], producer, source["phase"]))
            require(source["ch"] == event["ch"],
                    "%s: channel %d consumer is fed by a channel %d publication"
                    % (summary["name"], event["ch"], source["ch"]))
            # A spatial reconstruction at index i writes band 0 of wavelet[i-1]
            # (`lowpass_index = index - 1` in ReconstructWaveletBand), so the
            # consumer at index 1 is fed by the publication event of index 2.
            require(source["index"] == event["index"] + 1,
                    "%s: index %d consumer is fed by an index %d publication; the "
                    "spatial producer writes wavelet[index-1], so it must be %d"
                    % (summary["name"], event["index"], source["index"],
                       event["index"] + 1))
            checked += 1
    require(checked > 0, "producer identity: no consumer/producer pair in the ledger")
    report["producer_identity"] = {"pairs_checked": checked}


def check_recovery(summaries, report):
    """Per resolution, every inducted channel must be restored by publish-all.

    The claim is per resolution and per channel, so it is counted that way: one
    resolution passing does not carry the other, and one channel passing does
    not carry the other two.
    """
    by_name = {s["name"]: s for s in summaries}
    counts = {}
    for resolution in RESOLUTIONS:
        for arm in ("serial", "publishAll"):
            clean = 0
            for repetition in range(REPETITIONS):
                summary = by_name["%s-%s-%d" % (resolution, arm, repetition)]
                if summary["pixel_equal"] is True and not any(
                        e["first_difference"] for e in summary["comparisons"]):
                    clean += 1
            require(clean == REPETITIONS,
                    "recovery: %s/%s restored only %d of %d repetitions"
                    % (resolution, arm, clean, REPETITIONS))
            counts["%s/%s" % (resolution, arm)] = clean

        corrupt = 0
        for repetition in range(REPETITIONS):
            summary = by_name["%s-induceAll-%d" % (resolution, repetition)]
            touched = differing_channels(summary["comparisons"], PHASE_OUTPUT)
            if summary["pixel_equal"] is False and touched == list(CHANNELS):
                corrupt += 1
        require(corrupt == REPETITIONS,
                "recovery: %s/induceAll corrupted all three channels in only %d of %d "
                "repetitions" % (resolution, corrupt, REPETITIONS))
        counts["%s/induceAll" % resolution] = corrupt

    report["recovery"] = counts


def check_pixel_digests(summaries, report):
    """A clean call must share the serial digest; a corrupt one must not reuse it."""
    serial_digests = {}
    for resolution in RESOLUTIONS:
        digests = {s["pixel_sha256"] for s in summaries
                   if s["name"].startswith(resolution + "-serial-")}
        require(len(digests) == 1,
                "pixel digests: the two %s serial controls disagree" % resolution)
        serial_digests[resolution] = digests.pop()

    for summary in summaries:
        resolution = summary["name"].split("-")[0]
        if summary["pixel_equal"] is True:
            require(summary["pixel_sha256"] == serial_digests[resolution],
                    "%s claims equal pixels but does not carry the serial digest"
                    % summary["name"])
        else:
            require(summary["pixel_sha256"] != serial_digests[resolution],
                    "%s claims differing pixels but carries the serial digest"
                    % summary["name"])

    report["pixel_digests"] = serial_digests


# --------------------------------------------------------------------------
# the 96-call matrix, same discipline
# --------------------------------------------------------------------------

def check_legacy(calls, summaries, plan, report):
    """Recompute the 96-call matrix counts from its own records.

    The aggregate rows are checked against the per-call records, not only against
    each other: a row can agree with itself while disagreeing with the calls it
    summarises.
    """
    require(plan["packet_sha256"] == PACKET_SHA, "legacy plan: unexpected packet")
    require(plan["packet_size"] == PACKET_SIZE, "legacy plan: unexpected packet size")
    require(len(summaries) == 12,
            "legacy summary: %d aggregate rows, expected 12" % len(summaries))
    require(len(calls) == 96,
            "legacy calls: %d records, expected 96" % len(calls))
    require(len({c["name"] for c in calls}) == 96,
            "legacy calls: duplicate call names")

    # Group the calls by the cell its name and fields describe.
    cells = {}
    for call in calls:
        resolution, kind, workers, delay, index = call["name"].split("-")
        require(call["resolution"] == resolution and call["kind"] == kind
                and call["workers"] == workers and call["delay"] == delay,
                "legacy call %s: fields disagree with its name" % call["name"])
        require(call["exit"] == 0, "legacy call %s: exit %r"
                % (call["name"], call["exit"]))
        cells.setdefault((resolution, kind, workers, delay), []).append(call)

    require(len(cells) == 12, "legacy calls: %d cells, expected 12" % len(cells))
    for key, group in cells.items():
        require(len(group) == 8,
                "legacy cell %s: %d calls, expected 8" % ("/".join(key), len(group)))

    total = 0
    differing = 0
    late = 0
    waits = 0
    for row in summaries:
        key = (row["resolution"], row["kind"], row["workers"], row["delay"])
        require(key in cells, "legacy row %s has no calls" % ("/".join(key),))
        group = cells[key]
        require(row["calls"] == len(group),
                "legacy %s: row says %d calls, %d recorded"
                % ("/".join(key), row["calls"], len(group)))
        require(len(row["differences"]) == len(group),
                "legacy %s: %d difference rows for %d calls"
                % ("/".join(key), len(row["differences"]), len(group)))

        actual = sum(1 for d in row["differences"] if d["changed_bytes"])
        require(actual == row["differing_outputs"],
                "legacy %s: claims %d differing outputs but %d rows carry bytes"
                % ("/".join(key), row["differing_outputs"], actual))

        recorded_late = sum(1 for c in group if c["late_producer_channels"])
        require(recorded_late == row["late_producer"],
                "legacy %s: claims %d late-producer calls, %d recorded"
                % ("/".join(key), row["late_producer"], recorded_late))
        recorded_waits = sum(1 for c in group if c["wait_signalled"])
        require(recorded_waits == row["signalled_waits"],
                "legacy %s: claims %d signalled waits, %d recorded"
                % ("/".join(key), row["signalled_waits"], recorded_waits))

        total += row["calls"]
        differing += row["differing_outputs"]
        late += row["late_producer"]
        waits += row["signalled_waits"]

    require(total == 96, "legacy: %d calls recorded, expected 96" % total)
    require(differing == 20, "legacy: %d differing outputs, expected 20" % differing)
    require(late == 19, "legacy: %d late-producer calls, expected 19" % late)
    require(waits == 32, "legacy: %d signalled waits, expected 32" % waits)

    report["legacy_96"] = {"calls": total, "differing_outputs": differing,
                           "late_producer": late, "signalled_waits": waits,
                           "cells": len(cells)}


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def validate():
    report = {}

    all_plan = read_json(ALL_CHANNELS / "plan.json")
    all_calls = read_json(ALL_CHANNELS / "calls.json")
    all_summaries = read_json(ALL_CHANNELS / "summary.json")
    all_verification = read_json(ALL_CHANNELS / "verification.json")

    check_inventory(all_plan, all_calls, all_summaries, report)
    check_arm_identity(all_calls)
    check_outcomes(all_calls, all_summaries)
    check_matrix(all_calls, all_summaries, report)
    check_first_differences(all_summaries, report)
    check_producer_identity(all_summaries, report)
    check_recovery(all_summaries, report)
    check_pixel_digests(all_summaries, report)

    require(all_verification["calls"] == len(all_calls),
            "verification: claims %r calls, ledger has %d"
            % (all_verification["calls"], len(all_calls)))
    require(all_verification["verdict"] == "ROOT_CAUSE_CONFIRMED",
            "verification: unexpected verdict %r" % all_verification["verdict"])

    legacy_plan = read_json(LEGACY / "plan.json")
    legacy_calls = read_json(LEGACY / "calls.json")
    legacy_summaries = read_json(LEGACY / "summary.json")
    check_legacy(legacy_calls, legacy_summaries, legacy_plan, report)

    report["verdict"] = "RECORDED_LEDGER_AUDIT_PASS"
    report["replay"] = False
    return report


def print_report(report):
    inventory = report["inventory"]
    print("inventory: %d calls over %d cells, %s x %s"
          % (inventory["calls"], inventory["cells"],
             ",".join(inventory["resolutions"]), ",".join(inventory["arms"])))
    print("packet:   %s" % inventory["packet_sha256"])
    print()
    print("per-channel corruption matrix (phase-1 = consumer output):")
    for key, entries in report["matrix"].items():
        resolution, arm = key.split("/")
        for entry in entries:
            print("  %-22s phase0=%-9s phase1=%-9s pixels_equal=%s"
                  % (key + "/" + entry["name"].rsplit("-", 1)[1],
                     entry["phase0"], entry["phase1"], entry["pixel_equal"]))
    print()
    print("first differing coefficient (row-major, byte offset 0 in every case):")
    for channel, row in sorted(report["first_difference"].items()):
        print("  ch%s  input %d -> %d   output %d -> %d"
              % (channel, row["phase_0_expected"], row["phase_0_actual"],
                 row["phase_1_expected"], row["phase_1_actual"]))
    print()
    print("producer identity: %d consumer/producer pairs, all same-channel and "
          "producer-index = consumer-index + 1" % report["producer_identity"]["pairs_checked"])
    print("recovery: %s" % ", ".join("%s=%d/%d" % (k, v, REPETITIONS)
                                     for k, v in sorted(report["recovery"].items())))
    print("serial pixel digests: %s" % report["pixel_digests"])
    print()
    legacy = report["legacy_96"]
    print("96-call matrix: %d calls, %d differing outputs, %d late producer, "
          "%d signalled waits"
          % (legacy["calls"], legacy["differing_outputs"],
             legacy["late_producer"], legacy["signalled_waits"]))
    print()
    print("decoder replay: no (recorded ledgers only)")
    print("VERDICT: %s" % report["verdict"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true",
                        help="mutate scratch copies of the ledgers and require rejection")
    args = parser.parse_args(argv)

    if args.self_test:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "rca160_ledger_selftest", REPO / "tools/rca160_ledger_selftest.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.run()

    try:
        report = validate()
    except ContractError as error:
        print("RCA160-LEDGER: FAIL -- %s" % error, file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_report(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
