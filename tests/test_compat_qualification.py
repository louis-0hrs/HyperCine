# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Corpus-free controls for the compatibility qualification matrix.

The matrix is evidence, so every guard below is a way it could lie: a row that
claims identity while carrying differing pixels, a verdict that contradicts its
own measurement, a decoder that was never named, a feature without a measured
case presented as a pass, or an artifact whose bytes no longer match its
recorded digest.

Synthetic fixtures only; no retained corpus and no external decoder is needed.
"""
import copy
import pathlib
import subprocess
import time
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import compat_qualification as cq
from compat_process import run_bounded

FRAME = bytes(range(256)) * 256
OTHER = bytes(reversed(bytes(range(256)))) * 256

FIXTURES = (
    ('standalone', 'flat.cfhd', FRAME),
    ('standalone', 'flat.input-p10', FRAME),
    ('standalone', 'flat.p10', FRAME),
    ('standalone', 'flat.bgra', FRAME),
    ('standalone', 'canonical.cfhd', OTHER),
    ('standalone', 'canonical.input-p10', FRAME),
    ('standalone', 'canonical.p10', OTHER),
    ('standalone', 'canonical.bgra', FRAME),
    ('standalone', 'gradient.cfhd', FRAME),
    ('standalone', 'gradient.input-p10', FRAME),
    ('standalone', 'gradient.p10', FRAME),
    ('standalone', 'gradient.bgra', FRAME),
    ('windows-oracle', 'flat.sdk-p10', FRAME),
    ('windows-oracle', 'flat.sdk-bgra', FRAME),
    ('windows-oracle', 'canonical.sdk-p10', OTHER),
    ('windows-oracle', 'canonical.sdk-bgra', FRAME),
    ('windows-oracle', 'gradient.sdk-p10', FRAME),
    ('windows-oracle', 'gradient.sdk-bgra', FRAME),
    ('windows-oracle', 'control.cfhd', FRAME),
    ('windows-oracle', 'control.sdk-p10', FRAME),
    ('macos-filmscan3', 'flat.cfhd', FRAME),
    ('macos-filmscan3', 'gradient.cfhd', FRAME),
)


def digest(payload):
    return cq.sha256_bytes(payload)


class SyntheticTree:
    """Build or mutate a fixture tree with honest or dishonestly edited digests."""

    def __init__(self, root):
        self.root = root

    def write(self, tamper=None, drop=()):
        groups = {}
        for group, name, payload in FIXTURES:
            if name in drop:
                continue
            groups.setdefault(group, []).append((name, payload))
        for group, entries in groups.items():
            directory = self.root / group
            directory.mkdir(parents=True, exist_ok=True)
            records = []
            for name, payload in entries:
                (directory / name).write_bytes(payload)
                size, sha = len(payload), digest(payload)
                if tamper and name == tamper[0]:
                    sha = tamper[1]
                records.append({'file': name, 'bytes': size, 'sha256': sha})
            (directory / 'manifest.json').write_text(json.dumps({'files': records}))
        return self.root


class Inventory(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def build(self, **kwargs):
        SyntheticTree(self.root).write(**kwargs)
        return cq.inventory(self.root)

    def test_inventory_binds_roles_and_payloads(self):
        artifacts, payloads = self.build()
        self.assertEqual(artifacts['standalone/flat.input-p10']['role'],
                         'encoder_input_p10')
        self.assertEqual(artifacts['windows-oracle/control.cfhd']['role'],
                         'legacy_gopro_packet')
        self.assertEqual(payloads['standalone/flat.cfhd'], FRAME)

    def test_digest_mismatch_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            self.build(tamper=('control.cfhd', '0' * 64))
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')

    def test_declared_length_mismatch_is_refused(self):
        SyntheticTree(self.root).write()
        manifest = self.root / 'standalone' / 'manifest.json'
        records = json.loads(manifest.read_text())
        for record in records['files']:
            if record['file'] == 'flat.cfhd':
                record['bytes'] = 65535
        manifest.write_text(json.dumps(records))
        with self.assertRaises(ValueError) as caught:
            cq.inventory(self.root)
        self.assertEqual(str(caught.exception), 'artifact length mismatch')

    def test_unknown_artifact_name_is_refused(self):
        SyntheticTree(self.root).write()
        manifest = self.root / 'standalone' / 'manifest.json'
        records = json.loads(manifest.read_text())
        records['files'].append({'file': 'unrelated.bin', 'bytes': 1,
                                 'sha256': '0' * 64})
        manifest.write_text(json.dumps(records))
        with self.assertRaises(ValueError) as caught:
            cq.inventory(self.root)
        self.assertEqual(str(caught.exception), 'unclassified reference artifact')

    def test_path_escape_is_refused(self):
        with self.assertRaises(ValueError):
            cq.bound_artifact(self.root, '../escape.p10', 3, '0' * 64)
        with self.assertRaises(ValueError):
            cq.bound_artifact(self.root, 'a/../../escape.p10', 3, '0' * 64)

    def test_missing_manifest_is_refused(self):
        (self.root / 'standalone').mkdir()
        with self.assertRaises(ValueError) as caught:
            cq.load_manifest(self.root / 'standalone' / 'manifest.json')
        self.assertEqual(str(caught.exception), 'missing fixture manifest')


class Matrix(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        SyntheticTree(self.root).write()
        self.artifacts, self.payloads = cq.inventory(self.root)
        self.matrix = cq.build_matrix(self.artifacts, self.payloads)

    def row(self, identifier):
        return next(row for row in self.matrix['rows'] if row['id'] == identifier)

    def test_build_passes_its_own_validation(self):
        cq.validate_matrix(self.matrix)
        cq.validate_matrix(self.matrix, self.root)

    def test_identical_row_may_bind_equal_digests(self):
        row = self.row('hypercine-packet-flat')
        self.assertEqual(row['status'], cq.STATUS_IDENTITY)
        for item in row['comparisons']:
            self.assertEqual(item['metrics']['mismatches'], 0)

    def test_differing_row_is_not_an_identity(self):
        row = self.row('hypercine-packet-canonical')
        self.assertEqual(row['status'], cq.STATUS_DIFFERENCE)
        self.assertTrue(any(item['metrics']['mismatches'] > 0
                            for item in row['comparisons']))

    def test_packets_without_capture_are_not_observable(self):
        row = self.row('hypercine-metal-filmscan3-packets')
        self.assertEqual(row['status'], cq.STATUS_NOT_OBSERVABLE)
        self.assertEqual(row['comparisons'], [])

    def test_identity_claim_with_measured_error_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['metrics']['mismatches'] = 12
        item['metrics']['max_error'] = 3
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'IDENTICAL verdict contradicts the measured error')

    def test_difference_without_measured_error_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['verdict'] = cq.VERDICT_DIFFERENT
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'DIFFERENT verdict without a measured difference')

    def test_status_contradicting_comparisons_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][1]['status'] = cq.STATUS_IDENTITY
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'row status contradicts its comparisons')

    def test_unbound_local_decoder_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'].append({
            'basis': cq.BASIS_LOCAL,
            'decoder': cq.recorded_decoder('ffmpeg-cfhd', 'n8.0'),
            'capture': {'artifact': 'local/x.p10', 'sha256': digest(FRAME)},
            'against': {'artifact': 'standalone/flat.p10', 'sha256': digest(FRAME)},
            'metrics': cq.metrics(0, 0, None, 32768),
            'verdict': cq.VERDICT_IDENTICAL,
        })
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(
            str(caught.exception),
            'local measurement without a bound decoder executable')

    def test_committed_comparison_may_not_claim_a_binary(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'][0]['decoder']['binary_sha256'] = '0' * 64
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(
            str(caught.exception),
            'committed-artifact comparison must not claim a binary digest')

    def test_identity_outside_admitted_scope_is_refused(self):
        for field, value in (('scan', 'interlaced'), ('gop', 2),
                             ('pixel_format', 'rgba10le'), ('geometry', '1920x1080')):
            broken = copy.deepcopy(self.matrix)
            broken['rows'][0]['scope'][field] = value
            with self.assertRaises(ValueError) as caught:
                cq.validate_matrix(broken)
            self.assertIn('outside the admitted scope', str(caught.exception))

    def test_self_comparison_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['against'] = item['capture']
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'comparison compares an artifact with itself')

    def test_backlog_entry_carrying_a_verdict_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['backlog'][0]['status'] = cq.STATUS_IDENTITY
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'backlog entry carries unexpected fields')

    def test_empty_backlog_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['backlog'] = []
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'backlog must be enumerated')

    def test_duplicate_row_id_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'].append(copy.deepcopy(broken['rows'][0]))
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'duplicate matrix row id')

    def test_unknown_direction_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['direction'] = 'universal-interoperability'
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'unknown compatibility direction')

    def test_zero_sample_comparison_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'][0]['metrics']['samples'] = 0
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'comparison without samples')

    def test_tampered_packet_digest_is_refused_on_disk(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['packet']['sha256'] = '1' * 64
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken, self.root)
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')

    def test_mutated_packet_bytes_are_refused_on_disk(self):
        (self.root / 'standalone' / 'flat.cfhd').write_bytes(OTHER)
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(self.matrix, self.root)
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')


class ReplayControls(unittest.TestCase):
    setUp = Matrix.setUp
    row = Matrix.row
    def test_mutated_capture(self):
        (self.root / 'standalone/flat.p10').write_bytes(OTHER)
        with self.assertRaisesRegex(ValueError, 'artifact digest mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_forged_consistent_metrics(self):
        item = self.matrix['rows'][1]['comparisons'][0]
        item['metrics']['max_error'] += 1
        with self.assertRaisesRegex(ValueError, 'comparison metrics mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_wrong_linkage(self):
        self.matrix['rows'][0]['comparisons'][0]['capture'] = cq.artifact_ref(
            self.artifacts, 'standalone/gradient.p10', 'committed_ffmpeg_decoded_p10')
        with self.assertRaisesRegex(ValueError, 'packet linkage mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_wrong_packet_role(self):
        self.matrix['rows'][0]['packet']['role'] = 'legacy_gopro_packet'
        with self.assertRaisesRegex(ValueError, 'packet role mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_json_scope_types(self):
        for field, value in [('gop', True), ('gop', 1.0), ('bit_depth', 10.0),
                             ('bit_depth', True), ('scan', 1), ('geometry', []),
                             ('range', None), ('chroma_siting', False), ('pixel_format', 0)]:
            broken = json.loads(json.dumps(self.matrix))
            broken['rows'][0]['scope'][field] = value
            with self.assertRaisesRegex(ValueError, 'invalid scope type: ' + field):
                cq.validate_matrix(broken, self.root)


class ProcessControls(unittest.TestCase):
    def run_tool(self, source, **kwargs):
        from compat_process import run_bounded
        return run_bounded([sys.executable, '-c', source], **kwargs)

    def test_success(self):
        self.assertEqual(self.run_tool("print('ok')"), b'ok\n' if sys.platform != 'win32' else b'ok\r\n')

    def test_hanging(self):
        with self.assertRaisesRegex(ValueError, '^tool timeout$'):
            self.run_tool('import time; time.sleep(60)', timeout=0.2)

    def test_overflow(self):
        with self.assertRaisesRegex(ValueError, '^tool output limit exceeded$'):
            self.run_tool("import os;\nwhile True: os.write(1,b'x'*4096)", stdout_limit=8192)

    def test_nonzero_secret_banner(self):
        with self.assertRaisesRegex(ValueError, '^tool nonzero exit$'):
            self.run_tool("import sys; print('SECRET_BANNER_PRIVATE'); sys.exit(3)")

    def test_stderr_overflow(self):
        with self.assertRaisesRegex(ValueError, '^tool output limit exceeded$'):
            self.run_tool("import os;\nwhile True: os.write(2,b'x'*4096)", stdout_limit=8192)

    def test_malformed_json(self):
        with self.assertRaises(json.JSONDecodeError):
            json.loads(self.run_tool("print('{bad')"))

    def test_launch_error_sanitized(self):
        from compat_process import run_bounded
        with self.assertRaisesRegex(ValueError, '^tool launch failed$'):
            run_bounded(['MISSING_SECRET_PRIVATE_EXECUTABLE'])

    def test_exited_parent_grandchild_cleanup(self):
        import time
        with tempfile.TemporaryDirectory() as temp:
            sentinel = Path(temp) / 'escaped'
            child = "import time,pathlib; time.sleep(1); pathlib.Path(%r).write_text('escaped')" % str(sentinel)
            source = "import subprocess,sys; subprocess.Popen([sys.executable,'-c',%r],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)" % child
            self.run_tool(source, timeout=0.5)
            time.sleep(1.2)
            self.assertFalse(sentinel.exists(), 'grandchild escaped containment')

    def test_inherited_pipe_descendant(self):
        import time
        with tempfile.TemporaryDirectory() as temp:
            sentinel = Path(temp) / 'escaped'
            child = "import time,pathlib; time.sleep(1); pathlib.Path(%r).write_text('escaped')" % str(sentinel)
            source = "import subprocess,sys; subprocess.Popen([sys.executable,'-c',%r])" % child
            with self.assertRaisesRegex(ValueError, '^tool timeout$'):
                self.run_tool(source, timeout=0.3)
            time.sleep(1.1)
            self.assertFalse(sentinel.exists())


class IndependentReviewControls(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  self.root=SyntheticTree(pathlib.Path(self.tmp.name)).write()
  a,p=cq.inventory(self.root);self.matrix=cq.build_matrix(a,p)
 def test_valid_control(self): cq.validate_matrix(self.matrix,self.root)
 def test_forged_identity_metrics_refused(self):
  r=self.matrix['rows'][1]
  for c in r['comparisons']:
   c['metrics']={'psnr_db':None,'max_error':0,'mismatches':0,'samples':32768};c['verdict']='IDENTICAL'
  r['status']='QUALIFIED_IDENTITY'
  with self.assertRaisesRegex(ValueError,'comparison metrics mismatch'):cq.validate_matrix(self.matrix,self.root)
 def test_foreign_packet_capture_refused(self):
  self.matrix['rows'][0]['comparisons'][0]['capture']['artifact']='standalone/gradient.p10'
  with self.assertRaisesRegex(ValueError,'packet linkage mismatch'):cq.validate_matrix(self.matrix,self.root)
 def test_identity_bool_scope_refused(self):
  self.matrix['rows'][0]['scope']['gop']=True
  with self.assertRaisesRegex(ValueError,'invalid scope type'):cq.validate_matrix(self.matrix,self.root)
 def test_unsupported_direction_refused(self):
  self.matrix['rows'][0]['direction']='hypercine-packet-to-hypercine-decoder'
  with self.assertRaisesRegex(ValueError,'unmeasured compatibility direction'):cq.validate_matrix(self.matrix,self.root)
 def test_packet_manifest_commit_relabel_refused(self):
  self.matrix['rows'][0]['packet']['source_commit']='1'*40
  with self.assertRaisesRegex(ValueError,'packet source commit mismatch'):cq.validate_matrix(self.matrix,self.root)
 def local_row(self):
  r=self.matrix['rows'][1];c=copy.deepcopy(r['comparisons'][0]);c['basis']='local_decoder'
  c['decoder']={'id':'ffmpeg-cfhd','version':'recorded','binary_sha256':'1'*64}
  c['capture']={'artifact':'local/ffmpeg-cfhd/canonical.p10','sha256':'2'*64}
  c['metrics']={'psnr_db':None,'max_error':0,'mismatches':0,'samples':32768};c['verdict']='IDENTICAL'
  r['comparisons']=[c];r['status']='QUALIFIED_IDENTITY'
  return c
 def test_local_fake_role_domain_psnr_refused(self):
  for field,value,reason in [('decoder','not-ffmpeg','local decoder role'),('samples',1,'sample domain'),('psnr_db',{'arbitrary':'json'},'PSNR type')]:
   c=self.local_row()
   if field=='decoder':c['decoder']['id']=value
   else:c['metrics'][field]=value
   with self.assertRaisesRegex(ValueError,reason):cq.validate_matrix(self.matrix,self.root)
 def test_local_recorded_not_replay_qualified(self):
  self.local_row();out=cq.validate_matrix(self.matrix,self.root);r=out['rows'][1]
  self.assertEqual(r['historical_status'],'QUALIFIED_IDENTITY')
  self.assertEqual(r['status'],'NOT_OBSERVABLE')
  self.assertEqual(r['replay_status'],'NOT_REPLAY_QUALIFIED')
  self.assertEqual(r['comparisons'][0]['replay_status'],'NOT_REPLAY_QUALIFIED')
 def test_cli_nonobject_evidence_sanitized(self):
  f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text('[]')
  self.cli_refused(f)
 def cli_refused(self,f):
  command=[sys.executable]+(['-O'] if sys.flags.optimize else [])+[str(Path(cq.__file__)), '--verify',str(f),'--fixtures',str(self.root)]
  p=subprocess.run(command,capture_output=True,text=True,timeout=10)
  self.assertEqual(p.returncode,2,p.stderr)
  self.assertEqual(p.stdout,'')
  self.assertEqual(p.stderr,'compat qualification failed: invalid evidence or bounded tool failure\n')
 def test_nested_manifest_sanitized(self):
  for bad in [[],{'files':[None]}, {'samples':{'bad':'shape'}}, {'files':['private']}]:
   (self.root/'standalone/manifest.json').write_text(json.dumps(bad))
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(self.matrix))
   self.cli_refused(f)
 def test_psnr_semantics_and_types(self):
  for value in [True,{},[],float('nan'),float('inf'),1.0]:
   c=self.local_row();c['metrics']['psnr_db']=value
   with self.assertRaisesRegex(ValueError,'PSNR'):cq.validate_matrix(self.matrix,self.root)
 def test_known_manifest_provenance(self):
  f=self.root/'standalone/manifest.json';d=json.loads(f.read_text());d['encoder_commit']='a'*40;f.write_text(json.dumps(d))
  a,p=cq.inventory(self.root);m=cq.build_matrix(a,p);cq.validate_matrix(m,self.root)
  m['rows'][0]['packet']['source_commit']=None
  with self.assertRaisesRegex(ValueError,'source commit mismatch'):cq.validate_matrix(m,self.root)
 def test_local_difference_scope_refused(self):
  c=self.local_row();c['metrics']={'psnr_db':90.0,'max_error':1,'mismatches':1,'samples':32768};c['verdict']='DIFFERENT'
  self.matrix['rows'][1]['status']='MEASURED_DIFFERENCE'
  self.matrix['rows'][1]['scope']['geometry']='1920x1080'
  with self.assertRaisesRegex(ValueError,'outside the admitted scope'):cq.validate_matrix(self.matrix,self.root)
 def test_cli_machine_readable_replay(self):
  self.local_row();f=self.root/'matrix.json';f.write_text(json.dumps(self.matrix))
  command=[sys.executable]+(['-O'] if sys.flags.optimize else [])+[str(Path(cq.__file__)), '--verify',str(f),'--fixtures',str(self.root)]
  p=subprocess.run(command,capture_output=True,text=True,timeout=10)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stderr,'')
  report=json.loads(p.stdout);r=report['rows'][1]
  self.assertEqual(r['historical_status'],'QUALIFIED_IDENTITY')
  self.assertEqual(r['replay_qualification'],'NOT_OBSERVABLE')
  self.assertEqual(r['comparisons'],['NOT_REPLAY_QUALIFIED'])
  self.assertNotIn(str(self.root),p.stdout)
 def test_cli_nested_row_shapes_sanitized(self):
  mutations=[('rows',[[]]),('rows',[{'id':'bad'}])]
  for field,value in mutations:
   m=copy.deepcopy(self.matrix);m[field]=value
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(m));self.cli_refused(f)
  for field in ['decoder','capture','against','metrics']:
   m=copy.deepcopy(self.matrix);m['rows'][0]['comparisons'][0][field]=[]
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(m));self.cli_refused(f)
  for field in ['artifact','sha256','role','producer','source_commit']:
   m=copy.deepcopy(self.matrix);m['rows'][0]['packet'][field]=[]
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(m));self.cli_refused(f)
 def test_producer_relabel_refused(self):
  self.matrix['rows'][0]['packet']['producer']='GoPro encoder'
  with self.assertRaisesRegex(ValueError,'packet producer mismatch'):cq.validate_matrix(self.matrix,self.root)
 def test_finite_mixed_output(self):
  self.assertEqual(run_bounded([sys.executable,'-c',"import os;os.write(1,b'x'*8192);os.write(2,b'y'*8192)"],stdout_limit=8192),b'x'*8192)
 def test_stderr_over_limit(self):
  with self.assertRaisesRegex(ValueError,'output limit'):run_bounded([sys.executable,'-c',"import os;os.write(2,b'x'*8193)"],stdout_limit=8192)
 def test_closed_pipe_descendant_cleanup(self):
  f=self.root/'sentinel';child="import time,pathlib;time.sleep(.4);pathlib.Path(%r).write_text('x')"%str(f)
  run_bounded([sys.executable,'-c',"import subprocess,sys;subprocess.Popen([sys.executable,'-c',%r],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)"%child],timeout=1)
  time.sleep(.6);self.assertFalse(f.exists())


class Metrics(unittest.TestCase):
    def test_identity_is_sample_level_not_byte_level(self):
        result = cq.diff_metrics(FRAME, FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(result, {'psnr_db': None, 'max_error': 0,
                                  'mismatches': 0, 'samples': cq.SAMPLE_COUNT})

    def test_single_code_value_difference_is_one_mismatch(self):
        altered = bytearray(FRAME)
        altered[0] = (altered[0] + 1) % 256
        result = cq.diff_metrics(bytes(altered), FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(result['mismatches'], 1)
        self.assertEqual(result['max_error'], 1)

    def test_length_mismatch_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            cq.diff_metrics(FRAME[:-1], FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(str(caught.exception), 'p10 buffer length differs')

    def test_wrong_sample_count_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            cq.diff_metrics(FRAME, FRAME, 65536)
        self.assertEqual(str(caught.exception), 'unexpected p10 sample count')


if __name__ == '__main__':
    unittest.main()
