# Private wire request runtime boundary

This internal adapter snapshots the existing session config with an explicit
wire dialect, ST 2073 edition and part mask, and decoder target. It admits only
legacy CFHD with no standard edition or parts claimed and no decoder-specific
requirement. VC-5, unknown dialects, any nonzero parts or edition, and any
specific or unknown decoder target are refused before session construction.
There is no dialect fallback. Decoder-specific interoperability and VC-5
serialization are not implemented or qualified by this boundary.

After wire admission a format-aware preflight dispatches on the declared source
format: a native YUV422p10 request is answered by `CheckNativePlaneCapability`,
every other request by the interleaved `CheckEncoderCapability`. The same
format, backend, geometry, storage and resource checks apply; the source domain
is selected from the request, never inferred or substituted. Only a permitted
request reaches the existing `EncoderSession` and its own backend selection and writer.
The adapter does not change existing constructors or public ABI. Backend fallback
never changes a wire choice. This opt-in boundary is intentionally more
conservative than the older selector: it requires the existing capability query
to permit allocation first. An absent backend with explicit scalar fallback may
be admitted by that query; a present but unqualified backend is refused even
where the older selector could separately admit scalar fallback. Capability queries may use exception machinery for invalid geometry;
no frame, frontend, writer, or session factory is reached on refusal.

The standalone test constructs actual small CPU sessions through both the default
production path and a counted injected path, checks the immutable
snapshot, and uses counted heap and session-factory adversaries to check wire
refusals. It exercises geometry, storage, memory, sample, format, reference and
missing-backend refusal through the same creation entry point. It does not claim
codec conformance, interoperability, GPU qualification or production readiness.

## Native YUV422p10 admission

The same boundary now admits the native plane source domain, because refusing it
at the wire layer made the reviewed path BGRA-only by accident rather than by
design. A native config with CPU scalar backend passes `CheckNativePlaneCapability`
and reaches the existing `EncoderSession`, whose native `Encode(const Planes&)`
path and shared writer are unchanged. The wire boundary adds no second writer and
no sample conversion.

Admitted and refused cases are test-enforced, not prose:

- Shape-only native configs (both sample strides zero) are permitted for a CPU
  query, and a bound layout (`luma_stride_samples`, `chroma_stride_samples`) is
  permitted as well.
- A native config that also carries `input_stride_bytes`/`input_bytes` is refused:
  the two addressing domains are exclusive, and the byte binding cannot budget
  planes.
- Half-bound (one zero stride) and negative sample strides are refused.
- Native requests for a **present** Metal, CUDA or Vulkan backend are refused
  with `ValidButNotImplemented`, including when `allow_cpu_scalar_fallback` is
  set. Backend fallback never repairs a present backend's missing planes
  frontend. An **absent** backend with explicit scalar fallback can be admitted
  as CPU-native, retaining the existing observable fallback reason.
- Invalid geometry and a required original-SDK oracle are refused before any
  session object or factory call.
- A native wire session refuses the interleaved `Encode(bytes, stride)` entry with
  `std::invalid_argument`; admission does not create a byte path.

The initial `29ac8993d8c0ae807299b1e9c8e463748ac0a4be` tests executed 144
checks per mode. Independent review identified that construction and negative
byte-input coverage alone did not demonstrate a successful native encode
through the default wire factory. That historical result does not cover the
additional checks below.

## Wire-created native encode acceptance (2026-11-19)

Both existing normal and optimized registrations now execute 205 checks. Two
deterministic 128x24 sequences each encode three distinct native frames through
the **default** `CreateWireSession` factory and a separately created direct
`EncoderSession` with identical configuration:

- Shape-only sample layout with packed input and the Medium quality profile.
- Bound padded layout with luma/chroma strides of 136/72 samples and the
  FilmScan2 quality profile.

The patterns are generated directly in the native Y, Cr, Cb domain, retain
ten-bit sample values including the accepted 1024 boundary, and leave invalid
1025 sentinels only in inactive padding. No BGRA conversion produces the input.
Every resulting packet must be nonempty and byte-identical to the direct
session; quantization/scale tables, frame index, rate limiter, previous sample
size, hysteresis state and all full coefficient hashes must also match.
Consecutive packets must differ, and the expected frame index is asserted
independently of the direct-session comparison. The nondefault profiles and
explicit padded binding are checked on the wire-created session itself.

Dedicated refusal rows now also cover native `input_bytes` alone, and a present
Vulkan backend with scalar fallback both disabled and enabled. They exercise
both counted and default factories and require refusal before construction.

Independent Linux x86_64 / GCC 14.2 Release review executed the following
production-source mutations in a separate checkout. Each mutation was rebuilt
and run in both normal and optimized modes; the expected assertion, rather
than a build failure or crash, made each corrected test fail:

| Production mutation | Corrected test in each mode | Original 144-check test |
| --- | --- | --- |
| Route native wire admission through the BGRA capability query | `native CPU wire request refused` | Not rerun for this mutation |
| Make only the default wire factory construct a BGRA session for native input | `native planes do not match the session source domain` | **Passes in both modes despite the broken factory** |

The second mutation demonstrates the exact earlier blind spot. Restoring the
production source and corrected test byte-for-byte restores 205 checks PASS in
each mode; the two existing native-plane tests also pass 706 checks each.

Source identities for this review:

| File | SHA-256 |
| --- | --- |
| `tests/test_encoder_wire_request.cpp` (corrected) | `aff5026e9583b09e73c1487e0b76526aa3b1410162935ed739243c73bb72032e` |
| `tests/test_encoder_wire_request.cpp` (original at `29ac8993`) | `a0e1ddc325edd26dfa6f7a197d246db281401cdf5abe4ebe5553c2639145bf5f` |
| `src/core/encoder_wire_request.cpp` (restored, unchanged) | `949286434ac89042c0a979d0ea50601392e54a7533d69d6a7b1e3697755489b9` |

Focused reproduction after the standard SDK-free Release configuration:

```sh
cmake --build build --target test_encoder_wire_request_normal \
  test_encoder_wire_request_optimized test_encoder_native_planes_normal \
  test_encoder_native_planes_optimized --parallel 2
ctest --test-dir build -C Release --output-on-failure --verbose \
  -R '^encoder_(wire_request|native_planes)_'
```

The #405 documentation-only correction at
`7d668d304135a296fced808b9cb29ab87ad40a60` is incorporated into this branch.
Its separate allocation-instrumentation evidence closeout is anchored to that
exact #405 source in [independent review 5971695746](https://github.com/louis-0hrs/HyperCine/pull/405#pullrequestreview-5971695746).

The final local strict standalone gate on Linux x86_64 / GCC 14.2 Release
registered and passed **162/162 tests**, zero failures or skips, in 148.74 s
with two CTest slots and required external FFmpeg decoding enabled. The two
wire registrations also passed in a separately compiled ASan+UBSan build
(205 checks each; leak detection disabled). No production source, CTest
registration or workflow changed in this correction.

The publication commit deliberately uses `[skip ci]` under the existing hosted
CI budget restriction. The new assertions therefore have local Linux evidence;
earlier Windows/Linux/sanitizer receipts belong to `29ac8993`, not the changed
test revision. Final native/hosted acceptance remains separate. The stacked
PR target also leaves portable-quality-state and RCA160 workflows absent;
their required final integration coverage is not waived.

These checks qualify internal CPU path consistency. They do not establish
decoded-source pixel correctness, external decoder interoperability, native
GPU operation, original-SDK equivalence or SMPTE VC-5 conformance.
