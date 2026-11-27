#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Fail-closed replay of the pinned Windows quality-state oracle manifests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from validation_support import integer, require

TRANSFER_SHA256 = "2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205"
PROFILES = {"low": 1, "medium": 2, "high": 3, "filmscan1": 4, "filmscan2": 5, "filmscan3": 6}
PER_PROFILE_SUITES = ("1080p-16f", "3840x2160-1f", "7680x4320-1f")
SUITE_GEOMETRY = {
    "1080p-16f": (1920, 1080, 16),
    "3840x2160-1f": (3840, 2160, 1),
    "7680x4320-1f": (7680, 4320, 1),
    "sequence-120f": (1920, 1080, 120),
}
SIGNED_INT_MAX = 2**31 - 1


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def validate_tables(row):
    for key in ("quant", "scale"):
        table = row.get(key)
        require(type(table) is list and len(table) == 36, f"invalid {key} table")
        for value in table:
            integer(value, 1, 32768)


def validate_probe(result):
    require(type(result) is dict and set(result) == {"limiter", "overbitrate", "quant", "scale"},
            "invalid probe result fields")
    integer(result["limiter"], maximum=SIGNED_INT_MAX)
    integer(result["overbitrate"], maximum=SIGNED_INT_MAX)
    validate_tables(result)


def validate_frames(frames, suite):
    width, height, count = SUITE_GEOMETRY[suite]
    require(type(frames) is list and len(frames) == count,
            f"frame inventory mismatch: suite={suite}, expected={count}")
    for index, frame in enumerate(frames):
        require(type(frame) is dict, f"invalid frame object at index {index}")
        actual = tuple(integer(frame.get(key)) for key in ("frame_index", "width", "height"))
        require(actual == (index, width, height), f"frame order or geometry mismatch at index {index}")
        integer(frame.get("previous_sample_bytes"), maximum=SIGNED_INT_MAX)
        integer(frame.get("fs_rate_limiter"), maximum=SIGNED_INT_MAX)
        validate_tables(frame)


def replay_manifest(manifest_path, profile, enum_value, suite, records, probe):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    require(type(manifest) is dict, "invalid manifest root")
    settings = manifest.get("profile")
    require(type(settings) is dict, "invalid manifest profile")
    require(integer(settings.get("quality_sdk_enum")) == enum_value, "manifest profile enum mismatch")
    require(integer(settings.get("gop")) == 1 and integer(settings.get("precision")) == 10,
            "unsupported manifest encoding settings")
    artifacts = manifest.get("artifacts", [])
    require(type(artifacts) is list, "invalid manifest artifacts")
    for artifact in artifacts:
        require(type(artifact) is dict and isinstance(artifact.get("path"), str),
                "invalid manifest artifact")
        rel = f"{profile}/{suite}/{artifact['path']}"
        record = records.get(rel)
        require(record == (integer(artifact.get("byte_length")), artifact.get("sha256")),
                f"manifest artifact reference disagrees with pinned transfer inventory: {rel}")

    frames = manifest.get("frames")
    validate_frames(frames, suite)
    probe_path = str(probe.resolve())
    prev_limiter = 0
    prev_overbitrate = 0
    transitions = 0
    for frame in frames:
        idx, width, height = frame["frame_index"], frame["width"], frame["height"]
        prev_bytes = frame["previous_sample_bytes"]
        argv = [probe_path, str(enum_value), str(width), str(height), str(idx),
                str(prev_limiter), str(prev_bytes), str(prev_overbitrate)]
        output = subprocess.check_output(argv, text=True)
        try:
            actual = json.loads(output)
            validate_probe(actual)
        except ValueError as exc:
            raise ValueError(f"invalid probe result: profile={profile}, frame={idx}: {exc}") from exc
        for key, got in (("fs_rate_limiter", actual["limiter"]), ("quant", actual["quant"]), ("scale", actual["scale"])):
            want = frame[key]
            if want != got:
                if key in ("quant", "scale"):
                    band = next(i for i, (a, b) in enumerate(zip(want, got)) if a != b)
                    detail = f"{key} band={band} expected={want[band]} actual={got[band]}"
                else:
                    detail = f"expected={want} actual={got}"
                raise ValueError(f"first mismatch: profile={profile},frame={idx},limiter={frame['fs_rate_limiter']},previous bytes={prev_bytes},{detail}; manifest={manifest_path}")
        if idx > 0 and frame["fs_rate_limiter"] != prev_limiter:
            transitions += 1
        prev_limiter = frame["fs_rate_limiter"]
        prev_overbitrate = actual["overbitrate"]
    return len(frames), transitions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    args = parser.parse_args()
    source_root = Path(__file__).resolve().parents[1]
    source_commit = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=source_root, text=True
    ).strip()
    root = args.corpus.resolve()
    transfer = root / "TRANSFER_SHA256.tsv"
    transfer_sha = digest(transfer)
    if transfer_sha != TRANSFER_SHA256:
        raise ValueError(f"TRANSFER_SHA256.tsv hash mismatch: expected={TRANSFER_SHA256} actual={transfer_sha}")
    records = {}
    for line_no, line in enumerate(transfer.read_text(encoding="ascii").splitlines(), 1):
        parts = line.split("\t")
        if len(parts) != 3:
            raise ValueError(f"invalid transfer record at line {line_no}")
        rel, size_text, expected = parts
        if rel in records or rel.startswith(("/", "\\")) or ".." in Path(rel).parts:
            raise ValueError(f"unsafe or duplicate transfer path at line {line_no}: {rel}")
        records[rel] = (int(size_text), expected)
    if len(records) != 1760:
        raise ValueError(f"transfer inventory count mismatch: expected=1760 actual={len(records)}")
    expected_bytes = sum(size for size, _ in records.values())
    if expected_bytes != 5970674259:
        raise ValueError(f"transfer inventory byte count mismatch: {expected_bytes}")

    # Hash every transferred input, manifest, and oracle artifact before using it.
    for index, (rel, (size, expected)) in enumerate(records.items(), 1):
        path = root / Path(rel)
        if not path.is_file() or path.stat().st_size != size:
            raise ValueError(f"artifact size mismatch: {rel}, expected={size}, actual={path.stat().st_size if path.exists() else 'MISSING'}")
        actual = digest(path)
        if actual != expected:
            raise ValueError(f"artifact SHA-256 mismatch: {rel}, expected={expected}, actual={actual}")
        if index % 200 == 0:
            print(f"verified corpus hashes {index}/{len(records)}", flush=True)

    manifests = sorted(root.glob("*/**/manifest.json"))
    if len(manifests) != 20:
        raise ValueError(f"manifest count mismatch: expected=20 actual={len(manifests)}")
    selected = []
    for profile, enum_value in PROFILES.items():
        for suite in PER_PROFILE_SUITES:
            matches = [p for p in manifests if p.parent.name == suite and p.parts[-3] == profile]
            if len(matches) != 1:
                raise ValueError(f"manifest/profile inventory mismatch: {profile}/{suite}, matches={len(matches)}")
            selected.append((matches[0], profile, enum_value, suite))
    for profile, enum_value in (("filmscan2", 5), ("filmscan3", 6)):
        matches = [p for p in manifests if p.parent.name == "sequence-120f" and p.parts[-3] == profile]
        if len(matches) != 1:
            raise ValueError(f"120-frame manifest inventory mismatch: {profile}, matches={len(matches)}")
        selected.append((matches[0], profile, enum_value, "sequence-120f"))

    compiler = os.environ.get("HYPERCINE_TEST_COMPILER", "NOT PROVIDED BY CTest")
    source_paths = ["include/hypercine/quality_state.h", "src/core/quality_state.cpp",
                    "upstream/Codec/quantize.c", "upstream/Codec/quantize.h", "upstream/Codec/wavelet.c"]
    source_hashes = {name: digest(source_root / name) for name in source_paths}
    checked_frames = 0
    rate_state_transitions = {}
    for manifest_path, profile, enum_value, suite in selected:
        count, transitions = replay_manifest(manifest_path, profile, enum_value, suite, records, args.probe)
        checked_frames += count
        if suite == "sequence-120f":
            rate_state_transitions[profile] = transitions

    print(json.dumps({"status": "PASS", "manifest_count": len(manifests), "checked_frames": checked_frames,
                      "transfer_sha256": transfer_sha, "transfer_files": len(records),
                      "transfer_bytes": expected_bytes, "compiler": compiler,
                      "source_commit": source_commit,
                      "rate_state_transitions_observed": rate_state_transitions,
                      "source_sha256": source_hashes}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (KeyError, OSError, ValueError, json.JSONDecodeError, subprocess.CalledProcessError) as exc:
        print("FAIL:", exc, file=sys.stderr)
        raise SystemExit(1)
