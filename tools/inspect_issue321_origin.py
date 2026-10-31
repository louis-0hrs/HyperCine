# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Inspect current retained SDK source/object links; no historical attestation.

COFF archive layout: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
Private member paths are used locally and never included in the output.
"""
import argparse
import hashlib
import json
import ntpath
from pathlib import Path
import subprocess
import sys

BASELINE = '11574d0295771edccadd17af14af74a539f924c7'


def require(ok):
    if not ok:
        raise ValueError('invalid origin evidence')


def archive_objects(data):
    require(data[:8] == b'!<arch>\n')
    at, longnames, objects = 8, b'', []
    while at < len(data):
        header = data[at:at + 60]
        require(len(header) == 60 and header[58:60] == b'`\n')
        length = header[48:58].strip()
        require(length.isdigit())
        size = int(length)
        payload = data[at + 60:at + 60 + size]
        require(len(payload) == size)
        name = header[:16].decode('ascii').rstrip()
        if name == '//':
            require(not longnames)
            longnames = payload
        elif name != '/':
            if name.startswith('/'):
                require(name[1:].isdigit())
                start = int(name[1:])
                require(start < len(longnames) and
                        (start == 0 or longnames[start - 1] == 0))
                end = longnames.find(b'\0', start)
                require(end >= start)
                name = longnames[start:end].decode('utf-8')
            else:
                name = name.rstrip('/')
            require(bool(name) and len(payload) >= 20)
            objects.append((name, payload))
        at += 60 + size
        if at % 2:
            require(data[at:at + 1] == b'\n')
            at += 1
    require(at == len(data) and bool(objects))
    return objects


def inspect(source_root, compile_record, library):
    def git(*args):
        return subprocess.check_output(['git', *args], stderr=subprocess.PIPE)

    names = git('ls-tree', '-r', '--name-only', BASELINE).decode().splitlines()
    lookup = {name.casefold(): name for name in names}
    require(len(lookup) == len(names))
    inputs = [Path(p) for line in compile_record.read_text(encoding='utf-16').splitlines()
              if line.startswith('^') for p in line[1:].split('|')]
    require(bool(inputs) and len(set(inputs)) == len(inputs))
    sources = []
    for path in inputs:
        relative = path.relative_to(source_root / 'upstream').as_posix()
        name = lookup[relative.casefold()]
        baseline = git('show', BASELINE + ':' + name)
        actual = path.read_bytes()
        sources.append({'source': name,
                        'current_sha256': hashlib.sha256(actual).hexdigest(),
                        'baseline_blob_sha256': hashlib.sha256(baseline).hexdigest(),
                        'raw_equal': actual == baseline,
                        'lf_normalized_equal': actual.replace(b'\r\n', b'\n') ==
                                               baseline.replace(b'\r\n', b'\n')})
    data = library.read_bytes()
    members = []
    for index, (name, payload) in enumerate(archive_objects(data)):
        path = Path(name)
        if not path.is_absolute():
            candidates = list((library.parents[2] /
                               'upstream/CFHDCodecStatic.dir/Release').rglob(ntpath.basename(name)))
            require(len(candidates) == 1)
            path = candidates[0]
        exists = path.is_file()
        members.append({'member_index': index, 'bytes': len(payload),
                        'sha256': hashlib.sha256(payload).hexdigest(),
                        'machine': int.from_bytes(payload[:2], 'little'),
                        'retained_object_exists': exists,
                        'retained_object_equal': exists and path.read_bytes() == payload})
    return {'baseline_commit': BASELINE, 'compile_record_sha256': hashlib.sha256(
                compile_record.read_bytes()).hexdigest(),
            'archive_sha256': hashlib.sha256(data).hexdigest(),
            'sources': sources, 'members': members,
            'scope': 'Current retained source/object observations; not historical source-to-binary attestation.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('sdk-source-root', 'compile-record', 'library', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    try:
        report = inspect(args.sdk_source_root, args.compile_record, args.library)
        with args.output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write('\n')
        print('Origin observations written; inspect equality fields and scope.')
        return 0
    except (OSError, ValueError, KeyError, UnicodeError, subprocess.SubprocessError):
        print('FAIL origin inspection; private diagnostics suppressed', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
