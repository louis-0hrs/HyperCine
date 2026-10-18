# Matched-SHA 1080p cross-host protocol — #120 / Draft #138

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

## Result and scope

A pre-registered protocol decoded the **same immutable 1920x1080 BGRA
FilmScan3 packet** on the #120 Windows host in **45 independent decoder calls**
across nine fresh processes. Every call returned `CFHD_ERROR_OKAY`, valid
geometry and the **same pixel/RGB SHA-256**; PSNR versus the fixed source was
**44.465986143258924 dB** throughout. The modal hash is identical to the one
recorded by the #116 M4 host.

This is the Windows arm only. It does not qualify the codec, does not clear the
M4 divergence and does not replace the pending encoder-packet comparison. The
classifier remains `REFERENCE_EVIDENCE_INCOMPLETE`, encoder gate `UNRESOLVED`,
release blocked. The original Handoff-1 M4 incident packet is still unavailable;
this is the reproducible packet whose SHA both hosts independently confirmed.

## Pre-registered protocol

Constants and acceptance criteria were fixed in
`tests/reference_decode/cross_host_1080.py` before measurement.

| field | fixed value |
| --- | --- |
| packet SHA-256 | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` |
| source BGRA SHA-256 | `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1` |
| packet size / geometry | 900396 bytes, 1920x1080 |
| requested output | BGRA, `CFHD_DECODED_RESOLUTION_FULL`, `CFHD_DECODING_FLAGS_NONE` |
| active metadata | `TAG_PROCESS_PATH = PROCESSING_ALL_ON`, flags 65535 |
| decode calls | 3 fresh processes x 5 opens (primary), extended to 6 x 5 for power |
| child bound | 20 s per process, kills and reaps the child on timeout |
| decoder threading | original SDK defaults, no override |
| pass criteria | any timeout, decode error or invalid output => INVALID; any pixel hash difference => `REFERENCE_DECODER_NONDETERMINISTIC`; stable output without an encoder packet pair remains `UNRESOLVED` |

The 3x5 primary arm is the protocol previously recommended in #116 Handoff 4
section 7. At the M4-observed 14/96 divergence rate, 15 calls would miss
divergence about 9.5% of the time, so a pre-registered 30-call extension was
added to make a null result informative. Both arms use the same packet,
configuration and worker; all child processes are independent.

## Windows arm configuration

| dimension | value |
| --- | --- |
| host | native Windows x64, AMD Ryzen 9 7940HS, 8 cores / 16 logical processors |
| GPU / CUDA | not linked (`cuda_linked: false`); `HYPERCINE_ENABLE_CUDA=OFF` |
| compiler | MSVC 19.44.35229.0, `/O2 /Ob2 /DNDEBUG` |
| CRT | dynamic (`/MD`); imports `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, `api-ms-win-crt-runtime-l1-1-0.dll` |
| worker binary SHA-256 | `246b71c16a1b3cb442e946f9c0cbdad38b8957e0b92fe07994f357ffff85241e` |
| worker source commit | `9d5f65437561e6c1b868c61fc4f6ed073d58ba38` |
| protocol script SHA-256 | `1c847f39693b80aff7c2000bdf268ef22a5785a52b5376f5aac309c00ffa0fbd` |
| upstream threading defaults | `_THREADED_DECODER=1`, `_ENTROPY_THREADS=(_THREADED_DECODER)`, `_INTERLACED_WORKER_THREADS=(_THREADED_DECODER)` on Windows, `THREADS_IN_LAST_WAVELET=4` |

The M4 host recorded dynamic `/MD` and 8 logical processors in #116 Handoff 4;
its CPU model, compiler version and exact worker binary lineage were not
published. Running this protocol with a worker of the same source lineage is
the still-missing matched half.

## Observations

| arm | processes | calls | distinct pixel hashes | distinct RGB hashes | PSNR, dB | decode median / max, ms |
| --- | --- | --- | --- | --- | --- | --- |
| primary 3 x 5 | 3 | 15 | 1 | 1 | 44.465986143258924 | 3.0669 / 5.2822 |
| extension 6 x 5 | 6 | 30 | 1 | 1 | 44.465986143258924 | 3.1714 / 5.9117 |
| combined | 9 | **45** | **1** | **1** | **44.465986143258924** | — |

All 45 statuses are zero; all metadata Open/Init/Set statuses are zero; every
call reports 1920x1080, pitch 7680 and 8294400 output bytes. The single pixel
hash is `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0`,
which is also the M4 host's modal variant. No child timed out.

**Cross-host comparison, same packet bytes:**

- M4 CUDA-free control: 96 calls, 10 pixel variants, 14/96 divergent, modal
  `c5e0e052...` in 87/96 (#116 Handoff 4).
- Windows worker: 45 calls, 1 pixel variant, 0/45 divergent, modal
  `c5e0e052...` in 45/45.
- Both hosts agree on the correct reconstruction. Only the divergence rate
  differs. At the M4 point estimate (14.6% per call) the probability of 45
  consecutive stable calls is about 0.08%; the exact 95% upper bound for the
  Windows rate is about 6.4%, below the M4 95% interval of roughly 8.2-23.3%.
  A one-sided Fisher comparison of 0/45 versus 14/96 gives p ~ 0.003. That
  treats calls as independent; opens within one process may be correlated, so
  this is descriptive rather than a formal host-effect estimate.

This does not establish a cause. The hosts ran different worker binaries and
CPU topologies; the M4 compiler version and exact binary identity are unknown.
It does show that packet identity alone does not determine the outcome, so
build/lineage, CPU topology and runtime scheduling remain active variables.

## Limits

- One host arm, 45 calls; not a production benchmark or a performance claim.
- Different binaries per host; the M4 arm was not rerun with this worker.
- No repeated encoder invocation and no encoded-packet pair were measured;
  `encoder_packets_equal` stays `null`.
- No CUDA, GPU, sanitizer, 4K/8K, Adobe/Resolve or second-decoder qualification.
- The original #116 Handoff-1 intermittent 1080p artifact is unavailable; this
  packet is the pinned re-creation whose SHA both hosts confirmed.
- A stable Windows result does not show that any other geometry or output
  format is stable; #138 already records instability for YUY2 output and for
  128px BGRA.

## Reproduction

The pinned inputs must match the hashes above; the work directory must be fresh.

```powershell
cmake --build build --config Release --target reference_decode_worker --parallel 4
python -B tests/reference_decode/cross_host_1080.py --worker build/tests/Release/reference_decode_worker.exe --packet build/phase2c-priority-a/reference.cfhd --source build/phase2c-priority-a/source.bgra --work build/cross-host-1080/windows-arm --output build/cross-host-1080-windows.json
python -B tests/reference_decode/cross_host_1080.py --worker build/tests/Release/reference_decode_worker.exe --packet build/phase2c-priority-a/reference.cfhd --source build/phase2c-priority-a/source.bgra --work build/cross-host-1080/windows-arm-30 --output build/cross-host-1080-windows-30.json --processes 6
```

Exit 1 means the evidence was collected but remains unresolved; 2 means
timeout or invalid evidence. Full measured JSON is committed as
`cross_host_1080_windows_20261018.json` and
`cross_host_1080_windows_30_20261018.json` after sanitization.

## Next action

The #116 M4 owner runs the identical protocol on the same packet and publishes
its host/build matrix (CPU model and topology, compiler version and flags, CRT
link mode, worker binary SHA and source lineage). Only then can the
divergence-rate difference be attributed to a configuration dimension rather
than to an unmeasured host difference. #120 stays open;
`REFERENCE_DECODER_NONDETERMINISTIC` / `UNRESOLVED` remains the reference gate.
