<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under the MIT License or Apache License, Version 2.0.
See LICENSE-MIT and LICENSE-APACHE for details. -->

# Nested ref-name guard qualification (issue #96)

Base: reviewed PR #104, `c4ae28ee0539963df13261dc8cdcb6d1c6270cd0`.
This separate child addresses the deeper-prefix boundary reported by the
independent Linux review; it does not integrate sibling PR #102 or change
the parent security branches.

The parent inspected only the first name after the Git namespace. A home-shaped
name nested below a topic, custom prefix or remote-tracking prefix could pass.
The guard now checks each name component using bounded three-component windows
and the existing classifier. It preserves the literal-ref scan, deduplicates
rule names and reports only redacted local/destination markers.

## Native macOS results

ARM64, macOS 26.5.2, Python 3.9.6, Apple Git 2.50.1, AppleClang 21.0.0,
CMake 3.31.5; external FFmpeg/ffprobe n8.0.

- Before correction: the expanded privacy suite ran 83 test methods and
  failed 21 sub-cases: 15 nested-home unit cases and six installed-hook pushes
  (three local refs and three destination refs) that incorrectly succeeded.
- After correction: full Python discovery **89/89 PASS**, including **41/41
  installed-hook methods**, no skips (43.33 seconds overall). Five new methods
  cover home/tilde variants, clean component names, both push roles and actual
  clean nested-ref publication/deletion against disposable local bare remotes.
- Fresh SDK-free Release configure/build and full CTest: **28/28 PASS**, no
  skips, 78.49 seconds, including actual external FFmpeg decode. Optional
  external SDK corpus replays were not configured. No codec source changed.
- Hook shell syntax and executable availability, worktree-local activation,
  staged and outgoing-commit privacy scans, manual diff/identity review and
  frozen-upstream checks are required before publication.

Reproduce with `python3 -m unittest discover -s ci -p 'test_*.py' -v` and
`ctest --test-dir build -C Release --output-on-failure` after configuring the
standalone CPU build with upstream/CUDA/Metal disabled and
`HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON`. Supply local FFmpeg/ffprobe executables
through the existing CMake options. Keep raw logs outside Git.

Native Windows/Linux and hosted results for this new child are not claimed
here. The combined security head still requires sibling integration and
fresh cross-platform qualification. No sanitizer, GPU, throughput or
production-main protection claim; historical exposure remains separate in #54.
