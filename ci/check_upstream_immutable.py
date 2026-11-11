#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Verify committed, indexed and materialized bytes of the frozen GoPro SDK.

The tree pin is the root tree of original SDK commit
11574d0295771edccadd17af14af74a539f924c7. Comparing the tree directly also works
in a shallow checkout, where the original SDK commit need not be present.
No Git objects, index entries or worktree files are written by this verifier.
"""
import argparse
import os
from pathlib import Path
import stat
import subprocess
import sys

FROZEN_UPSTREAM_TREE = '837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8'


def _git(repo, *args, input_data=None):
    env = dict(os.environ, GIT_NO_REPLACE_OBJECTS='1', GIT_OPTIONAL_LOCKS='0')
    try:
        result = subprocess.run(
            ['git', '-C', str(repo), '-c', 'core.fsmonitor=false',
             '-c', 'core.ignorecase=false', *args],
            input=input_data, capture_output=True, env=env, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError('frozen upstream Git verification unavailable') from error
    if result.returncode:
        # Git errors can contain local paths or user-controlled filenames.
        raise ValueError('frozen upstream Git verification failed')
    return result.stdout


def verify_upstream(repo, *, expected_tree=FROZEN_UPSTREAM_TREE):
    """Reject reference drift; expected_tree is injectable only for Git fixtures."""
    repo = Path(repo).resolve()
    root = os.fsdecode(_git(repo, 'rev-parse', '--show-toplevel')).strip()
    if Path(root).resolve() != repo:
        raise ValueError('frozen upstream verifier requires the repository root')
    tree = _git(repo, 'rev-parse', '--verify', 'HEAD:upstream').strip().decode('ascii')
    if tree != expected_tree:
        raise ValueError('committed upstream tree differs from the frozen SDK')

    entries = []
    expected_index = bytearray()
    listing = _git(repo, 'ls-tree', '-rz', '--full-tree', 'HEAD:upstream')
    for record in listing.split(b'\0'):
        if not record:
            continue
        metadata, relative = record.split(b'\t', 1)
        mode, kind, oid = metadata.split(b' ')
        # The original pin contains regular files only. Keep the worktree
        # verification closed to submodules, symlinks and ambiguous path input.
        if (mode not in (b'100644', b'100755') or kind != b'blob'
                or b'\n' in relative or b'\r' in relative):
            raise ValueError('unsupported frozen upstream tree entry')
        path = b'upstream/' + relative
        entries.append((path, oid, mode))
        expected_index.extend(mode + b' ' + oid + b' 0\t' + path + b'\0')
    if not entries:
        raise ValueError('frozen upstream tree is empty')
    index = _git(repo, 'ls-files', '--stage', '-z', '--', 'upstream/')
    if index != expected_index:
        raise ValueError('upstream index differs from the frozen SDK')

    # Do not trust index stat caches, assume-unchanged or skip-worktree flags.
    # Never run clean filters or diff drivers: they can conceal changed bytes.
    filemode = _git(repo, 'config', '--type=bool', '--default=true',
                    '--get', 'core.filemode').strip() == b'true'
    for path, _, mode in entries:
        candidate = repo / os.fsdecode(path)
        try:
            actual_mode = candidate.lstat().st_mode
            if not stat.S_ISREG(actual_mode):
                raise ValueError('upstream worktree contains a non-regular file')
            if filemode and bool(actual_mode & stat.S_IXUSR) != (mode == b'100755'):
                raise ValueError('upstream worktree executable mode differs from the frozen SDK')
            parent = candidate.parent
            while parent != repo:
                if not stat.S_ISDIR(parent.lstat().st_mode):
                    raise ValueError('upstream worktree contains a linked directory')
                parent = parent.parent
        except OSError as error:
            raise ValueError('upstream worktree is incomplete') from error
    paths = b'\n'.join(path for path, _, _ in entries) + b'\n'
    expected = [oid for _, oid, _ in entries]
    hashes = _git(repo, 'hash-object', '--no-filters', '--stdin-paths',
                  input_data=paths).splitlines()
    if len(hashes) != len(expected):
        raise ValueError('upstream worktree hash inventory is incomplete')
    for (path, want, _), raw in zip(entries, hashes):
        if want == raw:
            continue
        # The original tree mixes LF and CRLF text. Allow only that explicit
        # checkout difference, comparing actual bytes to the pinned blob, not
        # to output of an attribute-selected clean filter. Binary changes fail.
        original = _git(repo, 'cat-file', 'blob', want.decode('ascii'))
        materialized = (repo / os.fsdecode(path)).read_bytes()
        text_controls = {9, 10, 12, 13}
        if (any(byte < 32 and byte not in text_controls
                for data in (original, materialized) for byte in data)
                or original.replace(b'\r\n', b'\n') != materialized.replace(b'\r\n', b'\n')):
            raise ValueError('upstream worktree bytes differ from the frozen SDK')
    # Include ignored additions too: an ignored header can still affect a build.
    if _git(repo, 'ls-files', '--others', '-z', '--', 'upstream/'):
        raise ValueError('upstream worktree contains untracked files')
    return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        tree = verify_upstream(args.repo)
    except ValueError as error:
        print('FAIL:', error, file=sys.stderr)
        return 1
    except OSError:
        print('FAIL: frozen upstream filesystem verification unavailable', file=sys.stderr)
        return 1
    print('Frozen upstream committed tree, index and worktree: PASS (' + tree + ')')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
