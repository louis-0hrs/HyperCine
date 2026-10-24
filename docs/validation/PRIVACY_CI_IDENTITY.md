<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Commit identity checks through the publication CI command

Refs #54 and #114. Content reviewed in PR #114 as a test-only child of the
nested-ref fix from PR #109. In this combined child the ten identity methods
are carried by `aa0c61373e34d945bb084a6150d65e76534fb930`, and the #109 scanner
fix is cherry-picked as `bb8a977`.

`ci/test_privacy_ci.py` extracts and executes the actual Bash `run` block of
the publication workflow's commit-inspection step. Each test creates a local,
disposable Git repository containing the current scanner. Global/system Git
configuration and inherited Git environment overrides are excluded. No test
pushes to GitHub or reproduces actual contributor metadata.

Ten test methods exercise 22 workflow executions:

- PR and first-push ranges reject unapproved synthetic agent author and
  committer identities, together and independently, with redacted diagnostics.
- GitHub noreply and neutral agent identities pass, as do explicit human
  identities on a public domain; this preserves the existing policy.
- A later safe commit cannot conceal an intermediate rejected identity.
- Existing-branch pushes use their before SHA; empty or unavailable before
  SHAs use the main merge base.
- Missing PR bases or first-push merge bases fail closed; empty ranges pass.

Three further methods in the same file verify that replacement objects cannot
hide commit content, direct-mode identities or staged additions. The existing
workflow discovery pattern includes the file automatically. The scanner rules,
hooks and workflow are unchanged. This checks the existing agent-name
heuristic, not whether arbitrary human-looking names belong to agents. CI still
runs after publication and cannot prevent initial disclosure.

## Reproduce

Run from the repository root on Linux or macOS with Bash, Git and Python 3.8+:

```sh
python3 -m unittest discover -s ci -p test_privacy_ci.py -v
python3 -m unittest discover -s ci -p 'test_*.py' -v
```

The class intentionally skips native Windows because the workflow runs on
Ubuntu. Native Git-for-Windows hook qualification remains a separate gate.

## Verification

- Hosted Ubuntu, run 39013709166: the `Publication privacy (all new commits)`
  log lists all ten identity methods as `ok` and reports `Ran 93 tests`,
  `OK`. This is the required production-shell execution.
- Native Windows Git Bash, 2026-10-20, Git 2.56.0.windows.2 and Python 3.12.10:
  10/10 PASS, 122.182 s, with two explicit deviations on a scratch copy only -
  the POSIX-only decorator was removed, and the `python` symlink was replaced
  by an equivalent shell wrapper because the host lacks symlink privilege.
  This is supporting shell evidence, not an Ubuntu acceptance gate.
- Native macOS ARM64, 2026-10-17, AppleClang 21.0.0, CMake 3.31.5,
  Python 3.9.6, FFmpeg n8.0: new workflow suite 10/10 PASS; all Python suites
  99/99 PASS, zero skips; standalone Release CTest 28/28 PASS, zero skips,
  75.57 s including required external FFmpeg decode. A temporary mutation
  removing commit-envelope inspection made the PR rejection test fail as
  expected; it was never applied to the repository.

No codec, frozen upstream, sanitizer, performance or historical privacy
remediation claim follows from these publication-control tests. The security
stack still needs its separate integration and platform acceptance gates.
