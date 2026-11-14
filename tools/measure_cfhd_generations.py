#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded FFmpeg/CFHD control, NOT a HyperCine/GoPro/VC-5 qualification.

Trusted local FFmpeg only; no user media. Native YUV422p10le, 128x64,
film3, fresh single-frame encoder/decoder processes at every generation.
Run the independent unit tests before interpreting a real experiment.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile

WIDTH, HEIGHT = 128, 64
COUNTS = (WIDTH * HEIGHT, WIDTH * HEIGHT // 2, WIDTH * HEIGHT // 2)
FRAME_BYTES = sum(COUNTS) * 2
CHECKPOINTS = (1, 2, 5, 10, 25, 50)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def unpack_frame(raw, counts=COUNTS):
    if len(raw) != sum(counts) * 2:
        raise ValueError("invalid frame length")
    values = tuple(v[0] for v in struct.iter_unpack("<H", raw))
    if any(v > 1023 for v in values):
        raise ValueError("sample outside 10-bit domain")
    planes, start = [], 0
    for count in counts:
        planes.append(values[start:start + count])
        start += count
    return planes


def plane_metrics(reference, observed):
    if not reference or len(reference) != len(observed):
        raise ValueError("mismatched or empty planes")
    errors = [b - a for a, b in zip(reference, observed)]
    mse = sum(e * e for e in errors) / len(errors)
    return {
        "changed_samples": sum(e != 0 for e in errors),
        "max_abs_error": max(abs(e) for e in errors),
        "signed_bias": sum(errors) / len(errors),
        "mse": mse,
        # JSON has no portable numeric infinity; never invent a finite score.
        "psnr_db": "Infinity" if mse == 0 else 10 * math.log10(1023 ** 2 / mse),
    }


def compare_frames(reference, observed, counts=COUNTS):
    a, b = unpack_frame(reference, counts), unpack_frame(observed, counts)
    return {name: plane_metrics(x, y) for name, x, y in zip(("Y", "U", "V"), a, b)}


def run_chain(source, step, generations=50, counts=COUNTS):
    if type(generations) is not int or not 1 <= generations <= 50:
        raise ValueError("generations must be 1..50")
    unpack_frame(source, counts)
    previous, first = source, None
    seen = {sha256(source): 0}
    rows = []
    stable_since = None
    for generation in range(1, generations + 1):
        # Critical: pass the PREVIOUS decoded result, not the original source.
        output, evidence = step(previous, generation)
        unpack_frame(output, counts)
        if first is None:
            first = output
        digest = sha256(output)
        same_previous = output == previous
        stable_since = (generation - 1 if stable_since is None else stable_since) if same_previous else None
        row = {
            "generation": generation,
            "input_sha256": sha256(previous),
            "output_sha256": digest,
            "equals_previous": same_previous,
            "equals_first": output == first,
            "earlier_equal_generation": seen.get(digest),
            "versus_original": compare_frames(source, output, counts),
            "versus_first": compare_frames(first, output, counts),
            "versus_previous": compare_frames(previous, output, counts),
            "component_extrema": {name: {"min": min(p), "max": max(p),
                "at_zero": p.count(0), "at_1023": p.count(1023)}
                for name, p in zip(("Y", "U", "V"), unpack_frame(output, counts))},
            "step_evidence": evidence,
        }
        rows.append(row)
        seen.setdefault(digest, generation)
        previous = output
    return {
        "source_sha256": sha256(source), "generations": generations,
        "initial_lossless": rows[0]["output_sha256"] == sha256(source),
        "no_extra_loss_in_measured_generations": all(r["equals_first"] for r in rows[1:]) if generations >= 2 else None,
        "observed_final_stable_since_generation": stable_since,
        "rows": rows,
    }


def pattern(name):
    rng = random.Random(0x1337BEEF)
    planes = []
    for channel, count in enumerate(COUNTS):
        width = WIDTH if channel == 0 else WIDTH // 2
        low, high = (64, 940) if channel == 0 else (64, 960)
        values = []
        for index in range(count):
            x, y = index % width, index // width
            if name == "flat":
                value = 512
            elif name == "ramp":
                value = low + ((x * 13 + y * 7 + channel * 29) % (high - low + 1))
            elif name == "checker":
                value = high if (x + y) % 2 else low
            elif name == "noise":
                value = rng.randrange(low, high + 1)
            else:
                raise ValueError("unknown pattern")
            values.append(value)
        planes.extend(values)
    return struct.pack("<" + "H" * len(planes), *planes)


class FFmpegControl:
    """Fixed trusted-binary experiment; not a hostile subprocess sandbox."""
    def __init__(self, executable, expected_sha256, directory):
        self.exe = Path(executable).resolve(strict=True)
        if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
            raise ValueError("invalid executable digest")
        if sha256(self.exe.read_bytes()) != expected_sha256:
            raise ValueError("executable digest mismatch")
        self.pin, self.directory = expected_sha256, Path(directory)
        self.decoder_repeat_checks = []

    def command(self, args):
        # Fixed synthetic small inputs, one frame, bounded output and wall time.
        result = subprocess.run([str(self.exe), "-hide_banner", "-loglevel", "error", "-nostdin", "-n"] + args,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=8)
        if result.returncode or result.stderr:
            raise RuntimeError("FFmpeg control failed; raw diagnostics withheld")

    def decode(self, encoded, output):
        self.command(["-threads", "1", "-i", str(encoded), "-map", "0:v:0", "-frames:v", "1",
                      "-pix_fmt", "+yuv422p10le", "-c:v", "rawvideo", "-threads", "1",
                      "-f", "rawvideo", "-fs", str(FRAME_BYTES + 1), str(output)])
        with output.open("rb") as handle:
            raw = handle.read(FRAME_BYTES + 1)
        unpack_frame(raw)
        return raw

    def __call__(self, raw, generation):
        unpack_frame(raw)
        source = self.directory / ("g%02d-input.raw" % generation)
        encoded = self.directory / ("g%02d.avi" % generation)
        decoded = self.directory / ("g%02d-output.raw" % generation)
        source.write_bytes(raw)
        self.command(["-f", "rawvideo", "-pixel_format", "yuv422p10le", "-video_size", "128x64",
                      "-framerate", "30", "-color_range", "tv", "-colorspace", "bt709",
                      "-color_primaries", "bt709", "-color_trc", "bt709", "-chroma_sample_location", "left",
                      "-i", str(source), "-map", "0:v:0", "-frames:v", "1",
                      "-c:v", "cfhd", "-quality", "film3", "-pix_fmt", "+yuv422p10le",
                      "-threads", "1", "-color_range", "tv", "-colorspace", "bt709",
                      "-color_primaries", "bt709", "-color_trc", "bt709", "-chroma_sample_location", "left",
                      "-f", "avi", "-fs", "1048576", str(encoded)])
        output = self.decode(encoded, decoded)
        if generation == 1:
            # Repeated fresh-process decode of the identical immutable container.
            hashes = [sha256(output)]
            for repeat in range(4):
                target = self.directory / ("repeat-%d.raw" % repeat)
                hashes.append(sha256(self.decode(encoded, target)))
            if len(set(hashes)) != 1:
                raise RuntimeError("unstable decoder; control is unqualified")
            self.decoder_repeat_checks = hashes
        data = encoded.read_bytes()
        if len(data) > 1048576:
            raise RuntimeError("encoded file exceeds experiment bound")
        return output, {"avi_sha256": sha256(data), "avi_bytes": len(data),
                        "encoder_quality_option": "film3", "quant_prescale_state": "NOT_MEASURED"}

    def verify_unchanged(self):
        if sha256(self.exe.read_bytes()) != self.pin:
            raise RuntimeError("executable changed during experiment")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffmpeg-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=("flat", "ramp", "checker", "noise"), default="ramp")
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("report already exists")
    report = {
        "schema": "cfhd-generation-control/v1", "status": "CONTROL_ONLY",
        "runner_sha256": sha256(Path(__file__).read_bytes()), "ffmpeg_sha256": args.ffmpeg_sha256,
        "width": WIDTH, "height": HEIGHT, "pixel_format": "yuv422p10le", "range": "tv",
        "matrix": "bt709", "chroma_location": "left", "seed": "0x1337BEEF", "checkpoints": CHECKPOINTS,
        "limits": ["FFmpeg encoder/decoder only; no HyperCine or GoPro result", "no normative VC-5 qualification",
                   "fresh single-frame sessions; no persistent-session/adaptive-sequence proof",
                   "quality option fixed, actual quant/prescale tables NOT_MEASURED",
                   "repeatability is not independent pixel correctness", "binary pinned; dynamic-library hashes not recorded",
                   "no RGB conversion requested; no coefficient-domain instrumentation"],
        "cases": {},
    }
    for name in (args.case,):
        with tempfile.TemporaryDirectory(prefix="cfhd-generation-control-") as directory:
            step = FFmpegControl(args.ffmpeg, args.ffmpeg_sha256, directory)
            report["cases"][name] = run_chain(pattern(name), step)
            step.verify_unchanged()
            report["cases"][name]["fresh_process_decoder_repeat_sha256"] = step.decoder_repeat_checks
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, allow_nan=False)
        handle.write("\n")
    print("FFmpeg CFHD control completed; not a codec conformance verdict.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired):
        raise SystemExit("Control failed; no qualification result claimed.") from None
