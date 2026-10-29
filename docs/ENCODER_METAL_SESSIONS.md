# Internal Metal encoder sessions (#259)

This additive internal step binds the existing Metal frontend to the capability-
gated detail::EncoderSession. It does not create another encoder, writer,
public backend enum, or public product API. Existing FilmScan3 constructors and
legacy fixed-Q2 behavior are unchanged.

## Contract

- The explicit experimental Metal geometry list is 128x24, 256x256, 1280x720,
  1920x1080, 2048x1080, and 3840x2160. Shape legality is not qualification for
  other sizes. CUDA, Vulkan, CPU SIMD, RGB444, other qualities and non-30/1
  timebases do not gain support here.
- ProbeMetalEncoderDevice() obtains presence, maximum buffer length and
  recommended working-set size, without creating a queue, pipeline, frame or
  workspace. The Metal-disabled build reports Metal absent. The pure capability
  query consumes caller-supplied facts; it neither probes nor allocates frames.
- Metal requests must bind both input stride and input bytes. Preflight checks
  storage shape/overflow, 32-bit GPU indexing, maximum single buffer, caller CPU
  and GPU budgets, and the device working-set ceiling before factory invocation.
  The conservative 36*pixels + 1 MiB + stride*height planning allowance is not
  measured RSS, a reservation, or a guarantee of successful allocation under
  concurrent system pressure. Runtime allocation can still fail.
- Session construction invokes only the selected frontend factory, checking
  internal backend identity, exact geometry and sample capacity. Scalar ISA is
  explicit; Metal is not a CPU ISA. Factory failure never silently retries CPU.
- Absent/unqualified-backend fallback remains explicitly opt-in and rechecks the
  scalar contract. Resource, storage, profile and required-reference refusals
  cannot be repaired by fallback. No mid-frame backend switch is permitted.
- Storage and diagnostics remain bound across Encode, failure and Reset. Actual
  caller pointer alignment/extent is checked on every frame. A failed GPU frame
  does not advance the sole writer's frame counter or rate state.

## Evidence and reproduction

Portable selection tests use counting factories and explicit test doubles; they
are policy evidence, not proof of GPU execution. The native
encoder_metal_session_packets CTest is registered only with Metal enabled,
is GPU-required, has no skip exit, and is mandatory in the strict Metal inventory.
It checks complete packets, rate/frame state, all quantization/scale tables and
30 coefficient hashes against independent explicit-scalar sessions at six sizes.
It also checks padded storage, failure after real GPU completion, retry, Reset,
and two concurrent 16-distinct-frame sessions with opposite hashing policies.
Actual color/wavelet dispatch, command and wait counters must advance.

Configure the existing SDK-free standalone build with tests and external decode
required; enable HYPERCINE_ENABLE_METAL=ON on macOS. Then run:

    cmake --build build --parallel 4
    ctest --test-dir build -C Release --output-on-failure
    python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64

The native test optionally accepts an existing, fresh packet dump directory:
build/test_encoder_metal_session <dump-directory>. It writes scalar/Metal
packet pairs for all six sizes. Those packets can independently be wrapped with
tests/validate_standalone.py::avi using their actual dimensions and decoded
with FFmpeg/ffprobe to yuv422p10le. A native packet comparison is not itself an
independent decoder test. Existing mandatory native 4K/sequence FFmpeg gates are
preserved separately.

Original GoPro reference qualification remains missing; requesting it refuses
allocation. No 8K+, SMPTE conformance, production release or speedup is claimed.

## Native validation snapshot

Apple M2 Pro, arm64, macOS 26.5.2, AppleClang 21, Metal ON, upstream OFF:
strict complete standalone **119 registered / 119 PASS / 0 FAIL / 0 SKIP**,
95.50 seconds. This includes the existing mandatory native 16-frame 4K packet
and FFmpeg gate, not just the new session test.

The new internal-session dump was also decoded independently: **12 FFmpeg
single-frame decodes**, CPU and Metal at each of the six listed geometries.
ffprobe confirmed actual dimensions and yuv422p10le; byte counts, p10 range and
all decoded CPU/Metal pixel bytes matched. The new 2x16 session comparisons are
packet/state evidence, not 32 additional independent decodes.

For the deterministic first 3840x2160 fixture from the new session test:

- CFHD bytes: 4,620,328
- Packet SHA-256: 07352e026a504c8bde510fd5e655f867a8417bcf02856bd0fa48b66c53f657d4
- Decoded p10 SHA-256: 59461e69eb66fb1479bb6eec02bf0d3f95f20f313ddcd2b408efd872a635cd0c

The final whole native gate uses an isolated Python 3.12 interpreter. An initial
system-Python-3.9 run passed 118/119; the unrelated RCA selftest rejected an
unsupported Path.write_text(newline=...) argument. No RCA code, system Python or
upstream source was modified to obtain the successful final run.

Linux x86_64 GCC 14.2 strict complete standalone: **110/110 PASS**, no failures
or skips, 143.22 seconds. Focused Debug ASan+UBSan: **3/3**, 93.06 seconds,
including leak detection; focused TSan session contracts: **2/2**, 69.55 seconds.
Native macOS Debug ASan+UBSan including real Metal: **4/4**, 88.39 seconds;
LeakSanitizer disabled on that native run. Sanitizer counts are focused subsets,
not whole-suite sanitizer qualification. Tests-OFF library build passed, and CI
inventory guardrails passed **27/27** with normal Python and Python -O.

Independent review identified a padded-extent arithmetic corner: the last-row
input extent can fit uint64 while stride*height overflows. The final code rejects
that case before every factory, with fallback enabled and one-byte resource
limits. An independently compiled backward mutation removing this guard fails
the counting-factory regression against a passing baseline. Native concurrency
starts only after both sessions finish construction; post-Reset identity uses a
saved frontend value rather than a reference to the selection object.
