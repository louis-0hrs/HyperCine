#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Partial syntax evidence only; never a VC-5 conformance oracle."""
import argparse
import hashlib
import json
import os
import stat
import re
import sys

MAX_BYTES = 4 * 1024 * 1024
MAX_SEGMENTS = 65536
MAX_OUTPUT = 32768
PARTS = (1, 3, 4, 5, 6, 7, 10)
EDITIONS = {1: '2017', 3: '2015', 4: '2015', 5: '2015', 6: '2015', 7: '2022', 10: '2017'}
# Independently authored first-slice rules, derived from the integrated audit.
BASE = {12, 14, 20, 21, 35, 48, 53, 62, 101, 104, 105, 109}
IMAGE = {84, 102, 106, 107, 108}
REQUIRED = {12, 20, 21}
POSITIVE = {12, 14, 20, 21, 35, 53, 101, 102, 104, 105, 106, 107, 108}

# Refusal is a supported-subset outcome, not automatically a normative failure.
RESOURCE_REFUSALS = {'INPUT_BUDGET', 'SEGMENT_BUDGET', 'OUTPUT_BUDGET', 'REGULAR_FILE_REQUIRED'}
DEMONSTRATED_FAILURES = {'TRUNCATED_FRAMING', 'UNKNOWN_FRAMING', 'TRUNCATED_SEGMENT', 'MISSING_HEADER', 'RESERVED_LARGE_CHUNK', 'CHUNK_BOUNDS', 'DUPLICATE_HEADER', 'HEADER_RANGE', 'CHANNEL_STATE', 'SUBBAND_STATE', 'MISSING_BODY'}

class Refusal(ValueError):
    """Safe fixed-code error; never include input/paths in diagnostics."""


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's message may contain arbitrary argv, paths or credentials.
        raise Refusal('CLI_ARGUMENTS')


def require(condition, code):
    if not condition:
        raise Refusal(code)


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def identity(value, length):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{%d}' % length, value) is not None


def validate_context(c):
    require(type(c) is dict and set(c) == {'claim', 'acceptance_claim', 'parts', 'build_parts', 'runtime_parts', 'source', 'decoder'}, 'CONTEXT_FIELDS')
    require(c['claim'] in ('vc5', 'legacy-cfhd', 'identify'), 'CLAIM')
    require(c['acceptance_claim'] in ('encoder', 'decoder'), 'ACCEPTANCE_CLAIM')
    for name in ('parts', 'build_parts', 'runtime_parts'):
        v = c[name]
        require(type(v) is list and len(v) <= len(PARTS) and all(integer(x, 1, 10) and x in PARTS for x in v) and len(set(v)) == len(v), 'PARTS')
    require(1 in c['parts'], 'PART1_REQUIRED')
    s = c['source']
    require(type(s) is dict and set(s) == {'commit', 'tree', 'dirty', 'build_sha256', 'executable_sha256'}, 'SOURCE_FIELDS')
    require(identity(s['commit'], 40) and identity(s['tree'], 40) and type(s['dirty']) is bool and identity(s['build_sha256'], 64) and identity(s['executable_sha256'], 64), 'SOURCE_IDENTITY')
    d = c['decoder']
    require(type(d) is dict and set(d) == {'state', 'qualified', 'executable_sha256', 'version_id', 'configuration_sha256', 'warnings', 'errors', 'exit_code', 'pixel_format', 'output_sha256', 'materials_available', 'inventory_complete', 'pixels_equal'}, 'DECODER_FIELDS')
    require(d['state'] in ('not_run', 'completed', 'unavailable') and type(d['qualified']) is bool, 'DECODER_STATE')
    for k in ('materials_available', 'inventory_complete', 'pixels_equal'):
        require(type(d[k]) is bool, 'DECODER_BOOLEAN')
    require(integer(d['warnings'], 0, 1000000) and integer(d['errors'], 0, 1000000), 'DECODER_COUNTS')
    require(d['exit_code'] is None or integer(d['exit_code'], -255, 255), 'DECODER_EXIT')
    require(d['pixel_format'] in ('unknown', 'rg48', 'b64a', 'byr4', 'nv12', 'yuv422p10le'), 'PIXEL_FORMAT')
    for k in ('executable_sha256', 'configuration_sha256', 'output_sha256'):
        require(d[k] is None or identity(d[k], 64), 'DECODER_IDENTITY')
    require(d['version_id'] is None or (isinstance(d['version_id'], str) and re.fullmatch('[A-Za-z0-9_.-]{1,64}', d['version_id']) is not None), 'DECODER_VERSION')
    if d['state'] == 'completed':
        require(d['exit_code'] is not None and all(d[k] is not None for k in ('executable_sha256', 'configuration_sha256', 'output_sha256', 'version_id')) and d['pixel_format'] != 'unknown', 'COMPLETED_IDENTITY')


def inspect_bytes(data, parts):
    require(type(data) is bytes and len(data) <= MAX_BYTES, 'INPUT_BUDGET')
    require(len(data) >= 4, 'TRUNCATED_FRAMING')
    if data[:4] != b'VC-5':
        if data[:4] == bytes.fromhex('00010009'):
            return {'dialect': 'legacy-cfhd', 'status': 'NOT_APPLICABLE', 'reason': 'LEGACY_FRAMING_ONLY', 'segments': 1, 'chunks': 0, 'prescale': None}
        raise Refusal('UNKNOWN_FRAMING')
    require(len(data) % 4 == 0, 'TRUNCATED_SEGMENT')
    header = True
    fields = {}
    chunks = segments = 0
    offset = 4
    while offset < len(data):
        segments += 1
        require(segments <= MAX_SEGMENTS, 'SEGMENT_BUDGET')
        tag = int.from_bytes(data[offset:offset+2], 'big')
        value = int.from_bytes(data[offset+2:offset+4], 'big')
        tag = tag if tag < 32768 else 65536 - tag
        # Full 16-bit tags in header: 0x006A is PatternWidth, NOT 0x6Axxxxxx.
        is_chunk = 0x4000 <= tag <= 0x40ff or 0x6000 <= tag <= 0x6fff
        if is_chunk:
            if header:
                require(REQUIRED <= fields.keys(), 'MISSING_HEADER')
                header = False
            if tag >= 0x6000:
                require(tag >> 8 == 0x60, 'RESERVED_LARGE_CHUNK')
                size = ((tag & 255) << 16) | value
            else:
                size = value
            end = offset + 4 + size * 4
            require(end <= len(data), 'CHUNK_BOUNDS')
            chunks += 1
            offset = end
            continue
        require(header, 'BODY_TAG_UNSUPPORTED')
        require(tag in BASE or (3 in parts and tag in IMAGE), 'HEADER_TAG')
        require(tag not in fields, 'DUPLICATE_HEADER')
        require(tag != 84 or 1 <= value <= 4, 'IMAGE_FORMAT_RANGE')
        require(tag not in POSITIVE or value > 0, 'HEADER_RANGE')
        require(tag not in (101, 102, 35) or value <= 16, 'PRECISION_RANGE')
        require(tag != 12 or value <= 4, 'CHANNEL_RANGE')
        require(tag != 62 or (12 in fields and value < fields[12]), 'CHANNEL_STATE')
        require(tag != 48 or (14 in fields and value < fields[14]), 'SUBBAND_STATE')
        fields[tag] = value
        offset += 4
    require(REQUIRED <= fields.keys(), 'MISSING_HEADER')
    require(chunks > 0, 'MISSING_BODY')
    prescale = None
    if 109 in fields:
        v = fields[109]
        prescale = {'raw': v, 'table_b1': [(v >> s) & 15 for s in (0, 4, 8)], 'reference_layout': [(v >> s) & 3 for s in (14, 12, 10)], 'status': 'UNQUALIFIED', 'reason': 'UNRESOLVED_PACKING_INTERPRETATION'}
    return {'dialect': 'vc5', 'status': 'PASS', 'reason': 'PARTIAL_STRUCTURE_ONLY', 'segments': segments, 'chunks': chunks, 'prescale': prescale}


def evidence(data, context):
    validate_context(context)
    try:
        syntax = inspect_bytes(data, context['parts'])
    except Refusal as exc:
        reason = str(exc)
        status = 'BLOCKED' if reason in RESOURCE_REFUSALS else 'FAIL' if reason in DEMONSTRATED_FAILURES else 'UNQUALIFIED'
        dialect = 'vc5' if data[:4] == b'VC-5' else 'legacy-cfhd' if data[:4] == bytes.fromhex('00010009') else 'unknown'
        syntax = {'dialect': dialect, 'status': status, 'reason': reason, 'segments': 0, 'chunks': 0, 'prescale': None}
    claim = context['claim']
    mismatch = claim != 'identify' and syntax['dialect'] != 'unknown' and claim != syntax['dialect']
    if mismatch and syntax['status'] != 'FAIL':
        syntax = dict(syntax, status='FAIL', reason='DIALECT_CLAIM_MISMATCH')
    coverage = [{'part': p, 'edition': EDITIONS[p], 'requested': p in context['parts'], 'status': 'MISSING_COVERAGE', 'reason': 'PARTIAL_SYNTAX_ONLY' if p in (1, 3) else 'NOT_IMPLEMENTED'} for p in PARTS]
    d = context['decoder']
    checks = []
    def row(name, status, reason):
        checks.append({'claim': name, 'status': status, 'reason': reason})
    if syntax['status'] in ('FAIL', 'BLOCKED', 'UNQUALIFIED'):
        row('acceptance', syntax['status'], syntax['reason'])
    elif syntax['dialect'] == 'legacy-cfhd':
        row('vc5_acceptance', 'NOT_APPLICABLE', 'LEGACY_DIALECT_NO_VC5_CLAIM')
    else:
        missing = set(context['parts']) - (set(context['build_parts']) & set(context['runtime_parts']))
        if missing:
            row('enabled_parts', 'BLOCKED', 'MISSING_ENABLED_PART')
        if not d['materials_available'] or not d['inventory_complete']:
            row('reference_inventory', 'BLOCKED', 'MISSING_REFERENCE_MATERIAL_OR_INVENTORY')
        if d['state'] != 'completed':
            row('reference_decode', 'BLOCKED', 'REFERENCE_NOT_COMPLETED')
        elif d['warnings'] or d['errors'] or d['exit_code'] != 0:
            row('reference_decode', 'FAIL', 'DECODER_WARNING_OR_ERROR')
        elif not d['qualified']:
            row('reference_decode', 'UNQUALIFIED', 'UNQUALIFIED_REFERENCE')
        elif not d['pixels_equal']:
            row('reference_decode', 'FAIL', 'PIXELS_NOT_EQUAL')
        else:
            row('reference_decode', 'UNQUALIFIED', 'RECORDED_EXTERNAL_ASSERTION_NOT_REPLAYED')
        if syntax['prescale'] is not None:
            row('prescale', 'UNQUALIFIED', 'UNRESOLVED_PACKING_INTERPRETATION')
        row('coverage', 'UNQUALIFIED', 'INCOMPLETE_NORMATIVE_COVERAGE')
    priority = ('FAIL', 'BLOCKED', 'UNQUALIFIED', 'NOT_APPLICABLE', 'PASS')
    verdict = next(x for x in priority if any(c['status'] == x for c in checks))
    with open(__file__, 'rb') as own_source:
        inspector_digest = hashlib.sha256(own_source.read()).hexdigest()
    report = {'schema': 'hypercine.vc5-evidence/1', 'source': context['source'], 'inspector_sha256': inspector_digest, 'wire_claim': claim, 'acceptance_claim': context['acceptance_claim'], 'claim_classes': [{'class': 'VC5_BITSTREAM_CONFORMANCE' if context['acceptance_claim'] == 'encoder' else 'VC5_DECODER_CONFORMANCE', 'status': verdict}], 'procedure': {'document': 'RP 2073-2:2022', 'section': '9.2' if context['acceptance_claim'] == 'encoder' else '9.3', 'metadata': 'MISSING_COVERAGE' if 7 in context['parts'] else 'NOT_REQUESTED', 'errata': 'NOT_VERIFIED'}, 'input': {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}, 'decoder': d, 'enabled_parts': {'build': context['build_parts'], 'runtime': context['runtime_parts']}, 'syntax': syntax, 'coverage': coverage, 'checks': checks, 'verdict': verdict, 'reason': next(c['reason'] for c in checks if c['status'] == verdict)}
    require(len(json.dumps(report)) <= MAX_OUTPUT, 'OUTPUT_BUDGET')
    return report


def read_bounded(path, limit):
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NONBLOCK', 0))
    with os.fdopen(fd, 'rb') as stream:
        require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), 'REGULAR_FILE_REQUIRED')
        content = stream.read(limit + 1)
    require(len(content) <= limit, 'INPUT_BUDGET')
    return content


def unique_object(pairs):
    result = {}
    for k, v in pairs:
        require(k not in result, 'DUPLICATE_JSON_KEY')
        result[k] = v
    return result


def main(argv=None):
    parser = SafeArgumentParser(prog='vc5_inspect', description=__doc__, allow_abbrev=False)
    parser.add_argument('--input', required=True)
    parser.add_argument('--context', required=True)
    try:
        args = parser.parse_args(argv)
        context = json.loads(read_bounded(args.context, 16384), object_pairs_hook=unique_object)
        report = evidence(read_bounded(args.input, MAX_BYTES), context)
        print(json.dumps(report, sort_keys=True, separators=(',', ':')))
        return 1 if report['verdict'] == 'FAIL' else 2 if report['verdict'] == 'BLOCKED' else 3
    except (OSError, ValueError, TypeError, RecursionError):
        print('{"error":"INSPECTION_REFUSED","verdict":"BLOCKED"}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
