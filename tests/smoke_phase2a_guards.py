# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Manual native guard acceptance; loose synthetic captures stay in a new directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir()  # Never overwrite previous evidence.
    capture = args.output_dir.resolve() / "capture"
    env = dict(os.environ, HYPERCINE_PHASE2A_CAPTURE_DIR=str(capture),
               HYPERCINE_PHASE2A_DECODE_REPEATS="1", HYPERCINE_PHASE2A_FRESH_PROCESSES="0")
    rows = []

    def run(name, arguments, expected, marker, capture_env=False):
        result = subprocess.run([str(args.exe.resolve())] + arguments,
                                env=env if capture_env else None,
                                capture_output=True, text=True, timeout=180)
        output = result.stdout + result.stderr
        (args.output_dir / (name + ".log")).write_text(output, encoding="utf-8")
        ok = result.returncode == expected and marker in output
        rows.append(dict(case=name, exit=result.returncode, expected_exit=expected, passed=ok))
        if not ok:
            raise RuntimeError(name + " failed; inspect its local log")

    def hashes():
        return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(capture.iterdir())}

    try:
        run("new_capture", [], 77, "captured immutable packets", True)
        before = hashes()
        if set(before) != {"phase2a_host.cfhd", "phase2a_device.cfhd"}:
            raise RuntimeError("expected exactly two packets")
        packet = capture / "phase2a_host.cfhd"
        run("valid_decode", ["--decode", str(packet)], 0, "packet decode open=0 prepare=0 decode=0")
        oversized = args.output_dir.resolve() / "oversized.cfhd"
        oversized.write_bytes(bytes(2 * 1024 * 1024 + 1))
        run("oversized", ["--decode", str(oversized)], 1, "cannot read packet file")
        altered = bytearray(packet.read_bytes())
        # CODEC_TAG_FRAME_WIDTH=20 (codec.h); mutate the single 1920-wide
        # aligned header word only, keeping all coefficient bytes unchanged.
        offsets = [i for i in range(0, min(256, len(altered)) - 3, 4)
                   if altered[i:i+4] == bytes.fromhex("00140780")]
        if len(offsets) != 1:
            raise RuntimeError("synthetic width header not uniquely found")
        altered[offsets[0]+2:offsets[0]+4] = (1280).to_bytes(2, "big")
        wrong = args.output_dir.resolve() / "wrong_geometry.cfhd"
        wrong.write_bytes(altered)
        run("wrong_geometry", ["--decode", str(wrong)], 1,
            "packet is not the expected 1920x1080 synthetic fixture")
        run("existing_capture", [], 1, "capture directory must be new and writable", True)
        if hashes() != before:
            raise RuntimeError("immutable capture changed")
        data = dict(passed=True, cases=rows, immutable_packet_sha256=before,
                    capture_unchanged=True, pixel_oracle="NOT_QUALIFIED")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        data = dict(passed=False, cases=rows, error=str(error))
    (args.output_dir / "summary.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(data, indent=2))
    return 0 if data["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
