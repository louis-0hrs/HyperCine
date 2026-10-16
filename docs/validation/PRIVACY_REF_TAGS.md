<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Ref and annotated-tag privacy gate — issue #96

Base: PR #95 `00a99f54aff34c78ffec4404e095c76eb553fdd3`, stacked on
PR #92 `b21edb17bbe62926cc7dc4ebc4fffaacd5d75d6b`. This is an isolated
follow-up; neither parent branch nor protected main is changed.

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

Linux and Windows native results are not claimed. The real-hook test class is
POSIX-only; Windows Python skips it. Hosted CI and independent review are
separate exact-head gates. Existing CI begins after publication and does not
run on tag pushes; release services without installed hooks are outside this
local gate. See `docs/PRIVACY.md` for supported object types and limitations.

Next action: review this child independently before deciding integration into
#95/#92. Historical disclosure remediation remains with #54.
