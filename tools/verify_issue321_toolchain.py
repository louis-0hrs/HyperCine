# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Compare a retained MSVC record with the currently installed compiler.

Runs a separately operator-selected and SHA-256-pinned local compiler with /Bv only.
The retained record is an UNTRUSTED comparison input, never execution authority.
Exit 0: version agrees; 2: measured version drift; 1: probe failed.
Version agreement does not authenticate historical compiler bytes or execution.
Only hashes and versions are published, never paths or compiler diagnostics.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def digest(data):
    return hashlib.sha256(data).hexdigest()


def require(ok):
    if not ok:
        raise ValueError('invalid toolchain observation')


def assignment(text, key):
    values = re.findall(r'^set\(' + re.escape(key) + r' "([^"\r\n]+)"\)\s*$',
                        text, re.MULTILINE)
    require(len(values) == 1)
    return values[0]


def version(text):
    require(re.fullmatch(r'19\.\d+\.\d+(?:\.\d+)?', text) is not None)
    return text if text.count('.') == 3 else text + '.0'


def inspect(record, trusted_compiler, trusted_sha256):
    record_bytes = record.read_bytes()
    text = record_bytes.decode('utf-8')
    require(assignment(text, 'CMAKE_CXX_COMPILER_ID') == 'MSVC')
    expected = version(assignment(text, 'CMAKE_CXX_COMPILER_VERSION'))
    # The retained CMake record is evidence, not permission to execute a path.
    # The operator's independent local selection AND exact current-byte pin are
    # mandatory; neither value is ever derived from the retained record.
    require(re.fullmatch(r'[0-9a-fA-F]{64}', trusted_sha256) is not None)
    require(trusted_compiler.is_absolute() and
            trusted_compiler.name.casefold() == 'cl.exe')
    compiler = trusted_compiler.resolve(strict=True)
    recorded_compiler = Path(assignment(text, 'CMAKE_CXX_COMPILER'))
    require(recorded_compiler.is_absolute() and
            recorded_compiler.name.casefold() == 'cl.exe')
    require(recorded_compiler.resolve(strict=True) == compiler)
    compiler_bytes = compiler.read_bytes()
    require(digest(compiler_bytes) == trusted_sha256.lower())
    result = subprocess.run([str(compiler), '/Bv'], capture_output=True,
                            timeout=30, check=False)
    require(result.returncode in (0, 2))  # cl reports missing input with exit 2.
    output = (result.stdout + result.stderr).decode(errors='replace')
    versions = {version(v) for v in re.findall(r'\b19\.\d+\.\d+(?:\.\d+)?\b', output)}
    require(len(versions) == 1)
    require(record.read_bytes() == record_bytes and compiler.read_bytes() == compiler_bytes)
    actual = versions.pop()
    return {
        'scope': 'Current local compiler probe versus retained CMake version; not historical attestation.',
        'compiler_record_sha256': digest(record_bytes),
        'current_compiler_sha256': digest(compiler_bytes),
        'trust_binding': 'INDEPENDENT_LOCAL_PATH_AND_CURRENT_SHA256',
        'recorded_version': expected,
        'observed_version': actual,
        'version_equal': actual == expected,
        'probe_arguments': ['/Bv'],
        'probe_exit_code': result.returncode,
        'status': 'VERSION_AGREES' if actual == expected else 'TOOLCHAIN_VERSION_DRIFT',
        'historical_compiler_identity': 'NOT_ESTABLISHED',
        'limitations': [
            'The retained record is not an authenticated historical process log.',
            'A current executable hash cannot recover the historical compiler hash.',
            'Matching versions alone do not bind source, headers, commands, environment or binaries.',
            'No compilation, corpus export or historical execution proof is performed.'
        ]
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler-record', type=Path, required=True)
    parser.add_argument('--trusted-compiler', type=Path, required=True,
                        help='independently operator-selected local cl.exe')
    parser.add_argument('--trusted-compiler-sha256', required=True,
                        help='independently verified current cl.exe SHA-256')
    parser.add_argument('--sdk-source-root', type=Path, required=True,
                        help='retained SDK source tree; never write reports inside')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        output = args.output.resolve()
        sdk_root = args.sdk_source_root.resolve()
        require(sdk_root.is_dir() and output != sdk_root and sdk_root not in output.parents)
        golden = os.environ.get('HYPERCINE_GOLDEN_DIR')
        if golden:
            root = Path(golden).resolve()
            require(output != root and root not in output.parents)
        require(not output.exists())
        report = inspect(args.compiler_record, args.trusted_compiler,
                         args.trusted_compiler_sha256)
        with output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write('\n')
        print(report['status'])
        return 0 if report['version_equal'] else 2
    except (OSError, ValueError, UnicodeError, subprocess.SubprocessError):
        print('FAIL toolchain probe; private diagnostics suppressed', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
