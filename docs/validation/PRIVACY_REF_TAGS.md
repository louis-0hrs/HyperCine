<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Ref and annotated-tag privacy gate — issue #96

## Historical macOS qualification — PR #98

The original isolated follow-up was based on PR #95
`00a99f54aff34c78ffec4404e095c76eb553fdd3`, stacked on PR #92
`b21edb17bbe62926cc7dc4ebc4fffaacd5d75d6b`. The results and reproduction
commands below describe that historical scope, not the current test inventory.

Native validation: macOS ARM64, Python 3.9.6, Apple Git 2.50.1,
AppleClang 21.0.0, CMake 3.31.5. Build and raw logs are outside the Git tree.

| Check | Actual result |
| --- | --- |
| Before implementation, initial expanded installed-hook suite | 27 executed; 12 failed, reproducing missing ref/tag rejection |
| Final full Python discovery | 79/79 PASS, no skips |
| Installed-hook subset within the 79 | 35/35 PASS; 23 added regression tests |
| Existing privacy detector subset within the 79 | 38/38 PASS |
| Native standalone Release full CTest | 28/28 PASS, 76.86 seconds; includes external FFmpeg decode |
| Shell syntax, whitespace, frozen upstream status | PASS |

The 23 new installed-hook cases use disposable local named bare remotes and
synthetic inputs. They cover annotations on already-published commits, raw
tagger email/name, host-derived identity, nested annotations, local/destination
branch/tag names, home-shaped names, clean nested/annotated/lightweight tags,
clean and sensitive tag repointing, automatic follow-tags, mixed-ref atomic
hook rejection, safe deletion, unsupported blob/tree targets, missing tagger,
non-UTF-8 metadata, internal tag names and replacement-object concealment.
Rejected diagnostics are checked for rule markers and absence of the synthetic
sensitive value. No public test tag was pushed and no hooks were disabled.

Reproduce from the repository root with FFmpeg/ffprobe on PATH:

```sh
python3 -m unittest discover -s ci -p 'test_*.py' -v
sh -n .githooks/pre-commit .githooks/pre-push
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 3
ctest --test-dir build -C Release --output-on-failure
python3 ci/privacy_guard.py --staged
python3 ci/privacy_guard.py --commits 00a99f54aff34c78ffec4404e095c76eb553fdd3..HEAD
git status --porcelain upstream/
git diff --check
```

The recorded run used a clean external build directory with explicit local
FFmpeg/ffprobe executable configuration. These commands use the equivalent
portable PATH setup. No codec math, bitstream fixtures, core dependencies or
upstream files change; no new performance or SDK-parity claim is made.

That macOS run did not establish native Linux or Windows results. Subsequent
Windows qualification and current test enablement are recorded separately below.

## Current coverage after main integration

The preventive security stack landed through [PR #92](https://github.com/louis-0hrs/HyperCine/pull/92)
at main commit `e8ecdc093030a42bb29e7b8986412302e39f80cb`; see the
[issue #96 completion record](https://github.com/louis-0hrs/HyperCine/issues/96#issuecomment-6309469025).
`ci/test_privacy_hooks.py` now runs both `InstalledHookTests` (SHA-1) and
`Sha256InstalledHookTests` with native Python on Windows and POSIX, without a
Windows platform skip. Git for Windows launches the installed shell hooks.
The `windows-installed-hooks` job in `.github/workflows/privacy-guard.yml`
executes this matrix on Windows; missing prerequisites fail the tests.

The earlier [PR #102 exact-head review](https://github.com/louis-0hrs/HyperCine/pull/102#issuecomment-6223938426)
verified 35/35 hosted Windows installed-hook tests at
`c7e4aa1905f544d542e504a1ded80b8e94470ee6`. The later #96 completion record
reports 102/102 Windows installed-hook tests for the consolidated security
PR head. These are historical exact-head results, not a fresh post-merge main
CI run. Separate POSIX CI-shell and interpreter-stub tests retain their platform
limits. The original #102 branch must not be merged merely to duplicate the
Windows job already on main.

CI begins after publication and has no tag-push trigger; release services
without installed hooks remain outside this local gate. See
[the publication protocol](../PRIVACY.md) for supported object types and limits.
Historical disclosure remediation remains separate in #54.
