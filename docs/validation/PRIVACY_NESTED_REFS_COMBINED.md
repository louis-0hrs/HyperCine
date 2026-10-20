<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Nested ref-name guard on the combined security head

Issue: #96. This child follows PR #202 at
`7b17c0409ce8046355a88b524caa2af10ad6ad94` and imports the reviewed fix and
tests from PR #109 (`f497234b12be6382164c2c8576c0efc5b3edc109`). The workflow
identity tests introduced by PR #114 are already carried in this line at
`aa0c61373e34d945bb084a6150d65e76534fb930` and were re-verified separately.

## Defect reproduced at the parent head

`ref_findings` scanned the literal ref and only the suffix after the first two
namespace components. A home-shaped path starting one component deeper was not
recognised, because the home rules expect a leading separator, drive prefix or
tilde that a nested component does not provide.

Reproduced with synthetic names only, no real user data:

| Synthetic ref | Parent head | This child |
| --- | --- | --- |
| `refs/heads/topic/Users/synthetic-agent/review` | no finding | `PERSONAL_ABSOLUTE_PATH` |
| `refs/heads/topic/root/synthetic-agent/review` | no finding | `PERSONAL_ABSOLUTE_PATH` |
| `refs/heads/c/Users/synthetic-agent/review` | `PERSONAL_ABSOLUTE_PATH` | `PERSONAL_ABSOLUTE_PATH` |
| `refs/archive/deep/users-guide` | no finding | no finding |

The fix inspects each component as the start of a bounded three-component
window and scans both the raw and the `/`-prefixed form. Existing literal and
single-suffix scanning is preserved.

End to end, with the installed pre-push hook and a disposable bare remote, the
parent head accepted a push of that synthetic nested ref. This child rejects
the same push with `[LOCAL_REF]:0: PERSONAL_ABSOLUTE_PATH`, does not echo the
raw ref, and leaves the destination ref absent.

## Changes

Only the reviewed #109 content is added: `ci/privacy_guard.py`,
`ci/test_privacy_guard.py`, `ci/test_privacy_hooks.py`, `docs/PRIVACY.md` and
`docs/validation/PRIVACY_NESTED_REFS.md`. Scanner rules, hook entry points,
workflow, codec sources and frozen `upstream/` are unchanged.

## Verification

Platform: Windows x64, Git 2.56.0.windows.2, Python 3.12.10, CMake 3.31.12.
All remotes are disposable local bare repositories; nothing is pushed to
GitHub, and diagnostics were checked for redaction.

- Parent-head installed-hook matrix, unmodified: 96 tests, 96 passed, zero
  skips, 1915.443 s (48 SHA-1 and 48 SHA-256 cases).
- Child scanner suite: 43 tests, 39 passed, four existing POSIX shell skips,
  zero failures, 335.427 s.
- Child nested-ref hook cases: 3 passed, 70.222 s, covering local and
  destination refs plus clean nested ref and delete.
- Child full installed-hook matrix: see the run recorded in the PR handoff.
- PR #114 evidence: the hosted Ubuntu `Publication privacy` job in run
  39013709166 lists all ten `WorkflowIdentityTests` as `ok` (`Ran 93 tests`,
  `OK`). The same file also passed 10/10 under native Windows Python with Git
  Bash while using two documented deviations: the POSIX-only decorator was
  removed from a scratch copy, and the `python` symlink was replaced by an
  equivalent shell wrapper because this host lacks symlink privilege. The
  native suite still skips the class by design.

## Limits

This is Windows and Git Bash evidence, not native Linux or Ubuntu hook
execution. No MSVC CTest claim is made because no C++ or CMake source changed.
Historical public objects remain issue #54 work. No main merge, tag
publication, release or history rewrite is claimed or performed.
