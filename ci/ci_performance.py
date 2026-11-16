#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded hosted setup and timing; never replaces a build or test verdict."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

TOOLS = {'ffmpeg': ('-version', 'ffmpeg'),
         'ffprobe': ('-version', 'ffmpeg'),
         'ninja': ('--version', 'ninja-build')}
MAX_JUNIT_BYTES = 16 * 1024 * 1024
LABELS = ('standalone-build', 'standalone-tests', 'reference-build', 'reference-tests')


def missing_packages():
    """Probe actual executables, not image labels or cached success records."""
    missing = set()
    for tool, (flag, package) in TOOLS.items():
        executable = shutil.which(tool)
        if executable is None:
            missing.add(package)
            continue
        try:
            result = subprocess.run([executable, flag], stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=15, check=False)
        except (OSError, subprocess.TimeoutExpired):
            raise RuntimeError('installed tool probe failed: ' + tool) from None
        if result.returncode:
            raise RuntimeError('installed tool probe failed: ' + tool)
    return sorted(missing)


def prepare_linux(install=False):
    """Reuse functional image tools; install missing packages on hosted CI only."""
    if platform.system() != 'Linux':
        raise RuntimeError('Linux prerequisite setup requires Linux')
    packages = missing_packages()
    if packages:
        if (not install or os.environ.get('GITHUB_ACTIONS') != 'true' or
                os.environ.get('HC_RUNNER_ENVIRONMENT') != 'github-hosted'):
            raise RuntimeError('missing prerequisites; hosted installation not authorized')
        for command in (['sudo', '-n', 'apt-get', 'update'],
                        ['sudo', '-n', 'apt-get', 'install', '--no-install-recommends', '-y'] + packages):
            subprocess.run(command, stdin=subprocess.DEVNULL, check=True, timeout=600)
        if missing_packages():
            raise RuntimeError('prerequisites still missing after installation')
    result = {'setup': 'installed_missing' if packages else 'reused_image_tools',
              'installed_packages': packages, 'probed_tools': sorted(TOOLS)}
    print(json.dumps(result, sort_keys=True), flush=True)
    return result


def junit_timings(path):
    """Read diagnostic timing only; no PASS status is synthesized here."""
    with Path(path).open('rb') as handle:
        raw = handle.read(MAX_JUNIT_BYTES + 1)
    if len(raw) > MAX_JUNIT_BYTES or b'<!DOCTYPE' in raw.upper() or b'<!ENTITY' in raw.upper():
        raise ValueError('invalid timing XML')
    cases = list(ET.fromstring(raw).iter('testcase'))
    if not cases or len(cases) > 10000:
        raise ValueError('invalid timing inventory')
    timings, names = [], set()
    skipped = failed = 0
    for case in cases:
        name = case.attrib['name']
        seconds = float(case.attrib['time'])
        if not name or name in names or not math.isfinite(seconds) or seconds < 0:
            raise ValueError('invalid test timing')
        names.add(name)
        # Do not relay arbitrary XML attributes into public summaries.
        public_name = name if re.fullmatch(r'[A-Za-z0-9_.:+-]{1,180}', name) else (
            'redacted-test-' + hashlib.sha256(name.encode()).hexdigest()[:12])
        timings.append({'name': public_name, 'seconds': seconds})
        skipped += case.find('skipped') is not None or case.get('status') == 'notrun'
        failed += case.find('failure') is not None or case.find('error') is not None
    timings.sort(key=lambda item: (-item['seconds'], item['name']))
    return {'test_count': len(cases), 'skipped_count': skipped, 'failed_count': failed,
            'summed_test_seconds': sum(item['seconds'] for item in timings),
            'slowest_tests': timings[:10], 'junit_sha256': hashlib.sha256(raw).hexdigest()}


def checkout_sha():
    result = subprocess.run(['git', 'rev-parse', 'HEAD'], capture_output=True,
                            text=True, check=True, timeout=15)
    sha = result.stdout.strip()
    if not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', sha):
        raise ValueError('invalid checkout identity')
    return sha


def measure(command, output, label, junit=None):
    """Inherit normal output and propagate the real child exit code, no retries."""
    output = Path(output)
    output.unlink(missing_ok=True)
    if junit is not None:
        junit = Path(junit)
        if output.resolve() == junit.resolve():
            raise ValueError('timing output and JUnit must be distinct')
        junit.unlink(missing_ok=True)
    if label not in LABELS:
        raise ValueError('unknown measurement label')
    jobs = os.environ.get('HYPERCINE_STANDALONE_CTEST_JOBS', 'unspecified')
    if jobs not in ('1', '2', 'unspecified'):
        raise ValueError('invalid measured CTest parallelism')
    if not command:
        raise ValueError('missing measured command')
    source = checkout_sha()
    start = time.perf_counter()
    result = subprocess.run(command, check=False)
    elapsed = time.perf_counter() - start
    if not math.isfinite(elapsed) or elapsed < 0:
        raise ValueError('invalid elapsed time')
    code = result.returncode if result.returncode >= 0 else 128 - result.returncode
    report = {'schema': 'hypercine-ci-timing/v1', 'status': 'MEASURED' if code == 0 else 'COMMAND_FAILED',
              'label': label, 'checkout_sha': source, 'command_exit_code': code,
              'wall_seconds': elapsed, 'logical_cpus': os.cpu_count(),
              'ctest_parallel_jobs': jobs,
              'helper_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    if junit is not None and junit.is_file():
        try:
            report.update(junit_timings(junit))
        except (ValueError, KeyError, ET.ParseError, OSError):
            report['status'] = 'INVALID_TIMING_DATA'
            code = code or 2
    elif junit is not None:
        report['status'] = 'MISSING_TIMING_DATA'
        code = code or 2
    report['measurement_exit_code'] = code
    # Summed test elapsed time is NOT CPU utilization or a proven speedup.
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as handle:
        json.dump(report, handle, sort_keys=True, indent=2, allow_nan=False)
        handle.write('\n')
    print(json.dumps(report, sort_keys=True, allow_nan=False), flush=True)
    return code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    setup = sub.add_parser('prepare-linux')
    setup.add_argument('--install', action='store_true')
    timing = sub.add_parser('measure')
    timing.add_argument('--output', type=Path, required=True)
    timing.add_argument('--label', choices=LABELS, required=True)
    timing.add_argument('--junit', type=Path)
    timing.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.operation == 'prepare-linux':
        prepare_linux(args.install)
        return 0
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    return measure(command, args.output, args.label, args.junit)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        raise SystemExit('CI setup/timing failed; raw diagnostics withheld.') from None
