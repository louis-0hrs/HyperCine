# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Guard suite for the bounded Metal packet capture recorder (#381).

Synthetic fixtures only: no decoder, no network and no fixture media is
written. The live measurement is recorded in
`docs/validation/metal-packet-capture-381-20261118.json`, and the committed
file is re-verified here so an edited claim cannot drift away from the packet
and the retained macOS evidence it names.
"""
import copy
import json
import pathlib
import struct
import sys
import tempfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))

import metal_packet_capture as capture  # noqa: E402


def check(condition, message='assertion failed'):
    """Explicit check: stays active under `python3 -O`, unlike a bare assert."""
    if not condition:
        raise AssertionError(message)


def expect_error(function, needle):
    try:
        function()
    except ValueError as error:
        check(needle in str(error), (needle, str(error)))
        return
    raise AssertionError('expected refusal containing: ' + needle)


def blank_frame(value=0):
    return struct.pack('<%dH' % capture.SAMPLE_COUNT,
                       *([value] * capture.SAMPLE_COUNT))


def test_plane_layout_is_tight_and_total():
    planes = capture.plane_layout(1920, 1080)
    check([plane['plane'] for plane in planes] == ['y', 'u', 'v'])
    check([plane['width'] for plane in planes] == [1920, 960, 960])
    check([plane['height'] for plane in planes] == [1080, 1080, 1080])
    check([plane['stride_bytes'] for plane in planes] == [3840, 1920, 1920])
    check(planes[0]['offset_bytes'] == 0)
    check(planes[1]['offset_bytes'] == 1920 * 1080 * 2)
    check(planes[2]['offset_bytes'] == 1920 * 1080 * 2 + 960 * 1080 * 2)
    total = sum(plane['byte_length'] for plane in planes)
    check(total == capture.CAPTURE_BYTES == 1920 * 1080 * 4)
    check(sum(plane['width'] * plane['height'] for plane in planes) == capture.SAMPLE_COUNT)


def test_plane_layout_refuses_impossible_geometry():
    expect_error(lambda: capture.plane_layout(1921, 1080), 'even width')
    expect_error(lambda: capture.plane_layout(0, 1080), 'positive')
    expect_error(lambda: capture.plane_layout(1920.0, 1080), 'integers')
    expect_error(lambda: capture.plane_layout(True, 1080), 'integers')


def test_sample_domain_is_enforced():
    frame = struct.pack('<2H', 0, 1023)
    check(list(capture.sample_view(frame)) == [0, 1023])
    expect_error(lambda: capture.sample_view(struct.pack('<2H', 0, 1024)),
                 'outside 0..1023')
    expect_error(lambda: capture.sample_view(b'\x00\x00\x00'), 'length mismatch')


def test_diff_metrics_counts_samples_not_bytes():
    left = blank_frame(0)
    right = bytearray(left)
    right[0:2] = struct.pack('<H', 2)
    right[100:102] = struct.pack('<H', 3)
    metrics = capture.diff_metrics(left, bytes(right))
    check(metrics['samples'] == capture.SAMPLE_COUNT)
    check(metrics['mismatches'] == 2)
    check(metrics['max_error'] == 3)
    check(metrics['psnr_db'] == round(
        10.0 * (__import__('math').log10((1023.0 ** 2) / ((4 + 9) / capture.SAMPLE_COUNT))), 6))
    identical = capture.diff_metrics(left, left)
    check((identical['mismatches'], identical['max_error'], identical['psnr_db']) == (0, 0, None))


def test_diff_metric_matches_the_matrix_tool_on_its_own_domain():
    import compat_qualification as matrix
    left = struct.pack('<%dH' % 32768, *([7] * 32768))
    right = bytearray(left)
    right[2:4] = struct.pack('<H', 11)
    ours = capture.diff_metrics(left, bytes(right), 32768)
    theirs = matrix.diff_metrics(left, bytes(right), 32768)
    check((ours['mismatches'], ours['max_error'], ours['psnr_db']) == (
        theirs['mismatches'], theirs['max_error'], theirs['psnr_db']))


def test_changed_capture_is_detected_with_exact_metrics():
    frame = blank_frame(5)
    changed = capture.one_sample_change(frame)
    check(len(changed) == len(frame))
    metrics = capture.diff_metrics(changed, frame)
    check((metrics['mismatches'], metrics['max_error']) == (1, 1))
    check(changed[2:] == frame[2:])
    expect_error(lambda: capture.one_sample_change(blank_frame(1023)), 'saturated')
    expect_error(lambda: capture.one_sample_change(b'\x00\x00'), 'full frame')


def test_forged_metrics_are_rejected():
    left = blank_frame(0)
    right = bytearray(left)
    right[0:2] = struct.pack('<H', 4)
    right = bytes(right)
    honest = capture.diff_metrics(right, left)
    check(capture.require_metrics(right, left, honest) == honest)
    forged = {'mismatches': 0, 'max_error': 0, 'samples': capture.SAMPLE_COUNT,
              'psnr_db': None}
    expect_error(lambda: capture.require_metrics(right, left, forged), 'contradicts')
    tampered = dict(honest, max_error=3)
    expect_error(lambda: capture.require_metrics(right, left, tampered), 'contradicts')
    extra = dict(honest, verdict='IDENTICAL')
    expect_error(lambda: capture.require_metrics(right, left, extra), 'malformed')
    boolean = dict(honest, mismatches=True)
    expect_error(lambda: capture.require_metrics(right, left, boolean), 'malformed')


def committed_evidence_path():
    return ROOT / 'docs/validation/metal-packet-capture-381-20261118.json'


def test_decode_worker_options_bind_input_and_capture_stays_bounded():
    frame = bytes(capture.CAPTURE_BYTES)
    for name, extra in capture.DECODE_VARIANTS:
        with patch.object(capture, 'run_bounded', return_value=frame) as run:
            check(capture._decode('synthetic-ffmpeg', 'frame.avi', extra) == frame)
        argv = run.call_args.args[0]
        input_index = argv.index('-i')
        check(argv[input_index + 1] == 'frame.avi')
        check(argv.count('-i') == 1)
        before, after = argv[:input_index], argv[input_index + 2:]
        if name in ('single-thread', 'four-thread'):
            check('-threads' in before, 'decoder thread setting must precede input')
            check(before[before.index('-threads') + 1] ==
                  ('1' if name == 'single-thread' else '4'))
        else:
            check('-threads' not in before)
        check('-threads' not in after, 'decoder option must not configure output')
        if name == 'single-filter-thread':
            check(before[before.index('-filter_threads') + 1] == '1')
        check(after[after.index('-c:v') + 1] == 'rawvideo')
        check(after[after.index('-threads:v') + 1] == '1')
        check(after[after.index('-frames:v') + 1] == '1')
        check(after[after.index('-pix_fmt') + 1] == 'yuv422p10le')
        check(after[-1] == 'pipe:1')
        check(run.call_args.kwargs == {'timeout': capture.DECODE_TIMEOUT,
                                      'stdout_limit': capture.CAPTURE_BYTES + 1})


def test_command_evidence_cannot_swap_decoder_and_encoder_roles():
    original = json.loads(committed_evidence_path().read_text(encoding='utf-8'))
    for change in ('missing', 'swapped', 'count', 'output'):
        evidence = copy.deepcopy(original)
        if change == 'missing':
            del evidence['stability']['commands']
        else:
            argv = evidence['stability']['commands'][1]['argv']
            if change == 'swapped':
                index = argv.index('-threads')
                options = argv[index:index + 2]
                del argv[index:index + 2]
                argv[-1:-1] = options
            elif change == 'count':
                argv[argv.index('-threads') + 1] = '4'
            else:
                argv[argv.index('-threads:v') + 1] = '4'
        expect_error(lambda: capture.verify_evidence(evidence, **verified_arguments()),
                     'input/output roles')


def test_historical_capture_remains_digest_only_evidence():
    path = ROOT / 'docs/validation/metal-packet-capture-381-20261117.json'
    evidence = json.loads(path.read_text(encoding='utf-8'))
    check(evidence['schema'] == capture.LEGACY_SCHEMA)
    capture.verify_evidence(evidence, **verified_arguments())
    # Attaching today's commands cannot retroactively requalify the old run.
    evidence['stability']['commands'] = capture.recorded_commands()
    expect_error(lambda: capture.verify_evidence(evidence, **verified_arguments()),
                 'legacy evidence cannot claim')


def test_comparison_source_label_must_name_the_verified_macos_artifact():
    evidence = json.loads(committed_evidence_path().read_text(encoding='utf-8'))
    evidence['compared']['source'] = 'docs/validation/unrelated-record.json'
    expect_error(lambda: capture.verify_evidence(evidence, **verified_arguments()),
                 'comparison source')


def verified_arguments():
    return dict(packet_path=ROOT / 'tests/fixtures/macos-filmscan3/flat.cfhd',
                manifest_path=ROOT / 'tests/fixtures/macos-filmscan3/manifest.json',
                macos_path=ROOT / 'docs/validation/metal-filmscan3-ffmpeg.json')


def test_committed_evidence_reverifies_hash_only():
    evidence = json.loads(committed_evidence_path().read_text(encoding='utf-8'))
    capture.verify_evidence(evidence, **verified_arguments())
    check(evidence['capture']['sha256'] == evidence['compared']['macos_decoded_sha256'])
    check(evidence['capture']['byte_length'] == 8294400)
    check(evidence['capture']['samples'] == 4147200)
    check(evidence['decoder']['binary_sha256'])
    check(evidence['stability']['runs'] >= 2)


def test_committed_evidence_refuses_tampering():
    original = json.loads(committed_evidence_path().read_text(encoding='utf-8'))

    def mutated(function):
        evidence = copy.deepcopy(original)
        function(evidence)
        return lambda: capture.verify_evidence(evidence, **verified_arguments())

    expect_error(mutated(lambda e: e['capture'].update(byte_length=65536)), 'capture length')
    expect_error(mutated(lambda e: e['capture'].update(samples=32768)), 'sample count')
    expect_error(mutated(lambda e: e['capture'].update(pixel_format='yuv420p10le')),
                 'registered one')
    expect_error(mutated(lambda e: e['capture'].update(sha256='0' * 64)),
                 'stability digest')
    expect_error(mutated(lambda e: e['interpretation']['planes'][1].update(
        offset_bytes=0)), 'plane layout')
    expect_error(mutated(lambda e: e['interpretation'].update(range='full (0-1023)')),
                 'range interpretation')
    expect_error(mutated(lambda e: e['stability'].update(identical=False)), 'proven stable')
    expect_error(mutated(lambda e: e['stability'].update(runs=1)), 'proven stable')
    expect_error(mutated(lambda e: e['stability'].update(runs=2)), 'registered independent')
    expect_error(mutated(lambda e: e['stability'].update(variants=['default'])),
                 'registered independent')
    expect_error(mutated(lambda e: e['container'].update(codec='rawvideo')),
                 'container geometry or codec')
    expect_error(mutated(lambda e: e['packet'].update(artifact='other.cfhd')),
                 'packet provenance')
    expect_error(mutated(lambda e: e['stability'].update(digest='0' * 64)), 'stability digest')
    expect_error(mutated(lambda e: e['decoder'].update(binary_sha256='*' * 64)),
                 'executable is not bound')
    expect_error(mutated(lambda e: e['decoder'].update(id='gopro-sdk')), 'decoder role')
    expect_error(mutated(lambda e: e['packet'].update(byte_length=1)), 'packet length')
    expect_error(mutated(lambda e: e['packet'].update(sha256='0' * 64)), 'packet digest')
    expect_error(mutated(lambda e: e['compared'].update(macos_decoded_sha256='0' * 64)),
                 'retained macOS evidence')
    expect_error(mutated(lambda e: e['compared'].update(match=False)), 'match flag')
    expect_error(mutated(lambda e: e['negatives']['changed_capture'].update(
        detected=False)), 'changed-capture negative')
    expect_error(mutated(lambda e: e['negatives']['changed_capture'].update(
        max_error=0)), 'unexpected metrics')
    expect_error(mutated(lambda e: e['negatives']['forged_metrics'].update(
        rejected=False)), 'forged-metrics negative')
    expect_error(mutated(lambda e: e.update(limits=[])), 'no limits')
    expect_error(mutated(lambda e: e.update(issue='#999')), 'another issue')
    expect_error(mutated(lambda e: e.update(schema='hypercine.compat-matrix/1')),
                 'unsupported evidence schema')
    expect_error(mutated(lambda e: e['packet'].update(frame_index=1)),
                 'unexpected frame index')
    expect_error(mutated(lambda e: e['compared'].update(frame_index=1)),
                 'another frame')
    expect_error(mutated(lambda e: e['stability'].update(distinct_digests=2)),
                 'differing digests')
    expect_error(mutated(lambda e: e['stability'].update(distinct_digests=True)),
                 'differing digests')
    expect_error(mutated(lambda e: e['capture'].update(samples=float(capture.SAMPLE_COUNT))),
                 'sample count')
    expect_error(mutated(lambda e: e['decoder'].update(version='gopro sdk 1.0')),
                 'decoder version')
    expect_error(mutated(lambda e: e['interpretation'].update(range='outside studio (64-940)')),
                 'range interpretation')
    expect_error(mutated(lambda e: e['interpretation']['observed_samples'][0].update(
        minimum=1024)), '10-bit domain')
    expect_error(mutated(lambda e: e['interpretation']['observed_samples'][1].update(
        plane='y')), 'another plane')


def test_committed_evidence_files_are_pinned_by_digest():
    # The verifier cannot attest the provenance of an arbitrary copy: an
    # edited file declares its own schema. The committed artifacts are pinned
    # here so a silent rewrite inside the repository fails the guard suite.
    pinned = {
        ROOT / 'docs/validation/metal-packet-capture-381-20261118.json':
            'cbe80cfb6ef24daaea6e132f7eaa05b62ef35239d8e10f4adef99106908f8b26',
        ROOT / 'docs/validation/metal-packet-capture-381-20261117.json':
            '7bb27ecba0adf3cae8cf8508c2c1166a4c446c6876c4f8ef0fad35d3e68e5b10',
    }
    for path, expected in pinned.items():
        # Normalize line endings: Git may check the same blob out as CRLF.
        text = path.read_text(encoding='utf-8').replace('\r\n', '\n')
        check(capture.sha256(text.encode('utf-8')) == expected,
              'committed evidence digest changed: ' + path.name)
    check(json.loads((ROOT / 'docs/validation/metal-packet-capture-381-20261117.json')
                     .read_text(encoding='utf-8'))['schema'] == capture.LEGACY_SCHEMA)



def test_live_recorder_refuses_mismatched_decode_even_when_decoder_completed():
    args = verified_arguments()
    # A fully valid committed packet/manifest and authentic retained macOS
    # record: substitute only an apparently successful, wrong decoded digest.
    # No external decoder or hardware is required to prove this refusal.
    # Complete wrong frame and valid metadata: if digest refusal is removed,
    # downstream metrics must succeed and this test must fail for that reason.
    wrong_frame = bytes(capture.CAPTURE_BYTES)
    wrong_digest = capture.sha256(wrong_frame)
    check(wrong_digest != json.loads(committed_evidence_path().read_text(encoding='utf-8'))['capture']['sha256'])
    stability = {'runs': len(capture.DECODE_VARIANTS),
                 'variants': [name for name, _ in capture.DECODE_VARIANTS],
                 'distinct_digests': 1, 'identical': True, 'digest': wrong_digest}
    stream = {'codec_name': 'cfhd', 'width': capture.WIDTH,
              'height': capture.HEIGHT, 'pixel_format': capture.PIXEL_FORMAT}
    with patch.object(capture, 'decoder_identity',
                      return_value={'id': 'ffmpeg-cfhd', 'version': 'synthetic',
                                    'binary_sha256': '0' * 64}), \
         patch.object(capture, 'decode_capture',
                      return_value=(wrong_frame, stability, stream)):
        expect_error(lambda: capture.build_evidence(
            args['packet_path'], 'synthetic-ffmpeg', 'synthetic-ffprobe',
            args['manifest_path'], args['macos_path']),
            'decoded pixels disagree')


def test_hash_only_verifier_refuses_truthful_mismatch_without_macos_file():
    source = json.loads(committed_evidence_path().read_text(encoding='utf-8'))
    source['compared']['macos_decoded_sha256'] = '0' * 64
    source['compared']['match'] = False
    args = verified_arguments()
    # Even when the optional external macOS record isn't supplied, hash-only
    # verification must not turn a documented agreement into a negative claim.
    expect_error(lambda: capture.verify_evidence(
        source, packet_path=args['packet_path'],
        manifest_path=args['manifest_path'], macos_path=None),
        'decoded pixels disagree')


def test_retained_macos_frame_must_bind_the_same_packet():
    args = verified_arguments()
    source = json.loads(committed_evidence_path().read_text(encoding='utf-8'))
    retained = json.loads(args['macos_path'].read_text(encoding='utf-8'))
    retained['frames'][0]['raw_sha256'] = '0' * 64
    with tempfile.TemporaryDirectory() as directory:
        mismatched = pathlib.Path(directory) / 'foreign-mac-evidence.json'
        mismatched.write_text(json.dumps(retained), encoding='utf-8')
        expect_error(lambda: capture.verify_evidence(
            source, packet_path=args['packet_path'],
            manifest_path=args['manifest_path'], macos_path=mismatched),
            'belongs to another packet')


def test_packet_and_manifest_disagreement_is_refused():
    evidence = json.loads(committed_evidence_path().read_text(encoding='utf-8'))
    with tempfile.TemporaryDirectory() as directory:
        altered = pathlib.Path(directory) / 'flat.cfhd'
        altered.write_bytes(b'\x00' * 4)
        expect_error(lambda: capture.verify_evidence(
            evidence, packet_path=altered,
            manifest_path=ROOT / 'tests/fixtures/macos-filmscan3/manifest.json'),
            'packet length')
    with tempfile.TemporaryDirectory() as directory:
        manifest = json.loads(
            (ROOT / 'tests/fixtures/macos-filmscan3/manifest.json').read_text(encoding='utf-8'))
        manifest['samples'][0]['sha256'] = '0' * 64
        altered = pathlib.Path(directory) / 'manifest.json'
        altered.write_text(json.dumps(manifest), encoding='utf-8')
        expect_error(lambda: capture.verify_evidence(
            evidence, manifest_path=altered), 'not the manifest packet')


def test_live_capture_requires_a_decoder():
    check(capture.main(['--packet', 'x']) == 2)


def main():
    tests = [value for name, value in sorted(globals().items())
             if name.startswith('test_') and callable(value)]
    for test in tests:
        test()
        print('PASS ' + test.__name__)
    print('%d/%d metal packet capture guards passed' % (len(tests), len(tests)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
