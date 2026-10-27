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

Exit status (a caller must not treat a partial write as finished evidence):
  0  every required sweep was supplied, its directory existed, and it
     contributed at least one cell. Required labels: coarse, one-step,
     interleaved, smt, cliff.
  1  an output path and at least one sweep argument were given, but no cell
     was read. The JSON still records which sweeps were missing.
  2  usage: no output path, or no sweep arguments. Nothing is written.
  3  some cells were read, but at least one required sweep is missing, a
     supplied directory was absent, or a supplied directory held no cells.
     The JSON is written with evidence_complete false.

A previous revision skipped a missing directory and still exited 0, and it
stamped logical_cpus as the integer 48. That integer was the machine running
the builder, not a field in the sweep records. This revision records consumed
and missing sweeps, and it labels logical_cpus as the builder host.
"""
import collections
import datetime
import glob
import json
import os
import sys
from math import comb


# Labels in the documented invocation. Anything else may be consumed, but it
# does not fill one of these slots, and a run that omits one is not complete.
REQUIRED_SWEEPS = ("coarse", "one-step", "interleaved", "smt", "cliff")

SWEEP_SCRIPTS = {
    "coarse": "ci/rca159_thread_sweep.sh (coarse reference grid)",
    "one-step": "ci/rca159_ratio_sweep.sh (one-step CPU walk at fixed cap)",
    "interleaved": "ci/rca159_interleaved.sh (all arms per round, drift control)",
    "smt": "ci/rca159_smt_control.sh (physical cores vs SMT siblings)",
    "cliff": "ci/rca159_cliff.sh (fixed cap, CPU 3..6 interleaved)",
}


def load_records(path):
    """Return (call_rows, header_rows) from every jsonl file in a cell directory."""
    rows = []
    headers = []
    for name in sorted(glob.glob(os.path.join(path, "*.jsonl"))):
        for line in open(name, encoding="utf-8"):
            line = line.strip()
            if not line.startswith("{"):
                continue
            record = json.loads(line)
            if "call" in record:
                rows.append(record)
            else:
                headers.append(record)
    return rows, headers


def summarise(path, label, cell):
    rows, headers = load_records(path)
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
    packet_bytes = [h.get("packet_bytes") for h in headers if "packet_bytes" in h]
    packet_sha = [h.get("packet_sha256") for h in headers if "packet_sha256" in h]
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
        "_packet_bytes": packet_bytes,
        "_packet_sha256": packet_sha,
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


def publishable_directory(path):
    """Repo-relative paths may be recorded. Absolute paths are not: a published
    evidence file must not carry a host filesystem location."""
    if os.path.isabs(path):
        return None
    return path


def consensus(values):
    unique = []
    for value in values:
        if value not in unique:
            unique.append(value)
    if len(unique) == 1:
        return unique[0], "sweep_header"
    if not unique:
        return None, "not_in_records"
    return None, "disagreement"


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

    supplied = {label for label, _ in sweeps}
    consumed = []
    missing = []
    cells = []
    packet_bytes_seen = []
    packet_sha_seen = []

    for label, sweep in sweeps:
        if not os.path.isdir(sweep):
            print(f"missing sweep directory: {label}", file=sys.stderr)
            missing.append({
                "label": label,
                "reason": "directory_absent",
                "directory": publishable_directory(sweep),
            })
            continue
        sweep_cells = []
        jsonl_files = 0
        for name in sorted(os.listdir(sweep)):
            path = os.path.join(sweep, name)
            if not os.path.isdir(path):
                continue
            cell = summarise(path, label, name)
            if not cell:
                continue
            packet_bytes_seen.extend(cell.pop("_packet_bytes"))
            packet_sha_seen.extend(cell.pop("_packet_sha256"))
            jsonl_files += cell["fresh_processes"]
            sweep_cells.append(cell)
        if not sweep_cells:
            print(f"sweep has no cells: {label}", file=sys.stderr)
            missing.append({
                "label": label,
                "reason": "no_cells",
                "directory": publishable_directory(sweep),
            })
            continue
        cells.extend(sweep_cells)
        consumed.append({
            "label": label,
            "cells": len(sweep_cells),
            "jsonl_files": jsonl_files,
            "decode_calls": sum(c["decode_calls"] for c in sweep_cells),
            "directory": publishable_directory(sweep),
        })

    for label in REQUIRED_SWEEPS:
        if label not in supplied:
            missing.append({"label": label, "reason": "not_supplied"})
    if missing:
        print("sweeps missing: " + ", ".join(
            f"{entry['label']} ({entry['reason']})" for entry in missing), file=sys.stderr)

    packet_bytes, packet_bytes_source = consensus(packet_bytes_seen)
    packet_sha, packet_sha_source = consensus(packet_sha_seen)
    if packet_bytes_source == packet_sha_source:
        packet_source = packet_bytes_source
    elif "disagreement" in (packet_bytes_source, packet_sha_source):
        packet_source = "disagreement"
    else:
        packet_source = "partial"

    consumed_labels = {entry["label"] for entry in consumed}
    evidence_complete = (
        not missing and all(label in consumed_labels for label in REQUIRED_SWEEPS)
    )

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
    if n_l and n_t:
        fisher_p = fisher_two_sided(d_l, n_l, d_t, n_t)
    else:
        fisher_p = None

    modals = {c["modal_frame_sha256"] for c in cells}
    codes = sorted({code for c in cells for code in c["status_codes"]})

    doc = {
        "license": "MIT OR Apache-2.0",
        "issue": "https://github.com/louis-0hrs/HyperCine/issues/159",
        "evidence_complete": evidence_complete,
        "sweeps_consumed": consumed,
        "sweeps_missing": missing,
        "built_utc": datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"),
        "measured_utc": None,
        "measured_utc_note": (
            "Sweep records do not carry a measurement timestamp. This builder "
            "does not stamp a date it did not read. A previous revision wrote "
            "measured_utc 2026-10-18 into every document, including runs that "
            "did not measure anything that day."
        ),
        "scope": ("Linux CPU-only, pristine frozen upstream SDK. Measurement only: "
                  "no production change, no merge, no release. This file reports "
                  "only the sweeps listed in sweeps_consumed. sweeps_missing were "
                  "not measured by this invocation."),
        "supersedes": ("The '1 < ratio < 2 is untested' gap and the ratio-1.0 "
                       "boundary wording of docs/RCA159_THREAD_OVERSUBSCRIPTION.md "
                       "as of commit 5233a9e, when evidence_complete is true. "
                       "Rates and groupings are re-derived from the consumed "
                       "sweeps only. A partial invocation does not supersede that "
                       "record."),
        "environment": {
            "logical_cpus": {
                "builder_host": os.cpu_count(),
                "measurement_host": None,
                "source": "os.cpu_count of the process that ran this builder",
                "measured_from_sweeps": False,
                "note": (
                    "Not the measurement host. Sweep jsonl records do not contain "
                    "the measurement host's logical CPU count: cpu_max is the "
                    "decoder metadata tag and affinity_cpus is the affinity mask. "
                    "A previous revision wrote the integer 48 here, taken from the "
                    "builder machine, even when a sweep directory was missing."
                ),
            },
            "packet_bytes": packet_bytes,
            "packet_sha256": packet_sha,
            "packet_source": packet_source,
            "unmeasured_fields_note": (
                "os, distribution, compiler, cmake, build type, SMT sibling map "
                "and SDK revision are not in the sweep records. This builder no "
                "longer stamps them. The committed 2026-10-18 evidence document "
                "is a different artefact and is not re-certified by a partial run."
            ),
        },
        "method": {
            "probe": "tools/hypercine_decoder_thread_probe.cpp",
            "required_sweeps": list(REQUIRED_SWEEPS),
            "sweeps": [SWEEP_SCRIPTS[entry["label"]]
                       for entry in consumed if entry["label"] in SWEEP_SCRIPTS],
            "calls_per_process": 1,
            "note": ("One decode call per fresh process: no state from a previous "
                     "frame can be involved. Decodes report OKAY (code 0) even when "
                     "the frame digest differs. Groupings below cover consumed "
                     "sweeps only."),
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
                "fisher_exact_two_sided_p": fisher_p,
            },
        },
        "cells": cells,
    }

    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(doc, handle, indent=2)
        handle.write("\n")
    status = "complete" if evidence_complete else "incomplete"
    print(f"wrote {out_path}: {len(cells)} cells, "
          f"{doc['summary']['total_calls']} calls, evidence_{status}")
    if not cells:
        return 1
    if not evidence_complete:
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
