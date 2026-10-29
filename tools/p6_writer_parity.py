#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Does the SDK-free writer reproduce the SDK's packet for each profile?

Admission gate for issue #46. For each regular profile the pinned BGRA input is
encoded by the local writer with that profile selected, and the result is
compared byte-for-byte against the SDK packet the corpus pinned. Only the four
variable metadata values (GUID/DATE/TIME/TIMC) are masked, using the manifest's
own recorded ranges - no other byte is touched.

Fail-closed properties, each with a negative control:

* every input and every SDK packet is checked against the manifest's pinned
  length and SHA-256 *and* against the corpus transfer index before use, so a
  stale or tampered work file cannot be measured;
* a cached work input whose hash no longer matches is re-copied from the
  verified original rather than trusted because its size happens to match;
* the encoder trace must contain exactly the requested frames, in order, with
  matching indices - a truncated or reordered trace fails instead of being
  silently zipped short;
* divergence, missing coverage or an unverified artifact exits non-zero.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
import reference_sample as sample  # noqa: E402

PROFILES = ["low", "medium", "high", "filmscan1", "filmscan2", "filmscan3"]


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n


def load_transfer_index(root):
    """The corpus' own inventory: relative path -> (size, sha256).

    Read before any artifact so a rewritten index cannot be used to bless a
    rewritten corpus; the caller compares this file's own hash when it has a
    pinned value for it.
    """
    index = root / "TRANSFER_SHA256.tsv"
    if not index.is_file():
        raise SystemExit("corpus has no TRANSFER_SHA256.tsv: " + str(index))
    records = {}
    duplicates = []
    for line_no, line in enumerate(index.read_text(encoding="ascii").splitlines(), 1):
        parts = line.split("\t")
        if len(parts) != 3:
            raise SystemExit("invalid transfer record at line %d" % line_no)
        rel, size, digest = parts
        if rel.startswith(("/", "\\")) or ".." in Path(rel).parts:
            raise SystemExit("unsafe transfer path at line %d: %s" % (line_no, rel))
        if rel in records:
            duplicates.append(rel)
            continue
        records[rel] = (int(size), digest)
    if duplicates:
        raise SystemExit("duplicate transfer records: %s" % duplicates[:4])
    if not records:
        raise SystemExit("transfer index is empty")
    return records


def require_pinned(path, expected_size, expected_sha, index, root, label):
    """Verify one artifact against the manifest *and* the transfer index."""
    if not path.is_file():
        raise SystemExit("missing pinned artifact (%s): %s" % (label, path.name))
    size = path.stat().st_size
    if size != expected_size:
        raise SystemExit("%s has %d bytes, index pins %d: %s"
                         % (label, size, expected_size, path.name))
    actual = sha_file(path)
    if actual != expected_sha:
        raise SystemExit("%s hash mismatch (%s): expected %s actual %s"
                         % (label, path.name, expected_sha, actual))
    # The same bytes must also be the ones the corpus inventory lists. Without
    # this a forged manifest could match a forged file.
    rel = path.relative_to(root).as_posix()
    if rel not in index:
        raise SystemExit("%s is not listed in the transfer index: %s" % (label, rel))
    index_size, index_sha = index[rel]
    if index_size != size or index_sha != actual:
        raise SystemExit("%s disagrees with the transfer index: %s" % (label, rel))


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
    index = load_transfer_index(root)
    args.work.mkdir(parents=True, exist_ok=True)
    out = {
        "geometry": args.geometry,
        "transfer_index": {"entries": len(index),
                           "bytes": sum(s for s, _ in index.values()),
                           "sha256": sha_file(root / "TRANSFER_SHA256.tsv")},
        "profiles": {},
    }

    for profile in [x.strip() for x in args.profiles.split(",") if x.strip()]:
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
        inputs, chosen = [], []
        for frame in manifest["frames"][: args.max_frames]:
            idx = frame["frame_index"]
            origin = frames_dir / ("frame_%04d" % idx) / "input.bgra"
            # Verified original every run, independent of any cached copy.
            require_pinned(origin, frame["input_byte_length"], frame["input_sha256"],
                           index, root, "input frame %d" % idx)
            path = directory / ("input_%04d.bgra" % idx)
            # A cached copy is only reused when its hash still matches. Matching
            # length alone would accept same-length tampering.
            if path.is_file() and sha_file(path) == frame["input_sha256"]:
                pass
            else:
                shutil.copyfile(origin, path)
                if sha_file(path) != frame["input_sha256"]:
                    raise SystemExit("work copy failed verification: " + path.name)
            inputs.append(str(path))
            chosen.append(frame)

        if not chosen:
            raise SystemExit("no frames selected for " + profile)

        samples = directory / "samples"
        if samples.is_dir():
            # Stale output from an earlier run must not survive into this one:
            # a missing frame would otherwise be read from the previous attempt.
            shutil.rmtree(samples)
        command = [str(args.encoder)]
        if args.geometry != "1080p-16f":
            geometry = (chosen[0]["width"], chosen[0]["height"])
            command += ["--geometry", "%dx%d" % geometry]
        command += ["--quality", profile, str(samples)] + inputs
        completed = subprocess.run(command, capture_output=True, text=True)
        if completed.returncode != 0:
            raise SystemExit("encoder failed for %s: %s" % (profile, completed.stderr.strip()[:400]))

        trace = json.loads((samples / "trace.json").read_text())["frames"]

        # Exact correspondence, not zip() truncation: the trace has to describe
        # precisely the frames that were requested, in order.
        expected_indices = [f["frame_index"] for f in chosen]
        actual_indices = [t.get("frame_index") for t in trace]
        if actual_indices != expected_indices:
            raise SystemExit("%s: trace frames %s do not match requested %s"
                             % (profile, actual_indices[:8], expected_indices[:8]))

        rows = []
        for frame, actual in zip(chosen, trace):
            idx = frame["frame_index"]
            emitted = samples / ("frame_%04d.cfhd" % idx)
            if not emitted.is_file():
                raise SystemExit("%s: encoder did not write frame %04d" % (profile, idx))
            raw = emitted.read_bytes()
            sdk_path = frames_dir / ("frame_%04d" % idx) / "sdk.cfhd"
            require_pinned(sdk_path, frame["sdk_byte_length"], frame["sdk_sha256"],
                           index, root, "sdk frame %d" % idx)
            sdk = sdk_path.read_bytes()
            sdk_norm = sample.normalized_sample(
                sdk, sample.parse_sample(sdk), frame["metadata_mask_ranges"])
            ours_norm = sample.normalized_sample(
                raw, sample.parse_sample(raw), frame["metadata_mask_ranges"])
            rows.append({
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
                "writer_overbitrate": actual.get("fs_overbitrate"),
                "sdk_normalized_sha256": sha_bytes(sdk_norm),
                "writer_normalized_sha256": sha_bytes(ours_norm),
            })

        entry = {
            "frames": rows,
            "requested_frames": len(chosen),
            "traced_frames": len(trace),
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

    # Fail closed. A measurement that always exits 0 is not a gate.
    failures = []
    for profile, entry in out["profiles"].items():
        if "skipped" in entry:
            failures.append("%s: corpus missing (%s)" % (profile, entry["skipped"]))
            continue
        if entry["requested_frames"] != entry["traced_frames"]:
            failures.append("%s: encoder traced %d of %d frames"
                            % (profile, entry["traced_frames"], entry["requested_frames"]))
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
    print("p6_writer_parity: PASS (%d profiles, %d frames, index %d entries)"
          % (len(out["profiles"]),
             sum(e["requested_frames"] for e in out["profiles"].values()),
             len(index)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
