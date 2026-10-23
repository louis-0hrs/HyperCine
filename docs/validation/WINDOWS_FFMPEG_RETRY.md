# Windows Chocolatey feed retry qualification

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

Issue #99 follow-up, stacked on PR #100 parent
`5c564b15ea49af63a8d1abe7f27fdeb9a5ee092a`.

The real Chocolatey message `Response status code does not indicate success:
503 (Service Unavailable)` previously failed closed without retrying. Recognize
that wording for HTTP 429 and 5xx, preserving independent executable probes and
the three-attempt bound. Permanent 4xx errors still fail without retrying.

Native Windows x64 validation on 2026-10-16 (Python 3.10.9, MSVC 19.44,
CMake 3.31.6):

- New regression tests before correction: 11 failing assertions across two test
  methods; the permanent-status control passed.
- `python -m unittest discover -s ci -p 'test_*.py'`: 35 discovered,
  32 passed, three existing POSIX-specific skips. All 16 installer tests passed.
- Fresh SDK-free Release configure and full build succeeded.
- `ctest --test-dir build -C Release --output-on-failure`: 18/18 passed,
  no skips, 34.48 seconds.
- FFmpeg/ffprobe unavailable locally; external decode was not registered.
  Actual hosted Windows installation and required external decoding remain a
  separate exact-head CI gate. No live package-feed outage was induced.

Only the classifier, deterministic tests and this report change. Frozen upstream
and codec sources are untouched. No performance or sanitizer claim. This is a
fresh local implementation with pseudonymous Git identity; it does not reuse the
metadata-blocked review branch's commit or resolve historical metadata exposure.
