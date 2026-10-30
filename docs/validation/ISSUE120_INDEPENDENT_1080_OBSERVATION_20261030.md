# Issue #120: bounded independent 1080p decoder observations

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Status: **REVIEW — oracle remains BLOCKED**, native Windows x64, 2026-10-30.
Base main `04c348f`. Existing Phase2 quarantine and release inventory are already
integrated. This follow-up measures a candidate independent decoder on the
historical Phase2C synthetic sample without changing any pixel-quality gate.

## Fixed scope and identities

The observation harness accepts exactly the previously retained synthetic pair:

| Input | Bytes | SHA-256 |
| --- | ---: | --- |
| 1920x1080 FilmScan3 YUV422 progressive CFHD | 900396 | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` |
| Source BGRA8 | 8294400 | `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1` |

The packet hash is also recorded in `docs/CUDA_PHASE2C.md`. These are retained
bytes, not regenerated packets, and their hashes are checked before and after
the experiment. No additional source-generator seed or historical execution
authenticity is asserted.

The existing #11 adapters provide the minimal AVI wrapper, tight top-down p10
normalization and explicit BGRA conversion filters. The p10 contract is
Y|Cb|Cr, 16-bit little-endian containers with values 0..1023, 8294400 bytes.
BGRA output is tightly packed and top-down, also 8294400 bytes. AVI's synthetic
25-fps field belongs to the wrapper; it is not measured original playback.

Both executable hashes and version tokens are bound before decoding:

| Tool | Version token | Executable SHA-256 |
| --- | --- | --- |
| FFmpeg | `n9.0.1-11-ge47273f4d9-20260831` | `daeea959fd6a400eec29bf1ede560962e92546ec31fe5f60ccdc076a4c3db3fa` |
| FFprobe | `n9.0.1-11-ge47273f4d9-20260831` | `fe3a94d2b2899191234870d95e85fd2bcc9c0cee830e7d8d78fcea8a2375155f` |

This build is **not** added to the existing 128x128 qualification allowlist.
The experiment is separate from `ffmpeg_oracle_qualification` and cannot grant
release qualification, even if every observation/control succeeds.

## Plan and measured result

Before the first decode the runner exclusively creates a new scratch directory
and writes `plan.json`: input/tool/harness hashes, dimensions, three output arms,
five fresh decoder processes per arm, both source-row hypotheses, and fixed
negative controls. Each decoder process has a 30-second timeout. There is no
retry selection, adaptive frame choice or selection of a passing orientation.
FFprobe must report cfhd/1920x1080/yuv422p10le before the positive decodes.

All **15/15 positive decodes** complete successfully and pass output shape/range
validation. Each arm has one distinct SHA-256 across its five processes:

| Output arm | Distinct/5 | RGB PSNR vs source rows as stored | RGB PSNR vs source rows reversed |
| --- | ---: | ---: | ---: |
| Native yuv422p10le | 1 | Not a BGRA comparison | Not a BGRA comparison |
| BT.709 limited-to-full, neighbor | 1 | 10.13365 dB | 41.07295 dB |
| BT.709 full-to-full, neighbor | 1 | 10.73539 dB | 29.86866 dB |

Alpha equals the source in both BGRA arms. Maximum RGB error against reversed
source rows is 11 codes for limited-to-full and 22 for full-to-full. Both
orientation hypotheses remain separately reported. These are source-quality
observations, not GoPro pixel equality or a newly qualified 35-dB quality gate.
No existing threshold changes.

## Negative controls: retain the failed result

1. The first parsed nonzero quantization tag 53 has value 16 at byte offset
   65668. XOR 1 at its low value byte (65671) changes it to 17. This mutated
   packet decodes successfully to valid p10 pixels with a different hash:
   **PIXELS_DIFFER**, a discriminated control. The computed JSON retains the
   mutated packet/output hashes and actual process exit code.
2. Zeroing the initial four-byte tag was preregistered to require a nonzero
   decoder exit. FFmpeg instead returns **0** and emits exactly the original
   p10 output: **ACCEPTED**. The runner preserves this observation as
   `CONTROL_NOT_DISCRIMINATED`, writes the report and exits **1**.

The zero-tag sentinel did not discriminate. Its acceptance is **not evidence of
a CFHD conformance defect**: this experiment does not prove that the missing
initial tag is forbidden in every decoder-supported dialect. It demonstrates
that this chosen rejection control cannot support qualification. It must not
be silently replaced after observing the result and then called a preregistered
pass. Earlier failed trial evidence remains private; the final instrumented run
retains the same negative result and changes no criterion.

Adapter, missing-output, timeout or process-launch failures are strict failures;
only an actual nonzero decoder exit is called `DECODER_REJECTED`. Truncated or
out-of-range output from a zero-exit mutation is never converted into a passing
negative control. A stable positive result cannot override a failed control.

Computed evidence: [full observations](../evidence/issue120-ffmpeg-1080-20261030.json).
Raw media, original inputs, scratch outputs and raw process diagnostics stay
private and outside public commits.

## Reproduce

Bind the retained immutable files and candidate tools locally. Use new scratch
and report names; existing destinations and the input/corpus directories are
refused. The original packet/source hashes are hard-bound by the runner.

```powershell
python -B tests/observe_ffmpeg_1080.py `
  --packet "$env:HYPERCINE_ISSUE120_PACKET" `
  --source "$env:HYPERCINE_ISSUE120_SOURCE" `
  --ffmpeg "$env:HYPERCINE_FFMPEG" --ffprobe "$env:HYPERCINE_FFPROBE" `
  --work "$env:HYPERCINE_ISSUE120_SCRATCH" `
  --output issue120-new-observation.json
```

Exit 1 means a strict experiment/control failure; failed control observations
are retained in the report. Exit 77 means the candidate is unavailable or the
bounded observations completed while the pixel oracle remains unqualified.
Neither exit is a release-quality PASS. The optional real experiment is not an
unconditional CTest dependency on private inputs/tools.

`independent_ffmpeg_1080_contract` is registered for normal test configurations.
Its **15 SDK/tool-free controls** cover immutable-input tampering, incomplete
or divergent repeats, absent/unrecognized tools, malformed mutation output,
ignored mutations, distinct orientation hypotheses, strict failed-control
precedence and rejection of any attempt to promote the oracle.

## Native verification and release inventory

Windows x64, MSVC 19.44.35229.0, CMake 3.31.6, Python 3.10.9; Release,
upstream/tests ON, CUDA/benchmarks OFF. Native build succeeds. Complete
`ctest --test-dir build -C Release --output-on-failure`:
**70 PASS / 0 FAIL / 2 SKIP, 72 registered**, 144.76 seconds.
The new 15-control contract passes. Existing skips are
`standalone_sdk_decode_3_packets` (unqualified SDK pixel oracle) and
`ffmpeg_oracle_qualification` (tool not discovered through this configuration's
PATH). The observation command supplies its candidate explicitly; that does not
change the separate allowlist or CTest's tool discovery.

The existing release inventory is separately executed and returns **77**,
`release_gate=BLOCKED`, `independent_pixel_oracle.qualified=false`.
Counts are 8 PASS / 0 FAIL / 6 BLOCKED, including the four CUDA Phase2 tests
not registered in this CPU-only build. This does not execute or qualify those
four callsites. No broad GPU repeat is performed.

[Per-test native validation](../evidence/issue120-native-validation-20261030.json)
and [release-lane inventory](../evidence/issue120-reference-lane-20261030.json)
preserve the distinct CTest, failed experiment and blocked release outcomes.
The initial sandboxed MSBuild stalled before compilation and was terminated
after checking process ownership; the successful native build used
`cmake --build build --config Release --parallel 4 -- /nodeReuse:false`.
No test PASS is inferred from the terminated build.

## Remaining gate and next action

`tools/check_reference_release.py`, `tests/reference_oracle.h`, original-SDK
pixel callsites, thresholds and `upstream/` are unchanged. No GPU, Phase3,
performance or pixel-oracle acceptance is claimed.

Review the measured normalization/stability evidence and the failed zero-tag
sentinel. Before another trial, define a separately reviewed valid rejection
control and an independent pixel-correctness reference/tolerance contract for
this exact 1080p profile. Repeated FFmpeg agreement and source PSNR alone do not
prove exact reconstruction. Keep #120 and the release gate open/blocked.
