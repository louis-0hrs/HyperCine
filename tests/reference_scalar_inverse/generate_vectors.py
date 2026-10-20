#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Synthetic operator-contract vectors ONLY; never an original SDK oracle.

Python unbounded integers/Fraction and explicit operator sequences independently
encode the source-reviewed proposal. The C++ unit does not generate expectations.
"""
from fractions import Fraction
from pathlib import Path
import argparse


def floor(v, shift):
    return Fraction(v, 2**shift).__floor__()


def narrow(v, kind):
    if kind == 'S':
        return max(-32768, min(32767, v))
    if kind == 'W':
        return ((v + 32768) % 65536) - 32768
    return v


def row(low, high, mode):
    result = []
    n = len(low)
    for x in range(n):
        if x == 0 or x == n-1:
            a, b, c = (low[:3] if x == 0 else low[-1:-4:-1])
            weights = ((11,-4,1),(5,4,-1)) if x == 0 else ((5,4,-1),(11,-4,1))
            for sign, weights_one in zip((1,-1), weights):
                v = floor(sum(i*j for i,j in zip((a,b,c), weights_one))+4,3)
                v = floor(v+sign*high[x],1)
                result.append(narrow(v, 'S' if mode == 'vertical' or x == n-1 else 'W'))
        else:
            # Vertical is wide until final storage; the selected horizontal
            # original changes operator semantics at the vector/tail boundary.
            kind = None if mode == 'vertical' else ('S' if x <= 8*((n-1)//8) else 'W')
            for sign in (1,-1):
                if sign == 1:
                    v = narrow(low[x-1]-low[x+1],kind)
                elif kind == 'W':
                    v = narrow(narrow(-low[x-1],kind)+low[x+1],kind)
                else:
                    v = narrow(low[x+1]-low[x-1],kind)
                v = floor(narrow(v+4,kind),3)
                v = narrow(v+low[x],kind)
                v = narrow(v+sign*high[x],kind)
                result.append(narrow(floor(v,1),'S'))
    return result


def synth(w,h,seed,force_borders=False):
    extremes = [-32768,-32767,-16385,-16384,-9,-8,-7,-4,-1,0,1,3,4,7,8,9,16383,16384,32766,32767]
    planes = []
    state = seed
    for b in range(4):
        values = []
        for i in range(w*h):
            state = (1664525*state+1013904223) % (2**32)
            values.append(extremes[(state >> 16) % len(extremes)])
        planes.append(values)
    if force_borders:
        # Same vertical rows with LL==HL and LH==HH create chosen extreme
        # even-row horizontal inputs exactly, with zero odd rows. Both border
        # storage rules are forced to differ from the opposite narrowing.
        low=[0]*w;low[:3]=[32767,-32768,32767];low[-3:]=[-32768,32767,32767]
        planes=[low*h,[32767]*(w*h),low*h,[32767]*(w*h)]
    strips = []
    for b in (0,1):
        columns = [row([planes[b][y*w+x] for y in range(h)],
                       [planes[b+2][y*w+x] for y in range(h)], 'vertical') for x in range(w)]
        strips.append([[columns[x][y] for x in range(w)] for y in range(2*h)])
    output = [v for y in range(2*h) for v in row(strips[0][y],strips[1][y],'horizontal')]
    return planes,output


def render():
    lines = ['// Copyright (c) 2026 HyperCine Contributors.',
             '// Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.',
             '// Generated SYNTHETIC operator vectors; NOT original SDK bytes.',
             '#pragma once', '#include <cstdint>', '#include <vector>',
             'namespace ScalarSynthetic {',
             'struct Case { unsigned width, height; std::vector<std::int16_t> input[4]; std::vector<std::int16_t> expected; };',
             'inline const std::vector<Case> cases{']
    for w,h,seed,forced in [(16,3,0x1337BEEF,False),(24,4,0x1337BEF0,False),(16,3,0,True)]:
        planes,out = synth(w,h,seed,forced)
        values = lambda a: '{'+','.join(map(str,a))+'}'
        lines += ['{'+f'{w},{h},'+'{'+','.join(values(a) for a in planes)+'},'+values(out)+'},']
    lines += ['};', '} // namespace ScalarSynthetic', '']
    return '\n'.join(lines)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check',action='store_true')
    args=ap.parse_args()
    path=Path(__file__).with_name('synthetic_vectors.h')
    content=render()
    if args.check:
        if path.read_text()!=content:
            raise SystemExit('Synthetic vectors differ from independent operator model')
        print('PASS: three synthetic operator fixtures reproduce exactly; no SDK parity claim')
    else:
        path.write_text(content)


if __name__=='__main__':
    main()
