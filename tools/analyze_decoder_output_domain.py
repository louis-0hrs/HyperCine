#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Classify decoded BGRA frames by what the *output* looks like.

Diagnostic-only helper for RCA issue #160. It never decodes anything: it reads
already-decoded BGRA dumps plus the modal (most frequent) frame among them and
reports, per colour plane, which of these mutually exclusive shapes the frame
difference has:

  UNCHANGED          plane is byte-identical to the modal frame
  ZEROED             every sample of the plane is exactly 0
  ROW_OFFSET_RANK1   plane = modal + a[y]*b[x] (separable field), AC intact
  ROW_OFFSET_PARTIAL as above, but only a contiguous block of rows is affected
  OTHER              none of the above

The distinction matters for root-causing: "a whole plane is zero" and "the
plane carries a separable low-order field" point at the planar->interleaved
output/conversion stage, while a partially reconstructed wavelet would leave
high-frequency content missing instead.

Pure standard library on purpose (repository rule: no third-party dependencies
in tooling that guards codec behaviour). Subsampling keeps a 1080p sweep in the
seconds range; every reported fraction states its own support.

Usage:
  python3 tools/analyze_decoder_output_domain.py \
      --dumps 'scratch/p*/dec_c00.bgra' --out evidence.json [--width 1920 --height 1080]
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import sys
from array import array
from collections import Counter

PLANE_NAMES = ("B", "G", "R", "A")


def _load_planes(path: str, width: int, height: int, channels: int) -> list[bytes]:
    need = width * height * channels
    with open(path, "rb") as handle:
        data = handle.read(need)
    if len(data) != need:
        raise ValueError(f"{os.path.basename(path)}: short read {len(data)} != {need}")
    return [data[i::channels] for i in range(channels)]


def _frame_digest(planes: list[bytes]) -> str:
    digest = hashlib.sha256()
    for plane in planes[:3]:
        digest.update(plane)
    return digest.hexdigest()


def _delta(corrupt: bytes, reference: bytes) -> array:
    out = array("h", bytes(len(corrupt) * 2))
    for idx in range(len(corrupt)):
        out[idx] = corrupt[idx] - reference[idx]
    return out


def _rank1_fit(delta: array, width: int, height: int) -> dict:
    """Fit delta ~ a[y]*b[x] using row/column means; report the residual.

    For an exactly separable field the row means and column means reproduce it
    up to the global mean, so this needs no linear algebra and no dependency.
    """
    rows = [0.0] * height
    cols = [0.0] * width
    total = 0.0
    for y in range(height):
        base = y * width
        acc = 0
        for value in delta[base:base + width]:
            acc += value
        rows[y] = acc / width
        total += acc
    for x in range(width):
        acc = 0
        for y in range(height):
            acc += delta[y * width + x]
        cols[x] = acc / height
    mean = total / (width * height)
    if abs(mean) < 1e-9:
        return {"separable": False, "reason": "zero mean"}

    step_y = max(1, height // 270)
    step_x = max(1, width // 480)
    errors = []
    for y in range(0, height, step_y):
        a = rows[y]
        base = y * width
        for x in range(0, width, step_x):
            predicted = a * cols[x] / mean
            errors.append(abs(delta[base + x] - predicted))
    errors.sort()
    mae = sum(errors) / len(errors)
    p99 = errors[min(len(errors) - 1, int(0.99 * len(errors)))]
    within1 = sum(1 for e in errors if e <= 1.0) / len(errors)
    return {
        "separable": True,
        "row_mean_rms": round((sum(r * r for r in rows) / height) ** 0.5, 3),
        "column_mean_cv": round(_cv(cols), 4),
        "residual_mae": round(mae, 3),
        "residual_p99": round(p99, 3),
        "fraction_within_1": round(within1, 4),
        "rows_nonzero": sum(1 for r in rows if abs(r) > 0.5),
        "sampled_pixels": len(errors),
    }


def _cv(values: list[float]) -> float:
    if not values:
        return float("nan")
    mean = sum(values) / len(values)
    if abs(mean) < 1e-9:
        return float("nan")
    var = sum((v - mean) ** 2 for v in values) / len(values)
    return (var ** 0.5) / abs(mean)


def _contiguous_prefix(flags: list[bool]) -> bool:
    if not flags or not any(flags):
        return False
    first = flags.index(True)
    last = len(flags) - 1 - flags[::-1].index(True)
    return first == 0 and all(flags[first:last + 1])


def classify_plane(corrupt: bytes, reference: bytes, width: int, height: int) -> dict:
    if corrupt == reference:
        return {"shape": "UNCHANGED"}

    zeros = corrupt.count(0)
    if zeros == len(corrupt):
        return {"shape": "ZEROED", "zero_fraction": 1.0}

    delta = _delta(corrupt, reference)
    fit = _rank1_fit(delta, width, height)

    rows = [0.0] * height
    for y in range(height):
        base = y * width
        acc = 0
        for value in delta[base:base + width]:
            acc += value
        rows[y] = acc / width
    affected = [abs(value) > 0.5 for value in rows]

    separable = False
    if fit.get("separable"):
        amplitude = fit["row_mean_rms"]
        relative = fit["residual_mae"] / amplitude if amplitude > 1e-9 else 1.0
        separable = relative <= 0.10 and fit["fraction_within_1"] >= 0.60
    if separable:
        shape = "ROW_OFFSET_PARTIAL" if _contiguous_prefix(affected) else "ROW_OFFSET_RANK1"
    else:
        shape = "OTHER"

    return {
        "shape": shape,
        "zero_fraction": round(zeros / len(corrupt), 4),
        "affected_rows": sum(affected),
        "rows": height,
        "fit": fit,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dumps", required=True, help="glob of decoded BGRA files")
    parser.add_argument("--out", required=True, help="evidence JSON path")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--channels", type=int, default=4)
    args = parser.parse_args(argv)

    paths = sorted(glob.glob(args.dumps))
    if len(paths) < 2:
        print(f"need at least two dumps, found {len(paths)}", file=sys.stderr)
        return 2

    loaded = []
    for path in paths:
        planes = _load_planes(path, args.width, args.height, args.channels)
        loaded.append((os.path.basename(os.path.dirname(path)), path, planes))

    census = Counter(_frame_digest(planes) for _, _, planes in loaded)
    modal_digest, modal_count = census.most_common(1)[0]
    reference = next(planes for _, _, planes in loaded if _frame_digest(planes) == modal_digest)

    frames = []
    for label, path, planes in loaded:
        digest = _frame_digest(planes)
        record = {
            "id": label,
            "frame_sha256_planes_rgb": digest,
            "is_modal": digest == modal_digest,
            "planes": {},
        }
        if digest != modal_digest:
            for index in range(3):
                record["planes"][PLANE_NAMES[index]] = classify_plane(
                    planes[index], reference[index], args.width, args.height
                )
        frames.append(record)

    shapes = Counter()
    for record in frames:
        for result in record["planes"].values():
            shapes[result["shape"]] += 1

    evidence = {
        "schema": "hypercine.rca160.output_domain.v1",
        "diagnostic_only": True,
        "geometry": {"width": args.width, "height": args.height, "channels": args.channels},
        "calls": len(frames),
        "distinct_frames": len(census),
        "modal_frame_sha256_planes_rgb": modal_digest,
        "modal_share": round(modal_count / len(frames), 4),
        "shape_counts": dict(sorted(shapes.items())),
        "frames": frames,
    }

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(evidence, handle, indent=1, sort_keys=True)
        handle.write("\n")

    print(json.dumps({k: evidence[k] for k in
                      ("calls", "distinct_frames", "modal_share", "shape_counts")}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
