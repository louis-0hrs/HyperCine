#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic main-entry controls; copied packets are NOT encoder qualification."""
import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import p6_writer_parity as gate


class WriterTraceContract(unittest.TestCase):
    def trace(self):
        return {'frames': [dict(frame_index=i, quant=[1]*36, scale=[2]*36,
                                fs_rate_limiter=4, previous_sample_bytes=0)
                           for i in range(2)]}

    def invoke(self, document, rejected=False, tamper=False):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);corpus=root/'corpus';work=root/'work'
            base=corpus/'filmscan3'/'1080p-16f'
            frames=[];packets=[]
            for i in range(2):
                folder=base/'frames'/'filmscan3-sequence-1920x1080'/('frame_%04d'%i)
                folder.mkdir(parents=True)
                data=bytes([i+1])*32;packets.append(data)
                (folder/'input.bgra').write_bytes(bytes([i+3])*64)
                (folder/'sdk.cfhd').write_bytes(data)
                frames.append(dict(frame_index=i,input_byte_length=64,
                    input_sha256=gate.sha_file(folder/'input.bgra'),
                    sdk_byte_length=len(data),sdk_sha256=gate.sha_file(folder/'sdk.cfhd'),
                    metadata_mask_ranges=[],pattern='synthetic',quant=[1]*36,scale=[2]*36))
            (base/'manifest.json').write_text(json.dumps({'frames':frames}))
            records=['%s\t%d\t%s'%(p.relative_to(corpus).as_posix(),p.stat().st_size,gate.sha_file(p))
                     for p in sorted(corpus.rglob('*')) if p.is_file()]
            (corpus/'TRANSFER_SHA256.tsv').write_text('\n'.join(records)+'\n',encoding='ascii')
            def emit(command, **kwargs):
                directory=work/'filmscan3'/'samples';directory.mkdir(parents=True)
                (directory/'trace.json').write_text(json.dumps(document))
                for i,data in enumerate(packets):
                    if tamper and i==1:data=bytes([99])+data[1:]
                    (directory/('frame_%04d.cfhd'%i)).write_bytes(data)
                return mock.Mock(returncode=0,stderr='')
            argv=['gate','--corpus',str(corpus),'--work',str(work),
                  '--encoder',str(root/'synthetic-encoder'),'--profiles','filmscan3',
                  '--max-frames','2','--report',str(root/'report.json')]
            # Real CLI, files, hashes, transfer index and trace JSON. Only the
            # encoder and CFHD parsing/normalization are stand-ins. No goldens.
            with mock.patch.object(sys,'argv',argv), \
                 mock.patch.object(gate.subprocess,'run',side_effect=emit) as encoder, \
                 mock.patch.object(gate.sample,'parse_sample',return_value={}) as parse, \
                 mock.patch.object(gate.sample,'normalized_sample',side_effect=lambda data,*args:data), \
                 contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                if rejected:
                    with self.assertRaisesRegex(SystemExit,'trace frames .*do not match requested'):
                        gate.main()
                    parse.assert_not_called()
                    self.assertFalse((root/'report.json').exists())
                else:
                    self.assertEqual(gate.main(),1 if tamper else 0)
                    result=json.loads((root/'report.json').read_text())['profiles']['filmscan3']
                    self.assertEqual(result['traced_frames'],2)
                    self.assertEqual(result['all_normalized_equal'],not tamper)
                    self.assertEqual(parse.call_count,4)
                encoder.assert_called_once()

    def test_valid_integer_trace_reaches_comparison(self):
        self.invoke(self.trace())

    def test_packet_divergence_remains_failure(self):
        self.invoke(self.trace(),tamper=True)

    def test_boolean_indices_rejected_before_comparison(self):
        for i in (0,1):
            trace=self.trace();trace['frames'][i]['frame_index']=bool(i)
            with self.subTest(index=i):self.invoke(trace,rejected=True)

    def test_float_indices_rejected_before_comparison(self):
        for i in (0,1):
            trace=self.trace();trace['frames'][i]['frame_index']=float(i)
            with self.subTest(index=i):self.invoke(trace,rejected=True)

    def test_missing_null_and_string_indices(self):
        for value in (None,'0'):
            trace=self.trace();trace['frames'][0]['frame_index']=value
            with self.subTest(value=value):self.invoke(trace,rejected=True)
        trace=self.trace();del trace['frames'][0]['frame_index']
        self.invoke(trace,rejected=True)

    def test_malformed_frame_shapes(self):
        for value in (None,False,0,'frame',[]):
            trace=self.trace();trace['frames'][0]=value
            with self.subTest(value=value):self.invoke(trace,rejected=True)

    def test_malformed_frame_lists(self):
        for value in (None,False,0,'frames',{}, {'frame_index':0}):
            with self.subTest(value=value):self.invoke({'frames':value},rejected=True)

    def test_malformed_documents_and_missing_frames(self):
        for value in (None,False,0,'frames',[],{}):
            with self.subTest(value=value):self.invoke(value,rejected=True)

    def test_truncated_extra_reordered_and_duplicate_indices(self):
        for indices in ([0],[0,1,2],[1,0],[0,0]):
            trace=self.trace();trace['frames']=[dict(copy.deepcopy(trace['frames'][0]),frame_index=i)
                                              for i in indices]
            with self.subTest(indices=indices):self.invoke(trace,rejected=True)


if __name__=='__main__':
    unittest.main()
