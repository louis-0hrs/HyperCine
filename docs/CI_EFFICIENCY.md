<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Hosted CI event budget and review contract

Tracked under [infrastructure issue #349](https://github.com/louis-0hrs/HyperCine/issues/349).

## Why

On 2026-10-27, GitHub Actions recorded 710 workflow runs (424 push and 286
pull_request). Feature-branch commits with an open pull request sometimes
started the same Windows reference test and the five-job Standalone matrix
twice, and several successive commits kept older Standalone matrices running.
This document changes scheduling only. Dollar savings require later measurement;
the billing gross amount is not an invoice.

## Automatic coverage after this change

| Workflow | Feature-branch push | Pull request | main push | Tag push | Manual |
| --- | --- | --- | --- | --- | --- |
| CPU Reference CI | No | Yes | Yes | Yes | No |
| Standalone CFHD CI | No | Yes, paths-filtered | Yes, paths-filtered | No | Yes |
| RCA160 Python compatibility | No | Yes, paths-filtered | Yes, paths-filtered | Yes, paths-filtered | Yes |
| Portable quality state | No | Yes, paths-filtered | Yes, paths-filtered | Yes, paths-filtered | Yes |
| CUDA build (Linux, compile-only) | No | Yes, paths-filtered | Yes, paths-filtered | No | Yes |
| Privacy guard / publication-privacy | **Yes, every branch** | **Yes** | **Yes** | No (tag refs excluded by existing filter) | No |
| Privacy guard / Windows installed hooks | No | Yes | Yes | No | No |

The independent **Publication privacy (all new commits)** job remains unchanged
and is the mandatory status check on protected main. Its full-history commit
range and synthetic negative controls are not relaxed. Local pre-publication
privacy hooks remain mandatory regardless of hosted CI decisions.

The Standalone five-job matrix is unchanged: macOS arm64 Release + ASan/UBSan,
Windows MSVC Release, Linux Release + ASan/UBSan. Tests, validation manifests,
path filters, timeouts, native architecture checks and artifacts are unchanged.
The CPU Reference gate remains separate from Standalone.

The two new concurrency groups identify PRs by PR number. When a newer commit
arrives on the same PR, its obsolete Standalone/Quality State run can be
cancelled. Other events use the unique github.run_id; distinct main pushes,
tag pushes and manual qualifications therefore do not cancel each other.
The existing CPU/Python/CUDA concurrency settings are left unchanged.

Feature-branch pushes without an open PR are **not** a substitute for hosted
compile/test qualification. Open a PR before claiming hosted readiness. Agent
handoffs must cite exact source SHA and which native tests actually ran.
A skipped check, no run and a cancelled superseded run are **not PASS**.

## Initial qualification and rollback

The infrastructure change is published as one `[skip ci]` commit at the owner's
request. GitHub skips push/pull_request Actions on that commit, so **no hosted
or native CTest pass is claimed**. The mandatory publication-privacy check will
remain pending and the Draft PR must not be merged as-is.

Before integration, the maintainer must explicitly authorize an exact-head,
non-skipped qualification PR revision and verify:
- Required publication privacy job passes on the final review head.
- Five Standalone jobs, CPU Reference, and applicable Python/Quality/CUDA jobs
  report real results; no changes to test inventories or thresholds.
- Push to a development branch does not duplicate hosted compile matrices;
  a later commit to the same PR cancels superseded Standalone/Quality State.
- Separate main/tag/manual dispatches retain their intended coverage.
- No secrets, paths, private corpora or frozen upstream changes were published.

If an expected event no longer runs, revert the trigger/concurrency-only
commit through a reviewed PR. Do not weaken the privacy status requirement
or silently remove job/test coverage to restore green status.
