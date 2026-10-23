<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# GOP2 reference stack: Windows reconciliation, 2026-10-20

The reference stack is integrated in dependency order: #199's length binding
into #196's representation verifier, followed by #200's acceptance report and
this reconciliation, then the combined #196 branch into reference parent #190.
The existing reference packets, published manifests/indices, production codec
and frozen `upstream/` are unchanged.

## Source identities and historical scope

| Work | Exact source |
| --- | --- |
| #190 reference parent | `4bba30eeba106bdc9e96042d02694c026351a624` |
| #196 representation verifier | `4a9f3f116f1d9202c63b997a971dc5b7199d0e7c` |
| #199 row-length binding | `9c37423eb459fb61ddde198e955577e8cfb53d32` |
| #200 historical Windows report | `8988b49109bced51f9e314894881112c7e435e35` |

The [original Windows report](GOP2_ISSUE189_WINDOWS_READONLY_REVIEW.md) remains
scoped to `PINNED_REFERENCE_REVIEW_ACCEPTED_AT_4a9f3f1`. Its recorded source
hashes, test counts and timings belong to that historical run. They do not
establish execution of the subsequently added #199 guard. This follow-up
records a separate verification at `9c37423`; only documentation changes were
added during integration.

Validator fingerprints in the
[new machine-readable result](gop2_issue189_windows_length_binding_review_2026-10-20.json)
hash exact Git blobs. Checkout CRLF is normalized solely when checking the
executed source against those blobs. The older report hashes Windows checkout
bytes; differing newline-dependent fingerprints are not a codec/source change.

## Read-only reconciliation

The ignored local snapshot created for #200 was reused without copying or
regenerating the corpus. A new full traversal rejected reparse points and
rehashed all **2,144 files / 1,377,743,781 bytes**. Its sorted inventory digest
is unchanged:
`9be8c2f1e0a64c5c3e28a111bffbe4df1dfd5c03c83cad10f934b5e4c5f0b2c0`.
This is identity with the historical local snapshot, not a fresh read of the
network-bound corpus or a replay of its original SDK/CUDA producers.

Both geometries, 1920x1080 and 3840x2160, pass the integrated verifiers:

- 164 Phase-A original artifacts rehashed and 224 comparisons reverified.
- 114 Phase-B original stages rehashed and 180 comparisons reverified with
  #199's exact integer `byte_length == pitch * height` checks on originals and
  both comparison sides. Zero mismatches.
- All 180 Phase-B candidate/reference pairs additionally compared directly as
  byte arrays. All six private manifest/index files equal the published Git
  blobs, avoiding checkout newline conversion.

The new guard accepts the existing references without resealing metadata.
Running the current 17-method stage suite against exact parent `4a9f3f1`
reproduces the **four expected rejection failures**, zero errors. Against
`9c37423`, all **17/17** methods pass. The four cases cover changed band height,
changed signed-plane length, and shortened band/final-plane comparison lengths.

Archived-tool identities and observer regeneration remain historical evidence
from #200. The full snapshot rehash confirms those archived files are unchanged;
this follow-up does not claim a new binary build or original-observer replay.

## Native Windows validation

Windows x64, MSVC 19.44.35229, Python 3.10.9, Release:

- Full CPU Release CTest **16/16 PASS**, zero failures/skips, **28.23 seconds**,
  with `PYTHONOPTIMIZE=1`. Includes both GOP2 integrity suites and all four
  synthetic graph worker configurations.
- Optimized-Python stage methods **17/17**, Phase-A integrity **20/20**, and
  CI inventory helpers **8/8 PASS**. Counts overlap the CTest entries.
- No new CUDA, sanitizer, benchmark or full external-decode-suite result.
  FFmpeg/ffprobe are unavailable locally.

Build/test reproduction from the integrated checkout:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF `
    -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build --config Release --parallel 8
$env:PYTHONOPTIMIZE = '1'
ctest --test-dir build -C Release --output-on-failure
python -O -B -m unittest discover -s ci -p test_standalone_gate.py
```

The existing read-only verifier commands in the historical report reproduce
the artifact checks with the integrated source; exit code 2 intentionally
retains owner review. To use the existing local snapshot, bind it only in the
invoking process. Do not change the persistent corpus binding.

## Remaining decisions

Disposition: `PINNED_REFERENCE_LENGTH_BINDING_PASS_AT_9c37423` for the same two
pinned GOP2 packets and named stages as #200. This closes the verifier/report
version mismatch. Keep #190 Draft and #189 open pending the scoped owner
decision. #2/#13/#16 production promotion, #160 default threading, #144's
architecture contract and main/release decisions remain separate.
