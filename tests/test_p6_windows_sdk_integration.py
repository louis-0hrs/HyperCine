#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Comparison-site falsifiers using verified original packets, not encoder proof."""
import argparse
import json
import os
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import p6_windows_sdk_gate as gate


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--corpus', type=Path, required=True)
    args = parser.parse_args()
    root = (args.corpus/'group-c-sdk-state').resolve()
    index = gate.inventory(root, gate.PINS['group-c-sdk-state'])
    m = json.loads(gate.verified(root, 'manifest.json', index).read_text())
    session = gate.validate_sessions(m, 'group-c-sdk-state')[0]
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp); runner = work/'fake_encoder.py'
        runner.write_text("""
import json, os, shutil, sys
from pathlib import Path
root=Path(os.environ['WINDOWS_GATE_ROOT']); mode=os.environ['WINDOWS_GATE_MUTATION']
frames=json.loads((root/'manifest.json').read_text())['sessions'][0]['frames']
out=Path(sys.argv[5]);out.mkdir(parents=True)
trace=[]
for f in frames:
 i=f['frame_index'];shutil.copyfile(root/f['sdk_packet'],out/('frame_%04d.cfhd'%i))
 trace.append(dict(frame_index=i,fs_overbitrate=f['sdk_overbitrate_after_encode'],
                   fs_rate_limiter=f['sdk_rate_limiter_after_encode'],previous_sample_bytes=f['previous_sdk_sample_bytes'] or 0))
if mode=='truncate':trace.pop()
if mode=='reorder':trace[1],trace[2]=trace[2],trace[1]
if mode=='state':trace[3]['fs_overbitrate']=0
if mode=='previous':trace[0]['previous_sample_bytes']=1
if mode=='tamper':
 p=out/'frame_0000.cfhd';data=bytearray(p.read_bytes());data[87]^=1;p.write_bytes(data)
if mode=='missing':(out/'frame_0002.cfhd').unlink()
(out/'trace.json').write_text(json.dumps(dict(frames=trace)))
""")
        os.environ['WINDOWS_GATE_ROOT'] = str(root)
        for mode in ('baseline', 'truncate', 'reorder', 'state', 'previous', 'tamper', 'missing'):
            os.environ['WINDOWS_GATE_MUTATION'] = mode
            try:
                result = gate.compare(root, index, session, runner, work/mode, True)
            except ValueError as error:
                if mode not in ('truncate', 'reorder') or 'count/order' not in str(error):
                    raise
            except FileNotFoundError:
                if mode != 'missing':
                    raise
            else:
                if mode == 'baseline':
                    if result['status'] != 'PASS': raise AssertionError('valid baseline rejected')
                elif mode in ('state', 'previous', 'tamper'):
                    if result['status'] != 'FAIL': raise AssertionError('mutation escaped')
                    if mode == 'tamper' and result['frames'][0]['first_diff'] != 87:
                        raise AssertionError('same-length mutation not localized')
                else:
                    raise AssertionError('negative control escaped')
            print('comparison-site control', mode, 'PASS')

if __name__ == '__main__':
    main()
