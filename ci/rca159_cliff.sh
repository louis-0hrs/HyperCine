#!/usr/bin/env bash
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
#
# RCA #159 cliff control: hold the decoder thread count constant and step the
# available CPU count one at a time across the transition, interleaved.
#
# The earlier sweeps varied the cap and the affinity on a coarse grid, which
# left the transition between ratio 1.0 and ratio 2.0 unmeasured. This control
# fixes one thread cap and walks CPU count 3,4,5,6 in every round, so a host
# drift cannot be mistaken for a boundary effect.
#
# Usage: HYPERCINE_THREAD_PROBE=build/hypercine_decoder_thread_probe \
#        HYPERCINE_PACKET=/path/to/packet.cfhd  ./ci/rca159_cliff.sh
set -u

PROBE=${HYPERCINE_THREAD_PROBE:-}
PACKET=${HYPERCINE_PACKET:-}
OUT=${HYPERCINE_SWEEP_OUT:-rca159-cliff}
ROUNDS=${HYPERCINE_ROUNDS:-5}
PROCS=${HYPERCINE_SWEEP_PROCS:-30}
CAP=${HYPERCINE_CAP:-8}

if [ -z "$PROBE" ] || [ ! -x "$PROBE" ]; then
    echo "set HYPERCINE_THREAD_PROBE to a built hypercine_decoder_thread_probe" >&2
    exit 2
fi
if [ -z "$PACKET" ] || [ ! -f "$PACKET" ]; then
    echo "set HYPERCINE_PACKET to a CFHD packet file" >&2
    exit 2
fi

mkdir -p "$OUT"

run_cell () {
    local round=$1 aff=$2 tag=$3
    local dir="$OUT/${tag}"
    mkdir -p "$dir"
    local p
    for ((p = 0; p < PROCS; p++)); do
        taskset -c "0-$((aff - 1))" "$PROBE" 1 "$CAP" 0 "$PACKET" \
            > "$dir/r${round}p$(printf %02d "$p").jsonl" 2> "$dir/r${round}p$(printf %02d "$p").err"
    done
    echo "round $round $tag done"
}

for ((r = 1; r <= ROUNDS; r++)); do
    for aff in 3 4 5 6; do
        run_cell "$r" "$aff" "cap8_cpus${aff}"
    done
done

echo "CLIFF DONE"
