# Compatibility matrix verification boundaries

Verification checks fixture manifests, declared lengths and SHA-256, packet
roles, capture/expectation roles, packet-to-pattern associations, producer label
and known source commit. Unknown manifest provenance stays null, not invented.
Only the currently measured HyperCine-to-external and legacy-GoPro-to-SDK row
directions are accepted. In particular, these captures cannot qualify a
HyperCine decoder. Each capture decoder identity must match its artifact role.

Committed p10 metrics are recomputed from verified 16-bit little-endian samples.
Numeric scope fields require JSON integers, not booleans or floating-point
values; other declared scope fields require strings. All comparisons, including
recorded local evidence, require the exact sample domain, integer error metrics
and finite numeric-or-null PSNR with consistent error/null semantics.

The validate_matrix API returns a separate compat-verification/1 report without
mutating its matrix input. Each returned row preserves historical_status;
status is derived solely from replayed committed comparisons. Each row and
comparison exposes replay_status. Local captures are unavailable and always
NOT_REPLAY_QUALIFIED, not counted toward new replay qualification. A local-only
historical identity row therefore has current status NOT_OBSERVABLE. Mixed
rows are PARTIAL; their replay verdict covers the committed subset only.
Without a fixture root no comparison is replay-qualified. Verification reports
are not input matrix manifests and should not replace retained evidence.

The --verify CLI prints a machine-readable compat-verification/1 summary with
rows in input order, historical status, replay qualification and per-comparison
replay status. It deliberately omits user-supplied names, tool banners and paths.
Malformed matrix or manifest JSON shapes fail with exit 2 and a fixed sanitized
diagnostic. Success means evidence integrity, not codec correctness,
bidirectional support, application compatibility or VC-5 conformance.

Live version, probe and decode subprocesses have finite time and stream-produced
output caps. Pixels travel through capped stdout, not an unbounded output file.
Errors omit raw banners, argv and paths. POSIX uses a private process group;
Windows assigns a suspended child to a kill-on-close Job Object before resuming,
including the fast-exiting-parent case. Job assignment failure refuses execution.
These contain ordinary descendants, not deliberately hostile executables escaping
POSIX sessions or exploiting the OS. Use trusted locally selected decoders only.

Run tests/test_compat_qualification.py using Python normal and optimized modes.
Synthetic controls do not replace native platform validation or the applicable
full repository suite. Original fixture media and manifests remain immutable.
