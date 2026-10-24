#!/usr/bin/env bash
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
#
# RCA #159 SMT control: separate physical cores from runnable logical CPUs.
#
# Every previous arm on this lane counted logical processors. On this host the
# sibling pairs are 0-1, 2-3, 4-5, ... so "taskset -c 0-3" grants TWO physical
# cores, not four, and "taskset -c 0" grants one core of which the decoder can
# use a single thread. A ratio computed against logical CPUs therefore mixes two
# different quantities.
#
# This sweep holds the worker count fixed and varies affinity two ways at the
# same nominal size:
#
#   arm P (physical) : one thread per core, siblings excluded   0,2,4,...
#   arm S (smt)      : both siblings of the same cores          0-1,0-3,0-5,...
#
# If divergence tracks physical cores, arm P and arm S agree at equal N.
# If it tracks runnable threads, arm S diverges at half the N of arm P.
#
# Usage: HYPERCINE_THREAD_PROBE=build/hypercine_decoder_thread_probe \
#        HYPERCINE_PACKET=/path/to/packet.cfhd  ./ci/rca159_smt_control.sh
set -u

PROBE=${HYPERCINE_THREAD_PROBE:-}
PACKET=${HYPERCINE_PACKET:-}
OUT=${HYPERCINE_SWEEP_OUT:-rca159-smt-control}
ROUNDS=${HYPERCINE_ROUNDS:-3}
PROCS=${HYPERCINE_SWEEP_PROCS:-40}
CAP=${HYPERCINE_CAP:-4}

if [ -z "$PROBE" ] || [ ! -x "$PROBE" ]; then
    echo "set HYPERCINE_THREAD_PROBE to a built hypercine_decoder_thread_probe" >&2
    exit 2
fi
if [ -z "$PACKET" ] || [ ! -f "$PACKET" ]; then
    echo "set HYPERCINE_PACKET to a CFHD packet file" >&2
    exit 2
fi

mkdir -p "$OUT"

# One thread per physical core, sibling excluded.
phys () {
    local n=$1 out=""
    local i
    for ((i = 0; i < n; i++)); do
        out="${out}${out:+,}$((i * 2))"
    done
    echo "$out"
}

run_cell () {
    local round=$1 aff=$2 tag=$3
    local dir="$OUT/${tag}"
    mkdir -p "$dir"
    local p
    for ((p = 0; p < PROCS; p++)); do
        taskset -c "$aff" "$PROBE" 1 "$CAP" 0 "$PACKET" \
            > "$dir/r${round}p$(printf %02d "$p").jsonl" 2> "$dir/r${round}p$(printf %02d "$p").err"
    done
    echo "round $round $tag done"
}

for ((r = 1; r <= ROUNDS; r++)); do
    for n in 1 2 3 4 6 8; do
        run_cell "$r" "$(phys "$n")" "phys_${n}c_${CAP}w"
    done
    for n in 1 2 3 4; do
        run_cell "$r" "0-$((n * 2 - 1))" "smt_${n}c_${CAP}w"
    done
done

echo "SMT CONTROL DONE"
