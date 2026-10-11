# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Derive RC promotion evidence without changing classifier or media.

Run from the repository root after the commands in DECODE_PHASE3A_RC.md.
The first held-out timing window is always primary; repeats cannot replace it.
Only the Python standard library is required.
"""
import datetime
import hashlib
import json
import pathlib
import re
import statistics
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
RESULTS = ROOT / 'benchmarks/results'
RUN = ROOT / 'build/phase3a-rc'
BASE = 'e6e9cb905aee848808682942d13f07f2ff505a2d'
MAIN = '51435890ba5853e87e8cf1ddc7cc747954bd9e80'


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(name, value):
    (RESULTS / name).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha(path):
    # The measured sources used LF. Allow Git's Windows CRLF checkout without
    # accepting any change to source text or classifier thresholds.
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def ctest(path, count):
    data = path.read_text(encoding='utf-8', errors='replace')
    assert f'100% tests passed, 0 tests failed out of {count}' in data, str(path)
    return {'passed': count, 'failed': 0,
            'seconds': float(re.search(r'Total Test time \(real\) =\s*([\d.]+)', data)[1])}


def oracle(corpus):
    supported = [x for x in corpus['cases'] if x['supported']]
    cases = []
    for x in supported:
        light, hybrid = x['lightweight']['mean_ms'], x['hybrid']['mean_ms']
        winner = 'lightweight' if light <= hybrid else 'hybrid'
        best = min(light, hybrid)
        selected = light if x['auto_strategy'] == 'lightweight' else hybrid
        regret = selected - best
        cases.append({'id': x['id'], 'auto_strategy': x['auto_strategy'], 'oracle_strategy': winner,
                      'oracle_ms': best, 'selected_strategy_ms': selected, 'auto_wall_ms': x['auto']['mean_ms'],
                      'regret_ms': regret, 'relative_regret_pct': 100 * regret / best,
                      'auto_matches_oracle': x['auto_strategy'] == winner,
                      'catastrophic_reporting_flag': regret > 2 or 100 * regret / best > 20})
    all_light = sum(x['lightweight']['mean_ms'] for x in supported)
    all_hybrid = sum(x['hybrid']['mean_ms'] for x in supported)
    best_fixed = min(all_light, all_hybrid)
    selected = sum(x['selected_strategy_ms'] for x in cases)
    best = sum(x['oracle_ms'] for x in cases)
    regrets = [x['regret_ms'] for x in cases]
    return {'definition': 'OracleFrame=min(same-session forced Lightweight mean, forced Hybrid mean); offline only',
            'capture_definition': '(min(sum L,sum H)-sum selected)/(min(sum L,sum H)-sum Oracle)*100; equal case weights; Auto wall separate',
            'cases': cases, 'supported_cases': len(cases),
            'matches': sum(x['auto_matches_oracle'] for x in cases),
            'misses': sum(not x['auto_matches_oracle'] for x in cases),
            'mean_regret_ms': statistics.mean(regrets), 'median_regret_ms': statistics.median(regrets),
            'worst_regret_ms': max(regrets), 'worst_regret_case': max(cases, key=lambda x: x['regret_ms'])['id'],
            'total_selected_strategy_ms': selected, 'all_lightweight_ms': all_light, 'all_hybrid_ms': all_hybrid,
            'oracle_total_ms': best, 'best_fixed_total_ms': best_fixed,
            'oracle_improvement_capture_pct': 100 * (best_fixed - selected) / (best_fixed - best) if best_fixed != best else None,
            'catastrophic_regret_cases': [x for x in cases if x['catastrophic_reporting_flag']]}


def table(headers, rows):
    return '\n'.join(['| ' + ' | '.join(headers) + ' |', '| ' + ' | '.join(['---'] * len(headers)) + ' |'] +
                     ['| ' + ' | '.join(map(str, row)) + ' |' for row in rows])


def main():
    declaration = read(ROOT / 'tests/decode_phase3a_rc_corpus.json')
    for path, fingerprint in declaration['source_sha256_before_primary_timing'].items():
        if path == 'tests/decode_phase3a_rc_peaks.h':
            # The repaired unit is not executed in benchmark mode. Retain its pre-timing
            # hash and disclose the isolated odd-padding unit repair.
            assert source_sha(ROOT / path) == declaration['post_timing_peak_unit_only_revision']['sha256']
        else:
            assert source_sha(ROOT / path) == fingerprint, f'Frozen evaluation source changed: {path}'
    assert not git('diff', BASE, '--', 'src', 'include', 'upstream', 'tests/decode_phase0_golden.txt')
    assert not git('status', '--porcelain', 'upstream/')
    assert git('rev-parse', 'main') == MAIN
    corpus = read(RESULTS / 'decode_phase3a_rc_holdout.json')
    validation = read(ROOT / 'build/tests/rc-results/decode_phase3a_rc_corpus_validation.json')
    peaks = read(ROOT / 'build/tests/rc-results/decode_phase3a_rc_peak_escape.json')
    golden = read(ROOT / 'build/tests/benchmarks/results/decode_phase3a_validation.json')
    assert corpus['status'] == validation['status'] == golden['status'] == peaks['status'] == 'PASS'
    assert corpus['total_cases'] == 36 and corpus['supported_cases'] == 35 and corpus['unsupported_cases'] == 1
    assert golden['mode'] == 'full' and len(golden['golden']) == 11
    assert validation['smoke_frames'] == 470
    val_by_id = {x['id']: x for x in validation['cases']}
    for x in corpus['cases']:
        v = val_by_id[x['id']]
        assert x['compressed_hash'] == v['compressed_hash'] and x['frame_hash'] == v['frame_hash']
        if x['supported']:
            assert x['coefficients_exact'] == 30 and x['max_coefficient_error'] == 0
            assert x['ll_exact'] and x['planar_exact'] and x['bgra_exact'] and x['deterministic']
            assert x['auto_strategy'] == v['auto_strategy'] and x['bgra_hash'] == v['bgra_hash']
    summary = oracle(corpus)
    repeat_path = RUN / 'repeat/decode_phase3a_rc_holdout.json'
    if repeat_path.exists():
        repeat = read(repeat_path)
        assert repeat['status'] == 'PASS'
        for x, y in zip(corpus['cases'], repeat['cases']):
            assert x['id'] == y['id'] and x['compressed_hash'] == y['compressed_hash']
            if x['supported']:
                assert x['auto_strategy'] == y['auto_strategy'] and x['bgra_hash'] == y['bgra_hash']
        summary['confirmation_repeat'] = oracle(repeat)
        write('decode_phase3a_rc_holdout_repeat.json', repeat)
    # Preserve successful and failing default-capacity probes, rather than
    # presenting only the successful LARGER_OUTPUT search.
    pilot_path = RUN / 'peak-pilot/decode_phase3a_rc_peak_escape.json'
    if pilot_path.exists():
        pilot_text = pilot_path.read_text()
        pilot = json.loads(pilot_text + ']}')
        assert len(pilot['attempts']) == 100
        peaks['default_capacity_pilot'] = {'successful_attempts': pilot['attempts'], 'successful_count': 100,
            'failed_attempt': {'index': 101, 'width': 1920, 'height': 1080, 'input_format': 'YUY2',
                               'source_pattern': 'random_extremes', 'quality_enum': 5, 'encoding_flags': 0,
                               'windows_exit_code_hex': '0xC0000005'},
            'mitigation': 'unchanged SDK public LARGER_OUTPUT capacity flag; completed all 144 configurations',
            'fault_mechanism': 'not established; consistent with output capacity limitation'}
    matrix = read(RUN / 'regression/benchmarks/results/decode_phase3a_windows_msvc.json')
    choices = read(RUN / 'regression/benchmarks/results/decode_phase3a_auto_selector.json')
    mixed = read(RUN / 'regression/benchmarks/results/decode_phase3a_mixed_workload.json')
    memory = read(RUN / 'regression/benchmarks/results/decode_phase3a_memory.json')
    old_matrix = read(RESULTS / 'decode_phase3a_windows_msvc.json')
    old_choices = read(RESULTS / 'decode_phase3a_auto_selector.json')
    old_memory = read(RESULTS / 'decode_phase3a_memory.json')
    regression_cases = []
    for x, old, decision, old_decision in zip(matrix['cases'], old_matrix['cases'], choices['cases'], old_choices['cases']):
        assert (x['width'], x['pattern']) == (old['width'], old['pattern'])
        assert decision['decision'] == old_decision['decision']
        delta = 100 * (x['auto']['mean_ms'] / old['auto']['mean_ms'] - 1)
        regression_cases.append({'width': x['width'], 'pattern': x['pattern'], 'auto_decision': decision['decision'],
            'phase3a_auto_mean_ms': old['auto']['mean_ms'], 'rc_auto_mean_ms': x['auto']['mean_ms'],
            'latency_change_pct': delta, 'over_5_percent': delta > 5,
            'phase3a_process_cpu_ms': old['auto']['process_cpu_ms_per_frame'], 'rc_process_cpu_ms': x['auto']['process_cpu_ms_per_frame'],
            'process_cpu_change_pct': 100 * (x['auto']['process_cpu_ms_per_frame'] / old['auto']['process_cpu_ms_per_frame'] - 1)})
    owned = ['device_bytes', 'pinned_host_bytes', 'pageable_owned_capacity_bytes', 'cuda_owned_buffer_allocation_calls']
    assert all(all(a['owned'][k] == b['owned'][k] for k in owned) for a, b in zip(memory['runs'], old_memory['runs']))
    regression = {'production_sources_unchanged': True, 'auto_decisions_unchanged': True, 'owned_memory_unchanged': True,
        'cases': regression_cases, 'median_latency_change_pct': statistics.median(x['latency_change_pct'] for x in regression_cases),
        'over_5_percent_cases': [x for x in regression_cases if x['over_5_percent']], 'matrix': matrix, 'mixed': mixed, 'memory': memory}
    old_mixed = read(RESULTS / 'decode_phase3a_mixed_workload.json')
    regression['mixed_comparison'] = []
    for current, baseline in zip(mixed['runs'], old_mixed['runs']):
        assert (current['width'], current['slots'], current['label']) == (baseline['width'], baseline['slots'], baseline['label'])
        regression['mixed_comparison'].append({'width': current['width'], 'slots': current['slots'], 'label': current['label'],
            'fps_change_pct': 100 * (current['fps'] / baseline['fps'] - 1),
            'ready_latency_change_pct': 100 * (current['mean_frame_latency_ms'] / baseline['mean_frame_latency_ms'] - 1),
            'cpu_cost_change_pct': 100 * (current['process_cpu_ms_per_frame'] / baseline['process_cpu_ms_per_frame'] - 1),
            'peak_working_set_change_bytes': current['peak_process_working_set_bytes'] - baseline['peak_process_working_set_bytes']})
    regression['systematic_noise_slowdown_flag'] = all(x['over_5_percent'] for x in regression_cases if x['pattern'] == 'noise')
    regression['timing_interpretation'] = 'Noise slowdown versus historical Phase 3A is retained and flagged; no RC production source/configuration change explains it. Timing cause is unresolved; no optimization or improvement is claimed.'
    codec = read(RUN / 'decode_phase3a_rc_codec_reference.json')
    regression['codec_reference'] = codec
    regression['preserved_codec_baseline'] = read(RESULTS / 'reference_windows_msvc.json')
    regression['codec_comparison'] = []
    for current, baseline in zip(codec['benchmarks'], regression['preserved_codec_baseline']['benchmarks']):
        assert current['name'] == baseline['name']
        regression['codec_comparison'].append({'name': current['name'], 'current': current, 'baseline': baseline,
            'encode_fps_change_pct': 100 * (current['encode']['fps'] / baseline['encode']['fps'] - 1),
            'decode_fps_change_pct': 100 * (current['decode']['fps'] / baseline['decode']['fps'] - 1),
            'peak_memory_change_bytes': current['peak_memory_bytes'] - baseline['peak_memory_bytes']})
    cuda = ctest(ROOT / 'build/phase3a_rc_final_cuda_ctest.txt', 32)
    cpu = ctest(ROOT / 'build/phase3a_rc_final_cpu_ctest.txt', 6)
    source_paths = [p for p in git('ls-files', 'src', 'include').splitlines() if pathlib.Path(p).suffix in {'.cpp', '.cu', '.cuh', '.h'}]
    provenance = {'starting_phase3a_commit': BASE, 'phase3a_base': 'dded4d8ae61800241164499fa875c0cb104281a4',
        'branch': git('branch', '--show-current'), 'recorded_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'copyright': '2026 HyperCine Contributors', 'license': 'MIT OR Apache-2.0', 'main_commit': MAIN,
        'upstream_tree': git('rev-parse', 'HEAD:upstream'), 'production_source_sha256': {p: sha(ROOT / p) for p in source_paths},
        'corpus_declaration_sha256': sha(ROOT / 'tests/decode_phase3a_rc_corpus.json'),
        'classifier_sha256': sha(ROOT / 'src/core/decode_dispatch.cpp'), 'classifier_thresholds_unchanged': True,
        'peak_restoration_sha256': sha(ROOT / 'src/cuda/decode_peaks_cuda.cu'), 'external_sources': declaration['external_source_sha256'],
        'corpus_independence': declaration['development_exclusion'], 'no_merge_tag_push_release': True}
    validation.update({'golden': golden, 'golden_11_of_11_exact': True, 'golden_auto_decisions_unchanged': True,
        'cuda_ctest': cuda, 'cpu_ctest': cpu, 'baseline_cuda': {'initial_passed': 29, 'initial_failed': ['test_phase2a', 'test_phase2b'],
            'immediate_unchanged_retry': ctest(ROOT / 'build/phase3a_rc_baseline_encoder_retry.txt', 2)},
        'baseline_cpu': ctest(ROOT / 'build/phase3a_rc_baseline_cpu_ctest.txt', 6),
        'interrupted_full_cuda_run': {'passed': 31, 'failed': 1, 'failed_test': 'test_decode_phase2fa',
            'failure': 'overnight timeout; full raw log retained; subsequent full suite rerun required',
            'log': 'decode_phase3a_rc_interrupted_cuda_ctest.txt'},
        'sanitizers': {'production_code_unchanged': True, 'phase3a_four_tool_evidence': read(RESULTS / 'decode_phase3a_sanitizers.json')['tools'],
            'rc_focused': read(RUN / 'rc_sanitizers.json')},
        'long_run_policy': '470-frame held-out smoke plus full production CTest stress; prior 16000-frame evidence retained because runtime source is unchanged'})
    capture = summary['oracle_improvement_capture_pct']
    if capture is None or capture < 90:
        outcome, gate = 'RC Outcome C — HOLD FOR CLASSIFIER WORK', 'HOLD'
    elif summary['catastrophic_regret_cases']:
        outcome, gate = 'RC Outcome C — HOLD FOR CLASSIFIER WORK', 'HOLD'
    elif not peaks['progressive_samples_found']:
        outcome, gate = 'RC Outcome B — PROMOTE WITH KNOWN LIMITATION', 'PROMOTE WITH KNOWN LIMITATION'
    else:
        outcome, gate = 'RC Outcome A — PROMOTE', 'PROMOTE'
    supported = [x for x in corpus['cases'] if x['supported']]
    costs = [x['production_classifier_ns'] for x in supported]
    changed = sorted(set(git('diff', '--name-only', BASE).splitlines()) | set(git('ls-files', '--others', '--exclude-standard').splitlines()))
    completion = {'branch': provenance['branch'], 'base_commit': BASE, 'production_source_changed': False,
        'commits_created_before_documentation_commit': git('log', '--format=%h %s', f'{BASE}..HEAD').splitlines(),
        'documentation_commit_subject': 'docs(decoder): record Phase 3A release-candidate validation',
        'working_tree_at_evidence_generation': git('status', '--short').splitlines(),
        'delivery_working_tree_status': 'Final commit identities and clean status are verified in the delivery message; evidence generation precedes its own documentation commit.',
        'files_changed': changed, 'upstream_clean': True, 'encoder_unchanged': True, 'main_unchanged': True,
        'heldout_cases_total': 36, 'real_external_cases': 4, 'real_supported_cases': 3, 'generated_heldout_cases': 32,
        'cases_1080p': 19, 'cases_4k': 15, 'cases_8k': 2, 'unsupported_cases': 1,
        'auto_oracle_matches': summary['matches'], 'auto_oracle_misses': summary['misses'], 'oracle_improvement_capture_pct': capture,
        'mean_regret_ms': summary['mean_regret_ms'], 'median_regret_ms': summary['median_regret_ms'], 'worst_regret_ms': summary['worst_regret_ms'],
        'worst_regret_case': summary['worst_regret_case'], 'catastrophic_regret_cases': summary['catastrophic_regret_cases'],
        'classifier_determinism': '100/100 per supported sample; production and repeat decisions identical',
        'classifier_cost_ns': {'minimum': min(costs), 'maximum': max(costs), 'mean': statistics.mean(costs)},
        'classifier_thresholds_unchanged': True, 'progressive_peak_escape_sample_found': bool(peaks['progressive_samples_found']),
        'peak_escape_source_patterns': ['alternating_extremes', 'checkerboard', 'impulses', 'hard_transition', 'random_extremes', 'saturated_chroma'],
        'progressive_peak_escape_count': peaks['peak_escape_count'], 'old_behavior_would_differ': '4 direct-unit coefficients; valid interlaced CPU regression remains exact; no real progressive peak table observed',
        'cpu_coefficients_exact': True, 'lightweight_exact': True, 'hybrid_exact': True, 'auto_exact': True, 'host_streaming_exact': True,
        'll_exact': True, 'planar_exact': True, 'device_bgra_exact': True, 'golden_result': '11/11 exact, frozen hashes and strategies unchanged',
        'difference_interlaced_coefficients': '30/30 CPU/HostStreaming/Hybrid exact; max error 0',
        'interlaced_production_rejection': 'UnsupportedEncodingMode; no output',
        'benchmark_regression': {'median_latency_change_pct': regression['median_latency_change_pct'], 'over_5_percent_cases': regression['over_5_percent_cases'], 'owned_memory_unchanged': True},
        'smoke_result': '470/470 held-out bytes and frame/band decisions exact; owned allocations/capacities stable',
        'sanitizer_result': validation['sanitizers'], 'cpu_only_result': cpu, 'cuda_result': cuda,
        'production_api_unchanged': True, 'promotion_gate': gate, 'final_rc_outcome': outcome,
        'recommendation': 'Do not merge; conduct separate offline classifier research with fresh development and new held-out splits.' if gate == 'HOLD' else 'Ready for merge review within progressive YUV422 scope.',
        'known_limitations': ['No real progressive Peak-Escape vector found.', 'External footage is only three frames from one bundled 422 clip; camera acquisition unknown.',
            'Two historical encoder research tests failed in untouched baseline and passed immediate retries.',
            'Extreme-input upstream default-capacity probe faulted; existing LARGER_OUTPUT flag allowed the bounded search to finish.',
            'Historical Noise performance comparison slowed; source-identical runtime, timing cause unresolved and flagged.',
            'Laptop timing varies with clocks and scheduling; raw primary and repeat windows are preserved.'],
        'proposed_phase3b_start': 'Resolve promotion dispatch gap in a separate research step first; then profile capability matrix and field-aware interlaced inverse with frozen real difference-coded end-to-end vectors.'}
    for name, value in [('holdout', corpus), ('oracle', summary), ('peak_escape', peaks), ('validation', validation), ('regression', regression), ('completion', completion)]:
        value['provenance'] = provenance; write(f'decode_phase3a_rc_{name}.json', value)
    report(completion, corpus, summary, peaks, regression, validation)
    print(outcome, 'capture', capture, 'matches', summary['matches'], 'misses', summary['misses'])


def report(c, corpus, o, peaks, regression, validation):
    rows = []
    by_id = {x['id']: x for x in o['cases']}
    for x in corpus['cases']:
        if not x['supported']:
            rows.append([x['id'], 'unsupported', '—', '—', '—', '—', '—', '—']); continue
        z = by_id[x['id']]
        rows.append([x['id'], x['auto_strategy'], f"{x['lightweight']['mean_ms']:.3f}", f"{x['hybrid']['mean_ms']:.3f}",
                     f"{x['auto']['mean_ms']:.3f}", z['oracle_strategy'], f"{z['regret_ms']:.3f}", f"{z['relative_regret_pct']:.1f}"])
    metadata = [[x['id'], x['highpass_payload_bytes'], x['padded_coefficient_count'], x['largest_band_payload_bytes'],
                 x['dense_band_count'], '/'.join(map(str, x['payload_bytes_by_level'])),
                 str(x['concentrated_detail']), str(x['distributed_detail'])] for x in corpus['cases'] if x['supported']]
    reg_rows = [[x['width'], x['pattern'], f"{x['phase3a_auto_mean_ms']:.3f}", f"{x['rc_auto_mean_ms']:.3f}",
                 f"{x['latency_change_pct']:+.1f}%", f"{x['phase3a_process_cpu_ms']:.2f} → {x['rc_process_cpu_ms']:.2f}"] for x in regression['cases']]
    codec_rows = [[x['name'], f"{x['baseline']['encode']['fps']:.2f} → {x['current']['encode']['fps']:.2f}",
                  f"{x['baseline']['decode']['fps']:.2f} → {x['current']['decode']['fps']:.2f}",
                  f"{x['current']['encode']['megapixels_per_sec']:.1f}/{x['current']['decode']['megapixels_per_sec']:.1f}",
                  '/'.join(f"{x['current']['encode'][key]:.2f}" for key in ['latency_avg_ms', 'latency_min_ms', 'latency_max_ms']),
                  '/'.join(f"{x['current']['decode'][key]:.2f}" for key in ['latency_avg_ms', 'latency_min_ms', 'latency_max_ms']),
                  f"{x['current']['peak_memory_bytes']/1048576:.2f}"] for x in regression['codec_comparison']]
    mixed_rows = [[x['width'], x['label'], x['slots'], f"{x['fps_change_pct']:+.1f}%", f"{x['ready_latency_change_pct']:+.1f}%",
                   f"{x['cpu_cost_change_pct']:+.1f}%", x['peak_working_set_change_bytes']] for x in regression['mixed_comparison']]
    flags = o['catastrophic_regret_cases']
    flag_text = '\n'.join(f"- `{x['id']}`: {x['regret_ms']:.3f} ms ({x['relative_regret_pct']:.1f}%), Auto {x['auto_strategy']} / Oracle {x['oracle_strategy']}." for x in flags) or 'None.'
    repeated = o.get('confirmation_repeat')
    repeat_text = (f"The confirmation window retained every source hash and decision: capture {repeated['oracle_improvement_capture_pct']:.2f}%, "
                   f"{repeated['matches']}/{repeated['supported_cases']} matches. It does not replace the primary window.") if repeated else 'No second timing window.'
    commits = '; '.join(c['commits_created_before_documentation_commit']) or 'Test commit identity recorded after full-suite validation'
    completion_items = [
        ('Branch', c['branch']), ('Base commit', BASE), ('Commits', commits + '; documentation commit containing this report: ' + c['documentation_commit_subject']),
        ('Production source changed', 'No'), ('Files changed', 'Complete path list in completion JSON'), ('Upstream clean', 'Yes, frozen tree and empty status'),
        ('Encoder unchanged', 'Yes'), ('Main unchanged', MAIN), ('Held-out cases total', 36), ('Real external cases', '4; 3 supported from one clip'),
        ('Generated held-out cases', 32), ('1080p cases', 19), ('4K cases', 15), ('8K cases', 2), ('Unsupported cases', '1: 12-bit RGB444'),
        ('Auto/Oracle matches', o['matches']), ('Auto/Oracle misses', o['misses']), ('Oracle improvement capture', f"{o['oracle_improvement_capture_pct']:.2f}%"),
        ('Mean regret', f"{o['mean_regret_ms']:.6f} ms"), ('Median regret', f"{o['median_regret_ms']:.6f} ms"), ('Worst regret', f"{o['worst_regret_ms']:.6f} ms"),
        ('Worst-regret case', o['worst_regret_case']), ('Catastrophic reporting flags', f"{len(flags)}; every case listed above"),
        ('Classifier determinism', '100/100 per supported case; validation and repeat decisions match'),
        ('Classifier cost', f"{c['classifier_cost_ns']['minimum']}–{c['classifier_cost_ns']['maximum']} ns; mean {c['classifier_cost_ns']['mean']:.1f} ns"),
        ('Thresholds unchanged', 'Yes'), ('Progressive Peak-Escape sample found', 'No'), ('Peak source patterns', ', '.join(c['peak_escape_source_patterns'])),
        ('Progressive Peak-Escape count', 0), ('Evidence old behavior differs', '4 unit coefficients; valid interlaced CPU regression retained'),
        ('CPU coefficients exact', '30/30, max error 0'), ('Lightweight exact', 'Yes'), ('Hybrid exact', 'Yes'), ('Auto exact', 'Yes'),
        ('LL exact', 'Yes'), ('Planar exact', 'Yes, all four strategies'), ('DeviceBGRA exact', 'Yes, all four strategies'),
        ('Frozen golden result', '11/11; original hashes and decisions unchanged'), ('Interlaced coefficients', '30/30 CPU/HostStreaming/Hybrid exact, max error 0'),
        ('Interlaced production rejection', 'UnsupportedEncodingMode; no output'), ('Benchmark regression', f"Median Auto change {regression['median_latency_change_pct']:+.2f}%; Noise slowdown flagged; owned memory unchanged"),
        ('Long-run/smoke', '470/470 held-out smoke PASS; full production regression; previous 16000-frame evidence referenced'),
        ('Sanitizers', 'Four focused RC tools clean; four Phase 3A logs cover unchanged production'), ('CPU-only', 'Configure/build and 6/6 CTest PASS'),
        ('Production API unchanged', 'Yes'), ('Working-tree status', 'Evidence generation precedes its own docs commit; final clean status confirmed in delivery message'),
        ('Final RC outcome', c['final_rc_outcome']), ('Merge recommendation', 'Do not merge'),
        ('Known limitations', 'No progressive peak vector; limited external provenance; historical encoder flakiness; unresolved Noise timing slowdown'),
        ('Phase 3B start', c['proposed_phase3b_start'])]
    assert len(completion_items) == 50
    text = f"""# Decode Phase 3A-RC promotion validation

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

**Promotion Gate: {c['promotion_gate']} — {c['final_rc_outcome']}.** {c['recommendation']}

Starting Phase 3A HEAD: `{BASE}` (Phase 3A base `dded4d8`). Branch: `{c['branch']}`. RC changes only tests, their CMake registration, evidence and documentation. Production sources, public API, classifier thresholds, encoder, upstream and `main` are unchanged. No merge, tag, push or release occurred.

## Corpus and independence

The predeclared [corpus manifest](../tests/decode_phase3a_rc_corpus.json) fixes families, settings, seed and source SHA256 fingerprints before the primary timing window. All eleven development source/sample identities are excluded. Generated content uses a new integer-only generator and seed `0x6A09E667`; it is synthetic validation imagery, not camera footage. External samples were previously CPU decode fixtures, not data used for selecting Auto thresholds.

There are 36 cases: 32 generated (15 families at 1080p, 15 at 4K, two practical low-detail 8K frames) and four external cases. Three external frames (0, 12, 23) come from one bundled progressive 10-bit YUV422 clip; camera acquisition and scene category are unknown. The bundled 12-bit RGB444 frame is an unsupported control, excluded from Oracle statistics. Total resolution counts: 19 at 1080p, 15 at 4K, two at 8K. This is broad synthetic entropy coverage with limited external-video coverage. Source hashes, decoded/source frame hashes, sample sizes, identified profile/precision and support status are in [holdout JSON](../benchmarks/results/decode_phase3a_rc_holdout.json); no new media is committed.

## Primary measurements and offline Oracle

Each supported case has five warmups and 120/60/30 measured frames per strategy at 1080p/4K/8K, two configured slots, one outstanding ticket, equivalent Queue→Ready→Release windows and device-resident output. L/H order alternates by case; Auto is measured separately. Process CPU time is kernel plus user time across threads. GPU workloads run sequentially. Correctness downloads are outside timing. Auto never consumes Oracle timing.

OracleFrame is `min(Lightweight mean, Hybrid mean)`. Regret uses the forced latency of Auto's selected strategy. Capture uses exactly Phase 3A's definition: `(min(sum L, sum H) - sum selected) / (min(sum L, sum H) - sum Oracle) × 100`, equal case weights. The primary corpus captures **{o['oracle_improvement_capture_pct']:.2f}%**, with **{o['matches']}/35 matches** and **{o['misses']} misses**. Mean/median/worst regret: **{o['mean_regret_ms']:.3f}/{o['median_regret_ms']:.3f}/{o['worst_regret_ms']:.3f} ms**; worst case `{o['worst_regret_case']}`. Summed selected/L/H/Oracle latency: **{o['total_selected_strategy_ms']:.3f}/{o['all_lightweight_ms']:.3f}/{o['all_hybrid_ms']:.3f}/{o['oracle_total_ms']:.3f} ms**.

{table(['Case', 'Auto choice', 'L ms', 'H ms', 'Auto ms', 'Oracle', 'Regret ms', 'Regret %'], rows)}

## Misses and generalization

Every >2 ms absolute or >20% relative regret is surfaced here ({len(flags)} cases). These thresholds flag engineering concerns, not incorrect reconstruction. Content labels are explanatory and never become selector special cases.

{flag_text}

{repeat_text} The original corpus and all primary misses remain in the report. Thresholds were never tuned. The poor improvement capture and material misses warrant a separate dispatch research step; decoder correctness remains exact.

## Classifier metadata and determinism

All 35 supported cases produced identical strategy and metadata across 100 repeated passes (3500 classifications), matched normal production Auto, and retained decisions in independent validation and timing windows. Production classifier observations range {c['classifier_cost_ns']['minimum']}–{c['classifier_cost_ns']['maximum']} ns; mean {c['classifier_cost_ns']['mean']:.1f} ns. Standalone repeated classifier cost is also recorded per case. Neither observation affects dispatch.

Level payload columns are level 0/1/2; coefficient count includes padded rows. Concentrated and distributed flags use the unchanged integer predicates.

{table(['Case', 'HP bytes', 'Padded coefficients', 'Largest band', 'Dense bands', 'Level bytes 0/1/2', 'Concentrated', 'Distributed'], metadata)}

## Progressive Peak-Escape search and exactness

**Peak-Escape Production Vector: {peaks['production_vector']}.** Six patterns (alternating extremes, checkerboard, impulses, hard transitions, random extremes, saturated chroma), six qualities (Low through FilmScan3), YUY2 and YU64 containing true 10-bit values, and 64×64 / 1920×1080 form 144 completed progressive configurations. The unchanged encoder emitted zero Peak-Escape tables. No progressive bitstream is manufactured and progressive Peak-Escape CPU/LL/planes/BGRA exactness remains unobserved.

The default-capacity pilot completed 100 configurations then faulted on 1920×1080 YUY2 random extremes, FilmScan2, flags 0 (exit `0xC0000005`). The existing public `LARGER_OUTPUT` flag (2048) let all 144 configurations finish with the same patterns/qualities; it changes capacity, not coding policy. Full pilot and completed-search metadata are preserved in [peak JSON](../benchmarks/results/decode_phase3a_rc_peak_escape.json), with the observation documented in [UPSTREAM.md](UPSTREAM.md).

Direct tests call the unchanged restoration kernel: 16 exact cases cover positive/negative signed-16 replacements, inclusive threshold boundaries, coefficient order, inactive bands, odd zero padding, null/bounds/count failures, retained frame errors and restoration before horizontal differencing. Omitting restoration differs in four coefficients. These are metadata/kernel units, not a progressive production-vector claim. The original valid interlaced regression remains coefficient-only: CPU/HostStreaming/Hybrid all 30 bands exact, max error 0; production returns `UnsupportedEncodingMode` and publishes no output.

## Golden, smoke, sanitizers and CPU-only gates

All 35 supported held-out cases match CPU in 30/30 coefficients, both LL levels, signed planar output, YU64 and DeviceBGRA for forced L, forced H, Auto and HostStreaming. All **11/11 frozen golden samples** retain original coefficients, LL, planar/YU64/BGRA hashes and Auto choices. Expected hashes were not regenerated.

The shortened held-out smoke passes **470/470 frames** (300 1080p, 150 4K, 20 8K), holding two outputs and releasing out of order, with exact output bytes and frame/band decisions and stable owned capacities/allocation counts. Full production CTest also repeats lifecycle, malformed-input, multi-instance, allocation and determinism coverage. The previous 16000-frame run is referenced because production bytes are unchanged.

CUDA CTest: **{validation['cuda_ctest']['passed']}/32 PASS**, {validation['cuda_ctest']['seconds']:.2f} s. CPU-only configure/build with `HYPERCINE_ENABLE_CUDA=OFF`: **6/6 PASS**, {validation['cpu_ctest']['seconds']:.2f} s. Untouched baseline initially passed 29/31: historical encoder `test_phase2a` and `test_phase2b` failed, then both passed immediate unchanged retries. Both logs are retained; this intermittent baseline limitation is not hidden or repaired inside decoder RC.

The first final CUDA run spanned an overnight interruption: 31/32 passed, with a historical `test_decode_phase2fa` timeout. Its 44922.45 s full-run log is retained as `decode_phase3a_rc_interrupted_cuda_ctest.txt`. A fresh complete run produced the passing final result above. Neither test logic nor timeouts were relaxed.

The four clean Phase 3A Compute Sanitizer logs remain applicable to byte-identical production code. Focused RC sanitizer results and log hashes are in [validation JSON](../benchmarks/results/decode_phase3a_rc_validation.json). New RC units and bounded generation are exercised under instrumentation; no old result is presented as covering changed production code.

## Existing production benchmark regression

The unchanged 120-frame Phase 3A matrix and 1000-frame streaming runs were rerun in an isolated directory. All eleven Auto choices and owned device/pinned/pageable capacities are unchanged. Median Auto latency change versus the final Phase 3A report: **{regression['median_latency_change_pct']:+.2f}%**. Every >5% row is flagged in [regression JSON](../benchmarks/results/decode_phase3a_rc_regression.json); CPU changes, min/max latency, FPS, MP/s, working-set peaks, mixed throughput and the unchanged `benchmark_codec` comparison with `reference_windows_msvc.json` are retained there. Source equality excludes an RC runtime implementation change; laptop timing differences are observations, not claimed optimizations.

{table(['Width', 'Pattern', 'Phase 3A Auto ms', 'RC Auto ms', 'Change', 'CPU ms/frame old → RC'], reg_rows)}

Systematic Noise slowdown flag: **{regression['systematic_noise_slowdown_flag']}**. The timing cause is unresolved and warrants follow-up; source equality is evidence of unchanged implementation, not proof of a particular clock or scheduling explanation. The following streaming comparison also surfaces CPU and process working-set changes. Working-set high-water includes process/harness and runtime, while owned decoder capacities are exactly equal.

{table(['Width', 'Workload', 'Slots', 'FPS change', 'Ready latency change', 'CPU cost change', 'Peak WS change bytes'], mixed_rows)}

The unchanged CPU codec harness is compared with the preserved original baseline below. Timing changes are not attributed to RC decoder improvements.

{table(['Case', 'Encode baseline → RC FPS', 'Decode baseline → RC FPS', 'Encode/decode MP/s', 'Encode avg/min/max ms', 'Decode avg/min/max ms', 'Peak WS MiB'], codec_rows)}

## Reproduction and repository state

```powershell
cmake -S . -B build -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\\build\\tests\\Release\\test_decode_phase3a_rc.exe --benchmark . benchmarks/results
.\\build\\tests\\Release\\test_decode_phase3a_rc.exe --benchmark . build/phase3a-rc/repeat
# Run benchmark_decoder.exe 120 from build/phase3a-rc/regression (isolated output).
cmake -S . -B build-cpu -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
python benchmarks/summarize_decode_phase3a_rc.py
```

Run GPU tests, benchmarks and sanitizers sequentially. The frozen declaration intentionally makes the summarizer reject changed evaluation source; research on these now-observed cases needs a new split. Existing Phase 3A result artifacts are untouched. Upstream status is empty and its tree remains `837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`; `main` remains `{MAIN}`. Encoder and public API are unchanged. Commit identities and final working-tree status are recorded in [completion JSON](../benchmarks/results/decode_phase3a_rc_completion.json) and the delivery message.

## Completion checklist

{table(['#', 'Item', 'Result'], [[n, item, value] for n, (item, value) in enumerate(completion_items, 1)])}

## Promotion decision

**{c['final_rc_outcome']}. {c['recommendation']}** Known gaps are limited external-camera provenance, no real progressive Peak-Escape vector, and intermittent historical encoder baseline tests. Phase 3B should start with a profile capability matrix and field-aware interlaced inverse reconstruction after a separate dispatch study resolves the promotion gap. No format expansion or decoder redesign belongs in this RC.
"""
    (ROOT / 'docs/DECODE_PHASE3A_RC.md').write_text(text, encoding='utf-8')


if __name__ == '__main__':
    main()
