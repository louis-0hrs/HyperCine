#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Run a CTest oracle replay in fresh, disposable, exclusive scratch storage.

A fixed CTest destination must not make subsequent runs fail, but neither
may a rerun delete evidence from any prior or foreign invocation.
"""
import argparse
from pathlib import Path
import runpy
import sys
import tempfile


def run_in_scratch(fixtures, ffmpeg, ffprobe, scratch_parent, execute=None):
    """Use only a newly created directory; never inspect/unlink prior receipts."""
    fixture_root = Path(fixtures).resolve()
    parent = Path(scratch_parent)
    if parent.is_symlink() or (parent.exists() and not parent.is_dir()):
        raise ValueError('invalid replay scratch parent')
    resolved_parent = parent.resolve()
    if resolved_parent == fixture_root or fixture_root in resolved_parent.parents:
        raise ValueError('replay scratch overlaps retained input fixtures')
    parent.mkdir(parents=True, exist_ok=True)
    if parent.is_symlink() or not parent.is_dir():
        raise ValueError('invalid replay scratch parent')
    script = Path(__file__).with_name('compare_sdk_oracle.py')
    # No new process supervisor: the existing Python oracle executes here.
    # tempfile owns ONLY the directory it created with exclusive semantics.
    if execute is None:
        execute = runpy.run_path
    with tempfile.TemporaryDirectory(prefix='hc-oracle-', dir=parent) as scratch:
        previous = sys.argv
        try:
            sys.argv = [str(script), '--windows-fixtures', str(fixture_root),
                        '--ffmpeg', str(ffmpeg), '--ffprobe', str(ffprobe),
                        '--output', scratch]
            execute(str(script), run_name='__main__')
        finally:
            sys.argv = previous


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--windows-fixtures', required=True)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--scratch-parent', required=True)
    args = parser.parse_args()
    run_in_scratch(args.windows_fixtures, args.ffmpeg, args.ffprobe, args.scratch_parent)


if __name__ == '__main__':
    try:
        main()
    except Exception:
        # Public CI output must not include private executable or fixture paths.
        raise SystemExit('FAIL: isolated SDK oracle replay rejected') from None
