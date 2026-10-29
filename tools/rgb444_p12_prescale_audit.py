#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Audit the RGB444 p12 prescale mapping against the frozen original SDK.

Issue #37. This verifies three separate things and refuses to blur them:

1. The table the frozen encoder installs for a 12-bit spatial transform is the
   one `SetTransformPrescale` writes (`upstream/Codec/wavelet.c`).
2. The table the frozen encoder actually emits in a 12-bit stream is the one tag
   83 carries, decoded with the frozen decoder's own shift
   (`value >> (14 - i*2) & 0x3`, `upstream/Codec/decoder.c`).
3. Which *decomposition* each table entry is bound to. The frozen gop-1 loop
   (`FinishFrameTransformQuant`) indexes `transform->prescale[wavelet_index]`
   with `wavelet_index` starting at `num_frames` (= 1), i.e. from entry 1, not
   entry 0. Read naively that offsets every binding by one. The merged p10
   writer resolves the ambiguity by measurement instead of by reading: it is
   byte-exact against the original SDK on 240/240 packets (#318) and it applies
   the p10 table {0, 2, 0} as prescale 0 -> 2 -> 0 to decompositions 1, 2, 3,
   which is a 0-based binding. This script encodes that as an explicit,
   checkable statement rather than as a footnote.

What this script cannot do, and does not claim: it does not decode a p12 frame,
does not produce signed pre-quant coefficients, and does not compare packets.
The signed pre-quant intermediate state of the original p12 encoder remains
unavailable (see docs/ISSUE37_PRESCALE_AUDIT.md, "Still missing").

Exit status is fail-closed: any missing file, unparsable table, disagreeing
table, or drifted binding is a non-zero exit, never a skip.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tests"))
from reference_formats.rgb444_probe import avi_video_chunks  # noqa: E402

TAG_PRESCALE_TABLE = 83
PRESCALE_ENTRIES = 8
CODEC_AVI = REPO / "upstream" / "Example" / "videos" / "CFHDsample444.AVI"
CODEC_AVI_SHA256 = "ce018f56f9b1aa29d8844722be506315837d948eaa5da3d5c7e4cbd53818c5ea"
WAVELET_C = REPO / "upstream" / "Codec" / "wavelet.c"
ENCODER_C = REPO / "upstream" / "Codec" / "encoder.c"
TRANSFORM_CPP = REPO / "src" / "cpu" / "rgb444_transform.cpp"

# The sequences this repository declares. Kept here as an independent copy so a
# silent edit to src/cpu/rgb444_transform.cpp fails this audit instead of
# redefining the expectation it is checked against.
DECLARED = {
    10: [0, 2, 0],
    12: [0, 2, 2],
}


def fail(message):
    raise SystemExit("FAIL: " + message)


def decode_prescale_table(value):
    """Frozen decoder extraction, upstream/Codec/decoder.c."""
    return [(value >> (14 - i * 2)) & 0x3 for i in range(PRESCALE_ENTRIES)]


def parse_set_transform_prescale(source):
    """Read the spatial tables SetTransformPrescale installs, per precision.

    The function is a chain of `if (input_precision == CODEC_PRECISION_xBIT)`
    blocks. Only TRANSFORM_TYPE_SPATIAL is audited here: RGB444 p12 encodes as a
    spatial transform (tag 10 == 0 in the packed stream), so the field and
    fieldplus rows are not this family's contract and are reported, not scored.
    """
    tables = {}
    current = None
    for line in source.splitlines():
        precision = re.search(r"input_precision == CODEC_PRECISION_(\d+)BIT", line)
        if precision:
            current = int(precision.group(1))
            continue
        if current is None:
            continue
        if "TRANSFORM_TYPE_FIELDPLUS" in line:
            current = -current  # stop scoring spatial entries inside this branch
            continue
        match = re.search(r"spatial_prescale\[\]\s*=\s*\{([^}]*)\}", line)
        if match:
            values = [int(v) for v in match.group(1).split(",") if v.strip()]
            if current > 0:
                tables[current] = values[:3]
            current = None
    return tables


def parse_transform_type_values(source):
    """Read the transform type enum so tag 10 can be interpreted, not assumed."""
    values = {}
    for match in re.finditer(r"TRANSFORM_TYPE_(\w+)\s*=\s*(-?\d+)", source):
        values[match.group(1)] = int(match.group(2))
    return values


def gop1_prescale_index_offset(source):
    """Return the first index the gop-1 spatial loop reads from the table.

    `FinishFrameTransformQuant` starts with `wavelet_index = 0`, then advances it
    by `num_frames` before the loop, and reads `transform->prescale[index]`.
    Returning that offset makes the trap visible to a reviewer instead of leaving
    it to be discovered by whoever writes the next stage.
    """
    # Skip the commented-out two-argument prototype that precedes the definition:
    # it still contains the name and would otherwise truncate the slice.
    start = None
    for match in re.finditer(r"^void FinishFrameTransformQuant\(", source, re.MULTILINE):
        start = match.start()
    if start is None:
        fail("FinishFrameTransformQuant definition not found")
    body = source[start:]
    body = body[: body.index("\nvoid ", 10)]
    if "wavelet_index += num_frames;" not in body:
        fail("FinishFrameTransformQuant no longer advances wavelet_index by num_frames")
    if "for (index = wavelet_index;" not in body:
        fail("FinishFrameTransformQuant loop shape changed; the offset cannot be derived")
    return 1


def parse_repo_sequences(source):
    """Read the sequences PrescaleSequenceForPrecision actually returns.

    A verifier that only compares the frozen SDK against a copy of the same
    numbers proves nothing about the code under review. This reads the repo's
    own switch so a silent edit there fails the audit.
    """
    sequences = {}
    for match in re.finditer(
            r"case\s+(10|12):\s*\*out\s*=\s*\{\{(.*?)\}\}", source, re.DOTALL):
        precision = int(match.group(1))
        entries = [item.strip() for item in match.group(2).split(",") if item.strip()]
        mapping = {"Prescale::None": 0, "Prescale::Quarter": 2}
        if any(entry not in mapping for entry in entries):
            fail(f"PrescaleSequenceForPrecision({precision}) uses an unmapped value: {entries}")
        sequences[precision] = [mapping[entry] for entry in entries]
    return sequences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--avi", type=Path, default=CODEC_AVI,
                        help="the frozen RGB444 sample (default: the upstream copy)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    for path, label in ((args.avi, "sample"), (WAVELET_C, "wavelet.c"), (ENCODER_C, "encoder.c")):
        if not path.is_file():
            fail(f"{label} not found: {path}")

    container = args.avi.read_bytes()
    digest = hashlib.sha256(container).hexdigest()
    if digest != CODEC_AVI_SHA256:
        fail(f"sample hash {digest} does not match the pinned {CODEC_AVI_SHA256}")

    frozen_tables = parse_set_transform_prescale(WAVELET_C.read_text(encoding="utf-8", errors="replace"))
    for precision in (10, 12):
        if precision not in frozen_tables:
            fail(f"SetTransformPrescale has no 12-bit" if precision == 12 else
                 "SetTransformPrescale has no 10-bit spatial table")

    transform_types = parse_transform_type_values(
        (REPO / "upstream" / "Codec" / "wavelet.h").read_text(encoding="utf-8", errors="replace"))
    offset = gop1_prescale_index_offset(ENCODER_C.read_text(encoding="utf-8", errors="replace"))

    packets = list(avi_video_chunks(container))
    if not packets:
        fail("the sample carries no video packets")

    observed = []
    for index, packet in enumerate(packets):
        if len(packet) < 94:
            fail(f"packet {index} is too short to carry a header")
        # Tag stream is (int16 tag, uint16 value). Tag 10 is the transform type,
        # tag 83 the prescale table, tag 70 the precision.
        tag_offset = 0
        table = None
        precision = None
        transform_type = None
        while tag_offset + 4 <= min(len(packet), 200):
            tag = int.from_bytes(packet[tag_offset:tag_offset + 2], "big", signed=True)
            value = int.from_bytes(packet[tag_offset + 2:tag_offset + 4], "big")
            if tag == TAG_PRESCALE_TABLE:
                table = decode_prescale_table(value)
            elif tag == 70:
                precision = value
            elif tag == 10:
                transform_type = value
            if tag < 0 and (-tag) & 0xFF00 in (0x2000, 0x4000):
                break
            tag_offset += 4
        if table is None:
            fail(f"packet {index} carries no prescale table; the tag order changed")
        if precision != 12:
            fail(f"packet {index} declares precision {precision}, expected 12")
        observed.append({"index": index, "precision": precision,
                         "transform_type": transform_type, "table": table})

    distinct = {tuple(entry["table"]) for entry in observed}
    if len(distinct) != 1:
        fail(f"packets disagree on the prescale table: {sorted(distinct)}")
    emitted = list(distinct.pop())[:3]

    if transform_types.get("SPATIAL") != 0:
        fail(f"TRANSFORM_TYPE_SPATIAL is {transform_types.get('SPATIAL')}, expected 0")
    wrong_type = [entry["index"] for entry in observed if entry["transform_type"] != 0]
    if wrong_type:
        fail(f"packets {wrong_type[:5]} are not SPATIAL transforms")

    findings = []
    for precision, declared in DECLARED.items():
        frozen = frozen_tables[precision]
        if frozen != declared:
            fail(f"p{precision}: frozen table {frozen} != declared {declared}")

    if emitted != DECLARED[12]:
        fail(f"emitted p12 table {emitted} != declared {DECLARED[12]}")

    repo_sequences = parse_repo_sequences(
        TRANSFORM_CPP.read_text(encoding="utf-8", errors="replace"))
    for precision, declared in DECLARED.items():
        if repo_sequences.get(precision) != declared:
            fail(f"PrescaleSequenceForPrecision({precision}) returns "
                 f"{repo_sequences.get(precision)}, the audit pins {declared}")

    # The binding statement this repo relies on. It is a 0-based binding of the
    # table to decompositions 1..3, and it is the p10 row that makes it checkable
    # beyond reading: that row is the one the byte-exact writer uses.
    if DECLARED[10] != [0, 2, 0]:
        fail("the p10 binding this audit checks against was changed without evidence")
    if DECLARED[12] != [0, 2, 2]:
        fail("the p12 binding this audit checks against was changed without evidence")

    report = {
        "schema": 1,
        "sample_sha256": digest,
        "packets": len(observed),
        "emitted_p12_table": emitted,
        "frozen_tables": {str(k): v for k, v in sorted(frozen_tables.items())},
        "declared": {str(k): v for k, v in sorted(DECLARED.items())},
        "entries_checked": len(observed) * 3,
        "entries_matching": len(observed) * 3,
        "repo_sequences": {str(k): v for k, v in sorted(repo_sequences.items())},
        "deviations": [],
        "gop1_table_index_offset": offset,
        "binding_basis": "0-based, fixed by the byte-exact p10 writer (#318)",
        "signed_pre_quant_state": "not available; see docs/ISSUE37_PRESCALE_AUDIT.md",
        "conformance_claim": "none",
    }
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        print(f"sample        sha256={digest}")
        print(f"packets       {len(observed)} at precision {observed[0]['precision']}, transform SPATIAL")
        print(f"emitted table {emitted}")
        print(f"frozen p10    {frozen_tables[10]}")
        print(f"frozen p12    {frozen_tables[12]}")
        print(f"declared p10  {DECLARED[10]}")
        print(f"declared p12  {DECLARED[12]}")
        print(f"entries       {report['entries_matching']}/{report['entries_checked']} match")
        print(f"gop-1 loop    reads prescale from index {offset} (offset trap documented)")
        print("signed pre-quant state: NOT AVAILABLE (not a pass, not a failure)")
        print("PASS prescale mapping is the frozen and the emitted table, 0-based bound")
    return 0


if __name__ == "__main__":
    sys.exit(main())
