<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Original Descale: native Windows replay for #220

Source head: `94ce0c97d0041d5dc36276259da97beafaaa96a2`.
Date: 2026-10-27. Native Windows AMD64, Python 3.10.9,
MSVC 19.44.35229.0, x64 C++17 Release, SSE2 baseline.

## Result

The previously unavailable corpus is reachable when the authenticated replay
is run outside the restricted execution environment. All 30 original artifacts
and their JSON/SHA sidecars authenticate successfully. All six original SSE2
comparisons pass with zero mismatched signed16 samples. The driver reauthenticates
the corpus after execution. No corpus bytes or pinned identities were changed.

| Case | Mismatched samples | Actual and expected SHA-256 |
| --- | ---: | --- |
| 1920x1080_ch0_Y | 0 | `7c69960849b67eb21ddadaa2aafb0062a02c2d42bc4f6690a7a88246fd4237cb` |
| 1920x1080_ch1_V | 0 | `0312e9fdd04dca2c0bdc14673a9b1e0eaa4a13dacb64b7f1592f2639313b279b` |
| 1920x1080_ch2_U | 0 | `94e35fd30db993a70d0554c3abe6fc3824e8dc40a2c9c5df704009d2cb9854c8` |
| 3840x2160_ch0_Y | 0 | `46e24d49acc671cbc010ccfca6cec9cba50aff8f8060bd1e2b5e9eba87d6d665` |
| 3840x2160_ch1_V | 0 | `f3a72b59810aa7d8d85923eaeb5f6b0e6e00717fbaddc4d54b6e6692cc7479e3` |
| 3840x2160_ch2_U | 0 | `09eae95f0a42fc214491f8c562aeda50242a49d017f985fe24e22b318971afe9` |

## Build and buffer evidence

Compiler arguments: `/O2 /std:c++17 /EHsc`, include directory containing the
prepared original source and support header; no explicit ISA override.

- Prepared-source SHA-256:
  `241d2dd1591f63793e74428e40e4e4448b52af289a61f47d69f976dae743a8dd`.
- Probe executable SHA-256:
  `583cb5d5c7f69fe7ee21f041abea15b1469661258f4024ad669e3db749ee108b`.
- The unchanged probe owns separate 32-byte-aligned input, output and scratch
  allocations, with 32-byte guards. Every executed case checks all guards and
  verifies the four inputs unchanged. No check failed.
- Input pitches are `2*w` bytes; output pitch is `4*w` bytes. All four accepted
  widths satisfy 16-byte pitch alignment.
- Existing MSVC C4005 warning: generated `_UNALIGNED` redefines the runtime
  header macro. The prepared bytes were preserved for same-source replay.

Reproduction after source preparation and native compilation:

```text
python tools/replay_original_descale.py --authenticate --probe <native-probe>
```

The raw sanitized driver result is retained locally in
`build/windows-original-replay.json`; generated binaries remain ignored.

## Evidence boundary

This establishes native MSVC replay of the six authentic original input/output
bindings. It does not establish the historical reference executable's compiler
flags, ISA dispatch, allocation identities or observer ordering. The archived
Windows build/buffer owner signoff remains `SEPARATE_OWNER_GATE`.
The contract remains `DESIGN_REVIEW_REQUIRED`, with
`implementation_authorized: false`. No new inverse arithmetic, decoder
integration, GPU execution or performance qualification is claimed.

The dedicated harness regressions pass 9/9 with temporary files inside the
workspace. No upstream source changes were made.

Complete locally registered Release CTest: **33/33 PASS**, zero skips,
99.09 seconds. Configuration: standalone ON, upstream OFF, tests ON,
benchmarks OFF, Visual Studio 2022 x64. FFmpeg/ffprobe are unavailable;
the ten additional external-decode tests were not registered. This is not
a new local 43-test external-decode qualification.
