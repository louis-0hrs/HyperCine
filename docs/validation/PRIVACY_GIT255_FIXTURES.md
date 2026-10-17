<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Legacy remote fixtures across Git versions — issue #96

Parent: PR #112, `c829a245154924410a6349284de40ff84a33379c`.
The hosted Git 2.55 failure on #110/#112 occurs while creating nested remotes
with `git remote add`, before either namespace rejection assertion runs.

Construct these legacy configurations with `git config remote.<name>.url`
and `git config remote.<name>.fetch` instead. Assert that Git lists the remote,
then run the original actual fetch/push and redacted rejection checks. The
private imported commit must still remain unpublished in the disposable bare
public remote. No feature-detection skip, lowered assertion or production
scanner change is introduced. Existing configurations remain the attack input
regardless of whether a newer Git permits creating them via its porcelain.

Reproduction: `python3 -m unittest discover -s ci -p 'test_*.py'`.
Native macOS ARM64 / Apple Git 2.50.1 / Python 3.9.6: **92/92 PASS**, including
all **46 installed-hook methods**, zero skips. Native Release full standalone
CTest with required FFmpeg n8.0 decode is recorded in the PR handoff.
Hosted Git 2.55 execution is a separate required confirmation after publication;
the native 2.50.1 result is not reported as a 2.55 test.

Run the full CTest gate after configuring the SDK-free standalone build with
upstream, benchmarks and CUDA disabled and external decode required:
`ctest --test-dir build -C Release --output-on-failure`.
Raw logs stay outside Git. No codec, upstream or other active branch changes.
The parent security stack and historical remediation remain independently open.
