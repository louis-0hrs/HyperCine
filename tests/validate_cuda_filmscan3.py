#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
"""Run complete CPU/CUDA FilmScan3 parity and independently decode both packets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from validate_standalone import avi


def decode(ffmpeg: Path, sample: Path, output: Path, width: int = 1920, height: int = 1080) -> bytes:
    if sample.is_symlink() or output.is_symlink():
        raise ValueError(f'refusing to write through a symlink: {output}')
    wrapper=output.with_suffix('.avi')
    if wrapper.is_symlink():
        raise ValueError(f'refusing to write through a symlink: {wrapper}')
    wrapper.write_bytes(avi(sample.read_bytes(),width,height))
    subprocess.run([str(ffmpeg),'-v','error','-xerror','-y','-i',str(wrapper),
                    '-frames:v','1','-pix_fmt','yuv422p10le','-f','rawvideo',str(output)],check=True)
    raw=output.read_bytes()
    expected=width*height*4
    if len(raw)!=expected:
        raise ValueError(f'FFmpeg output size {len(raw)} != {expected} for {width}x{height}')
    return raw


def refuse_symlink_in_path(path: Path) -> None:
    if path.is_absolute():
        cursor=Path(path.anchor)
        parts=path.parts[1:]
    else:
        cursor=Path('.')
        parts=path.parts
    for part in parts:
        if part=='.':
            continue
        cursor=cursor/part
        if cursor.is_symlink():
            raise ValueError(f'refusing to write through a symlink: {cursor}')
        if not cursor.exists():
            return


def prepare_output_dir(path: Path) -> None:
    refuse_symlink_in_path(path)
    if path.exists():
        if path.is_symlink() or not path.is_dir():
            raise ValueError(f'output path exists and is not a directory: {path}')
        return
    path.mkdir(parents=True)
    if path.is_symlink() or not path.is_dir():
        raise ValueError(f'output path exists and is not a directory: {path}')


def exclusive_work_dir(output_dir: Path) -> Path:
    # mkdtemp creates one new directory. It never reuses or deletes a path the
    # caller already had, including a previous cuda-filmscan3-run subdirectory.
    work_dir=Path(tempfile.mkdtemp(prefix='cuda-filmscan3-',dir=output_dir))
    if work_dir.is_symlink() or not work_dir.is_dir():
        raise ValueError(f'refusing to use a non-directory working path: {work_dir}')
    output_stat=os.stat(output_dir)
    parent_stat=os.stat(work_dir.parent)
    if (output_stat.st_dev,output_stat.st_ino)!=(parent_stat.st_dev,parent_stat.st_ino):
        raise ValueError(f'working directory escaped the output directory: {work_dir}')
    if work_dir.resolve().parent!=output_dir.resolve():
        raise ValueError(f'working directory is not inside the output directory: {work_dir}')
    return work_dir


def assert_exclusive_artifacts(work_dir: Path) -> None:
    names=('trace.json','cpu.cfhd','gpu.cfhd','cpu.yuv','gpu.yuv','cpu.avi','gpu.avi')
    for name in names:
        artifact=work_dir/name
        if artifact.is_symlink() or not artifact.is_file():
            raise ValueError(f'missing exclusive artifact: {name}')
        if artifact.resolve().parent!=work_dir.resolve():
            raise ValueError(f'artifact escaped the working directory: {artifact}')
    print('ARTIFACTS: '+','.join(str(work_dir/name) for name in names))


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare',required=True,type=Path)
    parser.add_argument('--ffmpeg',required=True,type=Path)
    parser.add_argument('--output-dir',required=True,type=Path)
    parser.add_argument('--geometry',default='1920x1080',
                        help='WIDTHxHEIGHT of the compared packets (default 1920x1080)')
    args=parser.parse_args()
    try:
        width_text,height_text=args.geometry.lower().split('x',1)
        width,height=int(width_text),int(height_text)
    except ValueError:
        raise ValueError(f'--geometry expects WIDTHxHEIGHT, got: {args.geometry}')
    if width<=0 or height<=0:
        raise ValueError(f'--geometry needs positive dimensions, got: {args.geometry}')
    # Never delete a caller-chosen path. Each run writes only into a new
    # mkdtemp directory, so sentinels, a previous cuda-filmscan3-run, and any
    # symlink the caller left behind stay untouched.
    prepare_output_dir(args.output_dir)
    work_dir=exclusive_work_dir(args.output_dir)
    print(f'WORK_DIR: {work_dir}')
    run=subprocess.run([str(args.compare),'--output-dir',str(work_dir),
                        '--geometry',f'{width}x{height}'],text=True,capture_output=True)
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
    cpu=decode(args.ffmpeg,work_dir/'cpu.cfhd',work_dir/'cpu.yuv',width,height)
    gpu=decode(args.ffmpeg,work_dir/'gpu.cfhd',work_dir/'gpu.yuv',width,height)
    if cpu!=gpu:
        first=next(i for i,(a,b) in enumerate(zip(cpu,gpu)) if a!=b)
        raise ValueError(f'independent FFmpeg decodes differ at byte {first}')
    assert_exclusive_artifacts(work_dir)
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
