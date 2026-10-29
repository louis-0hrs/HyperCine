#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Does the SDK-free writer reproduce the SDK's packet when fed another profile?

Measurement on issue #46. For each of the six regular profiles, the pinned
BGRA input is encoded by the local writer with that profile selected, and the
resulting packet is compared byte-for-byte against the SDK packet the corpus
pinned. Variable metadata (GUID/DATE/TIME/TIMC) is normalized on both sides
only, using the manifest's own recorded ranges - no other byte is masked.

This is a diagnostic: the local encoder used here had profile acceptance
widened locally for the experiment and is NOT the merged code.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
import reference_sample as sample  # noqa: E402

PROFILES = ["low", "medium", "high", "filmscan1", "filmscan2", "filmscan3"]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--corpus", required=True, type=Path)
    p.add_argument("--encoder", required=True, type=Path)
    p.add_argument("--geometry", default="1080p-16f")
    p.add_argument("--profiles", default=",".join(PROFILES))
    p.add_argument("--max-frames", type=int, default=16)
    p.add_argument("--work", required=True, type=Path)
    p.add_argument("--report", type=Path)
    args = p.parse_args()

    root = args.corpus.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    out = {"geometry": args.geometry, "profiles": {}}

    for profile in [p.strip() for p in args.profiles.split(",") if p.strip()]:
        base = root / profile / args.geometry
        if not (base / "manifest.json").is_file():
            out["profiles"][profile] = {"skipped": "no %s corpus" % args.geometry}
            print("%-9s SKIPPED (no corpus)" % profile, flush=True)
            continue
        manifest = json.loads((base / "manifest.json").read_text())
        frames_dir = base / "frames" / f"{profile}-sequence-1920x1080"
        if not frames_dir.is_dir():
            frames_dir = next((base / "frames").iterdir())

        directory = args.work / profile
        directory.mkdir(parents=True, exist_ok=True)
        inputs = []
        chosen = []
        for frame in manifest["frames"][: args.max_frames]:
            path = directory / ("input_%04d.bgra" % frame["frame_index"])
            if not path.is_file():
                src = frames_dir / ("frame_%04d" % frame["frame_index"]) / "input.bgra"
                path.write_bytes(src.read_bytes())
            if len(path.read_bytes()) != frame["input_byte_length"]:
                raise SystemExit("input byte length mismatch: " + path.name)
            inputs.append(str(path))
            chosen.append(frame)

        samples = directory / "samples"
        command = [str(args.encoder)]
        if args.geometry != "1080p-16f":
            # The corpus directory names its geometry; the encoder needs it
            # explicitly, and defaulting silently would measure 1080p bytes
            # against a 4K packet.
            geometry = chosen[0]["width"], chosen[0]["height"]
            command += ["--geometry", "%dx%d" % geometry]
            if (geometry[0], geometry[1]) != (chosen[0]["width"], chosen[0]["height"]):
                raise SystemExit("geometry not derivable from the manifest")
        command += ["--quality", profile, str(samples)] + inputs
        subprocess.run(command, check=True, capture_output=True, text=True)
        trace = json.loads((samples / "trace.json").read_text())["frames"]

        rows = []
        for frame, actual in zip(chosen, trace):
            idx = frame["frame_index"]
            raw = (samples / ("frame_%04d.cfhd" % idx)).read_bytes()
            sdk = (frames_dir / ("frame_%04d" % idx) / "sdk.cfhd").read_bytes()
            sdk_norm = sample.normalized_sample(
                sdk, sample.parse_sample(sdk), frame["metadata_mask_ranges"])
            ours_norm = sample.normalized_sample(
                raw, sample.parse_sample(raw), frame["metadata_mask_ranges"])
            row = {
                "frame_index": idx,
                "pattern": frame["pattern"],
                "sdk_bytes": len(sdk),
                "writer_bytes": len(raw),
                "length_equal": len(raw) == len(sdk),
                "normalized_equal": ours_norm == sdk_norm,
                "first_normalized_diff": first_diff(ours_norm, sdk_norm),
                "tables_match_corpus": (
                    actual["quant"] == frame["quant"] and actual["scale"] == frame["scale"]),
                "writer_limiter": actual["fs_rate_limiter"],
                "writer_previous_sample_bytes": actual["previous_sample_bytes"],
                "sdk_normalized_sha256": sha(sdk_norm),
                "writer_normalized_sha256": sha(ours_norm),
            }
            rows.append(row)
        entry = {
            "frames": rows,
            "all_length_equal": all(r["length_equal"] for r in rows),
            "all_normalized_equal": all(r["normalized_equal"] for r in rows),
            "all_tables_match": all(r["tables_match_corpus"] for r in rows),
            "byte_deltas": [r["writer_bytes"] - r["sdk_bytes"] for r in rows],
        }
        out["profiles"][profile] = entry
        print("%-9s len_equal=%s normalized_equal=%s tables_match=%s deltas=%s"
              % (profile, entry["all_length_equal"], entry["all_normalized_equal"],
                 entry["all_tables_match"], entry["byte_deltas"][:6]), flush=True)

    text = json.dumps(out, indent=1)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text + "\n")
        print("wrote", args.report)

    # Fail closed. A measurement that always exits 0 is not a gate: the whole
    # point is that a divergent profile stops the build rather than printing a
    # row nobody reads. Coverage is part of the contract too, so a profile that
    # was silently skipped because its corpus was missing is a failure here;
    # the caller decides which profiles it asked for via --profiles.
    failures = []
    for profile, entry in out["profiles"].items():
        if "skipped" in entry:
            failures.append("%s: corpus missing (%s)" % (profile, entry["skipped"]))
            continue
        if not entry["all_length_equal"]:
            failures.append("%s: packet length differs from the SDK sample" % profile)
        if not entry["all_normalized_equal"]:
            bad = [f["frame_index"] for f in entry["frames"] if not f["normalized_equal"]]
            offsets = sorted({f["first_normalized_diff"] for f in entry["frames"]
                              if f["first_normalized_diff"] is not None})
            failures.append("%s: %d/%d frames differ from the SDK packet "
                            "(frames %s, first-diff offsets %s)"
                            % (profile, len(bad), len(entry["frames"]), bad[:8], offsets))
        if not entry["all_tables_match"]:
            failures.append("%s: emitted tables differ from the pinned manifest" % profile)
    if not out["profiles"]:
        failures.append("no profile was measured")
    if failures:
        for line in failures:
            print("FAIL " + line, file=sys.stderr)
        return 1
    print("p6_writer_parity: PASS (%d profiles)" % len(out["profiles"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
