#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
"""Prove the CUDA validator never disturbs a caller's output directory.

The parity test writes into a caller-chosen path. The rule is that nothing the
caller already put there may change: not a sentinel file, not a previous run's
subdirectory, not a symlink. This test drives the real output-directory helpers
from `validate_cuda_filmscan3` directly, so it stays honest about what the
production path actually does, and it needs no GPU.

Deliberately mutation-checked: with `prepare_output_dir` replaced by
`shutil.rmtree`, the sentinel assertions below fail.
"""
import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import validate_cuda_filmscan3 as v  # noqa: E402

checks = 0
failures = []


def check(condition, label):
    global checks
    checks += 1
    if not condition:
        failures.append(label)


def digest_tree(root):
    """Every entry under root, with inode and content hash, symlinks unresolved."""
    snapshot = {}
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root)
        st = os.lstat(path)
        if path.is_symlink():
            snapshot[str(rel)] = ('symlink', st.st_ino, os.readlink(path))
        elif path.is_dir():
            snapshot[str(rel)] = ('dir', st.st_ino, None)
        else:
            snapshot[str(rel)] = ('file', st.st_ino, hashlib.sha256(path.read_bytes()).hexdigest())
    return snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keep', action='store_true', help='leave the scratch tree in place')
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix='cuda-outdir-') as scratch:
        root = Path(scratch)
        output_dir = root / 'out'
        output_dir.mkdir()

        # A caller who already used this directory: sentinels, an earlier run's
        # subdirectory, and a symlink pointing somewhere it must keep pointing.
        (output_dir / 'KEEP_PARENT').write_bytes(b'sentinel-parent\n')
        previous = output_dir / 'cuda-filmscan3-run'
        previous.mkdir()
        (previous / 'KEEP_CHILD').write_bytes(b'sentinel-child\n')
        (previous / 'trace.json').write_bytes(b'{"previous":true}\n')
        real_target = root / 'real-target'
        real_target.mkdir()
        (real_target / 'KEEP_TARGET').write_bytes(b'sentinel-target\n')
        link = output_dir / 'outlink'
        link.symlink_to(real_target, target_is_directory=True)

        before = digest_tree(root)

        # Two consecutive runs, exactly as a repeated ctest invocation does.
        work_dirs = []
        for _ in range(2):
            v.prepare_output_dir(output_dir)
            work_dirs.append(v.exclusive_work_dir(output_dir))
            v.assert_exclusive_artifacts  # referenced to keep the import honest

        after = digest_tree(root)

        # 1. Every pre-existing entry is byte- and inode-identical, still there.
        preexisting = {k: val for k, val in before.items() if not k.startswith('cuda-filmscan3-')
                       or k == 'outlink' or k.startswith('outlink/')
                       or k == 'real-target' or k.startswith('real-target/')
                       or k == 'out' or k == 'out/KEEP_PARENT'
                       or k == 'out/cuda-filmscan3-run'
                       or k.startswith('out/cuda-filmscan3-run/')}
        for key, val in preexisting.items():
            check(key in after, f'pre-existing entry disappeared: {key}')
            if key in after:
                check(after[key] == val, f'pre-existing entry changed: {key}')

        # 2. The sentinel files specifically, named so a failure is readable.
        check((output_dir / 'KEEP_PARENT').read_bytes() == b'sentinel-parent\n',
              'KEEP_PARENT content changed')
        check((previous / 'KEEP_CHILD').read_bytes() == b'sentinel-child\n',
              'KEEP_CHILD content changed')
        check((previous / 'trace.json').read_bytes() == b'{"previous":true}\n',
              'previous run trace.json was overwritten')
        check((real_target / 'KEEP_TARGET').read_bytes() == b'sentinel-target\n',
              'symlink target content changed')

        # 3. The symlink still points where it did; it was not followed or replaced.
        check(link.is_symlink(), 'output symlink was replaced by a real directory')
        check(os.readlink(link) == str(real_target), 'output symlink target changed')

        # 4. Each run produced its own directory, and they differ.
        check(len(work_dirs) == 2, 'expected two work directories')
        check(work_dirs[0] != work_dirs[1], 'second run reused the first run directory')
        for work in work_dirs:
            check(work.is_dir(), f'work directory missing: {work}')
            check(work.resolve().parent == output_dir.resolve(),
                  f'work directory escaped output dir: {work}')

        # 5. A symlink as the output directory itself is refused, not followed.
        refused = False
        try:
            v.prepare_output_dir(link)
        except ValueError as error:
            refused = 'symlink' in str(error)
        check(refused, 'prepare_output_dir accepted a symlink output directory')
        check((real_target / 'KEEP_TARGET').read_bytes() == b'sentinel-target\n',
              'symlink target changed after the refused call')

        if args.keep:
            print(f'SCRATCH: {scratch}')

    print(f'CHECKS: {checks}')
    if failures:
        for failure in failures:
            print(f'FAIL: {failure}')
        return 1
    print('CUDA-OUTPUT-DIR-NEGATIVE: PASS pre-existing files, subdirectories and '
          'symlinks survive repeated runs unchanged')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print('FAIL:', error)
        raise SystemExit(1)
