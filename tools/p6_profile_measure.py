#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Measure the six SDK profiles' actual per-frame tables from pinned packets.

Read-only. Extracts the band quant/scale tags the SDK wrote and compares them
against the SDK-free calculator (`quality_state_probe`). Nothing here is an
encoder: this only states whether the shared calculator reproduces the bytes
the SDK actually emitted for every profile, and if not, at which slot.
"""
import argparse
import hashlib
import json
import subprocess
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
import reference_sample as sample  # noqa: E402

PROFILES = {"low": 1, "medium": 2, "high": 3, "filmscan1": 4, "filmscan2": 5, "filmscan3": 6}


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 22), b""):
            h.update(block)
    return h.hexdigest()


def walk_tags(data, start=0, end=None, depth=0):
    """Yield (tag, value, offset, chunk_end) in file order, descending into levels.

    Band chunks (0x2000) and metadata chunks (0x4000) carry opaque payload, so
    their bodies are skipped using the same size encoding reference_sample uses.
    Level/channel chunks (0x2100, 0x2200) are containers: the band quant/scale
    tags live inside them, so those must be descended into, never skipped.
    """
    if end is None:
        end = len(data)
    if depth > 5:
        raise ValueError("excessive chunk nesting")
    at = start
    while at + 4 <= end:
        tag, value = struct.unpack_from(">hH", data, at)
        if tag < 0:
            code = -tag
            group = code & 0xFF00
            if group == 0x2000 or group == 0x4000:
                limit = at + 4 + (((code & 255) << 16) | value) * 4 if group == 0x2000 else at + 4 + value * 4
                yield tag, value, at, min(limit, end)
                at = limit
                continue
            if group in (0x2100, 0x2200):
                limit = at + 4 + (((code & 255) << 16) | value) * 4
                limit = min(limit, end)
                yield tag, value, at, limit
                yield from walk_tags(data, at + 4, limit, depth + 1)
                at = limit
                continue
        yield tag, value, at, at + 4
        at += 4


def extract_band_tables(data):
    """Map the packet's own tags onto the calculator's 36-slot band layout.

    Observed writer order per level chunk (verified against the actual bytes):
    tag 38 (level = lv+1) and tag 45 (level scale) precede the `-0x2100` chunk;
    inside it, each high-pass band is `48 b, 72, 49, 50, 51, 52, 53 quant,
    54 scale`. Levels are written 2 -> 1 -> 0 and bands 1 -> 3, while the shared
    calculator indexes `channel*12 + level*4 + band` with level ascending and
    band 0 holding the low-pass. So the packet order must be inverted here; a
    positional comparison would be wrong.

    Returns (quant, scale, level_scale) with `None` where the packet carries no
    value: the low-pass band (band 0) declares its quantization implicitly and
    has no 53/54 tag.
    """
    quant = [None] * 36
    scale = [None] * 36
    level_scale = [None] * 9
    level_of_chunk = {}
    channel, level, band = 0, None, None
    for tag, value, offset, _ in walk_tags(data):
        if tag == 62 and value in (1, 2):
            channel = value
        elif tag == 38:
            level = value - 1
        elif tag == 45 and level is not None:
            level_scale[channel * 3 + level] = value
        elif tag == 48:
            band = value
        elif tag == 53 and level is not None and band is not None:
            quant[channel * 12 + level * 4 + band] = value
        elif tag == 54 and level is not None and band is not None:
            scale[channel * 12 + level * 4 + band] = value
    return quant, scale, level_scale


def probe(binary, profile_enum, width, height, frame_index, limiter, prev_bytes, prev_over):
    out = subprocess.run(
        [str(binary), str(profile_enum), str(width), str(height), str(frame_index),
         str(limiter), str(prev_bytes), str(prev_over)],
        check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--corpus", required=True, type=Path)
    p.add_argument("--probe", required=True, type=Path)
    p.add_argument("--geometry", default="1080p-16f")
    p.add_argument("--report", type=Path)
    args = p.parse_args()

    root = args.corpus.resolve()
    result = {"geometry": args.geometry, "profiles": {}, "inventory": {}}

    # Fail closed on the transfer index before any packet is read.
    index = root / "TRANSFER_SHA256.tsv"
    records = {}
    for line in index.read_text(encoding="ascii").splitlines():
        rel, size, digest = line.split("\t")
        records[rel] = (int(size), digest)
    result["inventory"] = {
        "index_sha256": sha(index),
        "entries": len(records),
        "bytes": sum(s for s, _ in records.values()),
    }

    for profile, enum in PROFILES.items():
        base = root / profile / args.geometry
        if not (base / "manifest.json").is_file():
            result["profiles"][profile] = {"enum": enum, "skipped": "no %s corpus for this profile" % args.geometry}
            print("%-9s SKIPPED: no %s corpus" % (profile, args.geometry), flush=True)
            continue
        manifest = json.loads((base / "manifest.json").read_text())
        frames_dir = base / "frames" / f"{profile}-sequence-1920x1080"
        if not frames_dir.is_dir():
            frames_dir = next((base / "frames").iterdir())
        rows, prev_bytes, limiter, over = [], 0, (8 if enum == 5 else 4 if enum == 6 else 0), 0
        for frame in manifest["frames"]:
            idx = frame["frame_index"]
            raw = (frames_dir / ("frame_%04d" % idx) / "sdk.cfhd").read_bytes()
            rel = str((frames_dir / ("frame_%04d" % idx) / "sdk.cfhd").relative_to(root))
            size, digest = records[rel]
            if len(raw) != size or sha_bytes(raw) != digest:
                raise SystemExit("SDK packet failed its pinned size/SHA: " + rel)
            quants, scales, level_scales = extract_band_tables(raw)
            state = probe(args.probe, enum, frame["width"], frame["height"], idx,
                          limiter, prev_bytes, over)
            # Band 0 (the low-pass) carries no 53/54 tag in the packet: it is
            # declared implicitly. Only the 27 high-pass slots are comparable
            # positionally, so compare exactly those and state band 0 separately.
            high = [c * 12 + lv * 4 + b
                    for c in range(3) for lv in range(3) for b in (1, 2, 3)]
            p_q = {i: quants[i] for i in high}
            p_s = {i: scales[i] for i in high}
            missing_q = [i for i in high if p_q[i] is None]
            missing_s = [i for i in high if p_s[i] is None]
            quant_mismatch = [i for i in high
                              if p_q[i] is not None and p_q[i] != state["quant"][i]]
            scale_mismatch = [i for i in high
                              if p_s[i] is not None and p_s[i] != state["scale"][i]]
            # The manifest states the same tables per frame; it is a third,
            # independent statement written by the exporter, not by this script.
            man_q = frame.get("quant")
            man_s = frame.get("scale")
            row = {
                "frame_index": idx,
                "pattern": frame["pattern"],
                "sdk_bytes": len(raw),
                "sdk_quant_highpass": [p_q[i] for i in high],
                "sdk_scale_highpass": [p_s[i] for i in high],
                "sdk_lowpass_scale": [level_scales[c * 3 + lv]
                                      for c in range(3) for lv in range(3)],
                "calc_quant": state["quant"],
                "calc_scale": state["scale"],
                "calc_limiter": state["limiter"],
                "calc_overbitrate": state["overbitrate"],
                "quant_equal": not quant_mismatch and not missing_q,
                "scale_equal": not scale_mismatch and not missing_s,
                "quant_mismatch_slots": quant_mismatch,
                "scale_mismatch_slots": scale_mismatch,
                "missing_quant_slots": missing_q,
                "missing_scale_slots": missing_s,
                "manifest_quant_equal": (man_q == state["quant"]) if man_q else None,
                "manifest_scale_equal": (man_s == state["scale"]) if man_s else None,
                "packet_equals_manifest": (
                    None if man_q is None else
                    all(p_q[i] == man_q[i] and p_s[i] == man_s[i] for i in high)),
                # The limiter is not stored in the packet; it is only reachable
                # through the tables it produces. Report the feedback chain so a
                # divergence is attributable rather than implied.
                "feedback_chain": {"input_limiter": limiter,
                                   "input_prev_bytes": prev_bytes,
                                   "input_overbitrate": over,
                                   "next_limiter": state["limiter"]},
                "level_scale_tags": list(level_scales),
            }
            rows.append(row)
            prev_bytes, limiter, over = len(raw), state["limiter"], state["overbitrate"]
        result["profiles"][profile] = {
            "enum": enum,
            "manifest_frames": len(manifest["frames"]),
            "quant_all_equal": all(r["quant_equal"] for r in rows),
            "scale_all_equal": all(r["scale_equal"] for r in rows),
            "manifest_quant_all_equal": all(r["manifest_quant_equal"] for r in rows),
            "manifest_scale_all_equal": all(r["manifest_scale_equal"] for r in rows),
            "packet_equals_manifest_all": all(r["packet_equals_manifest"] for r in rows),
            "distinct_quant_vectors": sorted({json.dumps(r["sdk_quant_highpass"])
                                              for r in rows}),
            "limiter_chain": [r["feedback_chain"]["next_limiter"] for r in rows],
            "frames": rows,
        }
        entry = result["profiles"][profile]
        print("%-9s packet==calc quant/scale %s/%s  packet==manifest %s  "
              "distinct quant vectors %d  limiters %s"
              % (profile, entry["quant_all_equal"], entry["scale_all_equal"],
                 entry["packet_equals_manifest_all"],
                 len(entry["distinct_quant_vectors"]), entry["limiter_chain"][:6]),
              flush=True)

    text = json.dumps(result, indent=1, sort_keys=False)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text + "\n")
        print("wrote", args.report)
    else:
        print(text)


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    main()
