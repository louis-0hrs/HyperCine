<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Isolated scalar inverse research unit — issue 144

This implements the bounded research choice recorded in
[#201's architecture decision](https://github.com/louis-0hrs/HyperCine/pull/201#issuecomment-6287254552),
on a separate child of architecture head
`1acf35bf42431084e3bd8d30017bbeebcdf452a8`.
One private C++17 function reconstructs wavelet4 LL from four explicitly
supplied entropy-completed/dequantized wavelet5 signed bands. It implements
[the source-specific arithmetic proposal](../SCALAR_INVERSE_STAGE_144.md).
No packet parser, temporal inverse, output conversion, public decoder API,
real graph profile, second scheduler or GPU path is added. The graph remains
Synthetic and its implementation is unchanged.

## Interface and numerical model

`src/cpu/decode_spatial_scalar.{h,cpp}` defines move-only `OwnedSignedPlane`,
const input views and a mutable output view. The private research function
`InverseWavelet5To4` requires four disjoint aligned input planes with matching
channel/generation, wavelet5 bands0..3 in order, and a disjoint wavelet4/band0
output of doubled dimensions. It rejects invalid identity, unsupported shape,
short storage, aliasing, misalignment, pitch/extent overflow and address wrap
before writes. Input width is 16..480, divisible by8; height is 3..270. No
arbitrary profile or extreme frame allocation is accepted.

Each allocation has 32-byte alignment, padded pitch, zero initial padding and
an independent RAII owner. Views must cover every full padded row, with at most
2MiB per plane. Input padding is ignored; completed output padding is zero.
Move operations invalidate the moved-from view. Caller storage must really
exist and remain alive: arithmetic checks cannot validate arbitrary addresses.
Input values are already dequantized; no quant multiplier or destination's
next-stage prescale is applied.

The function uses explicit floor division for negative signed values and
explicit modulo wrapping, avoiding implementation-defined signed shifts and
out-of-range signed16 casts. Vertical convolution uses int32 with signed16
saturation on each stored strip. Horizontal behavior follows the reviewed
source model: saturated operations at SIMD-equivalent interior positions,
short-compound-assignment wrap at the tail, wrapped first-border insertion,
saturated last border and asymmetric endpoint filters. This models a specific
original x86 source path; its archived compiler/ISA selection is still a
separate Windows reference-owner confirmation, not a new Mac observation.

Cancellation is queried before allocation, before each row pair and before
returning Complete. Cancellation or a callback exception can leave partial
**private** output and must never cause publication. Callback scratch unwinds
with RAII. The graph harness retains all plane storage through worker drain
and publishes only in a dependent consumer after successful arithmetic.
No hard deadline or ability to kill an in-process C++ worker is claimed.

## Synthetic verification and original-reference boundary

Eight mandatory CTest names are explicitly required by `ci/run_standalone.py`:

- `scalar_inverse_arithmetic`: three small static operator fixtures, independent
  Python unbounded-integer/Fraction derivation, exhaustive output comparison,
  poisoned input padding, deterministic output padding, analytic DC controls
  at every distinct original pair shape and moved-owner checks.
- `scalar_inverse_rejection`: malformed identity/geometry/storage/aliasing,
  storage budgets/address wrap, real aligned allocation failure, cancellation
  at admission/middle/final boundary and exception cleanup with live-allocation
  accounting. Invalid requests leave output untouched.
- `scalar_inverse_graph_workers_1/2/4/16`: held producer and pending inverse,
  four successive generations, actual data read by a downstream consumer,
  canceled inverse with a completed private row pair and live scratch,
  failed producer, stale-generation callback rejection and lifetime until drain.
- `scalar_inverse_vectors`: regenerates the committed synthetic fixture header
  from the independent Python operator model without changing the file.
- `scalar_inverse_replay_contract`: seven guard methods, including actual C++
  probe execution on six **synthetic DC** fixture sets, deliberate generated
  output corruption, invalid sidecars/hash/lengths and missing originals/binding.
  Synthetic fixtures authenticate the harness only; they are not original SDK
  bytes or substitute original expectations.

Nine disposable-source numerical mutations are rejected by the arithmetic
fixtures: truncating negative division; vertical wrap; left-border saturation;
right-border wrap; removing the short tail; swapping band order; changing a
border coefficient; saturating the short tail; wrapping the SIMD interior.
Forced border vectors distinguish both endpoint storage rules. These are test
sensitivity checks, not evidence that the private real planes reach extrema.

The original 30 artifact identities remain unchanged in
`SCALAR_INVERSE_STAGE_PROPOSAL_144.json`. `validate_decode_spatial_scalar.py`
authenticates all30 original files, full hashes/lengths, compact row storage,
sidecars, producer/stage identities and common packet binding **before** any
candidate call. Each probe copies decoded little-endian inputs into aligned
planes and writes only a newly created temporary output directory. The harness
compares generated output directly with actual original LL bytes and digest,
then rehashes every source. No original is copied, resealed, exported or replaced.
The raw original files stay outside Git as separate loose files.

`HYPERCINE_SCALAR_ORACLE_ROOT` explicitly registers
`scalar_inverse_original_six_pairs`. A requested but missing test is rejected
by the standalone inventory gate. Missing or corrupt original files fail the
requested replay, not skip or pass. The standalone report separately states
`scalar_original_stage_replay=NOT_RUN` when this optional private gate was not
configured. A full synthetic/encoder suite PASS cannot become original parity.

## Measured native evidence

Native macOS26.5.2 ARM64, AppleClang21, CMake3.31.5, Python3.9.6 with optimization,
external FFmpeg n8.0. Native binary is Mach-O arm64. Final results are recorded
in [the machine-readable report](scalar_inverse_research_macos_2026-10-20.json):

| Gate | Actual result |
| --- | --- |
| Full SDK-free Release CTest | 40/40 PASS, zero skips, 74.89s |
| Full SDK-free ASan/UBSan CTest | 40/40 PASS, zero skips, 135.66s |
| Scoped TSan scalar graph workers1/2/4/16 | 4/4 PASS, zero skips, 1.43s |
| Numerical mutation sensitivity | 9/9 rejected |
| Replay guard / CI inventory methods | 7/7 and 6/6 PASS |
| C++17 -Wall -Wextra -Werror, source and C++ harness | PASS |
| Tests-OFF configuration | zero registrations |
| Six private original pairs | NOT RUN / BLOCKED_ACCESS |

The full suites overlap; these are not 80 distinct tests. TSan covers only the
four worker cases and the code they execute, not the whole encoder suite. Both full suites require external
decode and inspect registered/executed CTest inventories with no skipped tests.
ASan/UBSan uses frame pointers and no sanitizer recovery; every C++ compilation
entry is instrumented and ASan runtime linkage was checked. Leak detection is
disabled; Python and external FFmpeg are not sanitizer-instrumented.

The inherited private corpus binding is currently **unreachable (ENOENT)**.
The actual original-replay request returned failure1 with a redacted error;
a separately configured CTest request against missing originals also failed as
required. Therefore original six-pair equality is **NOT RUN / BLOCKED_ACCESS**.
No original SDK, CUDA/Metal, Adobe/Resolve, PSNR or performance result is claimed.
No benchmark was run; this is a correctness research unit with no speedup claim.

## Portable reproduction and remaining handoff

Full synthetic/rejection/worker coverage and existing external encoder gates:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
PYTHONOPTIMIZE=1 ctest --test-dir build -C Release --output-on-failure
```

For native Clang/GCC sanitizer qualification use a separate directory with
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'`
and `-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'`, then
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`
when running the full CTest command. MSVC requires its own sanitizer lane;
no equivalent MSVC UBSan result is inferred from this Mac run.

On an authorized fixture host, at the **same prototype source commit**:

```sh
: "${HYPERCINE_GOLDEN_DIR:?Configure original corpus binding privately}"
cmake -S . -B build-original -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_SCALAR_ORACLE_ROOT="$HYPERCINE_GOLDEN_DIR"
cmake --build build-original --config Release --parallel 4
ctest --test-dir build-original -C Release --output-on-failure -R scalar_inverse
```

Expected loose input roots are the existing `golden/windows/gop2/issue189-20261019-r1/GEOMETRY`
and `golden/windows/gop2/issue189-20261019-phaseb-r1/GEOMETRY/original` within the
private binding. No new golden export or GPU window is needed.

Next action: independent numerical review plus Windows original-reference-owner
archived compiler/SIMD confirmation and actual six-pair original replay at the
published prototype SHA. Keep #144/#47 open and the child Draft; full production
Milestone2, GOP1/GOP2 decoder support and main/phase integration remain separate.
