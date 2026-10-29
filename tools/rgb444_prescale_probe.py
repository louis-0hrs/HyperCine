#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Read the prescale table (CODEC_TAG_PRESCALE_TABLE, tag 83) out of original
CineForm RGB444 p12 samples.

Why this exists: the frozen upstream encoder selects a spatial prescale table
from the *precision* (`upstream/Codec/wavelet.c`, SetTransformPrescale) and then
applies the entry for each spatial level individually (`encoder.c`,
FinishFrameTransformQuant reads `transform->prescale[index]`). For 12-bit input
the spatial table is `{0, 2, 2, ...}`, i.e. levels 1 AND 2 are prescaled; for
10-bit it is `{0, 2, 0, ...}`, only level 1. A p12 encoder that prescales only
level 1 would therefore differ from the original on level 2.

That claim is only worth something if the original sample itself carries the
table. This probe reads tag 83 (written by `codec.c` PutTagPair) from the frozen
sample and prints it — decoding is done with the same bit-extraction the
frozen decoder uses (`decoder.c`: `value >> (14 - i*2) & 0x3`).

It does not encode, decode pixels, or link the SDK. Upstream is read-only.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tests"))
from reference_formats.rgb444_probe import avi_video_chunks  # noqa: E402

TAG_PRESCALE_TABLE = 83
PRECISION_10BIT = 10
PRECISION_12BIT = 12

# Frozen decoder extraction: upstream/Codec/decoder.c reads the tag as
#   prescale[i] = value >> (14 - i * 2) & 0x3
PRESCALE_ENTRIES = 8


def decode_prescale_table(value):
    return [(value >> (14 - i * 2)) & 0x3 for i in range(PRESCALE_ENTRIES)]


def find_prescale_tag(data):
    """Return (raw_value, offset) of the first tag 83, or (None, None).

    Walks the tag stream the same way reference_sample.parse_sample does:
    negative size tags open a container that must be skipped by its declared
    length. Only the sample header level is searched, which is where the frozen
    encoder writes the table (codec.c, before the channel data).
    """
    data = bytes(data)
    at = 0
    end = len(data)
    while at + 4 <= end:
        tag, value = struct.unpack_from(">hH", data, at)
        if tag == TAG_PRESCALE_TABLE:
            return value, at
        code = -tag if tag < 0 else tag
        group = code & 0xFF00
        if tag < 0 and group in (0x2000, 0x2100, 0x2200):
            at = at + 4 + (((code & 255) << 16) | value) * 4
            continue
        if tag == -0x4002:
            at = at + 4 + value * 4
            continue
        if tag == 2:
            at += 4 + 4 * value
            continue
        at += 4
    return None, None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--avi", required=True, type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    blob = args.avi.read_bytes()
    packets = avi_video_chunks(blob)
    report = {
        "container_sha256": hashlib.sha256(blob).hexdigest(),
        "packet_count": len(packets),
        "packets": [],
    }
    for number, packet in enumerate(packets):
        raw, offset = find_prescale_tag(packet)
        precision = None
        # PRECISION tag 70 = CODEC_TAG_PRECISION
        at = 0
        while at + 4 <= len(packet):
            tag, value = struct.unpack_from(">hH", packet, at)
            if tag == 70:
                precision = value
                break
            code = -tag if tag < 0 else tag
            if tag < 0 and (code & 0xFF00) in (0x2000, 0x2100, 0x2200):
                at = at + 4 + (((code & 255) << 16) | value) * 4
                continue
            at += 4
        report["packets"].append({
            "index": number,
            "precision": precision,
            "prescale_raw": raw,
            "prescale_offset": offset,
            "prescale_table": decode_prescale_table(raw) if raw is not None else None,
        })

    if args.json:
        print(json.dumps(report, indent=1, sort_keys=True))
    else:
        print("container", report["container_sha256"][:16], "packets", report["packet_count"])
        for item in report["packets"]:
            print("  #%02d precision=%s raw=%s table=%s" % (
                item["index"], item["precision"], item["prescale_raw"],
                item["prescale_table"]))

    missing = [p["index"] for p in report["packets"] if p["prescale_raw"] is None]
    if missing:
        print("prescale tag absent in packet(s):", missing, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
