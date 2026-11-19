# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Deterministic synthetic VC-5-like packet; never a conformance oracle."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "hypercine.vc5-synthetic-ledger/1"
SOURCE_FILES = (
    "CMakeLists.txt",
    "cmake/vc5_contract_inventory.cmake",
    "ci/run_standalone.py",
    "tests/test_vc5_synthetic_ledger.py",
    "tools/vc5_inspect.py",
    "tools/vc5_synthetic_packet.py",
)
COMMAND = ["python3", "-B", "tools/vc5_inspect.py", "--input", "<packet>", "--context", "<context>"]

def segment(tag, value):
    return tag.to_bytes(2, "big") + value.to_bytes(2, "big")

def packet():
    """Assemble tags and payload independently of the inspector implementation."""
    return b"VC-5" + b"".join((
        segment(12, 3),       # synthetic ChannelCount
        segment(20, 48),      # synthetic Width
        segment(21, 48),      # synthetic Height
        segment(109, 0x2800), # ambiguity sentinel
        segment(0x6000, 1), bytes((0xA5, 0x5A, 0xC3, 0x3C)),
    ))

def working_manifest(root=ROOT):
    """Hash exact source bytes, independent of Git index/staging state."""
    manifest = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                for name in SOURCE_FILES}
    encoded = json.dumps({"command": COMMAND, "files": manifest}, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return manifest, hashlib.sha256(encoded).hexdigest()

def repository_identity(root=ROOT):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(root), *args],
                                       text=True, timeout=3).strip()
    commit = git("rev-parse", "HEAD")
    tree = git("rev-parse", "HEAD^{tree}")
    dirty = bool(git("status", "--porcelain"))
    return commit, tree, dirty

def ledger(root=ROOT):
    data = packet()
    files, manifest_digest = working_manifest(root)
    commit, tree, dirty = repository_identity(root)
    interpreter_digest = hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest()
    return {
        "schema": SCHEMA,
        "scope": "synthetic_partial_structure_only",
        "standard": {"edition": "ST 2073-1:2017", "part": 1,
                     "feature_set": "elementary header/chunk framing subset"},
        "source": {"commit": commit, "committed_tree": tree, "dirty": dirty,
                   "files": files, "manifest_sha256": manifest_digest},
        "build": {"command": COMMAND, "manifest_sha256": manifest_digest,
                  "python_version": sys.version.split()[0],
                  "python_executable_sha256": interpreter_digest},
        "packet": {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                   "hex": data.hex()},
        "expectations": [
            {"field": "framing", "tag": None, "expected": "VC-5"},
            {"field": "ChannelCount", "tag": 12, "expected": 3},
            {"field": "Width", "tag": 20, "expected": 48},
            {"field": "Height", "tag": 21, "expected": 48},
            {"field": "PrescaleShift", "tag": 109, "raw": 0x2800,
             "table_b1": [0, 0, 8], "reference_layout": [0, 2, 2],
             "status": "UNQUALIFIED",
             "reason": "UNRESOLVED_PACKING_INTERPRETATION"},
            {"field": "chunk", "tag": "0x6000", "payload_bytes": 4},
        ],
        "conformance": "BLOCKED_REFERENCE",
        "limits": ["No normative bitstream claim", "No external SMPTE material",
                   "No decoder or pixel conformance claim"],
    }

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packet", action="store_true",
                        help="write the raw synthetic packet to stdout")
    args = parser.parse_args(argv)
    if args.packet:
        sys.stdout.buffer.write(packet())
    else:
        print(json.dumps(ledger(), sort_keys=True, separators=(",", ":")))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
