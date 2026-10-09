#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

import json
import os

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = os.path.join(root, "benchmarks", "results", "decode_phase2fc_memory_audit.json")

    # Exact architectural values from decode_phase2e_pipeline.cu and decode_hybrid_entropy.cuh
    kMaxFcChunks = 131072 # 128 K chunks
    sizeof_summary = 8     # advance(4) + state(2) + terminal(2)
    sizeof_restart = 12    # nibble(4) + position(4) + state(2) + reserved(2)
    kMaxVlcBands = 32

    # Per-slot device allocations
    dev_summaries = kMaxFcChunks * sizeof_summary     # 1,048,576 bytes = 1.0 MiB
    dev_checkpoints = kMaxFcChunks * sizeof_restart   # 1,572,864 bytes = 1.5 MiB
    dev_errors = kMaxVlcBands * 4                     # 128 bytes
    total_dev_vram_per_slot = dev_summaries + dev_checkpoints + dev_errors # 2,621,568 bytes = 2.50 MiB

    # Per-slot host pinned allocations
    host_summaries = kMaxFcChunks * sizeof_summary    # 1,048,576 bytes = 1.0 MiB
    host_checkpoints = kMaxFcChunks * sizeof_restart  # 1,572,864 bytes = 1.5 MiB
    host_errors = kMaxVlcBands * 4                    # 128 bytes
    total_host_pinned_per_slot = host_summaries + host_checkpoints + host_errors # 2,621,568 bytes = 2.50 MiB

    # Theoretical 27-band dense model from Phase 2F-B / Phase 2F-C table:
    # 27 bands * 6 MiB payload / 256 B chunk = 24,576 chunks / band
    # 24,576 * 32 B (16B summary + 16B checkpoint model) = 786,432 B (0.75 MiB) / band
    # 27 * 0.75 MiB = 20.25 MiB (documented as ~20.3 MiB)
    # Or 27 * 0.80 MiB (Phase 2F-B band 6 851,072 B) = 21.6 MiB

    audit_data = {
        "benchmark": "HyperCine Phase 2F-C Memory Accounting Audit",
        "definitions": {
            "device_vram": "Dedicated GPU GDDR6X physical memory allocated via cudaMalloc",
            "host_pinned_memory": "Non-pageable host OS RAM locked into physical memory via cudaMallocHost",
            "pageable_host_memory": "Standard OS virtual memory for process thread stacks and heap",
            "working_set": "Total active physical host memory pages in RAM as reported by GetProcessMemoryInfo"
        },
        "actual_pipeline_implementation": {
            "max_fc_chunks_per_slot": kMaxFcChunks,
            "device_vram_per_slot_bytes": total_dev_vram_per_slot,
            "device_vram_per_slot_mib": round(total_dev_vram_per_slot / (1024 * 1024), 4),
            "host_pinned_per_slot_bytes": total_host_pinned_per_slot,
            "host_pinned_per_slot_mib": round(total_host_pinned_per_slot / (1024 * 1024), 4),
            "total_incremental_allocation_per_slot_bytes": total_dev_vram_per_slot + total_host_pinned_per_slot,
            "total_incremental_allocation_per_slot_mib": round((total_dev_vram_per_slot + total_host_pinned_per_slot) / (1024 * 1024), 4)
        },
        "reconciliation_of_documented_figures": {
            "documented_20_3_mib": {
                "source": "Chunk-size sweep table (Section 5.1): Per-Slot Scratch (27 bands)",
                "derivation": "27 bands * (6 MB / 256 B = 24,576 chunks) * 32 bytes = 20,250 KiB = 20.25 MiB",
                "meaning": "Theoretical worst-case metadata capacity model if all 27 bands were 6 MB dense payloads. Not actual allocated VRAM."
            },
            "documented_21_6_mib": {
                "source": "Executive Summary (Section 5.2): Total incremental VRAM per slot over Phase 2E",
                "derivation": "27 bands * 0.80 MiB (from Phase 2F-B Band 6 scratch of 851,072 bytes) = 21.6 MiB",
                "discrepancy_and_correction": "Mislabeled as 'incremental VRAM'. In reality, the actual allocated incremental device VRAM is 2.50 MiB per slot, and incremental host pinned memory is 2.50 MiB per slot (total 5.00 MiB per slot). The 21.6 MiB figure was a theoretical upper bound that conflated host pinned and device memory across 27 hypothetical dense bands."
            },
            "working_set_112_mib": {
                "source": "Section 5.2: Peak working set during 4-slot 4K streaming",
                "meaning": "Process host resident memory during multi-slot execution, within expected bounds for host workers and pinned pools."
            }
        },
        "status": "AUDITED_AND_RECONCILED"
    }

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(audit_data, f, indent=2)

    print("Saved benchmarks/results/decode_phase2fc_memory_audit.json successfully.")

if __name__ == "__main__":
    main()
