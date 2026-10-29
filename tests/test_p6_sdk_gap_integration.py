#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Optional real-corpus falsifiers at the comparison site, not fabricated PASS."""
import argparse
import json
import os
import sys
import tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import p6_sdk_gap_gate as gate


def main():
    p=argparse.ArgumentParser();p.add_argument('--corpus',type=Path,required=True)
    a=p.parse_args();root=a.corpus.resolve();index=gate.load_transfer_index(root)
    with tempfile.TemporaryDirectory() as tmp:
        work=Path(tmp);runner=work/'fake_encoder.py'
        runner.write_text('#!'+sys.executable+'\n'+"""
import json,os,shutil,sys
from pathlib import Path
root=Path(os.environ['GAP_ROOT']);mode=os.environ.get('GAP_MUTATION','')
m=json.loads((root/'low/sequence-120f/manifest.json').read_text())
out=Path(sys.argv[5]);out.mkdir(parents=True)
trace=[]
for f in m['frames']:
 i=f['frame_index'];shutil.copyfile(root/f['packet'],out/('frame_%04d.cfhd'%i));trace.append({'frame_index':i})
if mode=='truncate':trace=trace[:-1]
if mode=='reorder':trace[1],trace[2]=trace[2],trace[1]
if mode=='tamper':
 p=out/'frame_0000.cfhd';b=bytearray(p.read_bytes());b[87]^=1;p.write_bytes(b)
(out/'trace.json').write_text(json.dumps({'frames':trace}))
""")
        runner.chmod(0o700);os.environ['GAP_ROOT']=str(root)
        for mode in ['','truncate','reorder','tamper']:
            os.environ['GAP_MUTATION']=mode
            try: result=gate.measure(root,index,runner,work/(mode or 'valid'),'low','sequence-120f',120)
            except ValueError as error:
                if mode not in ('truncate','reorder') or 'count/order' not in str(error):raise
            else:
                if mode=='' and result['status']!='PASS':raise AssertionError('valid baseline rejected')
                elif mode=='tamper':
                    if result['status']!='FAIL' or result['frames'][0]['first_diff']!=87:
                        raise AssertionError('same-length output mutation escaped')
                elif mode:raise AssertionError('negative control escaped: '+mode)
            print('comparison-site control',mode or 'baseline','PASS')

if __name__=='__main__':main()
