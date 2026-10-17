<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Five independent macOS reviews

Date: 2026-10-17. Native ARM64 macOS 26.5.2, AppleClang 21.0.0,
CMake 3.31.5, Python 3.9.6, FFmpeg n8.0. Each review uses a separate
archive of the exact reviewed source, without editing its owner's checkout.
These are completed review subtasks, not completion of five parent features.

| Issue | Reviewed source | Result and next action |
| --- | --- | --- |
| #44 | PR #50, `56fbe550d2f88ebf6cc3a3e81b0d37a3ae8e084f` | Reviewed geometry validation before multiplication, separate legacy/session allocation caps, fixed session geometry, stride extent overflow checks, bounded writer and state commit after successful framing. Fresh full standalone suite with required external decode: 31/31 PASS (204.46 s), including 19 real 4K frames, minimum PSNR 52.32135 dB and sixteen ordered decoded-sequence hashes. No new blocking defect found in this scope; experimental P4 acceptance remains a maintainer decision. Original SDK 4K parity is still REFERENCE_BLOCKED. |
| #45 | PR #135, `59da896391137d50ba147df617c763e40f51ddfb`; Metal source from main `e87ef0258ba37f5c78f1557c59d9c7331df5521c` | Independently checked all five resident-workspace totals, maximum-buffer bounds and padding equation against `Prepare`, `AllocateSurface` and `Buffer`. Confirmed reported workspace-peak limitation also affects a reachable 1080p pitch-only reprepare. No P5 code or hardware qualification. Follow-up: include concurrent old/new storage in workspace telemetry/budget after the P4 gate clears. |
| #46 | PR #133, `be0d01ddad0b9a6c8700efe56b75f06a470081ac` | Reviewed full-consumption `from_chars`, typed signed/unsigned limits, fixed generic parse failure, retained semantic validation, actual-process regression coverage. Fresh quality-only CTest 3/3 PASS. No blocking defect found; this test helper fix does not establish six-profile Mac encoder parity. P6 remains BLOCKED_BY_P4_P5. |
| #47 | PR #83, `fd844fd058e0d79d4eabd39769e453c65ea78ba3` | Confirmed real public fixture baseline 2/2 CTest PASS, then reproduced acceptance of ambiguous JSON and an unsanitized wrong-root TypeError. Isolated child [PR #136](https://github.com/louis-0hrs/HyperCine/pull/136) rejects duplicate keys, nonstandard constants, type aliases and wrong roots with generic failures; regression and actual decode 3/3 PASS. Review the child before accepting the verifier. Native P7.B decoder remains blocked. |
| #70 | PR #135, `59da896391137d50ba147df617c763e40f51ddfb` | Reviewed independent widened quotient/remainder floor oracle, six-tap boundary formulas, prescale pair sum versus individual taps, saturation, deterministic seed, active coefficient coverage and padding checks. Full contract project Release 5/5 and ASan/UBSan 5/5 PASS, including 1,698,720 wavelet checks. No new blocking defect found in this preparation scope; these are scalar checks, not NEON implementation or full-frame 4K qualification. |

## Metal review detail

`Buffer` already checks each allocation against `device.maxBufferLength`.
The missing protection is a total/shared-RAM budget, including atomic replacement;
adding another per-buffer check alone would not address that issue.

The model's five payload totals are independently confirmed:
31,622,400; 126,489,600; 505,958,400; 1,506,631,680; 2,148,545,280 bytes
for 1080p, 4K, 8K, 12K and 17K. These are estimates, not measured GPU use.

This review adds a reachable case to the existing #135 observation: same
1920x1080 input with stride changing from 7680 to 7696. Old/new payloads are
31,622,400 and 31,639,664 bytes. `Prepare` retains `workspace_` while allocating
`next`, so at least **63,262,064 payload bytes** coexist before assignment.
Its `peak_buffer_bytes` update observes only the new workspace, underreporting
that overlap by 31,622,400 bytes. `peak_device_allocated_bytes` is a distinct
device-wide metric. Host/readback planes, samples and driver overhead are excluded.
No GPU allocation was performed for this calculation.

## JSON finding

Calling the actual P7 entry point with mocked capture reproduces:
`schema=true`, `schema=1.0`, integer stream width represented as `1920.0`, and
duplicate schema keys accepted; a list-root reference raises uncaught TypeError.
Five new test methods fail before repair (10 failures, six errors). The repaired
P7 audit suite passes 3/3, including real FFmpeg decoding of the original public
24-frame sample and unchanged hashes. Parent #83's old 2/2 success therefore did
not cover this malformed-evidence behavior. Tool JSON uses the same strict reader.

## Full regression results

Current main `e87ef025`: 28/28 standalone Release CTest PASS (79.60 s).
Repaired P7 parent branch: 28/28 standalone Release CTest PASS (79.35 s).
Both required external FFmpeg and had no skips. Existing current-main CI Python
unit suites: 19/19 PASS. P7 fix commit: `8f547c6`.

## Reproduction

Use a clean source archive at each revision above. Configure FFmpeg/ffprobe
locations privately; raw logs, build diagnostics and generated media remain local.

```sh
# PR #50 and current-main / P7 standalone regression configurations:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_FFMPEG_EXECUTABLE="$FFMPEG" \
  -DHYPERCINE_FFPROBE_EXECUTABLE="$FFPROBE"
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure

# PR #133:
cmake -S . -B build/quality -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON
cmake --build build/quality --parallel 4
ctest --test-dir build/quality -C Release --output-on-failure

# PR #135, with a private checkout of the pinned #83 source:
cmake -S tests/macos_contracts -B build/contracts -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_P7_AUDIT_ROOT="$P7_AUDIT_ROOT"
cmake --build build/contracts --parallel 4
ctest --test-dir build/contracts -C Release --output-on-failure
```

For the sanitizer contract configuration use `RelWithDebInfo`,
`-fsanitize=address,undefined -fno-omit-frame-pointer` for C++ and
`-fsanitize=address,undefined` for the linker, with `ASAN_OPTIONS=detect_leaks=0`.
Python, external FFmpeg and GPU code are not instrumented by that run.

No private SDK corpus was configured for these review runs. Existing manifest
and coefficient tests use their checked-in reference data. No Windows/Linux
runtime, GoPro SDK native ARM64 build, Metal kernel, GoPro 4K packet identity,
NEON backend or performance result is inferred from these reviews. `upstream/`
and all implementation-owner worktrees remain unchanged. All parent issues
remain open; no review result clears a production dependency or authorizes merge.
