#!/usr/bin/env python3
"""Consolidate the RCA #159 follow-up sweeps into one evidence document.

The first RCA #159 pass compared "oversubscribed" against "not oversubscribed"
on a coarse grid (thread caps 1/2/4/8/16 against affinity 1/2/4/8/16/48). That
grid never sampled the band 1 < workers/cpus < 2, so the claim "the boundary sits
exactly at ratio 1.0" was never measured between those points.

This builder consumes the follow-up sweeps, which walk the CPU count one step at
a time at fixed thread caps, plus the interleaved and SMT controls, and reports
the rate grouped two ways:

  by ratio   (workers / affinity_cpus)
  by cpus    (absolute count of CPUs the process may run on)

Both groupings are emitted on purpose: the two are confounded on a grid, and the
document only claims a grouping after the other one has been checked.

Usage:
  build_rca159_ratio_evidence.py <output.json> <label>=<sweep_dir>...

Each sweep directory holds one subdirectory per cell with one ``*.jsonl`` per
fresh process. Label prefixes every cell id in the output.
"""
import collections
import glob
import json
import os
import sys
from math import comb


def load_rows(path):
    rows = []
    for name in sorted(glob.glob(os.path.join(path, "*.jsonl"))):
        for line in open(name, encoding="utf-8"):
            line = line.strip()
            if not line.startswith("{"):
                continue
            record = json.loads(line)
            if "call" in record:
                rows.append(record)
    return rows


def summarise(path, label, cell):
    rows = load_rows(path)
    if not rows:
        return None
    digests = [r.get("frame_sha256") or r.get("pixel_sha256") for r in rows]
    if any(d is None for d in digests):
        return None
    counts = collections.Counter(digests)
    modal, modal_calls = counts.most_common(1)[0]
    first = rows[0]
    threads = first.get("decoder_threads")
    if threads is None:
        threads = first.get("threads")
    workers = threads - 1 if isinstance(threads, int) else None
    cpus = first.get("affinity_cpus")
    return {
        "sweep": label,
        "cell": cell,
        "fresh_processes": len(glob.glob(os.path.join(path, "*.jsonl"))),
        "decode_calls": len(rows),
        "calls_per_process": 1,
        "cpu_max_metadata": first.get("cpu_max"),
        "decoder_threads": threads,
        "worker_threads": workers,
        "affinity_cpus": cpus,
        "ratio": round(workers / cpus, 4) if workers and cpus else None,
        "oversubscribed": (workers is not None and cpus is not None
                           and workers > cpus),
        "modal_frame_calls": modal_calls,
        "divergent_calls": len(rows) - modal_calls,
        "divergence_rate_pct": round(100.0 * (len(rows) - modal_calls) / len(rows), 2),
        "distinct_frame_digests": len(counts),
        "status_codes": sorted({r.get("code") for r in rows}),
        "geometry": sorted({(row.get("width"), row.get("height"), row.get("pitch"))
                             for row in rows}),
        "modal_frame_sha256": modal,
    }


def fisher_two_sided(k1, n1, k2, n2):
    total, N = k1 + k2, n1 + n2

    def hyper(i):
        return comb(n1, i) * comb(n2, total - i) / comb(N, total)

    observed = hyper(k1)
    p = 0.0
    for i in range(0, min(n1, total) + 1):
        prob = hyper(i)
        if prob <= observed + 1e-18:
            p += prob
    return p


def group(cells, key):
    out = collections.defaultdict(lambda: {"cells": 0, "calls": 0, "divergent": 0})
    for cell in cells:
        bucket = out[key(cell)]
        bucket["cells"] += 1
        bucket["calls"] += cell["decode_calls"]
        bucket["divergent"] += cell["divergent_calls"]
    for value in out.values():
        value["rate_pct"] = round(100.0 * value["divergent"] / value["calls"], 2)
    return dict(sorted(out.items(), key=lambda kv: (kv[0] is None, kv[0])))


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out_path = sys.argv[1]
    sweeps = []
    for arg in sys.argv[2:]:
        label, _, path = arg.partition("=")
        if not path:
            label, path = os.path.basename(os.path.normpath(arg)), arg
        sweeps.append((label, path))

    cells = []
    for label, sweep in sweeps:
        if not os.path.isdir(sweep):
            print(f"skip missing sweep: {sweep}", file=sys.stderr)
            continue
        for name in sorted(os.listdir(sweep)):
            path = os.path.join(sweep, name)
            if not os.path.isdir(path):
                continue
            cell = summarise(path, label, name)
            if cell:
                cells.append(cell)

    if not cells:
        print("no cells found", file=sys.stderr)
        return 1

    by_ratio = group(cells, lambda c: f"r={c['ratio']}")
    by_cpus = group(cells, lambda c: c["affinity_cpus"])
    by_workers = group(cells, lambda c: c["worker_threads"])

    # The bands that the follow-up sweeps actually fill, and the contrast that
    # separates "ratio drives it" from "absolute CPU count drives it":
    #   high ratio but >=5 CPUs  vs  low ratio but <=4 CPUs
    loose = [c for c in cells if c["ratio"] and c["ratio"] >= 2 and c["affinity_cpus"] >= 5]
    tight = [c for c in cells if c["ratio"] and c["ratio"] <= 1.5 and c["affinity_cpus"] <= 4]
    n_l = sum(c["decode_calls"] for c in loose)
    d_l = sum(c["divergent_calls"] for c in loose)
    n_t = sum(c["decode_calls"] for c in tight)
    d_t = sum(c["divergent_calls"] for c in tight)

    modals = {c["modal_frame_sha256"] for c in cells}
    codes = sorted({code for c in cells for code in c["status_codes"]})

    doc = {
        "license": "MIT OR Apache-2.0",
        "issue": "https://github.com/louis-0hrs/HyperCine/issues/159",
        "measured_utc": "2026-10-18",
        "scope": ("Linux CPU-only, pristine frozen upstream SDK. Measurement only: "
                  "no production change, no merge, no release."),
        "supersedes": ("The '1 < ratio < 2 is untested' gap and the ratio-1.0 "
                       "boundary wording of docs/RCA159_THREAD_OVERSUBSCRIPTION.md "
                       "as of commit 5233a9e. Rates and groupings are re-derived "
                       "from these sweeps; the older coarse-grid cells stay in the "
                       "record and are not silently dropped."),
        "environment": {
            "os": "Linux x86_64",
            "distribution": "Debian GNU/Linux 13",
            "logical_cpus": 48,
            "smt_siblings": "pairs 0-1, 2-3, 4-5, ...",
            "compiler": "GCC 14.2.0",
            "cmake": "3.31.6",
            "build_type": "Release",
            "sdk_revision": "11574d0295771edccadd17af14af74a539f924c7",
            "sdk_tree_unmodified": True,
            "packet_bytes": 900396,
            "packet_sha256":
                "2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52",
        },
        "method": {
            "probe": "tools/hypercine_decoder_thread_probe.cpp",
            "sweeps": [
                "ci/rca159_thread_sweep.sh (coarse reference grid)",
                "ci/rca159_ratio_sweep.sh (one-step CPU walk at fixed cap)",
                "ci/rca159_interleaved.sh (all arms per round, drift control)",
                "ci/rca159_smt_control.sh (physical cores vs SMT siblings)",
                "ci/rca159_cliff.sh (fixed cap, CPU 3..6 interleaved)",
            ],
            "calls_per_process": 1,
            "note": ("One decode call per fresh process: no state from a previous "
                     "frame can be involved. Decodes report OKAY (code 0) even when "
                     "the frame digest differs."),
            "affinity_source": "sched_getaffinity(0)",
            "thread_source": "/proc/self/status Threads after the decode returns",
            "significance_test": "Fisher exact, two-sided",
        },
        "summary": {
            "total_cells": len(cells),
            "total_calls": sum(c["decode_calls"] for c in cells),
            "distinct_modal_frame_digests_across_all_cells": len(modals),
            "modal_frame_sha256": sorted(modals)[0] if len(modals) == 1 else None,
            "status_codes_observed": codes,
            "divergence_by_ratio": by_ratio,
            "divergence_by_affinity_cpus": by_cpus,
            "divergence_by_worker_threads": by_workers,
            "contrast_high_ratio_wide_cpu_vs_low_ratio_narrow_cpu": {
                "high_ratio_ge2_and_ge5_cpus": {
                    "cells": len(loose), "calls": n_l, "divergent": d_l,
                    "rate_pct": round(100.0 * d_l / n_l, 2) if n_l else None,
                },
                "low_ratio_le1_5_and_le4_cpus": {
                    "cells": len(tight), "calls": n_t, "divergent": d_t,
                    "rate_pct": round(100.0 * d_t / n_t, 2) if n_t else None,
                },
                "fisher_exact_two_sided_p": fisher_two_sided(d_l, n_l, d_t, n_t),
            },
        },
        "cells": cells,
    }

    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(doc, handle, indent=2)
        handle.write("\n")
    print(f"wrote {out_path}: {len(cells)} cells, "
          f"{doc['summary']['total_calls']} calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
