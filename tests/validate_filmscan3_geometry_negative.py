#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Force validate_filmscan3.py's real comparison path to reject a wrong geometry.

validate_filmscan3_4k_ffmpeg.py already covers a wrong declared size on the
FFmpeg wrapper. It does not exercise the validator that decides whether an
encode is accepted, so a validator that silently ignored --geometry would still
look green.

This gate feeds wrong geometries into the real entry point and requires a
non-zero exit for each of them. A gate that passes when the validator accepts
these would be reporting coverage it never had.

The positive control runs the same entry point at the manifest geometry and
requires success, so this file cannot pass by refusing everything.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

# (name, geometry, required reason, why it must be rejected)
#
# The reason is required, not decorative: a validator that crashes on a bad
# argument also exits non-zero. Without the reason this gate would pass while
# the geometry contract it claims to protect is simply unreachable.
CASES = [
    ('4k-not-in-manifest', '3840x2160', 'NOT validated',
     'a geometry the manifest does not contain is UNCHECKED, never a pass'),
    ('zero-width', '0x2160', 'positive',
     'the digit shape is not a geometry'),
    ('zero-height', '3840x0', 'positive',
     'a zero dimension is not a geometry'),
    ('absurd-width', '99999999999x2160', 'NOT validated',
     'outside any representable frame'),
]
POSITIVE = '1920x1080'


def run_case(validator, probe, encoder, geometry, directory):
    """Return (exit code, combined output) for one geometry."""
    command = [sys.executable, str(validator), '--probe', str(probe),
               '--encoder', str(encoder), '--output-dir', str(directory),
               '--geometry', geometry]
    completed = subprocess.run(command, text=True, capture_output=True)
    return completed.returncode, (completed.stdout or '') + (completed.stderr or '')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validator', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--encoder', required=True, type=Path)
    args = parser.parse_args()

    for required in (args.validator, args.probe, args.encoder):
        if not required.exists():
            print('FAIL: missing required path: %s' % required, file=sys.stderr)
            return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix='hypercine-fs3-geometry-negative-') as tmp:
        root = Path(tmp)
        for name, geometry, must_say, reason in CASES:
            directory = root / name
            directory.mkdir()
            code, output = run_case(args.validator, args.probe, args.encoder, geometry, directory)
            if code == 0:
                failures.append('%s (%s): validator accepted it; must fail because %s'
                                % (name, geometry, reason))
            elif must_say not in output:
                # Refusing for an unrelated reason is not the contract under test.
                failures.append('%s (%s): rejected without saying %r; it exited %d for '
                                'another reason, so the geometry contract was not exercised'
                                % (name, geometry, must_say, code))

        positive = root / 'positive'
        positive.mkdir()
        code, _ = run_case(args.validator, args.probe, args.encoder, POSITIVE, positive)
        if code != 0:
            failures.append('positive control (%s): validator failed with %d; '
                            'this gate would otherwise pass by refusing everything'
                            % (POSITIVE, code))

    if failures:
        for line in failures:
            print('FAIL: %s' % line, file=sys.stderr)
        return 1

    print('GEOMETRY-NEGATIVE: PASS (%d wrong geometries rejected, positive control accepted)'
          % len(CASES))
    return 0


if __name__ == '__main__':
    sys.exit(main())
