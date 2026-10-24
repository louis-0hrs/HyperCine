#!/usr/bin/env python3
"""Build the sanitized RCA #159 evidence document from sweep directories.

Accepts both probe generations: the development probe emitted
``pixel_sha256``/``threads``, the repository probe emits
``frame_sha256``/``decoder_threads``. Both are normalised here so a single
document can cite every run.

Usage:
  build_rca159_evidence.py <output.json> <sweep_dir>...

Each sweep directory is expected to contain one subdirectory per cell, holding
one ``*.jsonl`` per fresh process.
"""
import collections
import glob
import json
import os
import sys
from math import comb


def load_cell(path):
    rows = []
    for name in sorted(glob.glob(os.path.join(path, "*.jsonl"))):
        for line in open(name, encoding="utf-8"):
            line = line.strip()
            if not line.startswith("{"):
                continue
            record = json.loads(line)
            if "call" not in record:
                continue
            rows.append(record)
    return rows


def normalise(record):
    """Map either probe generation onto one vocabulary."""
    digest = record.get("frame_sha256") or record.get("pixel_sha256")
    threads = record.get("decoder_threads")
    if threads is None:
        threads = record.get("threads")
    cpus = record.get("affinity_cpus")
    return {
        "digest": digest,
        "threads": threads,
        "workers": threads - 1 if isinstance(threads, int) else None,
        "cpus": cpus,
        "cpu_max": record.get("cpu_max"),
        "code": record.get("code"),
        "width": record.get("w", record.get("width")),
        "height": record.get("h", record.get("height")),
        "pitch": record.get("pitch"),
    }


def summarise(path, experiment):
    raw = load_cell(path)
    if not raw:
        return None
    rows = [normalise(r) for r in raw]
    if any(r["digest"] is None for r in rows):
        return None
    counts = collections.Counter(r["digest"] for r in rows)
    modal, modal_calls = counts.most_common(1)[0]
    first = rows[0]
    return {
        "experiment": experiment,
        "cell": os.path.basename(path),
        "fresh_processes": len(glob.glob(os.path.join(path, "*.jsonl"))),
        "decode_calls": len(rows),
        "calls_per_process": 1,
        "affinity_cpus": first["cpus"],
        "decoder_threads": first["threads"],
        "worker_threads": first["workers"],
        "cpu_max_metadata": first["cpu_max"],
        "oversubscribed": (first["workers"] is not None
                           and first["cpus"] is not None
                           and first["workers"] > first["cpus"]),
        "modal_frame_calls": modal_calls,
        "divergent_calls": len(rows) - modal_calls,
        "divergence_rate_pct": round(100.0 * (len(rows) - modal_calls) / len(rows), 2),
        "distinct_frame_digests": len(counts),
        "status_codes": sorted({r["code"] for r in rows}),
        "geometry": sorted({(r["width"], r["height"], r["pitch"]) for r in rows}),
        "modal_frame_sha256": modal,
    }


def fisher_two_sided(k, n1, n2, total_k):
    """Two-sided Fisher exact p for k divergences in group 1."""
    def hyper(i):
        return comb(n1, i) * comb(n2, total_k - i) / comb(n1 + n2, total_k)
    observed = hyper(k)
    p = 0.0
    for i in range(0, min(n1, total_k) + 1):
        prob = hyper(i)
        if prob <= observed + 1e-18:
            p += prob
    return p


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out_path, sweeps = sys.argv[1], sys.argv[2:]

    cells = []
    for sweep in sweeps:
        if not os.path.isdir(sweep):
            continue
        for name in sorted(os.listdir(sweep)):
            path = os.path.join(sweep, name)
            if not os.path.isdir(path):
                continue
            cell = summarise(path, os.path.basename(os.path.normpath(sweep)))
            if cell:
                cells.append(cell)

    over = [c for c in cells if c["oversubscribed"]]
    sat = [c for c in cells if not c["oversubscribed"]]
    n_over = sum(c["decode_calls"] for c in over)
    d_over = sum(c["divergent_calls"] for c in over)
    n_sat = sum(c["decode_calls"] for c in sat)
    d_sat = sum(c["divergent_calls"] for c in sat)
    p = fisher_two_sided(d_over, n_over, n_sat, d_over + d_sat) if cells else None

    modals = {c["modal_frame_sha256"] for c in cells}
    codes = sorted({code for c in cells for code in c["status_codes"]})

    doc = {
        "license": "MIT OR Apache-2.0",
        "issue": "https://github.com/louis-0hrs/HyperCine/issues/159",
        "measured_utc": "2026-10-18",
        "scope": ("Linux CPU-only, pristine frozen upstream SDK. Measurement only: "
                  "no production change, no merge, no release."),
        "disclaimer": ("Thread count and CPU affinity are separate variables. The frozen "
                       "SDK sizes its worker pools from sysconf(_SC_NPROCESSORS_ONLN), "
                       "which ignores an affinity mask. An arm described as '1 logical "
                       "processor' and implemented with affinity alone therefore does "
                       "not limit the decoder."),
        "environment": {
            "os": "Linux x86_64",
            "distribution": "Debian GNU/Linux 13",
            "logical_cpus": 48,
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
            "sweep": "ci/rca159_thread_sweep.sh",
            "calls_per_process": 1,
            "note": ("One decode call per fresh process: no state from a previous frame "
                     "can be involved. Decodes report OKAY (code 0) even when the frame "
                     "digest differs."),
            "affinity_source": "sched_getaffinity(0)",
            "thread_source": "/proc/self/status Threads after the decode returns",
            "significance_test":
                "Fisher exact, two-sided, on divergent calls vs oversubscription",
        },
        "summary": {
            "total_cells": len(cells),
            "total_calls": n_over + n_sat,
            "oversubscribed_cells": len(over),
            "oversubscribed_calls": n_over,
            "oversubscribed_divergent": d_over,
            "oversubscribed_rate_pct": round(100.0 * d_over / n_over, 2) if n_over else None,
            "saturated_cells": len(sat),
            "saturated_calls": n_sat,
            "saturated_divergent": d_sat,
            "saturated_rate_pct": round(100.0 * d_sat / n_sat, 2) if n_sat else None,
            "fisher_exact_two_sided_p": p,
            "distinct_modal_frame_digests_across_all_cells": len(modals),
            "modal_frame_sha256": sorted(modals)[0] if len(modals) == 1 else None,
            "status_codes_observed": codes,
        },
        "cells": cells,
    }

    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(doc, handle, indent=2)
        handle.write("\n")
    print(f"wrote {out_path}: {len(cells)} cells, {n_over + n_sat} calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
