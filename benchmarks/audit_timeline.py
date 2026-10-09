#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

import json
import os

def audit_timelines():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    t_4k_noise_path = os.path.join(root, "benchmarks", "results", "timeline_phase2fc_4k_noise.json")
    t_1080p_noise_path = os.path.join(root, "benchmarks", "results", "timeline_phase2fc_1080p_noise.json")
    t_4k_texture_path = os.path.join(root, "benchmarks", "results", "timeline_phase2fc_4k_texture.json")

    results = {}
    for name, path in [("4k_noise", t_4k_noise_path), ("1080p_noise", t_1080p_noise_path), ("4k_texture", t_4k_texture_path)]:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        checks = []
        # Check 1: total_critical_path_ms > 0
        checks.append(("total_path_valid", data["total_critical_path_ms"] > 0))
        # Check 2: host_parse >= 0
        checks.append(("host_parse_non_negative", data["host_parse_ms"] >= 0))
        # Check 3: gpu_ll_ms >= 0
        checks.append(("gpu_ll_non_negative", data["gpu_ll_ms"] >= 0))
        # Check 4: color_convert_ms >= 0
        checks.append(("color_convert_non_negative", data["color_convert_ms"] >= 0))
        # Check 5: all bands have summary_end >= summary_start, cpu_resolve_end >= cpu_resolve_start, replay_end >= replay_start
        all_bands_ordered = True
        for b in data["bands"]:
            if b["summary_end"] < b["summary_start"] or b["cpu_resolve_end"] < b["cpu_resolve_start"] or b["replay_end"] < b["replay_start"]:
                all_bands_ordered = False
                break
        checks.append(("band_intervals_ordered", all_bands_ordered))

        results[name] = {
            "checks": checks,
            "all_passed": all(c[1] for c in checks)
        }

    return results

def main():
    res = audit_timelines()
    print("Timeline audit results:")
    for k, v in res.items():
        print(f"  {k}: all_passed={v['all_passed']}, checks={v['checks']}")

if __name__ == "__main__":
    main()
