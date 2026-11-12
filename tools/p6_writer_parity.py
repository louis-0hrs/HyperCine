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
* each run exclusively owns a new work directory; an existing run is never
  overwritten, healed, or recursively removed; verified copies live separately
  from encoder outputs;
* report bytes are published without replacement only after every requested
  comparison completes; a completed mismatch is explicitly reported as FAIL;
* the encoder trace must contain exactly the requested frames, in order, with
  matching indices - a truncated or reordered trace fails instead of being
  silently zipped short;
* divergence, missing coverage or an unverified artifact exits non-zero.
"""
import argparse
import contextlib
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from p6_output_paths import (Directory, absolute_path, bounded_chunks, component,
                             read_bounded, within)

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
import reference_sample as sample  # noqa: E402

PROFILES = ["low", "medium", "high", "filmscan1", "filmscan2", "filmscan3"]

# Tool resource budgets, not CineForm/SDK format maxima. The input ceiling
# follows the currently admitted 3840*2160 FilmScan3 pixel budget. Packet
# headroom exceeds the current writer's approximately 64-MiB enclosing chunk;
# it does not admit larger geometry or qualify another bitstream family.
MAX_INPUT_BYTES = 3840 * 2160 * 4
MAX_PACKET_BYTES = 128 * 1024 * 1024
MAX_TRANSFER_INDEX_BYTES = 8 * 1024 * 1024
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_TRACE_BYTES = 4 * 1024 * 1024


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


def parse_transfer_index(data):
    """Parse one immutable inventory snapshot; never interpret path selectors."""
    records = {}
    for line_no, line in enumerate(data.decode("ascii").splitlines(), 1):
        parts = line.split("\t")
        if len(parts) != 3:
            raise SystemExit("invalid transfer record at line %d" % line_no)
        rel, size, digest = parts
        names = rel.split("/")
        try:
            for name in names:
                component(name)
        except ValueError:
            raise SystemExit("unsafe transfer path at line %d" % line_no) from None
        if rel in records:
            raise SystemExit("duplicate transfer record at line %d" % line_no)
        if (not 1 <= len(size) <= 20 or not re.fullmatch(r"[0-9]+", size) or
                not re.fullmatch(r"[0-9a-f]{64}", digest)):
            raise SystemExit("invalid transfer size/hash at line %d" % line_no)
        records[rel] = (int(size), digest)
    if not records:
        raise SystemExit("transfer index is empty")
    return records


def load_transfer_index(root):
    """Compatibility entry point shared with the independent SDK gates."""
    index = root / "TRANSFER_SHA256.tsv"
    if not index.is_file():
        raise SystemExit("corpus has no TRANSFER_SHA256.tsv")
    with index.open("rb", buffering=0) as stream:
        data = read_bounded(stream, MAX_TRANSFER_INDEX_BYTES)
    return parse_transfer_index(data)


def require_pinned(path, expected_size, expected_sha, index, root, label):
    """Verify one artifact against the manifest *and* the transfer index."""
    if type(expected_size) is not int or expected_size < 0:
        raise SystemExit("invalid pinned byte length for " + label)
    if not path.is_file():
        raise SystemExit("missing pinned artifact (%s): %s" % (label, path.name))
    # The same bytes must also be the ones the corpus inventory lists. Without
    # this a forged manifest could match a forged file.
    rel = path.relative_to(root).as_posix()
    if rel not in index:
        raise SystemExit("%s is not listed in the transfer index: %s" % (label, rel))
    index_size, index_sha = index[rel]
    if index_size != expected_size or index_sha != expected_sha:
        raise SystemExit("%s disagrees with the transfer index: %s" % (label, rel))
    digest = hashlib.sha256()
    try:
        with path.open("rb", buffering=0) as source:
            for block in bounded_chunks(source, max(1, expected_size), exact=expected_size):
                digest.update(block)
    except ValueError:
        raise SystemExit(label + " byte length mismatch") from None
    actual = digest.hexdigest()
    if actual != expected_sha:
        raise SystemExit("%s hash mismatch (%s): expected %s actual %s"
                         % (label, path.name, expected_sha, actual))


def _pin(frame, key, relative, index, label):
    size, digest = frame[key + "_byte_length"], frame[key + "_sha256"]
    if type(size) is not int or size <= 0 or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise SystemExit("invalid manifest size/hash for " + label)
    if size > (MAX_INPUT_BYTES if key == "input" else MAX_PACKET_BYTES):
        raise SystemExit("manifest exceeds tool byte budget for " + label)
    if index.get(relative) != (size, digest):
        raise SystemExit(label + " disagrees with the transfer index")
    return size, digest


def _verified_file(directory, name, pin, label, destination=None):
    """Hash the exact bytes read/copied through pinned, no-follow handles."""
    size, digest = pin
    with directory.file(name) as source:
        actual = hashlib.sha256()
        count = 0
        for block in bounded_chunks(source, size, exact=size):
            count += len(block)
            actual.update(block)
            if destination is not None:
                destination.write(block)
        if count != size or actual.hexdigest() != digest:
            raise SystemExit(label + " hash mismatch")


def _verified_bytes(directory, name, pin, label):
    with directory.file(name) as source:
        data = read_bounded(source, pin[0], exact=pin[0])
    if len(data) != pin[0] or sha_bytes(data) != pin[1]:
        raise SystemExit(label + " hash mismatch")
    return data


def _plan(root, index, profiles, geometry, maximum, stack):
    plans = []
    for profile in profiles:
        try:
            profile_dir = stack.enter_context(root.child(profile))
            base = stack.enter_context(profile_dir.child(geometry))
            manifest_rel = profile + "/" + geometry + "/manifest.json"
            manifest_pin = index.get(manifest_rel)
            if manifest_pin is None or not 0 < manifest_pin[0] <= MAX_MANIFEST_BYTES:
                raise SystemExit("missing or oversized manifest transfer record")
            manifest_bytes = _verified_bytes(base, "manifest.json", manifest_pin, "manifest")
        except FileNotFoundError:
            plans.append({"profile": profile, "missing": True})
            continue
        if index.get(manifest_rel) != (len(manifest_bytes), sha_bytes(manifest_bytes)):
            raise SystemExit("manifest disagrees with the transfer index")
        manifest = json.loads(manifest_bytes)
        frames = manifest.get("frames") if isinstance(manifest, dict) else None
        if not isinstance(frames, list) or not frames:
            raise SystemExit("manifest has no frames")
        chosen = frames[:maximum]
        for position, frame in enumerate(chosen):
            if (not isinstance(frame, dict) or type(frame.get("frame_index")) is not int or
                    frame["frame_index"] != position):
                raise SystemExit("manifest frame indices must be consecutive integers starting at zero")
            for name in ("metadata_mask_ranges", "quant", "scale"):
                if not isinstance(frame.get(name), list):
                    raise SystemExit("invalid manifest " + name)
            if not isinstance(frame.get("pattern"), str):
                raise SystemExit("invalid manifest pattern")
        if geometry != "1080p-16f":
            shape = (chosen[0].get("width"), chosen[0].get("height"))
            if (any(type(v) is not int or v <= 0 for v in shape) or
                    any((f.get("width"), f.get("height")) != shape for f in chosen)):
                raise SystemExit("invalid or inconsistent manifest geometry")
        else:
            shape = None
        frames_parent = stack.enter_context(base.child("frames"))
        preferred = profile + "-sequence-1920x1080"
        names = frames_parent.names()
        if preferred in names:
            sequence = preferred
        elif len(names) == 1:
            sequence = component(names[0])
        else:
            raise SystemExit("ambiguous or missing corpus frame directory")
        frames_dir = stack.enter_context(frames_parent.child(sequence))
        pins = []
        for frame in chosen:
            idx = frame["frame_index"]
            frame_name = "frame_%04d" % idx
            relative = "/".join((profile, geometry, "frames", sequence, frame_name))
            input_pin = _pin(frame, "input", relative + "/input.bgra", index, "input frame %d" % idx)
            sdk_pin = _pin(frame, "sdk", relative + "/sdk.cfhd", index, "sdk frame %d" % idx)
            with frames_dir.child(frame_name) as source:
                _verified_file(source, "input.bgra", input_pin, "input frame %d" % idx)
                _verified_file(source, "sdk.cfhd", sdk_pin, "sdk frame %d" % idx)
            pins.append((input_pin, sdk_pin))
        plans.append(dict(profile=profile, frames=chosen, frames_dir=frames_dir,
                          pins=pins, geometry=shape))
    return plans


def _measure(plan, work, encoder_path, stack):
    profile = plan["profile"]
    directory = stack.enter_context(work.child(profile, create=True))
    inputs_dir = stack.enter_context(directory.child("inputs", create=True))
    inputs = []
    chosen = plan["frames"]
    for frame, (input_pin, _) in zip(chosen, plan["pins"]):
        idx = frame["frame_index"]
        name = "input_%04d.bgra" % idx
        with plan["frames_dir"].child("frame_%04d" % idx) as source:
            with inputs_dir.file(name, create=True) as output:
                _verified_file(source, "input.bgra", input_pin, "input frame %d" % idx,
                               destination=output)
                output.flush()
                os.fsync(output.fileno())
        inputs.append(str(inputs_dir.path / name))
    directory.require_absent("samples")
    work.visible()
    directory.visible()
    command = [str(encoder_path)]
    if plan["geometry"] is not None:
        command += ["--geometry", "%dx%d" % plan["geometry"]]
    command += ["--quality", profile, str(directory.path / "samples")] + inputs
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        # Child logs can contain private paths or tokens. They are not public
        # report data and cannot replace this gate's trusted status line.
        raise SystemExit("encoder failed for %s (exit %d)" % (profile, completed.returncode))
    work.visible()
    directory.visible()
    samples = stack.enter_context(directory.child("samples"))
    document = json.loads(samples.read("trace.json", MAX_TRACE_BYTES))
    trace = document.get("frames") if isinstance(document, dict) else None
    if not isinstance(trace, list) or any(
            not isinstance(frame, dict) or type(frame.get("frame_index")) is not int
            for frame in trace):
        raise SystemExit("%s: trace frames (invalid shape/index type) do not match requested" % profile)
    expected_indices = [f["frame_index"] for f in chosen]
    actual_indices = [t.get("frame_index") for t in trace]
    if actual_indices != expected_indices:
        raise SystemExit("%s: trace frames %s do not match requested %s" %
                         (profile, actual_indices[:8], expected_indices[:8]))
    rows = []
    for frame, actual, (_, sdk_pin) in zip(chosen, trace, plan["pins"]):
        idx = frame["frame_index"]
        raw = samples.read("frame_%04d.cfhd" % idx, MAX_PACKET_BYTES)
        with plan["frames_dir"].child("frame_%04d" % idx) as source:
            sdk = _verified_bytes(source, "sdk.cfhd", sdk_pin, "sdk frame %d" % idx)
        sdk_norm = sample.normalized_sample(sdk, sample.parse_sample(sdk), frame["metadata_mask_ranges"])
        ours_norm = sample.normalized_sample(raw, sample.parse_sample(raw), frame["metadata_mask_ranges"])
        rows.append({
            "frame_index": idx, "pattern": frame["pattern"],
            "sdk_bytes": len(sdk), "writer_bytes": len(raw),
            "length_equal": len(raw) == len(sdk),
            "normalized_equal": ours_norm == sdk_norm,
            "first_normalized_diff": first_diff(ours_norm, sdk_norm),
            "tables_match_corpus": actual["quant"] == frame["quant"] and actual["scale"] == frame["scale"],
            "writer_limiter": actual["fs_rate_limiter"],
            "writer_previous_sample_bytes": actual["previous_sample_bytes"],
            "writer_overbitrate": actual.get("fs_overbitrate"),
            "sdk_normalized_sha256": sha_bytes(sdk_norm),
            "writer_normalized_sha256": sha_bytes(ours_norm),
        })
    return {
        "frames": rows, "requested_frames": len(chosen), "traced_frames": len(trace),
        "all_length_equal": all(r["length_equal"] for r in rows),
        "all_normalized_equal": all(r["normalized_equal"] for r in rows),
        "all_tables_match": all(r["tables_match_corpus"] for r in rows),
        "byte_deltas": [r["writer_bytes"] - r["sdk_bytes"] for r in rows],
    }


def _run(args):
    profiles = args.profiles.split(",")
    if not profiles or any(p not in PROFILES for p in profiles) or len(set(profiles)) != len(profiles):
        raise SystemExit("profiles must be a nonempty, unique list of supported profile names")
    component(args.geometry)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.geometry):
        raise SystemExit("invalid geometry selector")
    if args.max_frames <= 0:
        raise SystemExit("max-frames must be positive")
    requested_work = absolute_path(args.work)
    component(requested_work.name)
    with contextlib.ExitStack() as stack:
        root = stack.enter_context(Directory.open(args.corpus))
        parent = stack.enter_context(Directory.open(requested_work.parent))
        work_path = parent.path / requested_work.name
        if (within(work_path, root.path) or within(root.path, work_path) or
                root.identity in parent.ancestors):
            raise SystemExit("work and corpus must be disjoint")
        parent.require_absent(requested_work.name)
        report_parent = None
        report_name = None
        report_in_work = False
        if args.report is not None:
            requested_report = absolute_path(args.report)
            report = requested_report
            component(report.name)
            # Resolve existing parent aliases without following a final report
            # link. An existing final entry is always rejected by lstat below.
            report = report.parent.resolve(strict=False) / report.name
            if within(report, root.path) or report == work_path:
                raise SystemExit("report must be outside the corpus and distinct from work")
            if within(report, work_path):
                relative = report.relative_to(work_path)
                if len(relative.parts) != 1 or relative.name.casefold() in PROFILES:
                    raise SystemExit("report inside work must be a separate top-level file")
                report_in_work = True
                report_name = relative.name
            else:
                report_parent = stack.enter_context(Directory.open(requested_report.parent))
                if (within(report_parent.path / report.name, root.path) or
                        root.identity in report_parent.ancestors):
                    raise SystemExit("report must be outside the corpus")
                report_parent.require_absent(report.name)
                report_name = report.name
        index_bytes = root.read("TRANSFER_SHA256.tsv", MAX_TRANSFER_INDEX_BYTES)
        index = parse_transfer_index(index_bytes)
        plans = _plan(root, index, profiles, args.geometry, args.max_frames, stack)
        root.visible()
        parent.visible()
        work = stack.enter_context(parent.child(requested_work.name, create=True))
        if report_in_work:
            report_parent = work
        out = {
            "geometry": args.geometry,
            "transfer_index": {"entries": len(index),
                               "bytes": sum(s for s, _ in index.values()),
                               "sha256": sha_bytes(index_bytes)},
            "profiles": {},
        }
        failures = []
        for plan in plans:
            profile = plan["profile"]
            if plan.get("missing"):
                out["profiles"][profile] = {"skipped": "no %s corpus" % args.geometry}
                failures.append("%s: corpus missing (%s)" % (profile, args.geometry))
                print("%-9s SKIPPED (no corpus)" % profile, flush=True)
                continue
            entry = _measure(plan, work, args.encoder, stack)
            out["profiles"][profile] = entry
            if not entry["all_length_equal"]:
                failures.append("%s: packet length differs from the SDK sample" % profile)
            if not entry["all_normalized_equal"]:
                bad = [f["frame_index"] for f in entry["frames"] if not f["normalized_equal"]]
                offsets = sorted({f["first_normalized_diff"] for f in entry["frames"]
                                  if f["first_normalized_diff"] is not None})
                failures.append("%s: %d/%d frames differ from the SDK packet (frames %s, first-diff offsets %s)" %
                                (profile, len(bad), len(entry["frames"]), bad[:8], offsets))
            if not entry["all_tables_match"]:
                failures.append("%s: emitted tables differ from the pinned manifest" % profile)
            print("%-9s len_equal=%s normalized_equal=%s tables_match=%s deltas=%s" %
                  (profile, entry["all_length_equal"], entry["all_normalized_equal"],
                   entry["all_tables_match"], entry["byte_deltas"][:6]), flush=True)
        out["status"] = "FAIL" if failures else "PASS"
        out["failures"] = failures
        work.visible()
        if report_parent is not None:
            report_parent.publish(report_name, (json.dumps(out, indent=1) + "\n").encode("utf-8"))
            print("wrote complete %s report" % out["status"])
        if failures:
            for line in failures:
                print("FAIL " + line, file=sys.stderr)
            return 1
        print("p6_writer_parity: PASS (%d profiles, %d frames, index %d entries)" %
              (len(out["profiles"]), sum(e["requested_frames"] for e in out["profiles"].values()), len(index)))
        return 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--corpus", required=True, type=Path)
    p.add_argument("--encoder", required=True, type=Path)
    p.add_argument("--geometry", default="1080p-16f")
    p.add_argument("--profiles", default=",".join(PROFILES))
    p.add_argument("--max-frames", type=int, default=16)
    p.add_argument("--work", required=True, type=Path,
                   help="new run directory under an existing parent; never reused")
    p.add_argument("--report", type=Path,
                   help="new report path outside corpus, or a top-level file in the new work directory")
    args = p.parse_args()
    try:
        return _run(args)
    except OSError:
        raise SystemExit("filesystem operation failed; existing outputs were not replaced") from None
    except (ValueError, KeyError, TypeError) as error:
        # Do not print OSError filenames or child output into public evidence.
        raise SystemExit("invalid input or output contract: " + str(error)) from None


if __name__ == "__main__":
    sys.exit(main())
