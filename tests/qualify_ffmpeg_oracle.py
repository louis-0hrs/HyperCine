#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Fail-closed qualification of an external FFmpeg build as a candidate oracle.

Qualification is bound to the three committed 128x128 standalone fixtures and
to an explicit list of measured FFmpeg builds. It proves four things on this
host: the inputs and pinned reference bytes match their manifest, fresh
processes of the candidate decoder agree, their output is byte-identical to the
pinned reference, and the comparison still rejects a different packet and a
mutated packet. It does not qualify any other packet, format, resolution or
GPU path, and it never promotes a release gate by itself.

Exit codes: 0 qualified, 1 control failed, 77 blocked (tool absent, build not on
the measured list, or a build-bound executable whose bytes are not the measured
binary).
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from validate_standalone import avi

# Reuse the existing platform-specific process supervisor. The external
# decoder and probe are untrusted even when their banners are allowlisted.
_REPO_ROOT = str(Path(__file__).resolve().parents[1])
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)
from tools import compat_process

VERSION_BUDGET_SECONDS = 30.0
PROBE_BUDGET_SECONDS = 60.0
DECODE_BUDGET_SECONDS = 120.0
TEXT_LIMIT_BYTES = 64 * 1024
MANIFEST_LIMIT_BYTES = 16 * 1024


WIDTH = 128
HEIGHT = 128
OUTPUT_BYTES = WIDTH * HEIGHT * 4  # yuv422p10le: Y + Cb + Cr, 16-bit LE
PEAK = 1023
PATTERNS = ('flat', 'gradient', 'canonical')
FIXTURE_NAMES = frozenset(
    pattern + suffix for pattern in PATTERNS
    for suffix in ('.bgra', '.cfhd', '.input-p10', '.p10'))
MANIFEST_FIELDS = {'copyright', 'license', 'encoder_commit', 'decoder', 'files'}
ENTRY_FIELDS = {'file', 'bytes', 'sha256'}
# Builds that reproduced the pinned n8.0 reference bytes on a measured host.
# Extend only after re-running this harness and reviewing the new evidence.
#   9.0.2  Windows x64 build (docs/validation/FFMPEG_ORACLE_QUALIFICATION_20261025.md)
#   7.1.5  Debian 13 x86_64 build, gcc 14.2.0-19 (same note, 2026-11-19 section)
QUALIFIED_BUILD_PREFIXES = ('n8.0', '9.0.2')
QUALIFIED_EXACT_BUILDS = frozenset({'7.1.5-0+deb13u1'})
# Executable bytes for exact builds whose evidence is a binary digest, not just
# a banner. Token admission says which build reports which version; this table
# says whether the file on disk is the executable that was actually measured.
# Legacy prefix entries stay token-only because no digest was recorded for them.
QUALIFIED_EXECUTABLE_SHA256 = {
    # docs/validation/FFMPEG_ORACLE_QUALIFICATION_20261025.md, 2026-11-19 and
    # the 2026-11-20 corrective receipt: Debian 13 x86_64 /usr/bin binaries.
    '7.1.5-0+deb13u1': {
        'ffmpeg': 'e8a8d46f5225f3062cec7c07fb145d58ae73c603cb740dcd5bad34bfb54e455a',
        'ffprobe': 'cefc5f81c0b18429cdbf8fa5a89e1e9c759235e10f93f9b329ff1b2fdff26f46',
    },
}
# Pre-registered discriminator control: flip one entropy-payload byte of the
# canonical packet. The offset is fixed here, not chosen per run.
MUTATION_OFFSET = 500
MUTATION_XOR = 0xFF

EXIT_QUALIFIED = 0
EXIT_FAILED = 1
EXIT_BLOCKED = 77


class Blocked(Exception):
    """The candidate oracle cannot be evaluated on this host."""


class Failed(Exception):
    """A bound input or control did not behave as the contract requires."""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    """Reject duplicate JSON keys rather than silently keeping the last one."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key')
        result[key] = value
    return result


def matches_text(value, pattern):
    return isinstance(value, str) and re.fullmatch(pattern, value) is not None


def validate_manifest(manifest):
    reason = 'invalid fixture manifest schema'
    if (not isinstance(manifest, dict) or manifest.keys() != MANIFEST_FIELDS or
            not matches_text(manifest['copyright'],
                             r'Copyright \(c\) [0-9]{4} HyperCine Contributors\.') or
            manifest['license'] != 'MIT OR Apache-2.0' or
            not matches_text(manifest['encoder_commit'], r'[0-9a-f]{40}') or
            not matches_text(manifest['decoder'],
                             r'FFmpeg [A-Za-z0-9_.+-]{1,80} [0-9a-f]{40}') or
            not isinstance(manifest['files'], list) or
            len(manifest['files']) != len(FIXTURE_NAMES)):
        raise Failed(reason)
    declared = {}
    for entry in manifest['files']:
        if not isinstance(entry, dict) or entry.keys() != ENTRY_FIELDS:
            raise Failed(reason)
        name, size, digest = entry['file'], entry['bytes'], entry['sha256']
        if (not isinstance(name, str) or name not in FIXTURE_NAMES or name in declared or
                type(size) is not int or size < 0 or
                not matches_text(digest, r'[0-9a-f]{64}')):
            raise Failed(reason)
        declared[name] = entry
    return declared


def load_manifest(fixtures):
    try:
        with (Path(fixtures) / 'manifest.json').open('rb') as handle:
            raw = handle.read(MANIFEST_LIMIT_BYTES + 1)
        if len(raw) > MANIFEST_LIMIT_BYTES:
            raise Failed('fixture manifest exceeds size limit')
        manifest = json.loads(raw.decode('utf-8'), object_pairs_hook=unique_object)
    except (OSError, ValueError, RecursionError):
        raise Failed('cannot read fixture manifest') from None
    return validate_manifest(manifest)


def load_bound(fixtures, declared, name):
    entry = declared.get(name)
    if entry is None:
        raise Failed('fixture {} is not declared in the manifest'.format(name))
    try:
        data = (Path(fixtures) / name).read_bytes()
    except OSError:
        raise Failed('cannot read fixture {}'.format(name)) from None
    if len(data) != entry['bytes']:
        raise Failed('{} size does not match manifest'.format(name))
    digest = sha256(data)
    if digest != entry['sha256']:
        raise Failed('{} sha256 does not match manifest'.format(name))
    return data


def find_tool(explicit, name):
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise Blocked('{} not found at the given path'.format(name))
        return str(path)
    found = shutil.which(name)
    if not found:
        raise Blocked('{} is not on PATH'.format(name))
    return found


def run_tool(argv, timeout, output_limit, *, candidate=False):
    """Run an external candidate with bounded capture and descendant ownership.

    Both stdout and stderr have the finite output_limit, including on
    Windows. A failed cleanup is never reported as an ordinary codec failure;
    no subprocess output, local path or command is surfaced in CI diagnostics.
    """
    try:
        return compat_process.run_bounded(
            argv, timeout=timeout, stdout_limit=output_limit)
    except compat_process.ToolFailure as error:
        if error.cleanup_failed or error.containment_unconfirmed:
            raise Blocked('external tool containment unconfirmed') from None
        if candidate:
            raise Blocked('candidate decoder unavailable') from None
        if error.primary_category in ('tool launch failed',
                                      'tool containment unavailable'):
            raise Blocked('external tool unavailable') from None
        raise Failed('external tool execution rejected') from None


def parse_banner(banner):
    """Return a bounded printable version token, never raw vendor text."""
    first = banner.splitlines()[0] if banner.splitlines() else ''
    match = re.match(r'^ffmpeg version ([A-Za-z0-9_.+-]{1,80})(?:[ \t]|$)',
                     first)
    if match is None:
        raise Blocked('unrecognized decoder banner')
    return match.group(1)


def build_token(ffmpeg):
    raw = run_tool([ffmpeg, '-version'], VERSION_BUDGET_SECONDS,
                   TEXT_LIMIT_BYTES, candidate=True)
    try:
        banner = raw.decode('utf-8')
    except UnicodeDecodeError:
        raise Blocked('unrecognized decoder banner') from None
    return parse_banner(banner)

def require_qualified_build(token):
    # Exact version token or a build suffix, never a bare string prefix:
    # "9.0.20" and "n8.0.1" are different builds from "9.0.2" and "n8.0".
    matched = (token in QUALIFIED_EXACT_BUILDS or
               any(token == prefix or token.startswith(prefix + '-')
                   for prefix in QUALIFIED_BUILD_PREFIXES))
    if not matched:
        raise Blocked('decoder build is not on the measured list')
    return token


def file_sha256(path, name):
    """Hash one executable without loading it into memory in one block."""
    digest = hashlib.sha256()
    try:
        with open(path, 'rb') as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b''):
                digest.update(chunk)
    except OSError:
        raise Blocked('{} executable sha256 unavailable'.format(name))
    return digest.hexdigest()


def require_measured_executables(token, ffmpeg, ffprobe):
    """Bind a build-bound entry to the measured executable bytes.

    Token admission says which build reports which banner; this says whether the
    file is the executable whose digest the measurement recorded. Both tools are
    hashed, because `ffprobe` is resolved independently and a token-only check
    would admit any probe. Legacy prefix entries return None: their documented
    evidence is a banner, so they stay token-only instead of pretending to a
    binary identity that was never measured.
    """
    expected = QUALIFIED_EXECUTABLE_SHA256.get(token)
    if expected is None:
        return None
    observed = {}
    for name, path in (('ffmpeg', ffmpeg), ('ffprobe', ffprobe)):
        digest = file_sha256(path, name)
        if digest != expected[name]:
            raise Blocked('{} sha256 differs from measured executable'.format(name))
        observed[name] = digest
    return observed


def validate_output(raw):
    """Enforce the normalization contract on one decoded image."""
    if len(raw) != OUTPUT_BYTES:
        raise Failed('decoded {} bytes, contract binds {}'.format(
            len(raw), OUTPUT_BYTES))
    samples = struct.unpack('<{}H'.format(OUTPUT_BYTES // 2), raw)
    if max(samples) > PEAK:
        raise Failed('decoded sample {} exceeds {} bit depth'.format(
            max(samples), PEAK))
    return samples


def probe_stream(ffprobe, container):
    raw = run_tool(
        [ffprobe, '-v', 'error', '-show_streams', '-of', 'json',
         str(container)],
        PROBE_BUDGET_SECONDS, TEXT_LIMIT_BYTES)
    try:
        document = json.loads(raw.decode('utf-8'))
    except (ValueError, RecursionError):
        raise Failed('ffprobe emitted invalid stream JSON') from None
    if not isinstance(document, dict):
        raise Failed('ffprobe emitted invalid stream JSON')
    streams = document.get('streams')
    if not isinstance(streams, list) or len(streams) != 1:
        raise Failed('ffprobe stream inventory mismatch')
    stream = streams[0]
    if not isinstance(stream, dict):
        raise Failed('ffprobe emitted invalid stream JSON')
    observed = (stream.get('codec_name'), stream.get('width'),
                stream.get('height'), stream.get('pix_fmt'))
    expected = ('cfhd', WIDTH, HEIGHT, 'yuv422p10le')
    if observed != expected or type(observed[1]) is not int or type(observed[2]) is not int:
        raise Failed('ffprobe stream contract mismatch')
    return observed


def decode(ffmpeg, ffprobe, packet, workdir, label):
    container = workdir / (label + '.avi')
    try:
        container.write_bytes(avi(packet))
    except OSError:
        raise Failed('cannot write temporary container') from None
    probe_stream(ffprobe, container)
    # Pipe-capture is size-bounded; a .yuv output file and read_bytes()
    # would bypass the supervisor's memory/output limits.
    raw = run_tool(
        [ffmpeg, '-v', 'error', '-xerror', '-i', str(container),
         '-frames:v', '1', '-c:v', 'rawvideo', '-pix_fmt', 'yuv422p10le',
         '-f', 'rawvideo', 'pipe:1'],
        DECODE_BUDGET_SECONDS, OUTPUT_BYTES)
    validate_output(raw)
    return raw

def self_check_rejects_corrupt_output():
    """The adapter must reject data outside its declared normalization."""
    try:
        validate_output(bytes(OUTPUT_BYTES - 1))
    except Failed:
        pass
    else:
        raise Failed('adapter accepted a truncated image')
    samples = [0] * (OUTPUT_BYTES // 2)
    samples[-1] = PEAK + 1
    try:
        validate_output(struct.pack('<{}H'.format(len(samples)), *samples))
    except Failed:
        pass
    else:
        raise Failed('adapter accepted an out-of-range sample')


def qualify(fixtures, ffmpeg, ffprobe, repeats):
    declared = load_manifest(fixtures)
    packets, references = {}, {}
    for pattern in PATTERNS:
        packets[pattern] = load_bound(fixtures, declared, pattern + '.cfhd')
        references[pattern] = load_bound(fixtures, declared, pattern + '.p10')
    self_check_rejects_corrupt_output()

    report = dict(state='QUALIFIED', packets=[], controls=[])
    with tempfile.TemporaryDirectory(prefix='hypercine-oracle-qual-') as temporary:
        workdir = Path(temporary)
        decoded = {}
        for pattern in PATTERNS:
            hashes = []
            for repeat in range(repeats):
                raw = decode(ffmpeg, ffprobe, packets[pattern], workdir,
                             '{}-{}'.format(pattern, repeat))
                hashes.append(sha256(raw))
                decoded.setdefault(pattern, raw)
            distinct = len(set(hashes))
            matches = decoded[pattern] == references[pattern]
            report['packets'].append(dict(
                packet=pattern, packet_sha256=sha256(packets[pattern]),
                reference_sha256=sha256(references[pattern]),
                output_sha256=sha256(decoded[pattern]), distinct=distinct,
                repeats=repeats, reference_match=matches))
            if distinct != 1:
                raise Failed('{} produced {} distinct outputs'.format(
                    pattern, distinct))
            if not matches:
                raise Failed('{} output differs from the pinned reference'.format(
                    pattern))

        # Control 1: the comparison must reject a different packet.
        cross = decoded['flat'] == references['gradient']
        report['controls'].append(dict(
            control='cross_packet', outcome='REJECTED' if not cross else 'ACCEPTED'))
        if cross:
            raise Failed('comparison accepted the wrong packet')

        # Control 2: a mutated packet must not qualify as the pinned reference.
        mutated = bytearray(packets['canonical'])
        if MUTATION_OFFSET >= len(mutated):
            raise Failed('mutation offset {} is outside the packet'.format(
                MUTATION_OFFSET))
        mutated[MUTATION_OFFSET] ^= MUTATION_XOR
        try:
            mutant = decode(ffmpeg, ffprobe, bytes(mutated), workdir, 'mutated')
        except Failed as error:
            report['controls'].append(dict(
                control='mutated_packet', outcome='DECODER_REJECTED',
                detail=str(error)))
        else:
            changed = mutant != references['canonical']
            report['controls'].append(dict(
                control='mutated_packet',
                outcome='PIXELS_DIFFER' if changed else 'ACCEPTED'))
            if not changed:
                raise Failed('mutated packet decoded to the pinned reference')
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixtures', default=str(
        Path(__file__).parent / 'fixtures' / 'standalone'))
    parser.add_argument('--ffmpeg')
    parser.add_argument('--ffprobe')
    parser.add_argument('--repeats', type=int, default=5)
    args = parser.parse_args()
    if args.repeats < 2:
        parser.error('--repeats must be at least 2 for a determinism claim')
    try:
        ffmpeg = find_tool(args.ffmpeg, 'ffmpeg')
        ffprobe = find_tool(args.ffprobe, 'ffprobe')
        token = require_qualified_build(build_token(ffmpeg))
        identity = require_measured_executables(token, ffmpeg, ffprobe)
        report = qualify(args.fixtures, ffmpeg, ffprobe, args.repeats)
    except Blocked as error:
        print('ORACLE_QUALIFICATION_BLOCKED reason={}'.format(error))
        return EXIT_BLOCKED
    except Failed as error:
        print('ORACLE_QUALIFICATION_FAILED reason={}'.format(error))
        return EXIT_FAILED
    # The legacy prefix policy allows vendor suffixes; those are not trusted
    # log fields. Report only the admitted static family or exact build ID.
    report['decoder_build'] = token if identity is not None else next(
        prefix for prefix in QUALIFIED_BUILD_PREFIXES
        if token == prefix or token.startswith(prefix + '-'))
    if identity is None:
        print('ORACLE_QUALIFICATION executable_identity=token-only')
    else:
        print('ORACLE_QUALIFICATION executable_identity=measured '
              'ffmpeg_sha256={ffmpeg} ffprobe_sha256={ffprobe}'.format(**identity))
    print('ORACLE_QUALIFICATION state=QUALIFIED decoder_build={} packets={} repeats={}'.format(
        report['decoder_build'], len(report['packets']), args.repeats))
    for packet in report['packets']:
        print('ORACLE_QUALIFICATION packet={packet} packet_sha256={packet_sha256} '
              'reference_sha256={reference_sha256} output_sha256={output_sha256} '
              'distinct={distinct} reference_match={reference_match}'.format(**packet))
    for control in report['controls']:
        print('ORACLE_QUALIFICATION control={control} outcome={outcome}'.format(
            **{key: value for key, value in control.items() if key != 'detail'}))
    return EXIT_QUALIFIED


if __name__ == '__main__':
    sys.exit(main())
