#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
"""Run complete CPU/CUDA FilmScan3 parity and independently decode both packets."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from validate_standalone import avi


def decode(ffmpeg: Path, sample: Path, output: Path) -> bytes:
    wrapper=output.with_suffix('.avi')
    wrapper.write_bytes(avi(sample.read_bytes(),1920,1080))
    subprocess.run([str(ffmpeg),'-v','error','-xerror','-y','-i',str(wrapper),
                    '-frames:v','1','-pix_fmt','yuv422p10le','-f','rawvideo',str(output)],check=True)
    raw=output.read_bytes()
    expected=1920*1080*4
    if len(raw)!=expected:
        raise ValueError(f'FFmpeg output size {len(raw)} != {expected}')
    return raw


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare',required=True,type=Path)
    parser.add_argument('--ffmpeg',required=True,type=Path)
    parser.add_argument('--output-dir',required=True,type=Path)
    args=parser.parse_args()
    # Never delete a caller-chosen directory, and never delete its contents.
    # The test writes only into a subdirectory it creates itself; clearing that
    # one is what makes a rerun reproducible. Anything a caller left behind in
    # the parent survives, because nothing here walks above our own directory.
    if args.output_dir.exists() and not args.output_dir.is_dir():
        raise ValueError(f'output path exists and is not a directory: {args.output_dir}')
    work_dir=args.output_dir/'cuda-filmscan3-run'
    if work_dir.is_symlink():
        raise ValueError(f'refusing to write through a symlinked working directory: {work_dir}')
    if work_dir.is_dir():
        shutil.rmtree(work_dir)
    elif work_dir.exists():
        raise ValueError(f'working path exists and is not a directory: {work_dir}')
    work_dir.mkdir(parents=True)
    run=subprocess.run([str(args.compare),'--output-dir',str(work_dir)],text=True,capture_output=True)
    sys.stdout.write(run.stdout);sys.stderr.write(run.stderr)
    if run.returncode==77:
        print('SKIP77: CUDA GPU required')
        return 77
    if run.returncode!=0:
        raise ValueError(f'complete packet comparator failed with {run.returncode}')
    if 'COEFFICIENT_HASHES: PASS (30/30)' not in run.stdout or 'FULL_PACKET: PASS' not in run.stdout:
        raise ValueError('comparator did not prove all 30 hashes and complete packet equality')
    trace=json.loads((work_dir/'trace.json').read_text())
    cpu_hashes=trace['cpu_coefficient_fnv64'];gpu_hashes=trace['gpu_coefficient_fnv64']
    if len(cpu_hashes)!=30 or cpu_hashes!=gpu_hashes:
        raise ValueError('trace does not contain 30 equal CPU/CUDA coefficient hashes')
    cpu=decode(args.ffmpeg,work_dir/'cpu.cfhd',work_dir/'cpu.yuv')
    gpu=decode(args.ffmpeg,work_dir/'gpu.cfhd',work_dir/'gpu.yuv')
    if cpu!=gpu:
        first=next(i for i,(a,b) in enumerate(zip(cpu,gpu)) if a!=b)
        raise ValueError(f'independent FFmpeg decodes differ at byte {first}')
    digest=hashlib.sha256(cpu).hexdigest()
    print(f'FFMPEG_DECODE: PASS CPU and CUDA packets decoded independently; yuv_sha256={digest}')
    print('COEFFICIENT_HASHES: PASS (30/30 from encoder trace)')
    print('PASS')
    return 0


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (OSError,ValueError,subprocess.CalledProcessError,json.JSONDecodeError) as error:
        print('FAIL:',error)
        raise SystemExit(1)
