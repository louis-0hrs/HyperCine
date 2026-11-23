# FFmpeg oracle subprocess safety — scoped Draft (#381, 2026-11-22)

This branch is stacked on the reviewed Draft #409 **HEAD 95089fc519ad0b1671179c4180967131eb09781b**, not on protected main. It adds process and privacy defenses only. It does not qualify SDK, GoPro, SMPTE VC-5 or GPU outputs, and it does not change the three pinned fixtures, expected reference hashes, binary allowlist, or frozen `upstream/`.

## Enforced contract

- `ffmpeg -version`: existing `tools/compat_process.run_bounded` with 30 s timeout and 65,536-byte stdout/stderr budgets. Failure/unavailable tool blocks with exit 77.
- `ffprobe -show_streams`: the same supervisor with 60 s / 65,536 bytes; nonzero exit, timeout, oversized data, invalid UTF-8/JSON, wrong stream or schema fail closed. No external stderr or raw stream fields are included in the public reason.
- `ffmpeg` decode: 120 s / **exactly 65,536 bytes** stdout cap, rawvideo to `pipe:1`, strict 65,536-byte output length and 10-bit sample bounds. No unbounded intermediate `.yuv` file or `Path.read_bytes()` on decoded tool output.
- Supervisor retains POSIX session-group and Windows Job Object ownership and bounded stderr. Cleanup/containment uncertainty blocks instead of silently allowing qualification; no untrusted command-line, local paths, vendor banner, stderr or executable diagnostics appear in public reasons.
- Invalid or unlisted banners are blocked without echoing original bytes. Successful reports print the exact measured Debian build ID or a static legacy build-family name, never the legacy vendor suffix. Executable admission and the legacy token-only disclaimer are unchanged.
- After executable admission, the fixture manifest is read as bounded UTF-8 JSON (16 KiB). The full standalone inventory, provenance fields, exact entry keys, unique fixture names and integer byte counts/lowercase SHA-256 declarations are required before any fixture data is loaded. Duplicate JSON keys, invalid encodings, excessive nesting and malformed inventories fail with exit 1 and fixed diagnostics.
- CTest registers `ffmpeg_oracle_process_contract` and `ffmpeg_oracle_process_contract_optimized` in both regular and SDK-free standalone modes. Combined with synthetic-ledger Draft #408, the expected registry becomes **169**, not the prior 167, pending actual CMake inventory enumeration.

## Independent acceptance still required

The new commit has **NOT_RUN** host-bound CTests/Windows/MSVC/Linux/ASan+UBSan/macos, and the exact-head private agent must verify source, build, normal and optimized test results, exact SHA tree and privacy. The new Python tests cover mocked budget wiring, 65,535/65,536/65,537 byte controls, public sentinel suppression, real local supervisor output/timeout and POSIX external-stderr shims. The integration tests also witness a real descendant PID and verify termination via native Windows process handles or Linux procfs after timeout and stderr overflow. Hosted/native execution receipts must still be recorded separately for each candidate.

The platform agent must:
1. Check out the exact branch HEAD in an isolated clean worktree. Run the new test script using `python -B` and `python -B -O`, record command and exit code, unittest cases/skips and SHA-256 of the complete logs.
2. Run clean strict standalone CTests and reconcile registry, JUnit and source file identities. For Windows, run the contained hung-parent + spawned-child negative with independent child PID/liveness verification; report any actual failures (do not mark skipped cases PASS).
3. Run the genuine measured Debian FFmpeg pair when available (byte hashes must match the existing note), demonstrate unchanged packet output hashes and cross/mutation discrimination; other platforms with unmatched tools report `77=BLOCKED`, not qualification.
4. Exercise a deliberate safety mutant (e.g. delete the output-length check or error-output sanitization) in an isolated copy. The specific corresponding negative test must turn RED; restored tree GREEN in both Python modes. Keep originals and protected main untouched.
5. Check staged/commit/patch/every-outgoing privacy gates; keep public evidence free of private binaries, local network/host details and golden corpora.

No Ready transition, merge, or release is justified by this source-only implementation. #259/#46 and actual normative-reference/SDK/product/GPU qualification remain separate.
