#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Give each optional real-corpus CTest invocation its own retained output run.

The parity tool owns creation and validation of every output path. This adapter
only chooses a fresh name; it neither creates nor removes any caller directory.
The report remains within that run, so repeated CTest executions cannot replace
an earlier packet comparison or input copy.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', required=True, type=Path)
    parser.add_argument('--encoder', required=True, type=Path)
    parser.add_argument('--geometry', default='1080p-16f')
    parser.add_argument('--profiles', default='low,medium,high,filmscan1,filmscan2,filmscan3')
    parser.add_argument('--max-frames', type=int, default=16)
    parser.add_argument('--work-root', required=True, type=Path)
    args = parser.parse_args()
    run = args.work_root / ('p6-parity-' + uuid.uuid4().hex)
    tool = Path(__file__).resolve().parents[1] / 'tools' / 'p6_writer_parity.py'
    return subprocess.call([
        sys.executable, str(tool), '--corpus', str(args.corpus),
        '--encoder', str(args.encoder), '--geometry', args.geometry,
        '--profiles', args.profiles, '--max-frames', str(args.max_frames),
        '--work', str(run), '--report', str(run / 'report.json'),
    ])


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except OSError:
        raise SystemExit('P6 parity runner unavailable')
