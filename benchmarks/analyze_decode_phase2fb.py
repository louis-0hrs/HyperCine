# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Derive research artifacts from the three idle rebench runs."""
import json,statistics,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/"benchmarks/results"
RAW=["decode_phase2fb_research.json","decode_phase2fb_idle_repeat2.json","decode_phase2fb_idle_repeat3.json"]
def save(name,data):
 (OUT/name).write_text(json.dumps({"license":"MIT OR Apache-2.0",**data},indent=2)+"\n")
def key(b): return b["width"],b["height"],b["pattern"],b["band_index"]
def main():
 runs=[json.loads((OUT/n).read_text()) for n in RAW]
 indices=[{key(b):b for b in r["bands"]} for r in runs]
 summaries=[]
 for fixture in runs[0]["bands"]:
  copies=[r[key(fixture)] for r in indices]
  sweeps=[]
  for chunk in [16,32,64,128,256,512,1024]:
   rows=[next(s for s in b["chunk_sweep"] if s["chunk_bytes"]==chunk) for b in copies]
   fields=["average_total_ms","average_cpu_resolution_ms","average_gpu_preprocessing_and_summary_d2h_ms","average_replay_ms","average_checkpoint_h2d_ms","average_replay_ready_ms"]
   sweeps.append({"chunk_bytes":chunk,"scratch_bytes":rows[0]["scratch_bytes"],
    "median_of_run_means":{f:statistics.median(s[f] for s in rows) for f in fields},
    "run_mean_ms":[s["average_total_ms"] for s in rows],
    "max_trial_ms":max(s["maximum_total_ms"] for s in rows),
    "correction_nibbles":rows[0]["correction_nibbles"],
    "max_correction_nibbles":rows[0]["max_correction_nibbles"],
    "full_chunk_corrections":rows[0]["full_chunk_corrections"]})
  summaries.append({"width":fixture["width"],"height":fixture["height"],"pattern":fixture["pattern"],"band_index":fixture["band_index"],
   "host_index_ms":[b["host_index_ms"] for b in copies],
   "phase2e_replay_ms":[b["phase2e_gpu_replay_ms"] for b in copies],
   "phase2fa_index_ms":[b["phase2fa_gpu_index_ms"] for b in copies],"sweep":sweeps})
 critical=next(b for b in summaries if b["width"]==3840 and b["pattern"]=="noise" and b["band_index"]==6)
 primary=next(s for s in critical["sweep"] if s["chunk_bytes"]==256)
 best=min(critical["sweep"],key=lambda s:s["median_of_run_means"]["average_total_ms"])
 all_state=json.loads((OUT/"decode_phase2fb_all_state_critical.json").read_text())
 a=all_state["bands"][0]
 f=next(b for b in runs[0]["bands"] if key(b)==key(critical))
 hierarchy=[]
 for depth in [2,3,4]:
  n=(f["compressed_bytes"]+1023)//1024
  levels=[n]
  for _ in range(depth-1):
   n=(n+15)//16
   levels.append(n)
  hierarchy.append({"levels":depth,"fanout":16,"map_counts":levels,
   "summary_map_bytes":sum(levels)*518*8,"status":"storage model; higher-level composition not implemented"})
 matrix=[
  {"candidate":"A hierarchical/all-state","exact":"sampled summaries only; no complete coefficient decode",
   "gpu_preprocessing_ms":statistics.median(a["gpu_summary_ms"]),"total_ms":None,"scratch_bytes":a["scratch_bytes"],
   "continue":False,"reason":"~13.8 ms and ~19.5 MiB before prefix/replay, versus ~2.6 ms complete B."},
  {"candidate":"B coarse-to-fine exact correction","exact":True,
   "total_ms":primary["median_of_run_means"]["average_total_ms"],"scratch_bytes":primary["scratch_bytes"],
   "continue":True,"reason":"Dense critical gate passed; texture remains workload dependent."},
  {"candidate":"C selective state constraints","exact":"structural over-approximation analysis; no decoder",
   "total_ms":None,"scratch_bytes":None,"continue":False,
   "reason":"518 possible initial states; local convergence retains distinct advances and emitted prefixes. No complete low-work solver derived."},
  {"candidate":"D recursive midpoint","exact":"not implemented","total_ms":None,"scratch_bytes":None,
   "continue":False,"reason":"No midpoint state/position oracle established without prefix work; not an impossibility proof."},
  {"candidate":"E compact dictionaries","exact":"structural analysis only","total_ms":None,"scratch_bytes":None,
   "continue":False,"reason":"All global output-aware classes remain distinct; local compression does not eliminate construction work."},
  {"candidate":"F CPU/GPU hybrid","exact":True,"total_ms":primary["median_of_run_means"]["average_total_ms"],
   "scratch_bytes":primary["scratch_bytes"],"continue":True,"reason":"Same implementation as B, not a separate additional winner."}]
 save("decode_phase2fb_candidate_matrix.json",{"outcome":"A","selected":"B/F isolated prototype","matrix":matrix,
  "hierarchy_storage_models":hierarchy,"best_critical_chunk_bytes_by_median":best["chunk_bytes"],
  "production_integrated":False,"phase2fc_warranted":"Separate experimental integration/evaluation; keep D/E defaults."})
 save("decode_phase2fb_chunk_sweep.json",{"raw_inputs":RAW,
  "aggregation":"median of three run means, five timed repeats/run","bands":summaries})
 stats={k:v for k,v in runs[0].items() if k in ["fsm_graph","gpu"]}
 stats["bands"]=[{k:v for k,v in b.items() if k!="chunk_sweep"} for b in runs[0]["bands"]]
 save("decode_phase2fb_fsm_stats.json",stats)
 files=["benchmarks/prototype_decode_phase2fb.cpp","benchmarks/decode_phase2fb_kernels.cu",
  "benchmarks/decode_phase2fb_gpu.h","tests/decode_phase2fb_support.h","tests/test_decode_phase2fb.cpp",
  "benchmarks/analyze_decode_phase2fb.py"]
 save("decode_phase2fb_provenance.json",{"base_commit":"773669cbbcccda8aedd5c5b968f7649df0fa5de3",
  "upstream_tree":"837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8",
  "raw_inputs":RAW,"source_sha256":{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in files},
  "note":"B kernels/timing unchanged after idle rebench; later change is bounded A verification/CLI only."})
 stdout=(ROOT/"build/phase2fb_codec_stdout.txt").read_text()
 current=json.loads(stdout[stdout.index("{"):stdout.rindex("}")+1])
 save("reference_windows_msvc_phase2fb.json",current)
 baseline=json.loads((OUT/"reference_windows_msvc.json").read_text())
 comparisons=[]
 for old,new in zip(baseline["benchmarks"],current["benchmarks"]):
  comparisons.append({"name":new["name"],"baseline":old,"current":new,
   "decode_fps_change_percent":100*(new["decode"]["fps"]/old["decode"]["fps"]-1),
   "encode_fps_change_percent":100*(new["encode"]["fps"]/old["encode"]["fps"]-1),
   "peak_memory_change_bytes":new["peak_memory_bytes"]-old["peak_memory_bytes"]})
 sanitizer=json.loads((OUT/"decode_phase2fb_sanitizers.json").read_text())
 for entry in sanitizer["runs"]:
  log=(OUT/entry["log"]).read_text()
  entry["zero_error_summary"]=("ERROR SUMMARY: 0 errors" in log or "RACECHECK SUMMARY: 0 hazards" in log)
  assert entry["exit_code"]==0 and entry["zero_error_summary"],entry
 save("decode_phase2fb_validation.json",{"full_cuda_ctest":{"passed":28,"failed":0,"seconds":954.83,"log":"decode_phase2fb_ctest.txt"},
  "cpu_only_ctest":{"passed":5,"failed":0,"seconds":6.82,"log":"decode_phase2fb_cpu_ctest.txt"},
  "focused_cases":39,"corpus_bands":13,"max_coefficient_error":0,"sanitizers":sanitizer["runs"],
  "codec_comparisons":comparisons,
  "interpretation":"Production codec source is unchanged. The 8K decode FPS difference is -1.15%, smaller than measured latency spread; no optimization or universal full-frame speedup is claimed.",
  "timing_scope":"Resident isolated band; excludes allocation, initial payload/table H2D, final diagnostic coefficient D2H. Includes summary D2H, CPU correction, checkpoint H2D and GPU error validation."})
 print("Critical B/F",primary["median_of_run_means"]["average_total_ms"],"ms;",primary["scratch_bytes"],"bytes; best chunk",best["chunk_bytes"])
if __name__=="__main__": main()
