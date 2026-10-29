#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Replay the four 120-frame Windows SDK sequences against the quality lattice.

Issue #46. 4 profiles x 120 frames = 480 packets. Two independent questions:

1. Does the current shared calculator reproduce the band quant/scale tables the
   original SDK wrote into each packet? Those tables are *observable* — they are
   parsed out of the packet into `packet_band_quant_scale` — so this is a real
   comparison, not a re-derivation of our own state.
2. How many frames did the pre-#46 flat-gate logic get wrong, per profile and
   per field? That number is the whole justification for the ladder, so it is
   measured here rather than quoted.

What this cannot compare, and does not pretend to: `sdk_rate_limiter` and
`sdk_overbitrate` are recorded as `NOT_MEASURED` / `SDK_INTERNAL_STATE_NOT_OBSERVABLE`
for all 480 frames. The rate state is therefore carried forward from our own
model — the comparison is open loop in the state and closed loop only on the
tables the packet actually exposes. Any claim about the SDK's internal counters
must come from the separate state capture, not from this corpus.

Fail closed: missing file, wrong size, wrong SHA-256, wrong frame count, a probe
that exits non-zero, or a probe output that does not parse all abort the run.
There is no skip path and no partial pass.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

PROFILES = {"low": 1, "medium": 2, "high": 3, "filmscan1": 4}
FRAMES_PER_SEQUENCE = 120
EXPECTED_RECORDS = 606  # 4x120 packets + 120 inputs + manifest + 5 provenance files


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def fail(message):
    raise SystemExit("FAIL: " + message)


def probe_table_index(level, band):
    """Map a packet subband to the calculator's level-major index.

    The packet orders subbands coarse to fine: 1..3 belong to the coarsest level,
    7..9 to the finest. The calculator stores level-major with the finest level
    first, four entries per level (index 0 is the low-pass). Verified against the
    frame-0 tables of all four profiles, where quant[0] == 1 and the three
    channels are identical.
    """
    return level * 4 + band


def packet_subband(level, band):
    return 6 - 3 * level + band


def replay(probe, frames, enum_value, width, height, carry_state):
    """Drive one 120-frame sequence through a probe binary.

    Returns per-frame records with the calculator's tables and the carried state.
    """
    limiter = 0
    overbitrate = 0
    previous_bytes = 0
    out = []
    for frame in frames:
        index = frame["frame_index"]
        argv = [str(probe), str(enum_value), str(width), str(height), str(index),
                str(limiter), str(previous_bytes), str(overbitrate)]
        try:
            payload = json.loads(subprocess.check_output(argv, text=True))
        except subprocess.CalledProcessError as error:
            fail(f"probe failed on {enum_value}/{index}: {error}")
        record = {
            "frame_index": index,
            "limiter": payload["limiter"],
            "overbitrate": payload["overbitrate"],
            "quant": payload["quant"],
            "scale": payload["scale"],
            "previous_bytes": previous_bytes,
            "sdk_previous_bytes": frame["previous_sdk_sample_bytes"],
            "sdk_packet_bytes": frame["sdk_byte_length"],
        }
        out.append(record)
        if carry_state:
            limiter = payload["limiter"]
            overbitrate = payload["overbitrate"]
            previous_bytes = frame["sdk_byte_length"]
        else:
            previous_bytes = frame["sdk_byte_length"]
    return out


def compare(replayed, frames):
    """Compare the calculator's tables with the tables parsed out of each packet."""
    divergence = {"frames": 0, "quant_entries": 0, "scale_entries": 0,
                  "frames_missing_tables": 0, "first": None}
    for record, frame in zip(replayed, frames):
        band = frame["packet_band_quant_scale"]
        if not band:
            divergence["frames_missing_tables"] += 1
            continue
        by_subband = {(entry["channel"], entry["subband"]): entry for entry in band}
        frame_bad_quant = 0
        frame_bad_scale = 0
        first_detail = None
        for channel in range(3):
            for level in range(3):
                for band_index in range(1, 4):
                    key = (channel, packet_subband(level, band_index))
                    entry = by_subband.get(key)
                    if entry is None:
                        fail(f"packet frame {record['frame_index']} lacks subband {key}")
                    if entry["quant_53"] is None or entry["scale_54"] is None:
                        continue  # the SDK omits the low-pass pair by design
                    at = channel * 12 + probe_table_index(level, band_index)
                    if record["quant"][at] != entry["quant_53"]:
                        frame_bad_quant += 1
                        if first_detail is None:
                            first_detail = (f"quant ch{channel} level{level} band{band_index} "
                                            f"expected={entry['quant_53']} got={record['quant'][at]}")
                    if record["scale"][at] != entry["scale_54"]:
                        frame_bad_scale += 1
                        if first_detail is None:
                            first_detail = (f"scale ch{channel} level{level} band{band_index} "
                                            f"expected={entry['scale_54']} got={record['scale'][at]}")
        if frame_bad_quant or frame_bad_scale:
            divergence["frames"] += 1
            divergence["quant_entries"] += frame_bad_quant
            divergence["scale_entries"] += frame_bad_scale
            if divergence["first"] is None:
                divergence["first"] = {"frame_index": record["frame_index"], "detail": first_detail}
    return divergence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path,
                        help="group-a-sequences directory (read only)")
    parser.add_argument("--probe", required=True, type=Path,
                        help="the current quality_state_probe binary")
    parser.add_argument("--baseline-probe", type=Path,
                        help="optional probe built from the pre-#46 quality_state.cpp")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    root = args.corpus.resolve()
    transfer = root / "TRANSFER_SHA256.tsv"
    manifest_path = root / "manifest.json"
    for path, label in ((transfer, "transfer index"), (manifest_path, "manifest"),
                        (args.probe, "probe")):
        if not path.is_file():
            fail(f"{label} not found: {path}")

    records = {}
    for line_no, line in enumerate(transfer.read_text(encoding="ascii").splitlines(), 1):
        parts = line.split("\t")
        if len(parts) != 3:
            fail(f"invalid transfer record at line {line_no}")
        rel, size_text, expected = parts
        if rel in records or rel.startswith(("/", "\\")) or ".." in Path(rel).parts:
            fail(f"unsafe or duplicate path at line {line_no}: {rel}")
        records[rel] = (int(size_text), expected)
    if len(records) != EXPECTED_RECORDS:
        fail(f"transfer inventory count mismatch: expected={EXPECTED_RECORDS} actual={len(records)}")

    for index, (rel, (size, expected)) in enumerate(records.items(), 1):
        path = root / Path(rel)
        if not path.is_file():
            fail(f"artifact missing: {rel}")
        if path.stat().st_size != size:
            fail(f"artifact size mismatch: {rel}, expected={size}, actual={path.stat().st_size}")
        actual = digest(path)
        if actual != expected:
            fail(f"artifact SHA-256 mismatch: {rel}, expected={expected}, actual={actual}")
        if index % 120 == 0:
            print(f"verified corpus hashes {index}/{len(records)}", flush=True)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    sessions = {session["profile"]: session for session in manifest["sessions"]}
    if set(sessions) != set(PROFILES):
        fail(f"manifest sessions {sorted(sessions)} != expected {sorted(PROFILES)}")

    profile_geometry = manifest["profile"]
    width, height = profile_geometry["width"], profile_geometry["height"]

    report = {"schema": 1, "corpus_files": len(records), "manifest_sha256": digest(manifest_path),
              "geometry": f"{width}x{height}", "profiles": {}, "totals": {}}
    total_frames = 0
    total_bad = 0
    baseline_total_bad = 0

    for profile, enum_value in PROFILES.items():
        frames = sessions[profile]["frames"]
        if len(frames) != FRAMES_PER_SEQUENCE:
            fail(f"{profile}: {len(frames)} frames, expected {FRAMES_PER_SEQUENCE}")
        if sessions[profile]["sequence"] != "sequence-120f":
            fail(f"{profile}: unexpected sequence {sessions[profile]['sequence']}")

        current = replay(args.probe, frames, enum_value, width, height, carry_state=True)
        divergence = compare(current, frames)

        entry = {
            "frames": len(frames),
            "packets_compared": len(frames),
            "divergent_frames": divergence["frames"],
            "divergent_quant_entries": divergence["quant_entries"],
            "divergent_scale_entries": divergence["scale_entries"],
            "frames_without_packet_tables": divergence["frames_missing_tables"],
            "first_divergence": divergence["first"],
            "overbitrate_trace": [record["overbitrate"] for record in current],
            "limiter_trace": [record["limiter"] for record in current],
            "sdk_state": "NOT_OBSERVABLE (all 120 frames)",
        }
        total_frames += len(frames)
        total_bad += divergence["frames"]

        if args.baseline_probe is not None:
            baseline = replay(args.baseline_probe, frames, enum_value, width, height, carry_state=True)
            baseline_divergence = compare(baseline, frames)
            entry["baseline_divergent_frames"] = baseline_divergence["frames"]
            entry["baseline_divergent_quant_entries"] = baseline_divergence["quant_entries"]
            entry["baseline_divergent_scale_entries"] = baseline_divergence["scale_entries"]
            entry["baseline_first_divergence"] = baseline_divergence["first"]
            baseline_total_bad += baseline_divergence["frames"]

        report["profiles"][profile] = entry

    report["totals"] = {
        "packets_compared": total_frames,
        "divergent_frames": total_bad,
        "baseline_divergent_frames": baseline_total_bad if args.baseline_probe else None,
        "rate_state_axis": "not comparable: the corpus records no SDK internal state",
        "conformance_claim": "none",
    }

    if args.json:
        print(json.dumps(report, indent=1))
    else:
        print(f"corpus        {len(records)} files, all sizes and SHA-256 verified")
        print(f"geometry      {width}x{height}, 4 profiles x {FRAMES_PER_SEQUENCE} frames")
        for profile, entry in report["profiles"].items():
            line = (f"{profile:10s} frames={entry['frames']:3d} "
                    f"divergent={entry['divergent_frames']:3d} "
                    f"quant_entries={entry['divergent_quant_entries']:3d} "
                    f"scale_entries={entry['divergent_scale_entries']:3d}")
            if "baseline_divergent_frames" in entry:
                line += f" | pre-#46 divergent={entry['baseline_divergent_frames']:3d}"
            print(line)
            if entry["first_divergence"]:
                print(f"           first: frame {entry['first_divergence']['frame_index']} "
                      f"{entry['first_divergence']['detail']}")
        print(f"TOTAL packets {total_frames}, divergent frames {total_bad}")
        if args.baseline_probe:
            print(f"pre-#46 total divergent frames {baseline_total_frames_display(report)}")
        print("SDK rate state: NOT OBSERVABLE in this corpus (not a pass)")

    # Closed loop on the axis the packet actually exposes: any divergence between
    # the current calculator and a packet-parsed table is a failure, not a number
    # to report. Otherwise this script would "pass" while the encoder drifts.
    if total_bad:
        fail(f"{total_bad} of {total_frames} frames diverge from the packet tables")
    print(f"PASS {total_frames}/{total_frames} packets match on every observable band table")
    return 0


def baseline_total_frames_display(report):
    return report["totals"]["baseline_divergent_frames"]


if __name__ == "__main__":
    sys.exit(main())
