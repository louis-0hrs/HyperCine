# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Directional-regret report for the frozen Phase 3A Auto rule.

Definitions are fixed before the fresh corpus is timed. This script does not
fit a threshold, drop a case, or change a strategy. Near-ties are a reporting
cut only. Material errors cannot fall inside that cut.
"""
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "benchmarks" / "results"
NEAR_ABS_MS = 0.25
NEAR_REL = 0.05


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def median(values):
    if not values:
        return 0.0
    ordered = sorted(values)
    n = len(ordered)
    if n % 2:
        return ordered[n // 2]
    return 0.5 * (ordered[n // 2 - 1] + ordered[n // 2])


def mean(values):
    return sum(values) / len(values) if values else 0.0


def oracle_name(light, hybrid):
    if light < hybrid:
        return "lightweight"
    if hybrid < light:
        return "hybrid"
    return "tie"


def near_tie(light, hybrid):
    gap = abs(light - hybrid)
    base = min(light, hybrid)
    return gap < NEAR_ABS_MS or (base > 0 and gap / base < NEAR_REL)


def classify_case(auto, light, hybrid):
    oracle = oracle_name(light, hybrid)
    if oracle == "tie" or auto == oracle:
        kind = "correct_lightweight" if auto == "lightweight" else "correct_hybrid"
        regret = 0.0
    elif auto == "hybrid" and oracle == "lightweight":
        kind = "conservative"
        regret = hybrid - light
    elif auto == "lightweight" and oracle == "hybrid":
        kind = "dangerous"
        regret = light - hybrid
    else:
        kind = "unknown"
        regret = 0.0
    faster = min(light, hybrid)
    relative = (regret / faster) if faster else 0.0
    row = {
        "kind": kind,
        "oracle": oracle,
        "auto": auto,
        "absolute_regret_ms": regret,
        "relative_regret": relative,
        "light_ms": light,
        "hybrid_ms": hybrid,
        "ratio_l_over_h": (light / hybrid) if hybrid else None,
        "ratio_h_over_l": (hybrid / light) if light else None,
        "near_tie": near_tie(light, hybrid),
    }
    if kind == "dangerous":
        row["material"] = regret > 1.0 or relative > 0.20
        row["severe"] = regret > 5.0 or light > 1.5 * hybrid
        row["catastrophic"] = regret > 10.0 or light > 2.0 * hybrid
    return row


def direction_summary(rows, label, historical=False):
    dangerous = [r for r in rows if r["kind"] == "dangerous"]
    conservative = [r for r in rows if r["kind"] == "conservative"]
    correct_l = [r for r in rows if r["kind"] == "correct_lightweight"]
    correct_h = [r for r in rows if r["kind"] == "correct_hybrid"]
    material = [r for r in dangerous if r["material"]]
    severe = [r for r in dangerous if r["severe"]]
    catastrophic = [r for r in dangerous if r["catastrophic"]]
    cons_pen = [r["absolute_regret_ms"] for r in conservative]
    dang_pen = [r["absolute_regret_ms"] for r in dangerous]
    hybrid_win = [r["ratio_l_over_h"] for r in rows if r["oracle"] == "hybrid" and r["ratio_l_over_h"]]
    light_win = [r["ratio_h_over_l"] for r in rows if r["oracle"] == "lightweight" and r["ratio_h_over_l"]]
    decided = [r for r in rows if not r["near_tie"]]
    decided_dangerous = [r for r in decided if r["kind"] == "dangerous"]
    n = len(rows)

    def rate(count):
        return (count / n) if n else 0.0

    def pack(items):
        return {
            "count": len(items),
            "ids": [item["id"] for item in items],
        }

    return {
        "label": label,
        "historical": historical,
        "frames": n,
        "correct_lightweight": len(correct_l),
        "correct_hybrid": len(correct_h),
        "conservative_misses": len(conservative),
        "dangerous_misses": len(dangerous),
        "accuracy": ((len(correct_l) + len(correct_h)) / n) if n else 0.0,
        "dangerous_miss_rate": rate(len(dangerous)),
        "material_dangerous_misses": len(material),
        "material_dangerous_miss_rate": rate(len(material)),
        "severe_dangerous_misses": len(severe),
        "severe_dangerous_miss_rate": rate(len(severe)),
        "catastrophic_dangerous_misses": len(catastrophic),
        "catastrophic_dangerous_miss_rate": rate(len(catastrophic)),
        "dangerous_total_regret_ms": sum(dang_pen),
        "dangerous_mean_regret_ms": mean(dang_pen),
        "dangerous_worst_regret_ms": max(dang_pen) if dang_pen else 0.0,
        "conservative_total_regret_ms": sum(cons_pen),
        "conservative_mean_regret_ms": mean(cons_pen),
        "conservative_median_regret_ms": median(cons_pen),
        "conservative_p95_regret_ms": percentile(cons_pen, 0.95),
        "conservative_worst_regret_ms": max(cons_pen) if cons_pen else 0.0,
        "conservative_relative_mean": mean([r["relative_regret"] for r in conservative]),
        "conservative_above_1ms": sum(1 for v in cons_pen if v > 1),
        "conservative_above_2ms": sum(1 for v in cons_pen if v > 2),
        "conservative_above_5ms": sum(1 for v in cons_pen if v > 5),
        "lightweight_to_hybrid_ratio_on_hybrid_winners": {
            "frames": len(hybrid_win),
            "median": median(hybrid_win),
            "p95": percentile(hybrid_win, 0.95),
            "maximum": max(hybrid_win) if hybrid_win else 0.0,
        },
        "hybrid_to_lightweight_ratio_on_lightweight_winners": {
            "frames": len(light_win),
            "median": median(light_win),
            "p95": percentile(light_win, 0.95),
            "maximum": max(light_win) if light_win else 0.0,
        },
        "dangerous_regret_over_conservative_regret": (
            (sum(dang_pen) / sum(cons_pen)) if sum(cons_pen) else None
        ),
        "cost_avoided_if_lightweight_on_hybrid_winners_ms": sum(
            r["light_ms"] - r["hybrid_ms"] for r in rows
            if r["oracle"] == "hybrid" and r["light_ms"] is not None and r["hybrid_ms"] is not None
        ),
        "near_tie_frames": sum(1 for r in rows if r["near_tie"]),
        "excluding_near_ties": {
            "frames": len(decided),
            "dangerous_misses": len(decided_dangerous),
            "dangerous_miss_rate": (len(decided_dangerous) / len(decided)) if decided else 0.0,
            "material_dangerous_misses": sum(1 for r in decided_dangerous if r["material"]),
        },
        "material_dangerous_inside_near_tie": sum(1 for r in material if r["near_tie"]),
        "all_misses_are_conservative": all(r["kind"] != "dangerous" for r in rows),
        "dangerous_ids": pack(dangerous),
        "material_ids": pack(material),
        "severe_ids": pack(severe),
        "catastrophic_ids": pack(catastrophic),
    }


def top_misses(rows, kind, limit=20):
    chosen = [r for r in rows if r["kind"] == kind]
    chosen.sort(key=lambda r: r["absolute_regret_ms"], reverse=True)
    out = []
    for row in chosen[:limit]:
        out.append({
            "id": row["id"],
            "width": row.get("width"),
            "height": row.get("height"),
            "content_class": row.get("content_class"),
            "quality": row.get("quality"),
            "provenance": row.get("provenance"),
            "lightweight_ms": row["light_ms"],
            "hybrid_ms": row["hybrid_ms"],
            "auto": row["auto"],
            "absolute_regret_ms": row["absolute_regret_ms"],
            "relative_regret": row["relative_regret"],
            "ratio_l_over_h": row["ratio_l_over_h"],
            "ratio_h_over_l": row["ratio_h_over_l"],
        })
    return out


def attach(case, auto, light, hybrid, **extra):
    row = classify_case(auto, light, hybrid)
    row["id"] = case.get("id") or f"{case.get('width')}_{case.get('pattern')}"
    row["width"] = case.get("width")
    row["height"] = case.get("height")
    row["content_class"] = case.get("content_class") or case.get("family") or case.get("pattern")
    row["quality"] = case.get("quality")
    row["provenance"] = case.get("provenance")
    row["workload_tag"] = case.get("workload_tag")
    row["gate_role"] = case.get("gate_role")
    for key, value in extra.items():
        row[key] = value
    if "stddev" in case.get("lightweight", {}) and "stddev" in case.get("hybrid", {}):
        n = case.get("measured_frames") or 1
        se = math.sqrt((case["lightweight"]["stddev_ms"] ** 2 + case["hybrid"]["stddev_ms"] ** 2) / n)
        row["gap_within_two_se"] = abs(light - hybrid) < 2 * se
    return row


def from_forced(cases, historical, label):
    rows = []
    for case in cases:
        if not case.get("supported", True):
            continue
        if "lightweight" not in case or "hybrid" not in case:
            continue
        auto = case.get("auto_strategy") or case.get("baseline_strategy")
        light = case["lightweight"]["mean_ms"]
        hybrid = case["hybrid"]["mean_ms"]
        rows.append(attach(case, auto, light, hybrid))
    summary = direction_summary(rows, label, historical)
    summary["top_dangerous"] = top_misses(rows, "dangerous")
    summary["top_conservative"] = top_misses(rows, "conservative")
    return rows, summary


def from_phase3a_windows(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    rows = []
    ambiguous = []
    for case in data["cases"]:
        light = case["lightweight"]["mean_ms"]
        hybrid = case["hybrid"]["mean_ms"]
        auto_ms = case["auto"]["mean_ms"]
        if abs(auto_ms - light) <= abs(auto_ms - hybrid):
            auto = "lightweight"
        else:
            auto = "hybrid"
        gap = abs(abs(auto_ms - light) - abs(auto_ms - hybrid))
        if gap < 0.05:
            ambiguous.append(case.get("pattern"))
        case = dict(case)
        case["id"] = f"p3a_{case['width']}_{case['pattern']}"
        case["content_class"] = case["pattern"]
        case["supported"] = True
        rows.append(attach(case, auto, light, hybrid, auto_wall_ms=auto_ms))
    summary = direction_summary(rows, "phase3a_original_development_matrix", True)
    summary["strategy_inferred_from_separate_auto_window"] = True
    summary["ambiguous_auto_window_ids"] = ambiguous
    summary["note"] = (
        "The original matrix stores Auto wall time separately from forced Lightweight and Hybrid. "
        "The chosen strategy is the forced path whose mean is closer to that Auto window. "
        "Phase 3A reported zero misclassifications."
    )
    return summary


def from_rc_oracle(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    rows = []
    for case in data["cases"]:
        auto = case["auto_strategy"]
        oracle = case["oracle_strategy"]
        # The file stores the faster mean and the chosen mean. Both path means are
        # known on a miss. On a match only the winner is stored, so ratios are partial.
        if auto == oracle:
            light = case["oracle_ms"] if oracle == "lightweight" else None
            hybrid = case["oracle_ms"] if oracle == "hybrid" else None
            kind = "correct_lightweight" if auto == "lightweight" else "correct_hybrid"
            regret = 0.0
            relative = 0.0
        elif auto == "hybrid" and oracle == "lightweight":
            light = case["oracle_ms"]
            hybrid = case["selected_strategy_ms"]
            kind = "conservative"
            regret = case["regret_ms"]
            relative = case["relative_regret_pct"] / 100.0
        elif auto == "lightweight" and oracle == "hybrid":
            hybrid = case["oracle_ms"]
            light = case["selected_strategy_ms"]
            kind = "dangerous"
            regret = case["regret_ms"]
            relative = case["relative_regret_pct"] / 100.0
        else:
            light = hybrid = None
            kind = "unknown"
            regret = case["regret_ms"]
            relative = case["relative_regret_pct"] / 100.0
        row = {
            "id": case["id"],
            "kind": kind,
            "oracle": oracle,
            "auto": auto,
            "absolute_regret_ms": regret,
            "relative_regret": relative,
            "light_ms": light,
            "hybrid_ms": hybrid,
            "ratio_l_over_h": (light / hybrid) if light and hybrid else None,
            "ratio_h_over_l": (hybrid / light) if light and hybrid else None,
            "near_tie": False if light is None or hybrid is None else near_tie(light, hybrid),
            "original_catastrophic_flag": case.get("catastrophic_reporting_flag", False),
            "content_class": case["id"],
            "quality": None,
            "width": None,
            "height": None,
            "provenance": "phase3a_rc_oracle",
        }
        if kind == "dangerous":
            row["material"] = regret > 1.0 or relative > 0.20
            row["severe"] = (hybrid or 0) > 0 and (regret > 5.0 or light > 1.5 * hybrid)
            row["catastrophic"] = (hybrid or 0) > 0 and (regret > 10.0 or light > 2.0 * hybrid)
        rows.append(row)
    summary = direction_summary(rows, "phase3a_rc_original_oracle", True)
    summary["ratio_note"] = "Match rows in the original RC oracle file do not store the slower path, so winner ratios are omitted for matches."
    summary["original_catastrophic_flags"] = sum(1 for r in rows if r.get("original_catastrophic_flag"))
    summary["every_miss_is_hybrid_when_lightweight_faster"] = all(
        r["kind"] != "dangerous" for r in rows
    )
    summary["every_original_catastrophic_flag_is_conservative"] = all(
        r["kind"] == "conservative" for r in rows if r.get("original_catastrophic_flag")
    )
    summary["top_dangerous"] = top_misses(rows, "dangerous")
    summary["top_conservative"] = top_misses(rows, "conservative")
    summary["lightweight_to_hybrid_ratio_on_hybrid_winners"] = {
        "available": False,
        "reason": "The original RC oracle file does not store the slower path on a match.",
    }
    summary["hybrid_to_lightweight_ratio_on_lightweight_winners"] = {
        "available": False,
        "reason": "The original RC oracle file does not store the slower path on a match.",
    }
    return summary


def load_cases(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    return data, data["cases"]


def subset(cases, pred):
    return [case for case in cases if pred(case)]


def policy_block(rows, value):
    lat = [value(row) for row in rows]
    pixels = sum(row["width"] * row["height"] for row in rows)
    seconds = sum(ms / 1000.0 for ms in lat)
    avg = mean(lat)
    return {
        "frames": len(rows),
        "mean_ms": avg,
        "p95_ms": percentile(lat, 0.95),
        "p99_ms": percentile(lat, 0.99),
        "worst_ms": max(lat) if lat else 0.0,
        "fps": (1000.0 / avg) if avg else 0.0,
        "megapixels_per_second": (pixels / 1e6 / seconds) if seconds else 0.0,
        "exceed_8_33_ms": sum(1 for ms in lat if ms > 8.33),
        "exceed_16_67_ms": sum(1 for ms in lat if ms > 16.67),
        "exceed_33_33_ms": sum(1 for ms in lat if ms > 33.33),
    }


def lcg_shuffle(items, seed):
    arr = list(items)
    state = seed & 0xFFFFFFFF
    for i in range(len(arr) - 1, 0, -1):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        j = state % (i + 1)
        arr[i], arr[j] = arr[j], arr[i]
    return arr


def expand(rows, weight, seed):
    sequence = []
    for row in rows:
        sequence.extend([row] * weight(row))
    return lcg_shuffle(sequence, seed)


def oracle_capture(rows):
    sum_l = sum(r["light_ms"] for r in rows)
    sum_h = sum(r["hybrid_ms"] for r in rows)
    sum_oracle = sum(min(r["light_ms"], r["hybrid_ms"]) for r in rows)
    sum_sel = 0.0
    for row in rows:
        sum_sel += row["light_ms"] if row["auto"] == "lightweight" else row["hybrid_ms"]
    best = min(sum_l, sum_h)
    span = best - sum_oracle
    capture = 1.0 if span <= 1e-12 else (best - sum_sel) / span
    return {
        "secondary_context_only": True,
        "oracle_capture": capture,
        "sum_lightweight_ms": sum_l,
        "sum_hybrid_ms": sum_h,
        "sum_selected_forced_ms": sum_sel,
        "sum_oracle_ms": sum_oracle,
    }


def fresh_rows(measurements):
    rows = []
    for case in measurements["cases"]:
        if not case.get("supported"):
            continue
        row = attach(
            case,
            case["auto_strategy"],
            case["lightweight"]["mean_ms"],
            case["hybrid"]["mean_ms"],
            auto_wall_ms=case["auto"]["mean_ms"],
        )
        row["auto_ms"] = case["auto"]["mean_ms"]
        rows.append(row)
    return rows


def workload_report(rows):
    def stats(sequence, label):
        block = {
            "label": label,
            "frames_in_sequence": len(sequence),
            "always_lightweight": policy_block(sequence, lambda r: r["light_ms"]),
            "always_hybrid": policy_block(sequence, lambda r: r["hybrid_ms"]),
            "auto": policy_block(sequence, lambda r: r["auto_ms"]),
        }
        return block

    photo = expand(rows, lambda r: 4 if r.get("provenance") == "reencoded_real" else 1, 0x3AF0BEEF ^ 0x10001)
    high = expand(rows, lambda r: 4 if r.get("workload_tag") == "high_entropy" else 1, 0x3AF0BEEF ^ 0x20002)
    sparse = expand(rows, lambda r: 4 if r.get("workload_tag") == "sparse_structured" else 1, 0x3AF0BEEF ^ 0x30003)
    equal = lcg_shuffle(rows, 0x3AF0BEEF)
    return {
        "weighting": "Predeclared. Equal weight is one copy of each primary frame, shuffled with seed 0x3AF0BEEF. Photo-heavy repeats re-encoded photographs four times. High-entropy-heavy and sparse-heavy repeat the manifest workload tag four times. Tags were assigned from the generator class or the photograph query, not from measured speed.",
        "equal_weight": stats(equal, "equal_weight"),
        "natural_photo_heavy": stats(photo, "natural_photo_heavy"),
        "high_entropy_heavy": stats(high, "high_entropy_heavy"),
        "sparse_structured_heavy": stats(sparse, "sparse_structured_heavy"),
    }


def write(path, payload):
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"wrote {path}")


def historical():
    measured, cases = load_cases(RESULTS / "decode_phase3ad_measurements.json")
    holdout, hold_cases = load_cases(RESULTS / "decode_phase3ad_holdout.json")
    dev_rows, dev = from_forced(subset(cases, lambda c: c.get("split") == "development" and c.get("supported")), True, "phase3ad_development_split")
    p3a_dev_rows, p3a_dev = from_forced(
        subset(cases, lambda c: c.get("split") == "development" and c.get("origin") == "phase3a_development" and c.get("supported")),
        True, "phase3a_development_rows_inside_3ad_session")
    rc_rows, rc_re = from_forced(
        subset(cases, lambda c: c.get("origin") == "phase3a_rc_corpus" and c.get("supported")),
        True, "phase3a_rc_remeasured_in_3ad")
    val_rows, val = from_forced(subset(cases, lambda c: c.get("split") == "validation" and c.get("supported")), True, "phase3ad_validation")
    hold_rows, hold = from_forced(subset(hold_cases, lambda c: c.get("supported")), True, "phase3ad_holdout")
    rc_original = from_rc_oracle(RESULTS / "decode_phase3a_rc_oracle.json")
    phase3a = from_phase3a_windows(RESULTS / "decode_phase3a_windows_msvc.json")
    claim = {
        "phase3a_original_dangerous_misses": phase3a["dangerous_misses"],
        "rc_original_every_miss_is_conservative": rc_original["every_miss_is_hybrid_when_lightweight_faster"],
        "rc_original_every_catastrophic_flag_is_conservative": rc_original["every_original_catastrophic_flag_is_conservative"],
        "rc_original_dangerous_misses": rc_original["dangerous_misses"],
        "rc_remeasured_dangerous_misses": rc_re["dangerous_misses"],
        "rc_remeasured_every_miss_is_conservative": rc_re["all_misses_are_conservative"],
        "validation_dangerous_misses": val["dangerous_misses"],
        "validation_every_miss_is_conservative": val["all_misses_are_conservative"],
        "holdout_dangerous_misses": hold["dangerous_misses"],
        "holdout_every_miss_is_conservative": hold["all_misses_are_conservative"],
        "development_split_dangerous_misses": dev["dangerous_misses"],
    }
    return {
        "note": "Historical evidence. These files were not rerun and were not used to change the classifier.",
        "measurement_session_gpu": measured.get("gpu"),
        "holdout_gpu": holdout.get("gpu"),
        "claim_check": claim,
        "phase3a_original": phase3a,
        "phase3a_development_rows_inside_3ad": p3a_dev,
        "phase3ad_development_split": dev,
        "phase3a_rc_original": rc_original,
        "phase3a_rc_remeasured": rc_re,
        "phase3ad_validation": val,
        "phase3ad_holdout": hold,
        "_rows_unused": len(dev_rows) + len(p3a_dev_rows) + len(rc_rows) + len(val_rows) + len(hold_rows),
    }


def self_check():
    tie = classify_case("lightweight", 5.0, 5.0)
    assert tie["kind"] == "correct_lightweight"
    dangerous = classify_case("lightweight", 12.0, 4.0)
    assert dangerous["kind"] == "dangerous" and dangerous["catastrophic"] and dangerous["severe"] and dangerous["material"]
    assert not near_tie(12.0, 4.0)
    assert near_tie(5.0, 5.1)
    conservative = classify_case("hybrid", 4.0, 4.4)
    assert conservative["kind"] == "conservative"
    # Relative 20 percent at a 0.2 ms gap is still above the 5 percent near-tie and can exceed 0.25 ms.
    assert classify_case("lightweight", 1.3, 1.0)["material"]
    shuffled = lcg_shuffle(list(range(8)), 0x3AF0BEEF)
    assert shuffled == lcg_shuffle(list(range(8)), 0x3AF0BEEF)
    assert percentile([1, 2, 3, 4], 1.0) == 4
    print("self-check PASS")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--self-check":
        self_check()
        return
    history = historical()
    history.pop("_rows_unused", None)
    if len(sys.argv) > 1 and sys.argv[1] == "--historical-only":
        write(Path(sys.argv[2]), history)
        claim = history["claim_check"]
        print(json.dumps(claim, indent=2))
        return
    measurements = json.loads((RESULTS / "decode_phase3af_measurements.json").read_text(encoding="utf-8"))
    corpus = json.loads((RESULTS / "decode_phase3af_corpus.json").read_text(encoding="utf-8"))
    correctness = json.loads((RESULTS / "decode_phase3af_correctness.json").read_text(encoding="utf-8"))
    if corpus.get("timing_performed") is not False:
        raise SystemExit("corpus manifest was not frozen before timing")
    rows = fresh_rows(measurements)
    by_hash = {case["id"]: case.get("sample_hash") for case in corpus["cases"]}
    for case in measurements["cases"]:
        if case["id"] not in by_hash or by_hash[case["id"]] != case.get("sample_hash"):
            raise SystemExit(f"measurement hash drift {case['id']}")
    primary = [row for row in rows if row.get("gate_role") == "primary"]
    if len(primary) < 100:
        raise SystemExit(f"primary supported frames {len(primary)} are below 100")
    summary = direction_summary(primary, "phase3af_primary_untouched", False)
    summary["top_dangerous"] = top_misses(primary, "dangerous")
    summary["top_conservative"] = top_misses(primary, "conservative")
    summary["oracle_capture"] = oracle_capture(primary)
    full = direction_summary(rows, "phase3af_all_supported_including_previously_timed", False)
    policies = {
        "population": "primary untouched supported frames, one each",
        "always_lightweight": policy_block(primary, lambda r: r["light_ms"]),
        "always_hybrid": policy_block(primary, lambda r: r["hybrid_ms"]),
        "auto": policy_block(primary, lambda r: r["auto_ms"]),
    }
    workloads = workload_report(primary)
    exact = {case["id"]: case for case in correctness["cases"]}
    failures = []
    for row in rows:
        item = exact.get(row["id"])
        if not item or not item.get("checked") or not item.get("exact"):
            failures.append(row["id"])
    provenance = {}
    resolution = {"1080p": 0, "4k": 0, "8k": 0, "other": 0}
    qualities = {}
    for row in primary:
        provenance[row.get("provenance") or "unknown"] = provenance.get(row.get("provenance") or "unknown", 0) + 1
        width = row["width"]
        if width >= 7680:
            resolution["8k"] += 1
        elif width >= 3840:
            resolution["4k"] += 1
        elif width >= 1920:
            resolution["1080p"] += 1
        else:
            resolution["other"] += 1
        qualities[row.get("quality") or "unknown"] = qualities.get(row.get("quality") or "unknown", 0) + 1
    low_conf = sum(1 for row in primary if row.get("gap_within_two_se"))
    report = {
        "license": "MIT OR Apache-2.0",
        "phase": "3A-F",
        "definitions": {
            "oracle": "minimum of forced Lightweight and Hybrid means; ties are optimal for either choice",
            "dangerous": "Auto selects Lightweight and Hybrid is strictly faster",
            "conservative": "Auto selects Hybrid and Lightweight is strictly faster",
            "material": "dangerous and (absolute regret > 1 ms or relative regret > 20 percent)",
            "severe": "dangerous and (absolute regret > 5 ms or Lightweight > 1.5x Hybrid)",
            "catastrophic": "dangerous and (absolute regret > 10 ms or Lightweight > 2x Hybrid)",
            "near_tie": "absolute gap < 0.25 ms or relative gap < 5 percent; reporting only",
            "regret_uses": "forced mean of the chosen architecture, not the separate Auto wall time",
        },
        "primary": summary,
        "all_supported": {
            "frames": full["frames"],
            "dangerous_misses": full["dangerous_misses"],
            "conservative_misses": full["conservative_misses"],
            "catastrophic_dangerous_misses": full["catastrophic_dangerous_misses"],
        },
        "composition": {
            "primary_frames": len(primary),
            "provenance": provenance,
            "resolution": resolution,
            "qualities": qualities,
            "real_image_sources_in_corpus": corpus.get("real_image_sources"),
            "low_confidence_gap_within_two_standard_errors": low_conf,
        },
        "correctness_failures": failures,
        "classifier_mean_ns": measurements.get("classifier_mean_ns"),
        "classifier_deterministic": measurements.get("classifier_deterministic"),
        "gpu": measurements.get("gpu"),
        "historical": history,
    }
    directional = dict(report)
    directional["workloads_omitted"] = "see policy comparison"
    policy = {
        "license": "MIT OR Apache-2.0",
        "phase": "3A-F",
        "gpu": measurements.get("gpu"),
        "machine_scope": "single measured machine; not a frame-rate guarantee",
        "unweighted_primary": policies,
        "workloads": workloads,
        "oracle_capture_secondary": summary["oracle_capture"],
        "correctness_failures": failures,
    }
    write(RESULTS / "decode_phase3af_directional_regret.json", directional)
    write(RESULTS / "decode_phase3af_policy_comparison.json", policy)
    print(json.dumps({
        "primary": len(primary),
        "dangerous": summary["dangerous_misses"],
        "material": summary["material_dangerous_misses"],
        "severe": summary["severe_dangerous_misses"],
        "catastrophic": summary["catastrophic_dangerous_misses"],
        "conservative": summary["conservative_misses"],
        "dangerous_regret": summary["dangerous_total_regret_ms"],
        "conservative_regret": summary["conservative_total_regret_ms"],
        "correctness_failures": failures,
        "auto_mean": policies["auto"]["mean_ms"],
        "hybrid_mean": policies["always_hybrid"]["mean_ms"],
        "light_mean": policies["always_lightweight"]["mean_ms"],
        "claim": history["claim_check"],
    }, indent=2))


if __name__ == "__main__":
    main()
