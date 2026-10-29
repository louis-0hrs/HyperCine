#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Negative controls for the #46 packet gate (tools/p6_writer_parity.py).

The gate is only worth its green result if it fails on the things it claims to
reject. Each case below builds a deliberately broken input and requires a
non-zero exit:

1. a same-length tampered *original* SDK packet must be rejected on its pinned
   hash, not accepted because the length still matches;
2. a truncated encoder trace must be rejected instead of being zipped short;
3. a reordered encoder trace must be rejected;
4. a same-length tampered *cached* work input must not be trusted: the gate has
   to re-copy from the verified original and still pass.

The baseline case (0) proves the fixture itself is valid, so a failure in 1-3 is
caused by the tampering and not by a broken fixture.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parents[1]
GATE = SOURCE_ROOT / "tools" / "p6_writer_parity.py"


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    return sha_bytes(Path(path).read_bytes())


def build_fixture(corpus_root, destination, profile="filmscan3", frames=1):
    """Mini corpus copied from the verified originals.

    `frames` is 2 for the trace-ordering controls: a one-frame fixture cannot
    express a reordering, so it would silently test nothing.
    """
    source = corpus_root / profile / "1080p-16f"
    manifest = json.loads((source / "manifest.json").read_text())
    selected = manifest["frames"][:frames]
    frames_dir = source / "frames" / ("%s-sequence-1920x1080" % profile)
    if not frames_dir.is_dir():
        frames_dir = next((source / "frames").iterdir())
    for frame in selected:
        frame_dir = frames_dir / ("frame_%04d" % frame["frame_index"])
        target_frames = (destination / profile / "1080p-16f" / "frames"
                         / ("%s-sequence-1920x1080" % profile)
                         / ("frame_%04d" % frame["frame_index"]))
        target_frames.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(frame_dir / "input.bgra", target_frames / "input.bgra")
        shutil.copyfile(frame_dir / "sdk.cfhd", target_frames / "sdk.cfhd")

    mini = dict(manifest)
    mini["frames"] = selected
    target_base = destination / profile / "1080p-16f"
    (target_base / "manifest.json").write_text(json.dumps(mini, indent=1))

    records = []
    for path in sorted(destination.rglob("*")):
        if path.is_file() and path.name != "TRANSFER_SHA256.tsv":
            records.append("%s\t%d\t%s" % (path.relative_to(destination).as_posix(),
                                           path.stat().st_size, sha_file(path)))
    (destination / "TRANSFER_SHA256.tsv").write_text("\n".join(records) + "\n", encoding="ascii")
    return mini


def run_gate(corpus, work, encoder, profile="filmscan3", extra=(), frames=1):
    command = [sys.executable, str(GATE), "--corpus", str(corpus),
               "--encoder", str(encoder), "--geometry", "1080p-16f",
               "--profiles", profile, "--max-frames", str(frames),
               "--work", str(work)] + list(extra)
    done = subprocess.run(command, capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


def write_stub_encoder(path, mode):
    """An encoder stand-in that traces frames in a deliberately wrong order."""
    path.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, sys\n"
        "mode = %r\n" % mode +
        "args = [a for a in sys.argv[1:] if not a.startswith('--')]\n"
        "# positional tail: OUTPUT, then inputs; flags carry their own value, so\n"
        "# recover OUTPUT as the first positional that is not a .bgra path.\n"
        "positional = []\n"
        "skip = False\n"
        "for a in sys.argv[1:]:\n"
        "    if skip:\n"
        "        skip = False\n"
        "        continue\n"
        "    if a == '--geometry':\n"
        "        skip = True\n"
        "        continue\n"
        "    if a == '--quality':\n"
        "        skip = True\n"
        "        continue\n"
        "    if a == '--reference-yuv' or a == '--coefficient-hash':\n"
        "        skip = (a == '--coefficient-hash')\n"
        "        continue\n"
        "    positional.append(a)\n"
        "out = positional[0]\n"
        "inputs = positional[1:]\n"
        "os.makedirs(out, exist_ok=True)\n"
        "frames = []\n"
        "for index in range(len(inputs)):\n"
        "    frames.append({'frame_index': index, 'fs_rate_limiter': 4,\n"
        "                   'fs_overbitrate': 0, 'previous_sample_bytes': 0,\n"
        "                   'sample_bytes': 0, 'quant': [0]*36, 'scale': [0]*36})\n"
        "    open(os.path.join(out, 'frame_%04d.cfhd' % index), 'wb').write(b'')\n"
        "if mode == 'truncate':\n"
        "    frames = frames[:max(0, len(frames) - 1)]\n"
        "elif mode == 'reorder' and len(frames) > 1:\n"
        "    frames[0]['frame_index'], frames[1]['frame_index'] = \\\n"
        "        frames[1]['frame_index'], frames[0]['frame_index']\n"
        "json.dump({'profile': 'stub', 'frames': frames}, open(os.path.join(out, 'trace.json'), 'w'))\n"
    )
    path.chmod(0o755)


def main():
    corpus_root = Path(os.environ["HYPERCINE_QUALITY_ORACLE_ROOT"]).resolve()
    encoder = os.environ.get("HYPERCINE_P6_ENCODER")
    if not encoder:
        raise SystemExit("HYPERCINE_P6_ENCODER must point at hypercine_encode_filmscan3")
    encoder = Path(encoder)

    failures = []
    with tempfile.TemporaryDirectory(prefix="p6-negative-") as tmp:
        tmp = Path(tmp)

        def fresh(name):
            root = tmp / name
            root.mkdir(parents=True, exist_ok=True)
            build_fixture(corpus_root, root)
            return root

        def fresh_two(name):
            root = tmp / name
            root.mkdir(parents=True, exist_ok=True)
            build_fixture(corpus_root, root, frames=2)
            return root

        # 0. Baseline: the fixture is valid, so the gate accepts it.
        base = fresh("baseline")
        code, log = run_gate(base, tmp / "w0", encoder)
        if code != 0:
            failures.append("baseline fixture was rejected: " + log[-500:])

        # 1. Same-length tampered original SDK packet.
        tampered = fresh("tampered-sdk")
        packet = (tampered / "filmscan3" / "1080p-16f" / "frames"
                  / "filmscan3-sequence-1920x1080" / "frame_0000" / "sdk.cfhd")
        raw = bytearray(packet.read_bytes())
        raw[len(raw) // 2] ^= 0xFF        # same length, different content
        packet.write_bytes(bytes(raw))
        code, log = run_gate(tampered, tmp / "w1", encoder)
        if code == 0:
            failures.append("same-length tampered SDK packet was accepted")
        elif "hash mismatch" not in log and "transfer index" not in log:
            failures.append("tampered SDK packet failed for the wrong reason: " + log[-400:])

        # 2. Truncated trace, 3. reordered trace.
        for mode in ("truncate", "reorder"):
            stub = tmp / ("stub-%s.py" % mode)
            write_stub_encoder(stub, mode)
            mini = fresh_two("trace-" + mode)
            code, log = run_gate(mini, tmp / ("w-" + mode), stub, frames=2)
            if code == 0:
                failures.append("%s trace was accepted by the gate" % mode)
            elif "do not match requested" not in log:
                failures.append("%s trace failed for the wrong reason: %s"
                                % (mode, log[-400:]))

        # 4. A tampered cached work input is not trusted: it is re-copied from
        # the verified original, so the run still passes and the cache is healed.
        heal = fresh("cache-heal")
        work = tmp / "w4"
        code, log = run_gate(heal, work, encoder)
        if code != 0:
            failures.append("cache-heal baseline failed: " + log[-400:])
        else:
            cached = work / "filmscan3" / "input_0000.bgra"
            if not cached.is_file():
                failures.append("gate did not leave a cached input to tamper")
            else:
                corrupt = bytearray(cached.read_bytes())
                corrupt[0] ^= 0xFF             # same length again
                cached.write_bytes(bytes(corrupt))
                code, log = run_gate(heal, work, encoder)
                if code != 0:
                    failures.append("gate trusted a same-length tampered cache: " + log[-400:])
                elif sha_file(cached) != sha_file(
                        heal / "filmscan3" / "1080p-16f" / "frames"
                        / "filmscan3-sequence-1920x1080" / "frame_0000" / "input.bgra"):
                    failures.append("gate reused the tampered cache instead of re-copying")

    if failures:
        for line in failures:
            print("FAIL: " + line, file=sys.stderr)
        return 1
    print("p6_gate_negative: PASS (baseline accepted; tampered sdk, truncated trace, "
          "reordered trace rejected; tampered cache healed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
