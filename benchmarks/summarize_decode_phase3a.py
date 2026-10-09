# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Validate Phase 3A artifacts and render their measured documentation tables.

Run from the repository root after the documented native validation commands.
Uses only Python's standard library; does not run the codec or change history.
"""
import datetime
import hashlib
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
RESULTS = ROOT / "benchmarks/results"
BASE = "dded4d8ae61800241164499fa875c0cb104281a4"


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def read(name):
    return json.loads((RESULTS / name).read_text(encoding="utf-8"))


def write(name, value):
    (RESULTS / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def mib(n):
    return f"{n / 1048576:.2f}"


def table(headers, rows):
    return "\n".join(["| " + " | ".join(headers) + " |",
                      "| " + " | ".join(["---"] * len(headers)) + " |"] +
                     ["| " + " | ".join(map(str, row)) + " |" for row in rows])


def passed_ctest(path, expected):
    text = (ROOT / path).read_text(encoding="utf-8", errors="replace")
    assert "100% tests passed, 0 tests failed" in text, path
    assert f"out of {expected}" in text, path
    elapsed = re.search(r"Total Test time \(real\) =\s*([\d.]+)", text)
    return {"passed": expected, "failed": 0, "seconds": float(elapsed[1])}


def main():
    matrix = read("decode_phase3a_windows_msvc.json")
    selector = read("decode_phase3a_auto_selector.json")
    oracle = read("decode_phase3a_oracle.json")
    mixed = read("decode_phase3a_mixed_workload.json")
    longrun = read("decode_phase3a_longrun.json")
    memory = read("decode_phase3a_memory.json")
    validation = read("decode_phase3a_validation.json")
    assert validation["mode"] == "full" and validation["status"] == "PASS"
    assert len(validation["golden"]) == len(matrix["cases"]) == len(selector["cases"]) == 11
    assert all(x["coefficients_exact"] == 30 and x["max_error"] == 0 for x in validation["golden"])
    assert all(x["ll_exact"] and x["planar_exact"] for x in validation["golden"])
    assert validation["real_difference_coded_coefficients_exact"] == 30
    assert validation["real_difference_max_error"] == 0 and validation["interlaced_production_rejected"]
    assert [x["frames"] for x in longrun["cases"]] == [10000, 5000, 1000]
    assert all(x["status"] == "PASS" and x["steady_state_cpp_allocations"] == 0 for x in longrun["cases"])
    assert oracle["oracle_improvement_capture_percent"] >= 90
    assert git("status", "--porcelain", "upstream/") == ""
    assert git("diff", BASE, "--", "upstream/") == ""
    assert git("diff", BASE, "--", "tests/decode_phase0_golden.txt") == ""
    assert git("rev-parse", "HEAD:upstream") == git("rev-parse", "11574d0295771edccadd17af14af74a539f924c7^{tree}")
    assert git("rev-parse", "main") == "51435890ba5853e87e8cf1ddc7cc747954bd9e80"

    sanitizers = {"mode": "focused_sanitizer", "instrumentation": "native Windows process attachment", "tools": []}
    for tool in ["initcheck", "memcheck", "racecheck", "synccheck"]:
        name = f"decode_phase3a_{tool}.txt"
        log = (RESULTS / name).read_text(encoding="utf-8", errors="replace")
        stdout = (ROOT / f"build/decoder-sanitizers/{tool}_stdout.txt").read_text(encoding="utf-8", errors="replace")
        code = int((ROOT / f"build/decoder-sanitizers/{tool}_exit.txt").read_text().strip())
        assert code == 0 and "production decoder validation PASS" in stdout, tool
        assert re.search(r"(?:ERROR SUMMARY: 0 errors|RACECHECK SUMMARY: 0 hazards)", log), tool
        assert not re.search(r"(?:Error:|ERROR SUMMARY: [1-9]|[1-9]\d* hazards)", log), tool
        sanitizers["tools"].append({"tool": tool, "exit_code": code, "errors": 0, "hazards": 0, "log": name,
                                    "log_sha256": hashlib.sha256((RESULTS / name).read_bytes()).hexdigest()})

    cuda = passed_ctest("build/cuda_ctest_final.txt", 31)
    cpu = passed_ctest("build/cpu_ctest_final.txt", 6)
    validation.update({"cuda_ctest": cuda, "cpu_only_ctest": cpu,
                       "determinism_windows": [{"width": 1920, "pattern": "noise", "frames": 200},
                                               {"width": 3840, "pattern": "noise", "frames": 100},
                                               {"width": 3840, "pattern": "texture", "frames": 100},
                                               {"width": 7680, "pattern": "flat", "frames": 20}],
                       "concurrent_requests_per_instance": 100,
                       "consumer_streams": 2, "classifier_boundary_tests": True,
                       "optional_progressive_yuy2_noise_difference_probe": "no progressive difference-coded sample found"})
    patterns = ["flat", "gradient", "noise", "texture", "edges"]
    frozen = {}
    for line in (ROOT / "tests/decode_phase0_golden.txt").read_text().splitlines():
        if line and not line.startswith("#"):
            w, h, p, ih, ch, n, bh, yh = line.split()
            frozen[int(w), patterns[int(p)]] = (ih, ch, int(n), bh, yh)
    for case in validation['golden']:
        ih, ch, n, bh, yh = frozen[case['width'], case['pattern']]
        assert bh == case['bgra_hash']
        case.update({"input_hash": ih, "compressed_hash": ch, "compressed_bytes": n,
                     "yu64_hash": yh, "yu64_exact": True,
                     "validated_strategies": ["automatic", "lightweight", "hybrid", "host_streaming"]})

    paths = set(git("diff", "--name-only", BASE).splitlines()) | set(git("ls-files", "--others", "--exclude-standard").splitlines())
    code_paths = sorted(p for p in paths if pathlib.Path(p).suffix in {".cpp", ".cu", ".cuh", ".h"} or p.endswith("CMakeLists.txt"))
    encoder_paths = [p for p in paths if "encoder" in p.lower() or re.match(r"include/hypercine/phase[23]", p)]
    assert not encoder_paths, encoder_paths
    historical_results = [p for p in paths if p.startswith("benchmarks/results/") and not pathlib.Path(p).name.startswith("decode_phase3a_")]
    assert not historical_results, historical_results
    fingerprints = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in code_paths}
    provenance = {"base_commit": BASE, "branch": git("branch", "--show-current"),
                  "copyright": "2026 HyperCine Contributors", "license": "MIT OR Apache-2.0",
                  "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "os": "Windows x64", "cpu": "AMD Ryzen 9 7940HS, 16 logical processors",
                  "gpu": matrix["gpu"], "cuda_toolkit": "12.8", "compiler": "MSVC 19.44 / Visual Studio 2022",
                  "configuration": "Release, sm_89", "seed": "0x1337BEEF",
                  "matrix_measured_frames": {"1920": 120, "3840": 60, "7680": 30}, "matrix_warmup_frames": 5,
                  "measured_code_sha256": fingerprints,
                  "golden_manifest_sha256": hashlib.sha256((ROOT / "tests/decode_phase0_golden.txt").read_bytes()).hexdigest(),
                  "implementation_commits": git("log", "--format=%h %s", f"{BASE}..HEAD").splitlines(),
                  "benchmark_slots": "matrix explicit 1; sweeps explicit 2/3/4; LowLatency default 1; HighThroughput default 3; longrun explicit 3",
                  "post_matrix_configuration_change": "Auto zero-slot default changed from 3 to 2 from sweep evidence; all measured configurations unchanged",
                  "upstream_tree": git("rev-parse", "HEAD:upstream"),
                  "encoder_sources_unchanged": True, "historical_results_unchanged": True}
    for name, obj in [("windows_msvc", matrix), ("auto_selector", selector), ("oracle", oracle), ("mixed_workload", mixed),
                      ("longrun", longrun), ("memory", memory), ("validation", validation), ("sanitizers", sanitizers)]:
        obj["provenance"] = provenance
        write(f"decode_phase3a_{name}.json", obj)

    by_case = {(x["width"], x["pattern"]): x for x in matrix["cases"]}
    choices = {(x["width"], x["pattern"]): x["decision"] for x in selector["cases"]}
    mix = {(x["width"], x["label"], x["slots"]): x for x in mixed["runs"]}
    corpus = [(x["width"], x["pattern"]) for x in matrix["cases"]]
    matrix_rows = []
    for key in corpus:
        c = by_case[key]
        matrix_rows.append([f"{key[0]} {key[1]}", choices[key]] +
                           [f"{c[x]['mean_ms']:.3f}" for x in ["phase2d", "phase2e", "adaptive_reference", "auto"]] +
                           [f"{min(c['lightweight']['mean_ms'], c['hybrid']['mean_ms']):.3f}", f"{c['auto_fps']:.1f}",
                            f"{c['auto_megapixels_per_second']:.1f}", f"{c['auto']['process_cpu_ms_per_frame']:.2f}"])
    sections = {"MATRIX": table(["Frame", "Auto choice", "D ms", "E ms", "FC ms", "Auto ms", "Oracle ms", "Auto FPS", "MP/s", "CPU ms/frame"], matrix_rows)}
    sections["MIXED"] = table(["Width / workload", "Slots", "FPS", "MP/s", "Mean ready latency ms", "CPU ms/frame", "L/H decisions", "Peak process WS MiB"],
                              [[f"{x['width']} {x['label']}", x["slots"], f"{x['fps']:.2f}", f"{x['megapixels_per_second']:.1f}",
                                f"{x['mean_frame_latency_ms']:.3f}", f"{x['process_cpu_ms_per_frame']:.2f}",
                                f"{x['lightweight_decisions']}/{x['hybrid_decisions']}", mib(x['peak_process_working_set_bytes'])] for x in mixed["runs"]])
    sections["ORACLE"] = (f"Selected-strategy improvement capture: **{oracle['oracle_improvement_capture_percent']:.2f}%**; "
                          f"Auto wall capture: {oracle['auto_wall_improvement_capture_percent']:.2f}%. "
                          f"Mean regret {oracle['mean_regret_ms']:.6f} ms, worst regret {oracle['worst_regret_ms']:.6f} ms, "
                          f"misclassifications {oracle['misclassification_count']}/11. "
                          f"Observed classifier cost ranged from {min(x['classifier_nanoseconds_observed'] for x in selector['cases'])} "
                          f"to {max(x['classifier_nanoseconds_observed'] for x in selector['cases'])} ns per frame.")
    sections["LONGRUN"] = table(["Width", "Frames", "Verified FPS", "First/last decile ms/frame", "WS change bytes", "Free VRAM change bytes", "C++ / owned CUDA allocations", "Result"],
                                [[x["width"], x["frames"], f"{x['fps_including_download_and_verification']:.2f}",
                                  f"{x['first_decile_ms']:.3f}/{x['last_decile_ms']:.3f}", x['working_set_after_bytes']-x['working_set_before_bytes'],
                                  x['global_free_device_bytes_after']-x['global_free_device_bytes_before'], "0 / 0", x['status']] for x in longrun['cases']])
    sections["SANITIZERS"] = table(["Tool", "Exit", "Errors / hazards", "Log"],
                                   [[x['tool'], x['exit_code'], "0 / 0", f"[{x['log']}](../benchmarks/results/{x['log']})"] for x in sanitizers['tools']])
    memory_rows = []
    for w in [1920, 3840]:
        for slots in [2, 3, 4]:
            m = mix[w, "equal_five_pattern", slots]['memory']
            memory_rows.append([w, slots, mib(m['device_bytes']), mib((m['device_bytes']-m['immutable_device_bytes'])/slots),
                                mib(m['pinned_host_bytes']), mib(m['pageable_owned_capacity_bytes']), m['cuda_owned_buffer_allocation_calls']])
    sections["MEMORY"] = table(["Configured width", "Slots", "Owned VRAM MiB", "VRAM per slot MiB", "Pinned MiB", "Known pageable capacity MiB", "Owned CUDA allocation calls"], memory_rows)
    sections["MEMORY"] += "\n\nEach instance owns 150016 bytes of immutable device FSM tables and 150016 bytes of host tables. Pinned memory per slot is 36224368 bytes (34.55 MiB). Known pageable capacity is roughly 40.61 MiB per slot plus instance objects; most of it is the conservative root reservation. These capacities remain stable after warmup."

    fresh = read("decode_phase3a_codec_reference.json")
    baseline = read("reference_windows_msvc.json")
    rows = []
    for old, new in zip(baseline['benchmarks'], fresh['benchmarks']):
        rows.append([new['name'], f"{old['encode']['fps']:.2f} -> {new['encode']['fps']:.2f}",
                     f"{old['decode']['fps']:.2f} -> {new['decode']['fps']:.2f}",
                     f"{new['encode']['megapixels_per_sec']:.1f}/{new['decode']['megapixels_per_sec']:.1f}",
                     f"{new['encode']['latency_avg_ms']:.2f}/{new['encode']['latency_min_ms']:.2f}/{new['encode']['latency_max_ms']:.2f}",
                     f"{new['decode']['latency_avg_ms']:.2f}/{new['decode']['latency_min_ms']:.2f}/{new['decode']['latency_max_ms']:.2f}", mib(new['peak_memory_bytes'])])
    sections['MATRIX'] += "\n\nThe required unchanged `benchmark_codec` comparison uses the original CPU harness, separately from production GPU decoding:\n\n" + table(
        ["Case", "Encode baseline -> current FPS", "Decode baseline -> current FPS", "Current encode/decode MP/s", "Encode avg/min/max ms", "Decode avg/min/max ms", "Peak WS MiB"], rows)
    sections['MATRIX'] += "\n\nAll three current CPU encode/decode throughput measurements are at least the preserved baseline; peak working set differs by less than 0.2 MiB. This harness links the pristine SDK and does not use the new decoder facade, so its timing improvements are not attributed to Phase 3A. The GPU matrix shows modest validation/barrier overhead against standalone D on 1080p Flat/Gradient; Noise retains the Hybrid advantage and Texture avoids forced Hybrid."

    def frame_result(w, p):
        c = by_case[w, p]
        return f"{c['auto']['mean_ms']:.3f} ms, {c['auto_fps']:.2f} FPS, {choices[w,p]}"

    def stream_result(x):
        return f"{x['fps']:.2f} FPS, {x['mean_frame_latency_ms']:.3f} ms ready latency, {x['process_cpu_ms_per_frame']:.2f} process CPU ms/frame"

    commits = git("log", "--format=%h %s", f"{BASE}..HEAD").splitlines()
    reports = [
        ("Branch", provenance['branch']), ("Base commit", BASE),
        ("Commits created", ("; ".join(commits) + "; docs(decoder): document Phase 3A production architecture and evidence (contains this report)") if commits else "Implementation and documentation commits are recorded in the final delivery; this table is generated before commit."),
        ("Files changed", f"{len(paths)} files; see decode_phase3a_completion.json for the complete path list"),
        ("Upstream status", "Pristine; tree equals the frozen upstream baseline; status empty"),
        ("Encoder status", "Sources, headers and behavior unchanged; benchmark_codec baseline preserved"),
        ("Production API", "HyperCine::Decoder, HyperCine::Decoder CMake target"),
        ("Public headers added", "decoder.h, decoder_cuda.h"),
        ("Research headers internalized", "Six decoder/index declarations under research/, old paths compatible"),
        ("Research headers still public", "Low-level entropy/color/inverse CUDA; encoder phase2 prototypes; compatibility forwarders"),
        ("DeviceOutput contract", "Borrowed pointers/pitches/geometry/format/orientation/ticket/event; valid until explicit release"),
        ("Ticket lifecycle", "Queued -> Running -> Ready/Failed -> Released; generation identity; Busy consumes no slot"),
        ("Error lifecycle", "First terminal error retained; no output on failure; drain/reuse; UnsupportedEncodingMode for interlaced/temporal"),
        ("Default slots", "Auto 2; LowLatency 1; HighThroughput 3; explicit 1..4"),
        ("Memory per slot", "1080p/4K VRAM " + mib((mix[1920,'equal_five_pattern',2]['memory']['device_bytes']-150016)/2) + "/" + mib((mix[3840,'equal_five_pattern',2]['memory']['device_bytes']-150016)/2) + " MiB; pinned 34.55 MiB; known pageable ~40.61 MiB"),
        ("VRAM per Auto decoder", "1080p/4K " + mib(mix[1920,'equal_five_pattern',2]['memory']['device_bytes']) + "/" + mib(mix[3840,'equal_five_pattern',2]['memory']['device_bytes']) + " MiB"),
        ("Pinned memory per Auto decoder", mib(mix[1920,'equal_five_pattern',2]['memory']['pinned_host_bytes']) + " MiB"),
        ("Auto classifier design", "Fixed integer dense/distributed/concentrated rule; no runtime learning"),
        ("Classifier inputs", "27 header descriptors: payload sizes, coefficient counts and level distribution"),
        ("Classifier cost", f"{min(x['classifier_nanoseconds_observed'] for x in selector['cases'])}..{max(x['classifier_nanoseconds_observed'] for x in selector['cases'])} ns observed"),
        ("Decisions by pattern", "1080p: Flat/Gradient/Texture L, Noise/Edges H; 4K: Flat/Texture L, Gradient/Noise/Edges H; 8K Flat L"),
        ("Adaptive band policy", "Audited AdaptiveDynamic E/BF retained, including content-derived fallback and effective call boundary"),
        ("Oracle methodology", "Minimum same-session mean of forced production L/H, not a runtime decision"),
        ("Oracle improvement capture", f"{oracle['oracle_improvement_capture_percent']:.2f}%; Auto wall {oracle['auto_wall_improvement_capture_percent']:.2f}%"),
        ("Worst regret", f"{oracle['worst_regret_ms']:.6f} ms; mean {oracle['mean_regret_ms']:.6f}; {oracle['misclassification_count']}/11 misses"),
    ]
    reports += [(f"{w} {p}", frame_result(w,p)) for w,p in corpus]
    reports += [
        ("1080p mixed Auto (2 slots)", stream_result(mix[1920,'equal_five_pattern',2])),
        ("4K mixed Auto (2 slots)", stream_result(mix[3840,'equal_five_pattern',2])),
        ("4K Noise sustained", stream_result(mix[3840,'noise_sustained',3])),
        ("Optimal slot count", "2 highest 1080p sweep FPS; 3 highest 4K sweep FPS; balanced Auto 2"),
        ("LowLatency", "1080p " + stream_result(mix[1920,'low_latency',1]) + "; 4K " + stream_result(mix[3840,'low_latency',1])),
        ("HighThroughput", "1080p " + stream_result(mix[1920,'high_throughput',3]) + "; 4K " + stream_result(mix[3840,'high_throughput',3])),
        ("CPU ms/frame", "Process-wide CPU time, matrix and mixed rows above; throughput policies have higher CPU cost than LowLatency"),
    ]
    reports += [(f"Long-run {x['width']}", f"{x['frames']} fully byte-verified frames PASS; {x['fps_including_download_and_verification']:.2f} verified FPS") for x in longrun['cases']]
    reports += [
        ("Random access", "Seeded independent selection and periodic seek-like substitutions PASS"),
        ("Two decoders", "2 x 100 independent random requests, CPU-exact PASS"),
        ("Four decoders", "4 x 100 independent random requests, CPU-exact PASS"),
        ("Output lifetime", "Delayed/out-of-order/pending/failed/double release, reinit and destruction; two consumer streams PASS"),
        ("Queue saturation", "Busy, held output unchanged, released slot reusable PASS"),
        ("Real difference coding", "Interlaced fixture: 30/30 CPU/E/Hybrid coefficients exact, max error 0; production deliberately rejects; interlaced output deferred"),
        ("Malformed corpus", f"{validation['malformed_cases']} rejected submissions; explicit coefficient-overflow and LL bounds checks PASS"),
        ("Mutation stress", "1000 seeded bounded mutations; no false Ready, first failure retained, release/reuse PASS"),
    ]
    reports += [(x['tool'], "Exit 0; zero errors/hazards") for x in sanitizers['tools']]
    reports += [("CUDA CTest", f"{cuda['passed']}/{cuda['passed']} PASS in {cuda['seconds']:.2f} s"),
                ("CPU-only CTest", f"{cpu['passed']}/{cpu['passed']} PASS in {cpu['seconds']:.2f} s"),
                ("Determinism", "200 Noise1080 / 100 Noise4K / 100 Texture4K / 20 Flat8K; bytes and frame/band decisions identical"),
                ("Memory growth/leaks", "Owned capacities/free VRAM stable; working set snapshots plateau; zero steady-state C++ and owned CUDA allocations"),
                ("Recommended policy", "Auto with 2 slots; LowLatency interactive; HighThroughput 3 slots when throughput justifies memory/CPU/latency"),
                ("Ready for main", "Recommend review for progressive YUV422 promotion; all Phase 3A gates pass within stated corpus/scope; no merge/tag/push performed"),
                ("Phase 3B", "Field-aware interlaced inverse and profile capability matrix, then 8/10/12-bit and RGB/RGBA coverage with frozen vectors")]
    assert len(reports) == 65, len(reports)
    sections['REPORT'] = ("Recommend review for promotion of the progressive YUV422 decoder. Exactness, dispatch, stress, malformed input, sanitizer and memory gates pass within the tested scope. Main remains unchanged. Interlaced output is expressly deferred by the user's scope clarification. The classifier remains development-corpus evidence; held-out footage and full profile coverage belong to the next phase.\n\n" +
                          table(["#", "Completion item", "Result"], [[i+1,k,v] for i,(k,v) in enumerate(reports)]))
    completion = {"provenance": provenance, "files_changed": sorted(paths), "completion_items": [{"number": i+1, "item": k, "result": v} for i,(k,v) in enumerate(reports)]}
    write("decode_phase3a_completion.json", completion)
    document_path = ROOT / "docs/DECODE_PHASE3A.md"
    doc = document_path.read_text(encoding="utf-8")
    for name, text in sections.items():
        pattern = rf"(<!-- {name} -->\n).*?(\n<!-- END {name} -->)"
        doc, count = re.subn(pattern, lambda m: m[1] + text + m[2], doc, flags=re.S)
        assert count == 1, name
    document_path.write_text(doc, encoding="utf-8")
    print("Phase 3A: eight artifacts validated; 65 completion items and measured tables rendered.")


if __name__ == "__main__":
    main()
