# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Consolidate E evidence; fail rather than certify missing or failed gates.

Usage: python benchmarks/summarize_decode_phase3ae.py [repository root]
Raw runs live in build/phase3ae. Historical results are read, never rewritten.
"""
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
RAW = ROOT / "build/phase3ae"
OUT = ROOT / "benchmarks/results"
LICENSE = "MIT OR Apache-2.0"
BASE = "9f63cfa191671175a2a34991a3084631332f90bd"
MAIN = "51435890ba5853e87e8cf1ddc7cc747954bd9e80"


def read(path):
    return json.loads((ROOT / path).read_text(encoding="utf-8-sig"))


def raw(path):
    return read(Path("build/phase3ae") / path)


def sha(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def save(name, data, compact=False):
    data = {"license": LICENSE, "phase": "3A-E", **data}
    (OUT / ("decode_phase3ae_" + name + ".json")).write_text(
        json.dumps(data, indent=None if compact else 2) + "\n", encoding="utf-8")


def exact(case):
    return (case["descriptor_differences"] == 0
            and not any(case["coefficient_differences"])
            and not any(case["ll_differences"])
            and not any(case["planar_differences"])
            and case["public_planar_request_differences"] == 0
            and case["yu64_word_differences"] == 0
            and case["bgra_pixel_differences"] == 0)


def ctest(name, count):
    path = "benchmarks/results/decode_phase3ae_" + name + "_ctest.txt"
    text = (ROOT / path).read_text(encoding="utf-8-sig", errors="replace")
    match = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", text)
    elapsed = re.search(r"Total Test time \(real\) =\s+([\d.]+) sec", text)
    assert match and elapsed and tuple(map(int, match.groups())) == (100, 0, count), path
    return {"total": count, "passed": count, "failed": 0,
            "elapsed_seconds": float(elapsed[1]), "log": path, "sha256": sha(path)}


def main():
    pre, post = raw("pre/stage_compare.json"), raw("post/stage_compare.json")
    assert len(pre["cases"]) == len(post["cases"]) == 96
    assert all(exact(c) for c in post["cases"])
    for c in pre["cases"]:
        d = c["bgra_pixel_differences"]
        assert d > 0 and c["old_cpu_clone_matches_gpu"] == c["signed_cpu_clone_matches_reference"] == c["negative_y_mismatches"] == d
        assert c["descriptor_differences"] == c["public_planar_request_differences"] == c["yu64_word_differences"] == 0
        assert not any(c["coefficient_differences"] + c["ll_differences"] + c["planar_differences"])
        assert c["serialized_color_identical"]
    reproduction = raw("pre/reproduction.json")
    assert [c["bgra_mismatching_pixels"] for c in reproduction["cases"]] == [447, 93, 33]
    assert all(c["repeats"] == 100 and c["identical_counts_coordinates_values"] for c in reproduction["cases"])
    for c in reproduction["cases"]:
        c["slot"] = next(s["slot"] for s in pre["cases"] if s["frame"] == c["frame"] and s["requested_strategy"] == 1)
        c["slot_evidence"] = "same-input matching first ticket in independently initialized inspector audit"
    save("reproduction", reproduction)
    save("stage_compare", {"difference_units": "active coefficients / samples / YU64 words / BGRA pixels",
         "descriptor_check": "validated parser plus all 30 channel/level/band/width/height geometries against native CPU",
         "reference_offsets": {"BGRA": 24, "YU64_planar": 4}, "before": pre, "after": post})

    pixels = [json.loads(line) for line in (RAW / "pre/pixels.jsonl").read_text().splitlines()]
    assert len(pixels) == 12
    save("pixels", {"coordinate_convention": "zero-based output x,row; signed planes top-down; channel order Y/U/V and B/G/R/A",
         "address_order": ["Y", "U", "V", "BGRA"], "before": pixels,
         "after": [json.loads(line) for line in (RAW / "post/pixels.jsonl").read_text().splitlines()]}, compact=True)
    selected = [r for r in pixels if r["strategy"] == 1]
    distributions = []
    for row in selected:
        points = row["pixels"]
        assert all(p["cpu_yuv"] == p["gpu_yuv"] and p["gpu_yuv"][0] < 0 for p in points)
        hist = lambda values: {str(v): values.count(v) for v in sorted(set(values))}
        distributions.append({"frame": row["frame"], "pixels": len(points),
            "y_histogram": hist([p["gpu_yuv"][0] for p in points]),
            "u_min_max": [min(p["gpu_yuv"][1] for p in points), max(p["gpu_yuv"][1] for p in points)],
            "v_min_max": [min(p["gpu_yuv"][2] for p in points), max(p["gpu_yuv"][2] for p in points)],
            "x_modulo": {str(n): hist([p["x"] % n for p in points]) for n in (2, 4, 8, 16, 32, 64)},
            "row_edge_distance": hist([min(p["output_row"], 1079-p["output_row"]) for p in points]),
            "scalar_tail_pixels": sum(p["x"] >= 1888 for p in points),
            "first_pixel": points[0]})
    save("root_cause", {"classification": "deterministic shared color input signedness defect",
        "first_divergent_representation": "DeviceBGRA8", "first_wrong_operation": "uint16_t(raw_y) >> 2",
        "all_96_combinations_old_clone_matches_gpu_and_signed_clone_matches_reference": True,
        "default_mismatches_all_negative_y": 573,
        "brick_arithmetic": {"raw_y": -1, "old_uint16": 65535, "old_y8": 16383,
          "fixed_y8": -1, "old_limiter": 255, "fixed_limiter": 0,
          "old_luma_contribution": 18996, "old_prepack_bgr": [286,296,302],
          "cpu_bgra": [0, 0, 5, 255], "old_gpu_bgra": [255, 255, 255, 255]},
        "previous_frame_contamination": False, "slot_reuse_cause": False, "event_order_cause": False,
        "bounds_cause": False, "chroma_index_cause": False, "orientation_cause": False,
        "serialized_color_after_explicit_inverse_and_ready_wait_identical": True,
        "sentinel_values": [205, 165], "distributions": distributions})

    with (ROOT / "benchmarks/phase3ad_media/holdout_manifest.tsv").open(encoding="utf-8-sig", newline="") as stream:
        media = {r["id"]: r for r in csv.DictReader(stream, delimiter="\t")}
    manifest = []
    for c in reproduction["cases"]:
        m = media[c["frame"]]
        sample = Path("build/phase3ae/pre") / (c["frame"] + ".cfhd")
        manifest.append({**m, "source_sha256": sha(m["path"]), "sample_sha256": sha(sample),
          "source_fnv1a64": c["source_fnv1a64"], "sample_fnv1a64": c["sample_fnv1a64"],
          "sample_bytes": c["sample_bytes"], "old_bgra_fnv1a64": c["gpu_bgra_fnv1a64"],
          "reference_bgra_fnv1a64": c["cpu_bgra_fnv1a64"], "old_mismatches": c["bgra_mismatching_pixels"],
          "first32": c["first32"]})
    (ROOT / "tests/decode_phase3ae_photos.json").write_text(json.dumps({"license": LICENSE,
        "media_committed": False, "source_provenance": "retained Phase 3A-D CC0 manifest",
        "recipe": {"input": "1920x1080 YUY2, pitch 3840", "output": "YUV422 CFHD, FilmScan3, flags NONE",
          "seed": "0x51A77", "metadata": "NormalizeMetadata from decode_phase0_support.h",
          "runner": "test_decode_phase3ae --validate ROOT OUTPUT", "encoding_implementation": "PhotoSample in test_decode_phase3ae.cpp"},
        "cases": manifest}, indent=2) + "\n", encoding="utf-8")
    if "--audit-only" in sys.argv:
        print("Reproduction, stage, pixel, root-cause and photo provenance evidence consolidated.")
        return

    unit_before, unit = raw("pre/unit.json"), raw("post/unit.json")
    assert unit_before["direct_color_differences"] == 23502 and unit["direct_color_differences"] == 0
    assert all(c["bgra_pixel_differences"] == 32 for c in unit_before["encoded_cases"])
    assert all(exact(c) for c in unit["encoded_cases"])
    reuse, mixed, instances, golden = (raw("post/" + n + ".json") for n in ("reuse", "mixed", "instances", "golden"))
    assert all(c["repeats"] == 250 and c["mismatches_vs_cpu"] == 0 and c["owned_memory_stable"] for c in reuse["cases"])
    assert mixed["frames"] == 2000 and mixed["exact_every_frame"] and mixed["owned_capacities_stable"]
    assert [c["instances"] for c in instances["cases"]] == [2, 4] and all(c["real_photo_output_exact"] for c in instances["cases"])
    assert golden["samples"] == 11 and len(golden["cases"]) == 44 and all(exact(c) for c in golden["cases"])
    corpus = raw("post/corpus.json")
    assert corpus["failed"] == 0 and corpus["passed"] + corpus["unsupported"] == len(corpus["cases"]) == 158
    cuda, cpu = ctest("cuda", 33), ctest("cpu", 6)
    facade = read("build/tests/benchmarks/results/decode_phase3a_validation.json")
    assert facade["status"] == "PASS" and facade["steady_state_cpp_allocations"] == 0
    save("validation", {"photo_combinations": 96, "post_fix_all_stages_exact": True,
         "unit_before": unit_before, "unit_after": unit, "reuse_before": raw("pre/reuse.json"),
         "reuse_after": reuse, "golden": golden, "correctness_corpus": corpus,
         "cuda_ctest": cuda, "cpu_ctest": cpu, "mixed": mixed, "instances": instances,
         "existing_facade_lifetime_and_allocations": facade})

    sanitizers = []
    for phase in ("pre", "post"):
        for tool in ("memcheck", "initcheck", "racecheck", "synccheck"):
            path = f"benchmarks/results/decode_phase3ae_{phase}_native_{tool}.txt"
            log = (ROOT / path).read_text(encoding="utf-8-sig", errors="replace")
            summary = ("RACECHECK SUMMARY: 0 hazards displayed (0 errors, 0 warnings)" if tool == "racecheck" else "ERROR SUMMARY: 0 errors")
            assert summary in log, path
            if phase == "post" and tool == "memcheck":
                assert "LEAK SUMMARY: 0 bytes leaked in 0 allocations" in log
            exit_path = RAW / f"{phase}_native_{tool}_exit.txt"
            assert int(exit_path.read_text(encoding="utf-8-sig").strip()) == 0, exit_path
            sanitizers.append({"phase": phase, "tool": tool, "exit_code": 0, "errors": 0,
              "hazards": 0 if tool == "racecheck" else None, "warnings": 0 if tool == "racecheck" else None,
              "leak_check_full": phase == "post" and tool == "memcheck",
              "leaked_bytes": 0 if phase == "post" and tool == "memcheck" else None,
              "log": path, "sha256": sha(path)})
    save("sanitizers", {"runs": sanitizers, "coverage": "synthetic direct/encoded plus 3 photos x4 strategies all stages and poisoned slot/consumer reuse",
        "runner_modes": {"pre": "saved baseline.exe --focused-pre", "post": "test_decode_phase3ae.exe --sanitizer"},
        "failed_attach_attempt": {"log": "benchmarks/results/decode_phase3ae_pre_memcheck.txt",
          "result": "sandbox attach timed out; attempt terminated, native rerun succeeded; not counted as pass"}})

    windows = [(name, raw(name + "/performance.json")) for name in ("perf-old-1", "perf-new-1", "perf-new-2", "perf-old-2")]
    comparison = []
    for before in windows[0][1]["cases"]:
        key = before["frame"], before["strategy"]
        rows = [next(c for c in w["cases"] if (c["frame"], c["strategy"]) == key) for _, w in windows]
        old = statistics.mean(r["mean_ms"] for r in (rows[0], rows[3]))
        new = statistics.mean(r["mean_ms"] for r in (rows[1], rows[2]))
        deltas = [(rows[1]["mean_ms"] / rows[0]["mean_ms"] - 1)*100, (rows[2]["mean_ms"] / rows[3]["mean_ms"] - 1)*100]
        assert all(r["device_bytes"] == rows[0]["device_bytes"] and r["pinned_bytes"] == rows[0]["pinned_bytes"]
                   and r["cuda_allocation_calls"] == rows[0]["cuda_allocation_calls"] for r in rows)
        comparison.append({"frame": key[0], "strategy": key[1], "before_ms": old, "after_ms": new,
          "latency_change_percent": (new/old-1)*100, "paired_changes_percent": deltas,
          "repeatable_over_5_percent": all(d > 5 for d in deltas), "memory_unchanged": True})
    reference, current = read("benchmarks/results/reference_windows_msvc.json"), raw("post/benchmark_codec.json")
    reference_comparison = []
    for r, c in zip(reference["benchmarks"], current["benchmarks"]):
        reference_comparison.append({"name": r["name"], "historical_decode_ms": r["decode"]["latency_avg_ms"],
          "current_decode_ms": c["decode"]["latency_avg_ms"],
          "decode_latency_change_percent": (c["decode"]["latency_avg_ms"]/r["decode"]["latency_avg_ms"]-1)*100,
          "historical_peak_memory_bytes": r["peak_memory_bytes"], "current_peak_memory_bytes": c["peak_memory_bytes"]})
    save("performance", {"method": "sequential ABBA, 15 warmups and 120 measured frames per case/strategy/window; Queue through Ready+Release without download",
       "same_session": True, "old_binary_sha256": sha("build/phase3ae/baseline.exe"),
       "windows": dict(windows), "comparison": comparison, "initial_pre_fix_window": raw("pre/performance.json"),
       "benchmark_codec_current": current, "historical_reference_comparison": reference_comparison,
       "historical_comparison_is_cross_session_cpu_only": True})

    assert git("rev-parse", "main") == MAIN
    assert not git("status", "--porcelain", "upstream/")
    assert git("rev-parse", "HEAD:upstream") == git("rev-parse", "11574d0295771edccadd17af14af74a539f924c7^{tree}")
    encoders = [p for p in git("ls-tree", "-r", "--name-only", BASE, "src", "include").splitlines() if "encoder" in p]
    assert encoders and not git("diff", BASE, "--", *encoders)
    historic = [p for p in git("ls-tree", "-r", "--name-only", BASE, "docs", "benchmarks/results", "tests/decode_phase0_golden.txt").splitlines()
                if "PHASE3AE" not in p and "phase3ae" not in p]
    assert not git("diff", BASE, "--", *historic)
    integrity = {"upstream_pristine": True, "main_unchanged": MAIN, "encoder_unchanged": True,
       "historical_reports_and_results_unchanged": True, "golden_manifest_sha256": sha("tests/decode_phase0_golden.txt"),
       "history_sha256": {p: sha(p) for p in ("docs/DECODE_PHASE3A.md", "docs/DECODE_PHASE3A_RC.md", "docs/DECODE_PHASE3AD.md", "benchmarks/results/decode_phase3ad_holdout.json")},
       "repaired_binary_sha256": sha("build/tests/Release/test_decode_phase3ae.exe"),
       "color_source_sha256": sha("src/cuda/decode_color_cuda.cu")}
    changed = sorted(set(git("diff", "--name-only", BASE).splitlines()
                  + git("ls-files", "--others", "--exclude-standard").splitlines()))
    production = [p for p in changed if p.startswith(("src/", "include/"))]
    assert production == sorted(["include/hypercine/research/decode_phase2e.h", "include/hypercine/research/decoder_diagnostics.h",
         "src/core/decoder.cpp", "src/cuda/decode_color_cuda.cu", "src/cuda/decode_phase2e_pipeline.cu", "src/cuda/decode_phase2e_stub.cpp"])
    photo_impacts = [c for c in comparison if c["frame"].startswith("photo_")]
    impact_1080 = [c for c in comparison if c["frame"].startswith("1920_")]
    impact_4k = [c for c in comparison if c["frame"].startswith("3840_")]
    labels = ["branch", "base commit", "commits", "files changed", "production files changed", "upstream status", "encoder status", "main status",
      "original brick mismatch count", "original grass mismatch count", "original handwriting mismatch count", "repeatability before fix",
      "coefficients status", "LL status", "planar status", "YU64 status", "BGRA status", "first divergent stage", "first divergent frame",
      "first divergent coordinate", "CPU YUV there", "GPU YUV there", "CPU BGRA there", "GPU BGRA there", "pitch/address calculation",
      "root cause class", "exact root cause", "deterministic vs race", "previous-frame contamination", "slot reuse involvement", "stream/event involvement",
      "color math involvement", "bounds involvement", "signedness/overflow involvement", "minimal reproducer", "repair summary", "why repair is correct",
      "why previous corpus missed it", "post-fix brick mismatches", "post-fix grass mismatches", "post-fix handwriting mismatches", "500 repeat result",
      "Lightweight vs Hybrid equality", "HostStreaming equality", "11/11 Golden result", "broader correctness corpus result", "CUDA CTest", "CPU-only CTest",
      "initcheck", "memcheck", "racecheck", "synccheck", "mixed 2000-frame smoke", "two-instance result", "four-instance result", "output lifetime result",
      "allocation change", "VRAM change", "pinned-memory change", "1080p performance impact", "4K performance impact", "failing-photo performance impact",
      "final outcome", "remaining correctness blockers", "dispatcher blocker status", "merge recommendation", "recommended next step"]
    commits = git("log", "--format=%H %s", BASE+"..HEAD").splitlines()
    outcome = "A — FIXED" if not any(c["repeatable_over_5_percent"] for c in comparison) else "correctness fixed; repeatable performance flag requires investigation"
    values = [git("branch", "--show-current"), BASE, commits, changed, production, "pristine", "unchanged", MAIN,
      447, 93, 33, "100 per photo; complete images identical", "30/30 bands exact", "6/6 checkpoints exact", "BGRA ticket and public planar request exact", "bit-exact words",
      "original 447/93/33; now zero", "DeviceBGRA8 precision reduction", "photo_holdout_brick", {"x": 711, "bottom_up_output_row": 2, "plane_row": 1077},
      [-1,495,524], [-1,495,524], [0,0,5,255], [255,255,255,255], distributions[0]["first_pixel"]["addresses"],
      "signedness", "uint16_t reinterprets negative reconstructed samples before precision reduction", "deterministic logic", False, False, False,
      "input signedness; matrix formula unchanged", False, "unsigned reinterpretation before clamp; no inverse overflow", "64x64 encoded undershoot plus literal signed-plane vectors",
      "remove uint16 casts from Y/V/U precision reduction", "preserves negative overshoots for upstream-compatible limiter; positive values unchanged",
      "frozen generators missed negative values; direct test CPU oracle copied the unsigned defect; D checker stopped early",
      0, 0, 0, "500 per photo, slots 1/2, all full images exact", True, True, "11/11, four strategies, unchanged frozen hashes",
      {"passed": corpus["passed"], "unsupported_controls": corpus["unsupported"], "failed": corpus["failed"]}, cuda, cpu,
      "zero errors", "zero errors", "zero hazards/errors/warnings", "zero errors", mixed, instances["cases"][0], instances["cases"][1],
      "held/non-FIFO/consumer reuse; existing completed/failed output checks and destruction/drain pass", "zero steady-state C++ allocations; CUDA calls unchanged", "owned bytes unchanged", "owned bytes unchanged",
      impact_1080, impact_4k, photo_impacts, outcome, [], "Phase 3A-D HOLD remains", "review minimal fix separately; no automatic merge", "human review; keep Phase 3B dispatcher promotion blocked"]
    assert len(labels) == len(values) == 67
    fields = [{"number": i+1, "field": label, "value": value} for i, (label, value) in enumerate(zip(labels, values))]
    save("completion", {"branch": git("branch", "--show-current"), "base_commit": BASE,
       "commits": git("log", "--format=%H %s", BASE+"..HEAD").splitlines(),
       "commit_list_scope": "implementation commits at evidence generation; the evidence commit is the commit containing this file (git log BASE..HEAD)",
       "files_changed": changed, "production_files_changed": production, "integrity": integrity,
       "completion_fields": fields, "outcome": outcome,
       "output_correctness_blocker_cleared": True, "dispatcher_status": "Phase 3A-D HOLD remains",
       "remaining_correctness_blockers_in_supported_scope": [], "merge_recommendation": "review evidence and minimal repair separately; do not promote dispatcher",
       "next_step": "human review; Phase 3B remains blocked by independent Auto-dispatch HOLD",
       "merge_tag_push_performed": False})
    doc = ROOT / "docs/DECODE_PHASE3AE.md"
    marker = "\n<!-- GENERATED E RESULTS -->\n"
    prefix = doc.read_text(encoding="utf-8").split(marker)[0]
    lines = [marker, "### Completed correctness gates\n",
      f"Outcome: **{outcome}**. Brick/grass/handwriting now have 0/0/0 wrong pixels. "
      "All 96 photo configurations and all four requested strategies match upstream at every audited stage.\n",
      "Each photo passes 500 Lightweight repetitions (250 with one slot, 250 with two), "
      "including alternating black/white predecessors, poisoned completed surfaces, held/non-FIFO releases and registered consumer copies.\n",
      f"Frozen golden: 11/11 samples, 44 strategy comparisons, original sample and CPU output hashes unchanged. "
      f"Frozen RC/D corpus: {corpus['passed']}/{corpus['passed']} supported cases in {len(corpus['cases'])} total cases pass all four strategies and stages; "
      f"{corpus['unsupported']} unsupported controls remain unsupported, with zero failures. "
      "These controls are RGB444 and 8K noise above the existing 32 MiB sample limit. "
      "Coverage includes photographs, text/UI, architecture, foliage, water, grain/noise, gradients and high contrast; no dispatcher fitting is performed.\n",
      f"CUDA CTest: {cuda['passed']}/{cuda['total']} pass in {cuda['elapsed_seconds']:.2f} s. "
      f"CPU-only CTest: {cpu['passed']}/{cpu['total']} pass in {cpu['elapsed_seconds']:.2f} s. "
      "The first CUDA run had 32 passes and one obsolete unsigned CPU-oracle failure (706.18 s); "
      "its log is retained as `decode_phase3ae_cuda_ctest_first.txt`. A fresh full run follows the oracle correction; no timeout or tolerance was relaxed.\n",
      "All four native Compute Sanitizer tools pass both before and after repair: memcheck/initcheck/synccheck report zero errors; "
      "racecheck reports zero hazards, errors and warnings. Coverage includes the self-contained regression, all three real photos through "
      "all four strategies, stage downloads, isolated synchronized color and poisoned slot/consumer reuse. "
      "A first sandbox memcheck attempt could not attach and timed out; its failure log is preserved, its hung audit processes were terminated, "
      "and the native rerun passed. That attempt is not counted as a clean sanitizer run. Post-fix memcheck with full leak checking "
      "also reports 0 bytes leaked in 0 allocations.\n",
      "The 2000-frame mixed smoke verifies every full BGRA image while switching Lightweight, Hybrid and Auto over nine "
      "photo/synthetic/text cases with two held tickets and varying release order. Concurrent runs with two and four independent "
      "decoders each complete 100 real-photo frames per instance exactly. Existing facade tests also pass destruction/drain, failed "
      "work, consumer waits, malformed rejection and ticket identity checks.\n",
      f"Owned device/pinned capacities and CUDA allocation counts remain unchanged. Existing facade warm-state accounting reports "
      f"{facade['steady_state_cpp_allocations']} C++ allocations. Mixed-smoke working set changes from {mixed['working_set_before']} "
      f"to {mixed['working_set_after']} bytes; global free VRAM changes from {mixed['free_vram_before']} to {mixed['free_vram_after']} bytes. "
      "Global free VRAM is observational and includes driver activity; owned-byte equality is the allocator invariant. "
      "The repair adds no buffers, streams or events.\n",
      "### Performance regression measurement\n",
      "Same-session ABBA uses the saved old-kernel binary and repaired binary, 15 warmups followed by 120 measured frames per "
      "case/strategy/window. It measures Queue through Ready and Release without download. Means below average the two "
      "windows per version. All individual FPS, megapixels/s, min/max latency and peak working-set values are retained in the performance JSON.\n",
      "| Case | Strategy | Before ms | After ms | Latency change | Both paired comparisons >5% |\n| --- | --- | ---: | ---: | ---: | --- |"]
    for c in comparison:
        lines.append(f"| {c['frame']} | {'Lightweight' if c['strategy'] == 1 else 'Hybrid'} | {c['before_ms']:.4f} | {c['after_ms']:.4f} | {c['latency_change_percent']:+.2f}% | {c['repeatable_over_5_percent']} |")
    lines += ["\nPositive change means slower. Cross-window variation is recorded explicitly; this phase claims no optimization. "
      "The >5% flag requires both paired comparisons to regress. Owned device/pinned capacities and allocation calls match across all four windows.\n",
      "`benchmark_codec` was also run at 1080p, 4K and 8K and compared with the frozen Windows reference. "
      "That is the unchanged CPU SDK benchmark and a cross-session comparison, not evidence of GPU repair cost.\n",
      "| CPU reference benchmark | Historical decode ms | Current decode ms | Change | Historical/current peak bytes |\n| --- | ---: | ---: | ---: | --- |"]
    for c in reference_comparison:
        lines.append(f"| {c['name']} | {c['historical_decode_ms']:.4f} | {c['current_decode_ms']:.4f} | {c['decode_latency_change_percent']:+.2f}% | {c['historical_peak_memory_bytes']}/{c['current_peak_memory_bytes']} |")
    lines += ["\n### Repository integrity and recommendation\n",
      "Upstream is pristine; encoder files, main, frozen golden manifest and historical reports/results are unchanged. "
      "Only the shared CUDA color reduction changes decoding behavior. Research inspection and audit/test tools supply the evidence. "
      "No merge, tag or push was performed. The source baseline and report provenance hashes are retained in completion JSON.\n",
      "Review the minimal signedness repair and regression coverage as a correctness patch. The supported progressive YUV422 "
      "output blocker is cleared by the clean acceptance gates above. Phase 3A-D's Auto-dispatch HOLD remains; this phase "
      "does not authorize Phase 3B promotion or certify additional formats/backends.\n",
      "### Reproduction commands\n",
      "```powershell\ncmake --build build --config Release\nctest --test-dir build -C Release --output-on-failure\n"
      "cmake -B build-cpu -S . -DHYPERCINE_ENABLE_CUDA=OFF\ncmake --build build-cpu --config Release\n"
      "ctest --test-dir build-cpu -C Release --output-on-failure\n"
      ".\\build\\tests\\Release\\test_decode_phase3ae.exe --validate . build/phase3ae/post\n"
      ".\\build\\tests\\Release\\test_decode_phase3ae.exe --reuse . build/phase3ae/post\n"
      ".\\build\\tests\\Release\\test_decode_phase3ae.exe --golden . build/phase3ae/post\n"
      ".\\build\\tests\\Release\\test_decode_phase3ae.exe --mixed . build/phase3ae/post\n"
      ".\\build\\tests\\Release\\test_decode_phase3ae.exe --instances . build/phase3ae/post\n"
      "# Native host, repeat --tool for memcheck/initcheck/racecheck/synccheck:\n"
      "compute-sanitizer --tool memcheck --error-exitcode 99 .\\build\\tests\\Release\\test_decode_phase3ae.exe --sanitizer . build/phase3ae/post-native-memcheck\n"
      "python benchmarks/summarize_decode_phase3ae.py .\n```\n",
      "The corpus fingerprint TSV can be reconstructed from the `id`/`sample_hash` pairs in the unchanged D measurements and "
      "hold-out JSON files, then passed to `audit_decode_phase3ae_corpus ROOT EXPECTED_TSV OUTPUT_JSON`. "
      "Pre-fix reproductions require the saved old binary or the parent implementation, not the repaired binary.\n",
      "### Requested completion fields\n",
      "All 67 fields are below; full structured values and file/commit lists are in `decode_phase3ae_completion.json`. "
      "Commit entries capture implementation commits available when evidence is generated; the commit containing this report "
      "is identified by `git log 9f63cfa..HEAD`.\n",
      "| # | Field | Result |\n| ---: | --- | --- |"]
    for f in fields:
        v = f["value"]
        if isinstance(v, (dict, list)):
            value = json.dumps(v, ensure_ascii=False)
            if len(value) > 180:
                value = "Recorded in completion JSON field " + str(f["number"])
        else:
            value = str(v)
        lines.append(f"| {f['number']} | {f['field']} | {value.replace('|', '/')} |")
    doc.write_text(prefix + "\n".join(lines) + "\n", encoding="utf-8")
    print(f"E evidence consolidated: 96 photo combinations, 11 golden samples, {corpus['passed']} corpus passes, 33+6 CTest passes.")
    print("Performance flags:", [c["frame"] for c in comparison if c["repeatable_over_5_percent"]])


if __name__ == "__main__":
    main()
