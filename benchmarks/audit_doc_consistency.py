#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

import json
import os
import math

def check_value(name, doc_val, json_val, tol_abs=0.05, tol_rel=0.02):
    if json_val is None:
        return {
            "name": name,
            "documented_value": doc_val,
            "json_value": None,
            "absolute_difference": None,
            "relative_difference": None,
            "status": "MISSING_IN_JSON"
        }
    
    diff_abs = abs(doc_val - json_val)
    diff_rel = diff_abs / abs(json_val) if json_val != 0 else diff_abs
    
    passed = (diff_abs <= tol_abs) or (diff_rel <= tol_rel)
    return {
        "name": name,
        "documented_value": doc_val,
        "json_value": json_val,
        "absolute_difference": round(diff_abs, 6),
        "relative_difference": round(diff_rel, 6),
        "status": "PASS" if passed else "DISCREPANCY"
    }

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    msvc_path = os.path.join(root, "benchmarks", "results", "decode_phase2fc_windows_msvc.json")
    oracle_path = os.path.join(root, "benchmarks", "results", "decode_phase2fc_oracle.json")
    out_path = os.path.join(root, "benchmarks", "results", "decode_phase2fc_doc_consistency.json")
    
    with open(msvc_path, "r") as f:
        msvc = json.load(f)
    with open(oracle_path, "r") as f:
        oracle = json.load(f)
        
    audit_records = []
    
    # 1. Section 4.1 Resolution Latency Comparison
    res_map = {}
    for r in msvc["resolutions"]:
        res_map[r["name"]] = {p["pattern"]: p for p in r["patterns"]}
        
    doc_latencies = [
        # 1080p
        ("1080p_flat_cpu_ms", 4.13, res_map["1080p"]["flat"]["cpu_ms"]),
        ("1080p_flat_p2d_ms", 0.89, res_map["1080p"]["flat"]["p2d_ms"]),
        ("1080p_flat_p2e_ms", 2.50, res_map["1080p"]["flat"]["p2e_ms"]),
        ("1080p_flat_p2fc_static_ms", 2.46, res_map["1080p"]["flat"]["p2fc_static_ms"]),
        ("1080p_flat_bf_bands", 0, res_map["1080p"]["flat"]["bf_bands"]),
        ("1080p_flat_speedup_vs_2e", 1.02, res_map["1080p"]["flat"]["speedup_vs_2e"]),
        ("1080p_flat_speedup_vs_2d", 0.36, res_map["1080p"]["flat"]["speedup_vs_2d"]),
        ("1080p_flat_speedup_vs_cpu", 1.68, res_map["1080p"]["flat"]["speedup_vs_cpu"]),

        ("1080p_gradient_cpu_ms", 4.64, res_map["1080p"]["gradient"]["cpu_ms"]),
        ("1080p_gradient_p2d_ms", 2.58, res_map["1080p"]["gradient"]["p2d_ms"]),
        ("1080p_gradient_p2e_ms", 4.80, res_map["1080p"]["gradient"]["p2e_ms"]),
        ("1080p_gradient_p2fc_static_ms", 4.79, res_map["1080p"]["gradient"]["p2fc_static_ms"]),
        ("1080p_gradient_bf_bands", 0, res_map["1080p"]["gradient"]["bf_bands"]),
        ("1080p_gradient_speedup_vs_2e", 1.00, res_map["1080p"]["gradient"]["speedup_vs_2e"]),
        ("1080p_gradient_speedup_vs_2d", 0.54, res_map["1080p"]["gradient"]["speedup_vs_2d"]),
        ("1080p_gradient_speedup_vs_cpu", 0.97, res_map["1080p"]["gradient"]["speedup_vs_cpu"]),

        ("1080p_noise_cpu_ms", 21.49, res_map["1080p"]["noise"]["cpu_ms"]),
        ("1080p_noise_p2d_ms", 14.95, res_map["1080p"]["noise"]["p2d_ms"]),
        ("1080p_noise_p2e_ms", 14.70, res_map["1080p"]["noise"]["p2e_ms"]),
        ("1080p_noise_p2fc_static_ms", 5.24, res_map["1080p"]["noise"]["p2fc_static_ms"]),
        ("1080p_noise_bf_bands", 9, res_map["1080p"]["noise"]["bf_bands"]),
        ("1080p_noise_speedup_vs_2e", 2.80, res_map["1080p"]["noise"]["speedup_vs_2e"]),
        ("1080p_noise_speedup_vs_2d", 2.85, res_map["1080p"]["noise"]["speedup_vs_2d"]),
        ("1080p_noise_speedup_vs_cpu", 4.10, res_map["1080p"]["noise"]["speedup_vs_cpu"]),

        ("1080p_texture_cpu_ms", 5.75, res_map["1080p"]["texture"]["cpu_ms"]),
        ("1080p_texture_p2d_ms", 6.96, res_map["1080p"]["texture"]["p2d_ms"]),
        ("1080p_texture_p2e_ms", 7.81, res_map["1080p"]["texture"]["p2e_ms"]),
        ("1080p_texture_p2fc_static_ms", 7.90, res_map["1080p"]["texture"]["p2fc_static_ms"]),
        ("1080p_texture_bf_bands", 0, res_map["1080p"]["texture"]["bf_bands"]),
        ("1080p_texture_speedup_vs_2e", 0.99, res_map["1080p"]["texture"]["speedup_vs_2e"]),
        ("1080p_texture_speedup_vs_2d", 0.88, res_map["1080p"]["texture"]["speedup_vs_2d"]),
        ("1080p_texture_speedup_vs_cpu", 0.73, res_map["1080p"]["texture"]["speedup_vs_cpu"]),

        ("1080p_edges_cpu_ms", 5.92, res_map["1080p"]["edges"]["cpu_ms"]),
        ("1080p_edges_p2d_ms", 3.75, res_map["1080p"]["edges"]["p2d_ms"]),
        ("1080p_edges_p2e_ms", 7.30, res_map["1080p"]["edges"]["p2e_ms"]),
        ("1080p_edges_p2fc_static_ms", 7.67, res_map["1080p"]["edges"]["p2fc_static_ms"]),
        ("1080p_edges_bf_bands", 0, res_map["1080p"]["edges"]["bf_bands"]),
        ("1080p_edges_speedup_vs_2e", 0.95, res_map["1080p"]["edges"]["speedup_vs_2e"]),
        ("1080p_edges_speedup_vs_2d", 0.49, res_map["1080p"]["edges"]["speedup_vs_2d"]),
        ("1080p_edges_speedup_vs_cpu", 0.77, res_map["1080p"]["edges"]["speedup_vs_cpu"]),

        # 4K
        ("4k_flat_cpu_ms", 16.33, res_map["4k"]["flat"]["cpu_ms"]),
        ("4k_flat_p2d_ms", 1.67, res_map["4k"]["flat"]["p2d_ms"]),
        ("4k_flat_p2e_ms", 3.52, res_map["4k"]["flat"]["p2e_ms"]),
        ("4k_flat_p2fc_static_ms", 3.50, res_map["4k"]["flat"]["p2fc_static_ms"]),
        ("4k_flat_bf_bands", 0, res_map["4k"]["flat"]["bf_bands"]),
        ("4k_flat_speedup_vs_2e", 1.00, res_map["4k"]["flat"]["speedup_vs_2e"]),
        ("4k_flat_speedup_vs_2d", 0.48, res_map["4k"]["flat"]["speedup_vs_2d"]),
        ("4k_flat_speedup_vs_cpu", 4.67, res_map["4k"]["flat"]["speedup_vs_cpu"]),

        ("4k_gradient_cpu_ms", 16.60, res_map["4k"]["gradient"]["cpu_ms"]),
        ("4k_gradient_p2d_ms", 4.44, res_map["4k"]["gradient"]["p2d_ms"]),
        ("4k_gradient_p2e_ms", 7.01, res_map["4k"]["gradient"]["p2e_ms"]),
        ("4k_gradient_p2fc_static_ms", 6.85, res_map["4k"]["gradient"]["p2fc_static_ms"]),
        ("4k_gradient_bf_bands", 0, res_map["4k"]["gradient"]["bf_bands"]),
        ("4k_gradient_speedup_vs_2e", 1.02, res_map["4k"]["gradient"]["speedup_vs_2e"]),
        ("4k_gradient_speedup_vs_2d", 0.65, res_map["4k"]["gradient"]["speedup_vs_2d"]),
        ("4k_gradient_speedup_vs_cpu", 2.42, res_map["4k"]["gradient"]["speedup_vs_cpu"]),

        ("4k_noise_cpu_ms", 101.19, res_map["4k"]["noise"]["cpu_ms"]),
        ("4k_noise_p2d_ms", 67.79, res_map["4k"]["noise"]["p2d_ms"]),
        ("4k_noise_p2e_ms", 66.14, res_map["4k"]["noise"]["p2e_ms"]),
        ("4k_noise_p2fc_static_ms", 8.81, res_map["4k"]["noise"]["p2fc_static_ms"]),
        ("4k_noise_bf_bands", 9, res_map["4k"]["noise"]["bf_bands"]),
        ("4k_noise_speedup_vs_2e", 7.51, res_map["4k"]["noise"]["speedup_vs_2e"]),
        ("4k_noise_speedup_vs_2d", 7.70, res_map["4k"]["noise"]["speedup_vs_2d"]),
        ("4k_noise_speedup_vs_cpu", 11.49, res_map["4k"]["noise"]["speedup_vs_cpu"]),

        ("4k_texture_cpu_ms", 28.22, res_map["4k"]["texture"]["cpu_ms"]),
        ("4k_texture_p2d_ms", 19.49, res_map["4k"]["texture"]["p2d_ms"]),
        ("4k_texture_p2e_ms", 20.14, res_map["4k"]["texture"]["p2e_ms"]),
        ("4k_texture_p2fc_static_ms", 19.54, res_map["4k"]["texture"]["p2fc_static_ms"]),
        ("4k_texture_bf_bands", 0, res_map["4k"]["texture"]["bf_bands"]),
        ("4k_texture_speedup_vs_2e", 1.03, res_map["4k"]["texture"]["speedup_vs_2e"]),
        ("4k_texture_speedup_vs_2d", 1.00, res_map["4k"]["texture"]["speedup_vs_2d"]),
        ("4k_texture_speedup_vs_cpu", 1.44, res_map["4k"]["texture"]["speedup_vs_cpu"]),

        ("4k_edges_cpu_ms", 23.99, res_map["4k"]["edges"]["cpu_ms"]),
        ("4k_edges_p2d_ms", 9.22, res_map["4k"]["edges"]["p2d_ms"]),
        ("4k_edges_p2e_ms", 12.31, res_map["4k"]["edges"]["p2e_ms"]),
        ("4k_edges_p2fc_static_ms", 12.54, res_map["4k"]["edges"]["p2fc_static_ms"]),
        ("4k_edges_bf_bands", 0, res_map["4k"]["edges"]["bf_bands"]),
        ("4k_edges_speedup_vs_2e", 0.98, res_map["4k"]["edges"]["speedup_vs_2e"]),
        ("4k_edges_speedup_vs_2d", 0.74, res_map["4k"]["edges"]["speedup_vs_2d"]),
        ("4k_edges_speedup_vs_cpu", 1.91, res_map["4k"]["edges"]["speedup_vs_cpu"]),

        # 8K
        ("8k_flat_cpu_ms", 73.28, res_map["8k"]["flat"]["cpu_ms"]),
        ("8k_flat_p2d_ms", 6.25, res_map["8k"]["flat"]["p2d_ms"]),
        ("8k_flat_p2e_ms", 8.74, res_map["8k"]["flat"]["p2e_ms"]),
        ("8k_flat_p2fc_static_ms", 9.10, res_map["8k"]["flat"]["p2fc_static_ms"]),
        ("8k_flat_bf_bands", 0, res_map["8k"]["flat"]["bf_bands"]),
        ("8k_flat_speedup_vs_2e", 0.96, res_map["8k"]["flat"]["speedup_vs_2e"]),
        ("8k_flat_speedup_vs_2d", 0.69, res_map["8k"]["flat"]["speedup_vs_2d"]),
        ("8k_flat_speedup_vs_cpu", 8.05, res_map["8k"]["flat"]["speedup_vs_cpu"]),
    ]
    for name, doc_val, json_val in doc_latencies:
        audit_records.append(check_value(name, doc_val, json_val))

    # 2. Section 5.1 Chunk sweep
    chunk_map = {c["chunk_size"]: c for c in msvc["chunk_size_sweep"]}
    chunk_checks = [
        ("chunk_64B_band6_ms", 2.10, chunk_map[64]["band6_ms"]),
        ("chunk_64B_band7_ms", 2.18, chunk_map[64]["band7_ms"]),
        ("chunk_64B_texture_crit_ms", 0.01, chunk_map[64]["texture_crit_ms"]),
        ("chunk_64B_scratch_mib", 3.00, chunk_map[64]["scratch_bytes_per_band"] / (1024*1024)),

        ("chunk_128B_band6_ms", 2.35, chunk_map[128]["band6_ms"]),
        ("chunk_128B_band7_ms", 2.51, chunk_map[128]["band7_ms"]),
        ("chunk_128B_texture_crit_ms", 0.01, chunk_map[128]["texture_crit_ms"]),
        ("chunk_128B_scratch_mib", 1.50, chunk_map[128]["scratch_bytes_per_band"] / (1024*1024)),

        ("chunk_256B_band6_ms", 1.82, chunk_map[256]["band6_ms"]),
        ("chunk_256B_band7_ms", 2.02, chunk_map[256]["band7_ms"]),
        ("chunk_256B_texture_crit_ms", 0.01, chunk_map[256]["texture_crit_ms"]),
        ("chunk_256B_scratch_mib", 0.75, chunk_map[256]["scratch_bytes_per_band"] / (1024*1024)),

        ("chunk_512B_band6_ms", 1.06, chunk_map[512]["band6_ms"]),
        ("chunk_512B_band7_ms", 2.21, chunk_map[512]["band7_ms"]),
        ("chunk_512B_texture_crit_ms", 0.01, chunk_map[512]["texture_crit_ms"]),
        ("chunk_512B_scratch_mib", 0.38, chunk_map[512]["scratch_bytes_per_band"] / (1024*1024)),
    ]
    for name, doc_val, json_val in chunk_checks:
        audit_records.append(check_value(name, doc_val, json_val))

    # 3. Section 6.1 Worker scaling
    worker_map = {w["workers"]: w for w in msvc["worker_scaling"]}
    worker_checks = [
        ("worker_1_noise_ms", 13.38, worker_map[1]["noise_4k_ms"]),
        ("worker_1_fps", 74.7, worker_map[1]["fps"]),
        ("worker_1_texture_ms", 21.49, worker_map[1]["texture_4k_ms"]),

        ("worker_2_noise_ms", 11.03, worker_map[2]["noise_4k_ms"]),
        ("worker_2_fps", 90.7, worker_map[2]["fps"]),
        ("worker_2_texture_ms", 19.58, worker_map[2]["texture_4k_ms"]),

        ("worker_4_noise_ms", 9.24, worker_map[4]["noise_4k_ms"]),
        ("worker_4_fps", 108.3, worker_map[4]["fps"]),
        ("worker_4_texture_ms", 20.18, worker_map[4]["texture_4k_ms"]),

        ("worker_8_noise_ms", 9.06, worker_map[8]["noise_4k_ms"]),
        ("worker_8_fps", 110.4, worker_map[8]["fps"]),
        ("worker_8_texture_ms", 22.05, worker_map[8]["texture_4k_ms"]),

        ("worker_16_noise_ms", 9.01, worker_map[16]["noise_4k_ms"]),
        ("worker_16_fps", 111.0, worker_map[16]["fps"]),
        ("worker_16_texture_ms", 20.54, worker_map[16]["texture_4k_ms"]),
    ]
    for name, doc_val, json_val in worker_checks:
        audit_records.append(check_value(name, doc_val, json_val))

    # 4. Section 6.3 Slot scaling
    slot_map = {s["slots"]: s for s in msvc["slot_scaling"]}
    slot_checks = [
        ("slot_1_fps", 117.8, slot_map[1]["fps"]),
        ("slot_1_mpps", 976.7, slot_map[1]["mpps"]),
        ("slot_1_lat_ms", 8.49, slot_map[1]["latency_ms"]),

        ("slot_2_fps", 131.0, slot_map[2]["fps"]),
        ("slot_2_mpps", 1086.8, slot_map[2]["mpps"]),
        ("slot_2_lat_ms", 7.63, slot_map[2]["latency_ms"]),

        ("slot_3_fps", 130.1, slot_map[3]["fps"]),
        ("slot_3_mpps", 1079.5, slot_map[3]["mpps"]),
        ("slot_3_lat_ms", 7.68, slot_map[3]["latency_ms"]),

        ("slot_4_fps", 123.6, slot_map[4]["fps"]),
        ("slot_4_mpps", 1024.9, slot_map[4]["mpps"]),
        ("slot_4_lat_ms", 8.09, slot_map[4]["latency_ms"]),
    ]
    for name, doc_val, json_val in slot_checks:
        audit_records.append(check_value(name, doc_val, json_val))

    # 5. Section 3.2 Oracle table comparison
    oracle_map = {o["pattern"]: o for o in oracle["evaluations"]}
    oracle_checks = [
        ("oracle_flat_method_e_ms", 3.52, oracle_map["flat"]["method_e_ms"]),
        ("oracle_flat_static_ms", 3.50, oracle_map["flat"]["static_heuristic_ms"]),
        ("oracle_flat_adaptive_ms", 3.50, oracle_map["flat"]["adaptive_dynamic_ms"]),
        ("oracle_flat_oracle_ms", 3.50, oracle_map["flat"]["oracle_ms"]),
        ("oracle_flat_efficiency_pct", 100.0, oracle_map["flat"]["static_oracle_efficiency"] * 100.0),

        ("oracle_gradient_method_e_ms", 7.01, oracle_map["gradient"]["method_e_ms"]),
        ("oracle_gradient_static_ms", 6.85, oracle_map["gradient"]["static_heuristic_ms"]),
        ("oracle_gradient_adaptive_ms", 6.85, oracle_map["gradient"]["adaptive_dynamic_ms"]),
        ("oracle_gradient_oracle_ms", 6.85, oracle_map["gradient"]["oracle_ms"]),
        ("oracle_gradient_efficiency_pct", 100.0, oracle_map["gradient"]["static_oracle_efficiency"] * 100.0),

        ("oracle_noise_method_e_ms", 66.14, oracle_map["noise"]["method_e_ms"]),
        ("oracle_noise_static_ms", 8.81, oracle_map["noise"]["static_heuristic_ms"]),
        ("oracle_noise_adaptive_ms", 8.81, oracle_map["noise"]["adaptive_dynamic_ms"]),
        ("oracle_noise_oracle_ms", 8.64, oracle_map["noise"]["oracle_ms"]),
        ("oracle_noise_efficiency_pct", 98.1, oracle_map["noise"]["static_oracle_efficiency"] * 100.0),

        ("oracle_texture_method_e_ms", 20.14, oracle_map["texture"]["method_e_ms"]),
        ("oracle_texture_static_ms", 19.54, oracle_map["texture"]["static_heuristic_ms"]),
        ("oracle_texture_adaptive_ms", 19.54, oracle_map["texture"]["adaptive_dynamic_ms"]),
        ("oracle_texture_oracle_ms", 19.49, oracle_map["texture"]["oracle_ms"]),
        ("oracle_texture_efficiency_pct", 99.7, oracle_map["texture"]["static_oracle_efficiency"] * 100.0),

        ("oracle_edges_method_e_ms", 12.31, oracle_map["edges"]["method_e_ms"]),
        ("oracle_edges_static_ms", 12.54, oracle_map["edges"]["static_heuristic_ms"]),
        ("oracle_edges_adaptive_ms", 12.54, oracle_map["edges"]["adaptive_dynamic_ms"]),
        ("oracle_edges_oracle_ms", 12.31, oracle_map["edges"]["oracle_ms"]),
        ("oracle_edges_efficiency_pct", 98.2, oracle_map["edges"]["static_oracle_efficiency"] * 100.0),
    ]
    for name, doc_val, json_val in oracle_checks:
        audit_records.append(check_value(name, doc_val, json_val))

    # Summary
    pass_count = sum(1 for r in audit_records if r["status"] == "PASS")
    disc_count = sum(1 for r in audit_records if r["status"] == "DISCREPANCY")
    missing_count = sum(1 for r in audit_records if r["status"] == "MISSING_IN_JSON")
    
    result = {
        "benchmark": "HyperCine Phase 2F-C Documentation vs JSON Consistency Audit",
        "total_checks": len(audit_records),
        "pass_count": pass_count,
        "discrepancy_count": disc_count,
        "missing_count": missing_count,
        "records": audit_records
    }
    
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)
        
    print(f"Audit completed: {len(audit_records)} checks, {pass_count} PASS, {disc_count} DISCREPANCY, {missing_count} MISSING.")
    if disc_count > 0:
        print("\nDiscrepancies found:")
        for r in audit_records:
            if r["status"] == "DISCREPANCY":
                print(f"  {r['name']}: doc={r['documented_value']}, json={r['json_value']} (diff_abs={r['absolute_difference']})")

if __name__ == "__main__":
    main()
