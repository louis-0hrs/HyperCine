<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Internal CUDA encoder sessions (#259)

This additive internal step binds the existing CUDA FilmScan3 frontend to the
capability-gated `detail::EncoderSession` that already carries CPU scalar and
Metal. It does not create another encoder, writer, public backend enum or public
product API. Existing FilmScan3 constructors, the legacy fixed-Q2 path, the
pinned 30/1 timecode behaviour and `upstream/` are unchanged.

## Contract

- The explicit experimental CUDA geometry list is 128x24, 256x256, 1280x720,
  1920x1080, 2048x1080 and 3840x2160. Shape legality is not qualification for
  other sizes, and CUDA gets its own evidence records: CPU or Metal evidence
  never qualifies a GPU, and Metal device limits never qualify CUDA.
- `ProbeCudaEncoderDevice()` reads device count, device properties and total
  device memory from the driver. It allocates no buffer, creates no stream,
  frame or workspace and launches no kernel. A build without CUDA reports the
  backend absent instead of relying on a later launch failure.
- `cuda_available` is a runtime fact, and a CUDA session additionally requires
  bound input storage (stride and byte extent) plus a nonzero probed device
  memory size. Observing a card, or compiling the toolkit, grants nothing.
- Preflight keeps the existing conservative `36*pixels + 1 MiB + stride*height`
  planning ceiling and additionally requires the probed total device memory to
  cover it. This is static planning arithmetic, not measured RSS, a reservation
  or a guarantee under concurrent system pressure; runtime `cudaMalloc` can
  still fail.
- Session construction invokes only the selected factory and checks exact
  geometry, sample capacity, frontend identity (`FrontendBackend::Cuda`) and the
  GPU-identity ISA convention. The factory seam takes geometry only, so a
  fault-injection test mode cannot be reached from the session table.
- Refusal and fallback semantics are unchanged: absent-backend and
  unqualified-geometry results may use an explicitly requested scalar fallback
  only after a complete scalar re-query; format, profile, storage, resource and
  required-reference refusals are never repaired by switching backend. No
  mid-frame backend switch.
- CUDA is an experimental, unqualified attempt in this revision. It is not a
  product selector, not an original-SDK reference comparison, not a VC-5/SMPTE
  conformance statement and not a release gate.

## Evidence and reproduction

`tests/test_encoder_backend_selection.cpp` proves the portable contract on a
CPU-only host with counting and fake factories: CUDA cannot be satisfied by the
scalar or Metal seam, a CPU frontend cannot masquerade as CUDA, a CUDA frontend
cannot pass for Metal, wrong geometry/capacity/ISA and null factories produce
typed initialization failures without falling back, and `bad_alloc`/logic
errors propagate instead of being reported as an unavailable backend.

`tests/test_encoder_cuda_session.cpp` is the native gate. It is registered only
with `HYPERCINE_ENABLE_CUDA=ON`, is GPU-required with no skip exit, and is
mandatory in the strict standalone inventory for CUDA builds. It checks the
complete packet, all 36 quantization/scale entries, frame/rate state and the 30
coefficient hashes against independent explicit-scalar sessions at the six
geometries above, asserts six real spatial launches per frame (three channels,
two CUDA levels each; the prescale-2 middle level deliberately stays on the
scalar CPU path), and covers padded storage rejection without GPU traffic,
failure after real GPU completion with backend retention, `Reset`, and two
concurrent 16-frame sessions with opposite hashing policies.

Configure the SDK-free standalone build with tests, external decode required and
CUDA enabled. The build must use one consistent host compiler: on this host the
default GCC 14.2 front end combined with the toolkit's GCC 13 library path fails
to link `__cxa_call_terminate`, independently of any repository change.

That mismatch is now refused at configure time instead of surfacing as a link
error after the whole tree compiled. `cmake/cuda_host_compiler.cmake` compares
`CMAKE_CXX_COMPILER` with the host compiler nvcc reports, and a newer C++ front
end than nvcc's host compiler is a `FATAL_ERROR` naming the consistent configure
line. The rule is narrow by design: it judges only the GNU/GNU pair and only the
measured direction (a newer nvcc host compiler is rejected by nvcc itself), and
it stays silent when a version is unknown rather than guessing. The reason the
mismatch cannot be fixed here is the link order — CMake places
`CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES` ahead of the C++ driver's library search
path, so `-lstdc++` resolves to the host compiler's older release. GCC 14 emits
`__cxa_call_terminate` (CXXABI_1.3.15); GCC 13's libstdc++ does not export it.
`ci/test_cuda_host_compiler.py` proves the refusal fires in the real project and
tests the boundaries that must *not* refuse.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_C_COMPILER=gcc-13 \
      -DCMAKE_CUDA_HOST_COMPILER=g++-13 \
      -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
      -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_ENABLE_CUDA=ON \
      -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON -DCMAKE_CUDA_ARCHITECTURES=80
    cmake --build build --parallel
    ctest --test-dir build -C Release --output-on-failure

The above **CUDA-on** tree runs the native CUDA-required CTests. Do **not** run
`ci/run_standalone.py` against this tree: that strict inventory intentionally
rejects CUDA-enabled builds. Run its SDK-free CPU-only inventory in a separate
build directory instead:

    cmake -S . -B build-cpu-standalone -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
      -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_ENABLE_CUDA=OFF \
      -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
    cmake --build build-cpu-standalone --parallel
    python3 ci/run_standalone.py --build-dir build-cpu-standalone --system Linux --machine x86_64

`ci/run_standalone.py` requires the CPU-only standalone configuration and
rejects a CUDA-on tree, so the CUDA session gate is listed for CUDA builds while
the strict inventory itself is executed on the CPU tree. The native CUDA test
optionally accepts an existing fresh packet dump directory and writes
scalar/CUDA packet pairs for all six sizes; those packets can be wrapped with
`tests/validate_standalone.py::avi` and decoded with FFmpeg/ffprobe to
yuv422p10le. A packet comparison is not itself an independent decoder test, and
the existing mandatory FFmpeg gates remain separate.
