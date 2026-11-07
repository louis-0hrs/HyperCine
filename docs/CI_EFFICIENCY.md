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

## Event routing with workflows enabled (#350-only proposal)

This table is a **YAML trigger matrix**, not evidence that jobs ran or a
claim about the current repository workflow-enable state. Path filters still
apply; a disabled workflow dispatches **zero** jobs even when the table says
"Yes". Neither this Draft nor the separate Mac-only relay Draft #353 is merged
or activated by these instructions.

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

## Cross-PR mode boundary: Mac-only relay proposal (#352/#353)

The later [#353 Mac-only proposal](https://github.com/louis-0hrs/HyperCine/pull/353)
changes the *execution routing* for exactly three Standalone macOS lanes,
not the owner’s non-Mac hosted coverage or the required Actions-owned privacy
job. The five-row matrix above describes #350 **by itself**; do not treat that
hosted-Mac schedule as the production contract after a separately reviewed
#353 stack integration. #353 remains Draft: these descriptions are intended
future states, **not** an assertion of deployment or check success.

| Explicitly selected state | Windows/Linux/CUDA jobs | Three macOS Standalone lanes | Publication privacy |
| --- | --- | --- | --- |
| #350-only, workflows enabled | Hosted where trigger/path filters match | Hosted where trigger/path filters match | Actions-owned, required |
| #353 Mac-only mode, Mac healthy, workflows enabled | Hosted where trigger/path filters match | Private relay, independently verified; **no automatic hosted Mac jobs** | Actions-owned, required |
| #353 Mac-only mode, Mac offline/paused | Hosted jobs continue on eligible events | Fail closed / NOT RUN until owner recovery; **no automatic hosted fallback** | Actions-owned, required |
| Temporary owner-authorized compute-workflow qualification pause | **NOT RUN** for disabled workflows; no synthetic PASS | **NOT RUN** unless independently authorized/verified | Keep required Actions privacy enabled |
| Explicit owner-authorized manual hosted-Mac fallback (future; separately qualified) | Unchanged | Only explicitly acknowledged ref/SHA/owner; no automatic switch | Actions-owned, required |

For a feature push without a PR, #350's de-duplication rules still suppress
the listed compute workflow triggers; the privacy push check remains active.
For eligible main, PR and manual events, Mac-only routing must preserve
non-Mac GitHub-hosted execution even if the Mac is unavailable. No whole-workflow
disable or fabricated/skipped Mac success is a valid steady-state fallback.
The Mac relay/Reporter result must have its own separately qualified, exact
Head/Base and lane-capability evidence. A Mac outage must not switch on paid
hosted runners, reset the persistent pause or bypass owner resumption.

**Static negative cases for integration review:** disabled compute workflow +
matching PR event must produce **zero compute jobs**; Mac-offline eligible PR
must not start any hosted Mac job but must retain non-Mac eligibility; untrusted
manual actor or different SHA must not enable hosted Mac fallback; and a missing
relay result must not be counted as PASS. These are acceptance expectations,
**not executed tests**. Live trigger/cancellation behavior still needs
authorized exact-head qualification. See
[the #353 transition guide](https://github.com/louis-0hrs/HyperCine/blob/infra/premerge-controller-review-20261104/docs/SELF_HOSTED_CI_TRANSITION.md)
and [separate privacy window](https://github.com/louis-0hrs/HyperCine/blob/infra/premerge-controller-review-20261104/docs/INTEGRATION_PRIVACY_WINDOW.md);
the temporary cost window is **not** permanent Mac-only routing.

## Initial qualification and rollback

The infrastructure change is published as one `[skip ci]` commit at the owner's
request. GitHub skips push/pull_request Actions on that commit, so **no hosted
or native CTest pass is claimed**. The mandatory publication-privacy check will
remain pending and the Draft PR must not be merged as-is.

Before integration, the maintainer must explicitly authorize an exact-head,
non-skipped qualification PR revision and verify:
- Required publication privacy job passes on the final review head.
- For #350-only hosted mode, all five Standalone jobs, CPU Reference and
  applicable Python/Quality/CUDA checks report real results. For a separately
  authorized #353 Mac-only stack, two non-Mac Standalone jobs remain hosted
  while three actual Mac lanes require separately qualified relay receipts;
  do not require or launch paid hosted Mac jobs by accident. No changed
  test inventories or weakened thresholds in either mode.
- Push to a development branch does not duplicate hosted compile matrices;
  a later commit to the same PR cancels superseded Standalone/Quality State.
- Separate main/tag/manual dispatches retain their intended coverage.
- No secrets, paths, private corpora or frozen upstream changes were published.

If an expected **enabled and eligible** event no longer runs, investigate and
revert the trigger/concurrency-only commit through a reviewed PR if needed;
do not misdiagnose an intentionally disabled workflow as a trigger regression. Do not weaken the privacy status requirement
or silently remove job/test coverage to restore green status.
