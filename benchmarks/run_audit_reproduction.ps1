# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

# HyperCine Phase 2F-C Audit Benchmark Reproduction Script

$ErrorActionPreference = "Stop"
Write-Host "=======================================================================" -ForegroundColor Cyan
Write-Host "  HyperCine Decode Phase 2F-C Automated Audit & Reproduction Suite" -ForegroundColor Cyan
Write-Host "=======================================================================" -ForegroundColor Cyan

# Step 1: Pre-benchmark GPU state check
Write-Host "`n[1/5] Checking GPU Telemetry & Background Processes..." -ForegroundColor Yellow
nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,clocks.gr --format=csv
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv

# Step 2: Run Verification, Boundary, Determinism, and Selector Audit Suite
Write-Host "`n[2/5] Running Audit Verification Suite (test_decode_phase2fc_audit)..." -ForegroundColor Yellow
.\build\tests\Release\test_decode_phase2fc_audit.exe tests/decode_phase0_golden.txt
if ($LASTEXITCODE -ne 0) { throw "test_decode_phase2fc_audit failed!" }

# Step 3: Run Controlled Headline Benchmark, 5-Pattern Aggregate & Slot Scaling
Write-Host "`n[3/5] Running Benchmark Reproduction Suite (benchmark_decode_phase2fc_reproduction)..." -ForegroundColor Yellow
.\build\benchmarks\Release\benchmark_decode_phase2fc_reproduction.exe
if ($LASTEXITCODE -ne 0) { throw "benchmark_decode_phase2fc_reproduction failed!" }

# Step 4: Run Machine-Readable Document Consistency, Memory, and Timeline Audits
Write-Host "`n[4/5] Running Machine-Readable Consistency, Memory & Timeline Audits..." -ForegroundColor Yellow
python benchmarks/audit_doc_consistency.py
python benchmarks/audit_memory_accounting.py
python benchmarks/audit_timeline.py
python benchmarks/generate_audit_summary_json.py

# Step 5: Summary Verification
Write-Host "`n[5/5] Verifying Audit Artifacts Generated..." -ForegroundColor Yellow
Get-ChildItem benchmarks/results/decode_phase2fc* | Select-Object Name, Length, LastWriteTime

Write-Host "`n=======================================================================" -ForegroundColor Green
Write-Host "  Phase 2F-C Audit & Reproduction Suite Completed Successfully!" -ForegroundColor Green
Write-Host "=======================================================================" -ForegroundColor Green
