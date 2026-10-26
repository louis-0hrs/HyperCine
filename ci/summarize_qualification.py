#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Classify one CTest run without promoting skipped or absent evidence.

Inputs must belong to the same configuration/run: CTest --show-only=json-v1,
--output-junit, and its actual process exit code. PASS describes execution of
the registered contracts, never production readiness or pixel qualification.
Raw logs, executable paths and test output are deliberately not exported.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def summarize(inventory, junit, ctest_exit, required=()):
    registered = {}
    records = []

    def record(name, status, reason):
        records.append(dict(name=name, status=status, reason=reason))

    if inventory.get('kind') != 'ctestInfo' or not isinstance(inventory.get('tests'), list):
        raise ValueError('expected CTest json-v1 inventory')
    for test in inventory['tests']:
        name = test['name']
        if not isinstance(name, str) or not name:
            raise ValueError('invalid inventory test name')
        if name in registered:
            raise ValueError('duplicate inventory test name')
        registered[name] = test
    if not registered:
        record('(inventory)', 'INCONCLUSIVE', 'no registered tests')
    for name in sorted(set(required) - registered.keys()):
        record(name, 'INCONCLUSIVE', 'required test is not registered')

    if junit.tag not in ('testsuite', 'testsuites'):
        raise ValueError('expected JUnit testsuite or testsuites')
    seen = set()
    for case in junit.iter('testcase'):
        name = case.get('name')
        if not name:
            raise ValueError('missing JUnit test name')
        if name in seen:
            raise ValueError('duplicate JUnit test name')
        seen.add(name)
        failed = (case.find('failure') is not None or case.find('error') is not None
                  or case.get('status') == 'fail')
        if failed:
            record(name, 'FAIL', 'CTest reported failure/error')
        elif name not in registered:
            record(name, 'INCONCLUSIVE', 'result has no matching inventory entry')
        elif case.find('skipped') is not None or case.get('status') in ('disabled', 'notrun'):
            props = {p['name']: p.get('value') for p in registered[name].get('properties', [])}
            labels = props.get('LABELS', [])
            # A disabled test is missing evidence, even if it has oracle labels.
            skipped = case.find('skipped')
            returned_77 = skipped is not None and skipped.get('message') == 'SKIP_RETURN_CODE=77'
            if props.get('DISABLED') or case.get('status') == 'disabled' or (
                    case.get('status') == 'notrun' and not returned_77):
                record(name, 'INCONCLUSIVE', 'test was disabled or not run')
            elif 'reference-oracle' in labels and props.get('SKIP_RETURN_CODE') == 77:
                record(name, 'BLOCKED', 'reference gate skipped; no qualified oracle result')
            elif 'gpu-required' in labels and props.get('SKIP_RETURN_CODE') == 77:
                record(name, 'BLOCKED', 'required GPU evidence unavailable')
            elif 'platform-required' in labels and props.get('SKIP_RETURN_CODE') == 77:
                record(name, 'BLOCKED', 'required platform prerequisite unavailable')
            else:
                record(name, 'INCONCLUSIVE', 'skipped test has no classified evidence')
        elif case.get('status') not in (None, 'run'):
            record(name, 'INCONCLUSIVE', 'unknown test execution status')
        else:
            record(name, 'PASS', 'registered test completed successfully')
    for name in sorted(registered.keys() - seen):
        record(name, 'INCONCLUSIVE', 'registered test has no result')
    # A red process cannot be made green by a partial/stale JUnit document.
    if ctest_exit != 0 and not any(r['status'] == 'FAIL' for r in records):
        record('(ctest)', 'FAIL', 'nonzero CTest exit without a recorded failing test')
    if ctest_exit == 0 and any(r['status'] == 'FAIL' for r in records):
        record('(ctest)', 'FAIL', 'zero CTest exit contradicts failing test evidence')
    counts = Counter(r['status'] for r in records)
    status = next((s for s in ('FAIL', 'INCONCLUSIVE', 'BLOCKED') if counts[s]), 'PASS')
    return dict(copyright='Copyright (c) 2026 HyperCine Contributors.',
                license='MIT OR Apache-2.0', schema_version=1, status=status, ctest_exit=ctest_exit,
                scope='CTest contract execution only; no production or pixel qualification',
                counts={s: counts[s] for s in ('PASS', 'FAIL', 'BLOCKED', 'INCONCLUSIVE')},
                tests=records)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', required=True, type=Path)
    parser.add_argument('--junit', required=True, type=Path)
    parser.add_argument('--ctest-exit', required=True, type=int)
    parser.add_argument('--require', action='append', default=[])
    parser.add_argument('--out', type=Path)
    args = parser.parse_args(argv)
    try:
        report = summarize(json.loads(args.inventory.read_text(encoding='utf-8-sig')),
                           ET.parse(args.junit).getroot(), args.ctest_exit, args.require)
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError):
        # Do not leak machine-specific paths or raw logs through exception text.
        print('FAIL: missing, malformed or incompatible qualification evidence', file=sys.stderr)
        return 1
    content = json.dumps(report, indent=2) + '\n'
    if args.out:
        args.out.write_text(content, encoding='utf-8')
    else:
        print(content, end='')
    return {'PASS': 0, 'FAIL': 1, 'BLOCKED': 77, 'INCONCLUSIVE': 2}[report['status']]


if __name__ == '__main__':
    sys.exit(main())
