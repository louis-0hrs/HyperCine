#!/usr/bin/env bash
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
#
# RCA #159 sweep: separate decoder thread count from available CPUs.
#
# The frozen upstream SDK sizes its worker pools from sysconf(_SC_NPROCESSORS_ONLN),
# which ignores an affinity mask. So a "1 logical processor" arm implemented with
# an affinity tool alone does not limit the decoder at all. This sweep varies the
# two quantities independently and records both:
#
#   affinity : CPUs the process may run on (taskset / sched_getaffinity)
#   cpu_max  : TAG_CPU_MAX metadata value (the only real decoder thread cap)
#
# Each cell runs N fresh processes with ONE decode call each, so no state from a
# previous frame can be involved.
#
# Usage: HYPERCINE_THREAD_PROBE=build/hypercine_decoder_thread_probe \
#        HYPERCINE_PACKET=/path/to/packet.cfhd  ./ci/rca159_thread_sweep.sh
set -u

PROBE=${HYPERCINE_THREAD_PROBE:-}
PACKET=${HYPERCINE_PACKET:-}
OUT=${HYPERCINE_SWEEP_OUT:-rca159-sweep}
PROCS=${HYPERCINE_SWEEP_PROCS:-20}

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
    local aff=$1 cm=$2 tag=$3
    local dir="$OUT/$tag"
    mkdir -p "$dir"
    local p
    for ((p = 0; p < PROCS; p++)); do
        taskset -c "$aff" "$PROBE" 1 "$cm" 0 "$PACKET" \
            > "$dir/p$(printf %02d "$p").jsonl" 2> "$dir/p$(printf %02d "$p").err"
    done
    echo "done $tag"
}

# Part 1: thread cap varies, affinity fixed wide (no oversubscription possible).
for cm in 0 1 2 4 8 16; do
    run_cell 0-47 "$cm" "wide_cm${cm}"
done

# Part 2: thread cap fixed at 4 (=> 9 threads), affinity narrowed step by step.
# The transition from oversubscribed to saturated is the interesting boundary.
for aff in 0 0-1 0-3 0-7 0-15 0-47; do
    run_cell "$aff" 4 "cm4_aff${aff}"
done

echo "SWEEP DONE"
