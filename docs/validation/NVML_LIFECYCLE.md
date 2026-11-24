<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Balanced optional NVML provider lifetime — #115

Child of PR #124, parent 17d60ca241a6fb2ed97e72f2e891812923136832.
The runtime loader initialized NVML even when nvmlShutdown was unavailable.
Destruction then unloaded the DLL without releasing a successful initialization.
The optional provider now requires init, shutdown and device lookup entry points
before invoking init; incomplete providers remain unavailable and cannot abort
encoding.

The synthetic boundary compiles the actual extracted production sampler. Tests
count library loads/unloads and initialization/shutdown calls for missing required
symbols, initialization failure and device lookup failure. The new missing-shutdown
vector failed before the guard. Existing sampler restart/Stop, independent counter
failures, one-million-poll bounded storage and both actual JSON writers remain
covered. Device lookup failure after successful init still shuts down exactly once;
failed init never invokes shutdown.

Native NVIDIA/NVML and full CUDA translation-unit execution are unavailable here.
This qualification is limited to the actual sampler against synthetic callbacks;
a short native NVIDIA build/run remains required before hardware acceptance.
No codec math, allocation policy, GPU performance or bitstream changes are claimed.

```sh
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build-asan -C Debug -R '^phase3a_telemetry' --output-on-failure
```

Native test environment: macOS ARM64, AppleClang 21, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Full Release configuration uses HYPERCINE_BUILD_STANDALONE=ON,
HYPERCINE_BUILD_UPSTREAM=OFF, HYPERCINE_ENABLE_CUDA=OFF,
HYPERCINE_BUILD_BENCHMARKS=OFF and HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON.
Supply FFmpeg/ffprobe through PATH or the existing CMake executable cache options.
No protected-main merge or release; frozen upstream remains unchanged.

Native CI Python: 19/19 PASS. Debug ASan+UBSan actual-sampler/both-writer
CTest: 2/2 PASS, 1.29 seconds. This is synthetic provider coverage only.
Full native SDK-free Release required-external-decode CTest: 30/30 PASS,
zero skips, 76.33 seconds.
