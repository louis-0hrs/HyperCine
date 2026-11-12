#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Recover binary evidence from Windows build log; independently decode GoPro control.

Windows never needs FFmpeg. Its real SDK control packet and unmasked pixel planes
are carried in log records and decoded/compared on Mac. No SDK is emulated.

Every external tool (ffmpeg/ffprobe) is spawned through run_bounded(): a hard
wall budget, a stdout capture that is capped while it is produced, and a
fail-closed kill that reaps the child instead of leaving an orphan behind.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import re
import signal
import subprocess
import threading
import time
from pathlib import Path
from validation_support import require, hex_digest, p10_samples
from validate_standalone import avi, reference


# Conservative wall budgets for CI machines: a 128x128 one-frame replay needs
# well under a second locally, so these only fire on a genuinely stuck tool.
VERSION_BUDGET_SECONDS = 30.0
PROBE_BUDGET_SECONDS = 60.0
DECODE_BUDGET_SECONDS = 120.0
# ffprobe publishes one stream as a few hundred bytes; never buffer a runaway.
STDOUT_LIMIT_BYTES = 64 * 1024
STDOUT_CHUNK_BYTES = 8 * 1024
PROCESS_POLL_SECONDS = 0.05


class QuietArgumentParser(argparse.ArgumentParser):
    """Argument errors can appear in published CI logs; never echo raw CLI values."""
    def error(self, message):
        raise ValueError('invalid SDK oracle arguments')


# Log lines that carry a Windows-run observation. The summary line keeps
# quality_measured= and distinctOutputsMax= after the PASS marker was retired,
# so replay must select it explicitly instead of silently recording less.
OBSERVATION_MARKERS = ('distinctYU64=', 'quality_measured=', 'status=',
                       'STRICT_FAILURE', 'Oracle harness error')
ORACLE_NAMES = {name + ext for name in ('flat', 'gradient', 'canonical')
                for ext in ('.sdk-p10', '.sdk-bgra')} | {'control.cfhd', 'control.sdk-p10'}


def is_observation_line(line):
    return any(marker in line for marker in OBSERVATION_MARKERS)


def observation_receipt(line):
    """Preserve observation category and digest, never untrusted log text."""
    require(is_observation_line(line), 'not an oracle observation')
    marker = next(marker for marker in OBSERVATION_MARKERS if marker in line)
    return dict(marker=marker, line_sha256=hashlib.sha256(
        line.encode('utf-8')).hexdigest())


def validated_run_url(url):
    """Never copy user-info, query strings or private run locations into a report."""
    require(type(url) is str and re.fullmatch(
        r'https://github[.]com/louis-0hrs/HyperCine/actions/runs/[0-9]{1,20}', url),
        'invalid public oracle run URL')
    return url


def validated_ffmpeg_version(line):
    """Publish only a version token, never arbitrary vendor banner/build paths."""
    require(type(line) is str, 'invalid FFmpeg version output')
    match = re.match(
        r'^ffmpeg version ((?:n?[0-9]{1,3}(?:[.][0-9]{1,3}){1,3}'
        r'(?:[-+][A-Za-z0-9_.+-]{1,32})?|N-[0-9]{1,10}-g[0-9a-f]{7,40}))(?=\s|$)',
        line)
    require(match is not None, 'invalid FFmpeg version output')
    return 'ffmpeg version ' + match.group(1)


def isfinite_number(value):
    """A budget must be a real positive number: bools, NaN and inf are refused."""
    return type(value) in (int, float) and math.isfinite(value)


def metric(a, b, peak=1023):
    require(len(a) == len(b) and len(a) > 0, 'pixel plane lengths differ or are empty')
    errors = [x-y for x, y in zip(a, b)]
    total = sum(e*e for e in errors)
    first = next((i for i, e in enumerate(errors) if e), None)
    return dict(psnr_db=10*math.log10(peak*peak*len(a)/total) if total else None,
                max_error=max(map(abs, errors)), mismatches=sum(e != 0 for e in errors),
                first_index=first, first_actual=a[first] if first is not None else None,
                first_reference=b[first] if first is not None else None)


def p10_metrics(actual, expected):
    a, b = p10_samples(actual, 128, 128), p10_samples(expected, 128, 128)
    bounds = (0, 16384, 24576, 32768)
    return dict(overall=metric(a, b), planes={name: metric(a[bounds[i]:bounds[i+1]], b[bounds[i]:bounds[i+1]])
                for i, name in enumerate(('Y', 'Cb', 'Cr'))})


def extract_log(lines):
    """Accept exactly the bounded public oracle inventory before any extracted file is written."""
    data, active, encoded, declared, encoded_size = {}, None, [], 0, 0
    for line in lines:
        if 'ORACLE_DATA_BEGIN ' in line:
            require(active is None, 'nested oracle record')
            fields = line.split('ORACLE_DATA_BEGIN ', 1)[1].split()
            require(len(fields) == 2, 'invalid oracle header')
            name, size = fields
            require(name in ORACLE_NAMES and name not in data, 'invalid or duplicate oracle name')
            require(len(size) <= 8 and size.isascii() and size.isdecimal(), 'invalid oracle length')
            declared = int(size)
            require(0 < declared <= 16 * 1024 * 1024, 'oracle record too large')
            require(name == 'control.cfhd' or declared == 65536, 'invalid pixel storage')
            active, encoded, encoded_size = name, [], 0
        elif 'ORACLE_DATA_END ' in line:
            require(active is not None and
                    line.split('ORACLE_DATA_END ', 1)[1].strip() == active,
                    'unmatched oracle end')
            raw = base64.b64decode(''.join(encoded), validate=True)
            require(len(raw) == declared, 'oracle record length mismatch')
            data[active] = raw
            active = None
        elif 'ORACLE_DATA ' in line:
            require(active is not None, 'oracle payload outside record')
            part = line.split('ORACLE_DATA ', 1)[1].strip()
            encoded_size += len(part)
            require(encoded_size <= 4 * ((declared + 2) // 3),
                    'oracle payload exceeds declared length')
            encoded.append(part)
    require(active is None, 'unterminated oracle record')
    require(set(data) == ORACLE_NAMES, 'incomplete oracle inventory')
    return data



def read_bounded_log(path):
    """Hash and parse one immutable snapshot, not two independently read files.

    A producer may replace or rewrite a log between reads; the public digest
    must bind the exact bytes supplied to extract_log, even in that case.
    """
    require(0 < path.stat().st_size <= 64 * 1024 * 1024,
            'oracle log exceeds size limit')
    # The preflight stat is not a read bound: a concurrent writer may grow
    # the log before it is opened. Never call read_bytes() without a limit.
    with path.open('rb') as source:
        payload = source.read(64 * 1024 * 1024 + 1)
    require(0 < len(payload) <= 64 * 1024 * 1024,
            'oracle log exceeds size limit')
    return hashlib.sha256(payload).hexdigest(), payload.decode('utf-8').splitlines()


def read_bounded_decoded(path):
    """FFmpeg is an external process: never load an arbitrarily large decode."""
    with path.open('rb') as source:
        payload = source.read(65536 + 1)  # One 128x128 yuv422p10le frame.
    require(len(payload) == 65536, 'invalid decoded control size')
    return payload


def bounded_stdout_text(payload):
    """Decode external output strictly; a partial or non-UTF-8 capture is a failure."""
    try:
        text = payload.decode('utf-8')
    except UnicodeDecodeError:
        raise ValueError('invalid external tool output') from None
    require(text != '', 'invalid external tool output')
    return text


def parse_probe_stream(payload):
    """Parse one bounded ffprobe capture into the single control stream record."""
    text = bounded_stdout_text(payload)
    try:
        stream = json.loads(text)['streams'][0]
    except (ValueError, KeyError, IndexError, TypeError):
        # Malformed, truncated or empty inventory: one public message.
        raise ValueError('invalid oracle stream') from None
    require(type(stream) is dict, 'invalid oracle stream')
    return stream


def _reap_process_tree(process):
    """Kill a timed-out tool and, where the platform offers it, its descendants.

    POSIX uses the session we created, so a tool that forked helpers cannot
    keep the pipe open. Windows has no process group kill in the stdlib, so
    taskkill /T is the documented descendant sweep; the direct kill and the
    final wait are unconditional so a missing taskkill still reaps the child.
    """
    if os.name == 'nt':
        try:
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10)
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
    else:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except (OSError, ValueError):
            pass
    try:
        process.kill()
    except (OSError, ValueError):
        pass
    try:
        process.wait(timeout=10)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        pass


def run_bounded(argv, timeout, limit=None):
    """Run one external tool with a wall budget and a bounded, early capture.

    ``limit is None`` suppresses stdout entirely (the decoded frame is written
    to a file, never through a pipe). Otherwise at most ``limit`` bytes are
    retained; the (limit+1)-th byte stops the run instead of being accumulated
    and rejected afterwards, so a runaway banner cannot exhaust memory first.

    Every failure mode - missing tool, timeout, oversized stdout, non-zero exit
    - raises one generic ValueError. The caller's log may be public, so neither
    argv, tool output, nor an exception from the child may escape this call.
    """
    require(isfinite_number(timeout) and timeout > 0, 'invalid external tool budget')
    require(limit is None or (type(limit) is int and limit >= 0),
            'invalid external tool budget')
    options = dict(stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   bufsize=0, close_fds=True)
    if os.name == 'nt':
        options['creationflags'] = getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
    else:
        options['start_new_session'] = True
    try:
        process = subprocess.Popen(
            list(argv),
            stdout=subprocess.DEVNULL if limit is None else subprocess.PIPE,
            **options)
    except (OSError, ValueError):
        raise ValueError('external tool unavailable') from None
    chunks, over_limit = [], False
    if limit is None:
        pass
    else:
        def drain():
            nonlocal over_limit
            total = 0
            try:
                while True:
                    remaining = limit + 1 - total
                    if remaining <= 0 or over_limit:
                        over_limit = True
                        return
                    chunk = process.stdout.read(min(STDOUT_CHUNK_BYTES, remaining))
                    if not chunk:
                        return
                    total += len(chunk)
                    if total > limit:
                        over_limit = True
                        return
                    chunks.append(chunk)
            except (OSError, ValueError):
                # A closed pipe after our own kill is not extra output.
                return

        reader = threading.Thread(target=drain, name='bounded-stdout', daemon=True)
        reader.start()
    deadline = time.monotonic() + timeout
    try:
        while True:
            try:
                returncode = process.wait(timeout=PROCESS_POLL_SECONDS)
                break
            except subprocess.TimeoutExpired:
                if limit is not None and over_limit:
                    _reap_process_tree(process)
                    raise ValueError('external tool produced too much output') from None
                if time.monotonic() >= deadline:
                    _reap_process_tree(process)
                    raise ValueError('external tool exceeded its runtime budget') from None
        if limit is not None:
            reader.join(timeout=10)
            if over_limit:
                raise ValueError('external tool produced too much output')
    finally:
        if process.stdout is not None:
            try:
                process.stdout.close()
            except (OSError, ValueError):
                pass
    if returncode != 0:
        raise ValueError('external tool failed')
    return b''.join(chunks)


def write_exclusive(path, data):
    """Create an evidence file once: never follow a final symlink or truncate a prior receipt."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, 'O_NOFOLLOW'):
        flags |= os.O_NOFOLLOW
    with os.fdopen(os.open(path, flags, 0o600), 'wb') as target:
        target.write(data)


def main():
    parser = QuietArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--log')
    source.add_argument('--windows-fixtures')
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--run-url')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    root = Path(args.output)
    # The retained Windows fixture root is evidence INPUT, never an output target.
    if args.windows_fixtures:
        fixture_root = Path(args.windows_fixtures).resolve()
        destination = root.resolve()
        require(destination != fixture_root and fixture_root not in destination.parents,
                'refusing to write inside retained Windows fixture source')
    require(not root.is_symlink(), 'oracle output directory is a symlink')
    root.mkdir(parents=True, exist_ok=True)
    require(root.is_dir() and not root.is_symlink(), 'invalid oracle output directory')
    fixture = Path(__file__).parent/'fixtures'/'standalone'
    data = {}
    observations = []
    if args.windows_fixtures:
        manifest_path = Path(args.windows_fixtures) / 'manifest.json'
        require(0 < manifest_path.stat().st_size <= 64 * 1024,
                'fixture manifest exceeds size limit')
        # Enforce the same limit during IO, even if the file grows after stat().
        with manifest_path.open('rb') as source:
            manifest_bytes = source.read(64 * 1024 + 1)
        require(0 < len(manifest_bytes) <= 64 * 1024,
                'fixture manifest exceeds size limit')
        manifest = json.loads(manifest_bytes)
        require(type(manifest) is dict and type(manifest.get('files')) is list and
                len(manifest['files']) == len(ORACLE_NAMES), 'invalid fixture manifest')
        fixture_root = Path(args.windows_fixtures).resolve()
        for file in manifest['files']:
            require(type(file) is dict and type(file.get('file')) is str and
                    file['file'] in ORACLE_NAMES and file['file'] not in data,
                    'invalid or duplicate fixture name')
            declared = file.get('bytes')
            require(type(declared) is int and
                    (0 < declared <= 16 * 1024 * 1024 if file['file'] == 'control.cfhd'
                     else declared == 65536), 'invalid fixture byte count')
            path = fixture_root / file['file']
            require(path.resolve().parent == fixture_root, 'fixture escapes directory')
            require(path.stat().st_size == declared, 'fixture file length mismatch')
            # A producer may replace the artifact between stat and read.
            with path.open('rb') as source:
                raw = source.read(declared + 1)
            require(len(raw) == declared and
                    hashlib.sha256(raw).hexdigest() == file.get('sha256'),
                    'fixture length or digest mismatch')
            data[file['file']] = raw
        run_url, log_sha = manifest['run_url'], manifest['log_sha256']
    else:
        require(args.run_url, '--run-url required with --log' )
        run_url = args.run_url
        log_sha, log_lines = read_bounded_log(Path(args.log))
    if args.log:
        lines = log_lines
        for line in lines:
            if is_observation_line(line):
                require(len(observations) < 4096, 'too many oracle observations')
                observations.append(observation_receipt(line))
        data = extract_log(lines)
    require(set(data) == ORACLE_NAMES, 'incomplete oracle inventory')
    run_url = validated_run_url(run_url)
    hex_digest(log_sha)
    # Check ALL outputs (not just extracted records) before writing even the first byte.
    destinations = {'control.avi', 'control.ffmpeg-p10', 'comparison.json'}
    if args.log:
        destinations.update(ORACLE_NAMES)
    for name in sorted(destinations):
        target = root / name
        require(target.resolve().parent == root.resolve() and
                not target.exists() and not target.is_symlink(),
                'unsafe or occupied oracle destination')
    if args.log:
        for name in sorted(ORACLE_NAMES):
            write_exclusive(root / name, data[name])
    results = dict(copyright='Copyright (c) 2026 HyperCine Contributors.', license='MIT OR Apache-2.0',
        windows_run=run_url, log_sha256=log_sha, windows_observations=observations,
        ffmpeg_version=validated_ffmpeg_version(bounded_stdout_text(run_bounded(
            [args.ffmpeg, '-version'], timeout=VERSION_BUDGET_SECONDS,
            limit=STDOUT_LIMIT_BYTES)).splitlines()[0]),
        input_contract='128x128 top-down BT.709 studio YUV422 p10; YU64 LE Y0 Cr Y1 Cb >>6; BGRA SDK bottom-up',
        alpha_excluded=True, samples=[], control={})
    for pattern in ('flat', 'gradient', 'canonical'):
        raw = data[pattern+'.sdk-p10']
        rgb = data[pattern+'.sdk-bgra']
        # Declared inversion only, never pick the better orientation or mask pixels.
        topdown = b''.join(rgb[y*512:(y+1)*512] for y in reversed(range(128)))
        reference_rgb = (fixture/(pattern+'.bgra')).read_bytes()
        rows, _ = reference(pattern)
        source_rgb = [v for row in rows for pixel in row for v in pixel]
        record = dict(pattern=pattern, sdk_p10_sha256=hashlib.sha256(raw).hexdigest(),
            sdk_bgra_sha256=hashlib.sha256(rgb).hexdigest(),
            sdk_vs_ffmpeg_p10=p10_metrics(raw, (fixture/(pattern+'.p10')).read_bytes()),
            sdk_vs_uncompressed_p10=p10_metrics(raw, (fixture/(pattern+'.input-p10')).read_bytes()),
            sdk_vs_ffmpeg_rgb=metric([x for i, x in enumerate(topdown) if i%4 != 3],
                                    [x for i, x in enumerate(reference_rgb) if i%4 != 3], 255),
            sdk_vs_source_rgb=metric([x for i, x in enumerate(topdown) if i%4 != 3], source_rgb, 255))
        results['samples'].append(record)
    packet=data['control.cfhd']; container=root/'control.avi';write_exclusive(container, avi(packet))
    stream=parse_probe_stream(run_bounded(
            [args.ffprobe,'-v','error','-show_streams','-of','json',str(container)],
            timeout=PROBE_BUDGET_SECONDS, limit=STDOUT_LIMIT_BYTES))
    require(
            (stream.get('codec_name'),stream.get('width'),stream.get('height'),stream.get('pix_fmt')) ==
            ('cfhd',128,128,'yuv422p10le'), 'unexpected oracle stream')
    decoded=root/'control.ffmpeg-p10'
    run_bounded([args.ffmpeg,'-v','error','-xerror','-n','-i',str(container),'-frames:v','1',
        '-c:v','rawvideo','-pix_fmt','yuv422p10le','-f','rawvideo',str(decoded)],
        timeout=DECODE_BUDGET_SECONDS)
    raw=read_bounded_decoded(decoded);source=(fixture/'canonical.input-p10').read_bytes()
    results['control']=dict(packet_bytes=len(packet),packet_sha256=hashlib.sha256(packet).hexdigest(),
        sdk_p10_sha256=hashlib.sha256(data['control.sdk-p10']).hexdigest(),
        ffmpeg_p10_sha256=hashlib.sha256(raw).hexdigest(),
        sdk_vs_ffmpeg_p10=p10_metrics(data['control.sdk-p10'],raw),
        sdk_vs_source_p10=p10_metrics(data['control.sdk-p10'],source),
        ffmpeg_vs_source_p10=p10_metrics(raw,source))
    # Write genuine evidence before reporting any quality failure.
    write_exclusive(root/'comparison.json', (json.dumps(results,indent=2)+'\n').encode('utf-8'))
    for sample in results['samples']:
        overall=sample['sdk_vs_ffmpeg_p10']['overall']
        require(overall['psnr_db'] is None or overall['psnr_db'] > 35,
                'SDK sample PSNR gate failed')
        if sample['pattern'] == 'flat':
            require(overall['max_error'] == 0, 'flat sample mismatch')
    for name in ('sdk_vs_ffmpeg_p10','sdk_vs_source_p10','ffmpeg_vs_source_p10'):
        overall=results['control'][name]['overall']
        require(overall['psnr_db'] is None or overall['psnr_db'] > 35,
                'control PSNR gate failed')
    print(json.dumps(results,indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception:
        # CLI/CTest logs may be published: never echo fixture paths, SDK log
        # text, subprocess argv or exception messages to those public logs.
        raise SystemExit('FAIL: SDK oracle replay rejected or unavailable')
