# Combined internal profile/session and original-reference gates

## Source and resolution contract

Base: `04c348f848de1eb3a47d3b90fe191722c1ab559e`.

- [#320](https://github.com/louis-0hrs/HyperCine/pull/320): 6fbee4a072681247feef55b9cf1317eceecdced9
- [#322](https://github.com/louis-0hrs/HyperCine/pull/322): c9172d497e3de5d827e0d4b77439ca910fe89af7
- [#331](https://github.com/louis-0hrs/HyperCine/pull/331): 59febad252d0ca9f65790542622a11b034b1a039

Three-parent merge; canonical #320 first parent. An ours merge scaffold establishes the source parents, then its tree is explicitly replaced by the independently reconstructed complete union before commit. No ours-only tree is committed.

The merge preserves all three original commit histories. Existing Draft source
branches are not rewritten. The executable union equals the independently
reconstructed and previously CPU-tested tree above; those earlier results are
historical, not fresh integration-commit execution. New receipts are published
on this Draft integration PR and name the actual tested commit.

## Exact contribution and overlap inventory

- #320: all canonical implementation, tests, build definitions and evidence retained.
- #322: all 15 added files and its build registrations imported at final head,
  including strict integer/frame/state schema checks and regression controls.
- #331: all three added files and its quality-only Group-A registrations imported.
- CMakeLists.txt is the explicit union of non-overlapping blocks, not a replacement
  of canonical GPU/session registrations. The full blob inventory is in the JSON.
- Only the two branch-local PROJECT.md status replacements are omitted.
  Combined status is prepended to canonical PROJECT.md; older status stays historical.
- No duplicate quality-state ladder patch; src/, include/, upstream/, existing GPU
  matrix tests and CI remain byte-identical to canonical #320. No private corpus edit.

| PR | Path | Resolution |
| --- | --- | --- |
| #320 | .gitignore | unchanged from source |
| #320 | CMakeLists.txt | explicit union of all registrations; base definitions occur once |
| #320 | PROJECT.md | branch-specific status omitted; combined status replaces it, canonical history retained |
| #320 | ci/run_standalone.py | unchanged from source |
| #320 | ci/test_standalone_gate.py | unchanged from source |
| #320 | docs/ENCODER_BACKEND_SELECTION.md | unchanged from source |
| #320 | docs/ENCODER_CAPABILITY_CONTRACT.md | unchanged from source |
| #320 | docs/ENCODER_PROFILE_SESSIONS.md | unchanged from source |
| #320 | docs/PROFILE_BACKEND_INTEGRATION_259.json | unchanged from source |
| #320 | docs/PROFILE_BACKEND_INTEGRATION_259.md | unchanged from source |
| #320 | docs/README.md | unchanged from source |
| #320 | src/core/encoder_backend_selection.cpp | unchanged from source |
| #320 | src/core/encoder_capability.cpp | unchanged from source |
| #320 | src/core/encoder_session_config.h | unchanged from source |
| #320 | tests/test_encoder_backend_selection.cpp | unchanged from source |
| #320 | tests/test_encoder_capability.cpp | unchanged from source |
| #320 | tests/test_encoder_cuda_profile_matrix.cpp | unchanged from source |
| #320 | tests/test_encoder_metal_profile_matrix.cpp | unchanged from source |
| #320 | tests/test_encoder_profile_sessions.cpp | unchanged from source |
| #322 | CMakeLists.txt | explicit union of all registrations; base definitions occur once |
| #322 | PROJECT.md | branch-specific status omitted; combined status replaces it, canonical history retained |
| #322 | docs/.gitattributes | unchanged from source |
| #322 | docs/SDK_GAP_ORACLES_46.md | unchanged from source |
| #322 | docs/SDK_GAP_PARITY_46.json | unchanged from source |
| #322 | docs/SDK_GAP_TRAJECTORY_46.json | unchanged from source |
| #322 | docs/SDK_GOLDENS_CURRENT_46.json | unchanged from source |
| #322 | docs/SDK_GOLDENS_REFRESH_46.json | unchanged from source |
| #322 | docs/SDK_GOLDENS_REFRESH_46.md | unchanged from source |
| #322 | tests/test_p6_sdk_gap_gate.py | unchanged from source |
| #322 | tests/test_p6_sdk_gap_integration.py | unchanged from source |
| #322 | tests/test_p6_windows_sdk_gate.py | unchanged from source |
| #322 | tests/test_p6_windows_sdk_integration.py | unchanged from source |
| #322 | tools/p6_sdk_gap_gate.py | unchanged from source |
| #322 | tools/p6_windows_sdk_gate.py | unchanged from source |
| #322 | tools/sdk_gap_corpus.py | unchanged from source |
| #322 | tools/sdk_packet_capture.cpp | unchanged from source |
| #331 | CMakeLists.txt | explicit union of all registrations; base definitions occur once |
| #331 | PROJECT.md | branch-specific status omitted; combined status replaces it, canonical history retained |
| #331 | docs/ISSUE46_GROUP_A_REPLAY.md | unchanged from source |
| #331 | tests/test_issue46_group_a_verifier.py | unchanged from source |
| #331 | tools/verify_issue46_group_a.py | unchanged from source |

## Portable acceptance commands

Set BUILD_CPU, BUILD_QUALITY and REPORT_DIR to fresh external directories.
Set HYPERCINE_SDK_GAP_CORPUS_ROOT, HYPERCINE_SDK_GAP_INDEX_SHA256,
HYPERCINE_WINDOWS_SDK_ROOT and HYPERCINE_ISSUE46_GROUP_A_CORPUS privately
to the existing independently pinned read-only corpus. Do not substitute regenerated fixtures.

    cmake -S . -B "$BUILD_CPU" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF -DHYPERCINE_ENABLE_VULKAN=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON -DHYPERCINE_SDK_GAP_CORPUS_ROOT="$HYPERCINE_SDK_GAP_CORPUS_ROOT" -DHYPERCINE_SDK_GAP_INDEX_SHA256="$HYPERCINE_SDK_GAP_INDEX_SHA256" -DHYPERCINE_WINDOWS_SDK_ROOT="$HYPERCINE_WINDOWS_SDK_ROOT"
    cmake --build "$BUILD_CPU" --parallel 2
    python3 ci/run_standalone.py --build-dir "$BUILD_CPU" --system Linux --machine x86_64

    cmake -S . -B "$BUILD_QUALITY" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF -DHYPERCINE_ENABLE_VULKAN=OFF -DHYPERCINE_ISSUE46_GROUP_A_CORPUS="$HYPERCINE_ISSUE46_GROUP_A_CORPUS"
    cmake --build "$BUILD_QUALITY" --parallel 2
    ctest --test-dir "$BUILD_QUALITY" -C Release --parallel 2 --output-on-failure --output-junit "$REPORT_DIR/quality.xml"

The strict standalone runner records its own completed JUnit and gate JSON.
Quality-only has a separate inventory: do not count Group-A packet-table replay
as a standalone registration. Require unique registered/completed cases, numeric
process exit zero and zero failures/skips before claiming full acceptance.
Retain command lines, compiler/OS, complete JUnit hashes and blob maps outside Git.

## Qualification boundaries

- CPU scalar admits six profiles; production CUDA/Metal FilmScan3-only.
- SDK packet tables are observable; unobserved SDK counters are not inferred.
- Historical source-to-binary/compiler/ISA, seed/explicit FRMR, GPU-original and independent pixel-oracle qualification are not certified.
- #187 HOLD; #46/#259/#321 remain open; no release or conformance claim.
- Original historical Medium/High 78/120 failures remain failed; the original
  40-frame ramp remains NON_QUALIFYING_NO_HOLD. Do not relabel either.
- Original A/B/C artifacts replayed on Linux are not Windows-native execution.
- Directly captured Group-C state is distinct from Group-A observable packet tables.
- Native A2/Metal/Windows and hosted CI are PENDING for the eventual integration
  commit until source-bound receipts are published. Previous #320 receipts remain
  separately attributed; unchanged blobs are not automatically new native passes.
- Native GPU profile matrices use measurement-only seams and real dispatch; they
  do not grant production admission or prove original-SDK GPU parity.
- No main merge, release, broad GPU-profile, sanitizer/performance or SMPTE/VC-5 claim.
