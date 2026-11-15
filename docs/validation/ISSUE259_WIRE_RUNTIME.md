# Private wire request runtime boundary

This internal adapter snapshots the existing session config with an explicit
wire dialect, ST 2073 edition and part mask, and decoder target. It admits only
legacy CFHD with no standard edition or parts claimed and no decoder-specific
requirement. VC-5, unknown dialects, any nonzero parts or edition, and any
specific or unknown decoder target are refused before session construction.
There is no dialect fallback. Decoder-specific interoperability and VC-5
serialization are not implemented or qualified by this boundary.

After wire admission the existing `CheckEncoderCapability` performs its normal
format, backend, geometry, storage and resource checks. Only a permitted request
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
