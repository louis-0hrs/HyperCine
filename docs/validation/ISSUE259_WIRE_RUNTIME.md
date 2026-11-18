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
request
reaches the existing `EncoderSession` and its own backend selection and writer.
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
- Metal, CUDA and Vulkan native requests are refused with
  `ValidButNotImplemented`, including when `allow_cpu_scalar_fallback` is set.
  Backend fallback never repairs a missing planes frontend.
- Invalid geometry and a required original-SDK oracle are refused before any
  session object or factory call.
- A native wire session refuses the interleaved `Encode(bytes, stride)` entry with
  `std::invalid_argument`; admission does not create a byte path.

The `encoder_wire_request_normal` and `encoder_wire_request_optimized`
registrations cover these cases (144 checks each). Two source mutations were
executed and restored byte-identically: routing native through the BGRA gate
fails with `native CPU wire request refused`, and routing every request through
the native gate fails the pre-existing BGRA admission with
`CFHD capability refused`. Neither run changed a test or an expectation.
