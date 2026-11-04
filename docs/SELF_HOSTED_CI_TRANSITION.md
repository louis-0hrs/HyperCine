<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Self-hosted CI transition: routing, isolation and verification

Status: **proposal, not enabled on main.** Hardware/pool qualification and
runner registration have not been independently verified here. No hosted
validation or pool execution for this corrected routing head is claimed.

Tracked under [infrastructure issue #349](https://github.com/louis-0hrs/HyperCine/issues/349).
Related: the trigger/concurrency budget in the pending `docs/CI_EFFICIENCY.md`
draft (PR #350). That change decides *when* a workflow starts; this one decides
*where* its jobs run. They are independent and can land in either order.

## 1. What this change does and does not do

It adds a routing layer only:

| Aspect | Change |
| --- | --- |
| Runner selection | `runs-on` expression can select a self-hosted pool when repository variable `HYPERCINE_CI_SELF_HOSTED` is `1` |
| Test commands, inventories, thresholds | unchanged |
| Path filters, `on:` triggers, concurrency | unchanged |
| Job names and matrix rows | unchanged (a required check keeps its name, so branch protection stays valid) |
| `upstream/` | untouched |
| Publication privacy | unchanged, and deliberately excluded from the pool (see §3) |

With the variable unset — the default on every clone, and the state this draft
merges in — the resolved routing table is byte-for-byte the current one. That
is a property the local checker proves, not an intention
(`DefaultRoutingUnchanged` in §5).

## 2. Runner labels and lane assignment

Two labels are required per job: the mandatory `self-hosted`, plus exactly one
`hc-*` capability label. A host carries its capability label only after the
out-of-band qualification below; the label is not a statement about what the
machine happens to be, it is a statement about what was verified.

| Capability label | Host contract | Lanes |
| --- | --- | --- |
| `hc-linux-x64-cpu` | Linux x86_64, CMake, Ninja, GCC, FFmpeg, Git | Standalone Linux Release, Standalone Linux ASan/UBSan, Quality state Linux |
| `hc-linux-x64-cuda12.8` | additionally toolkit 12.8.1 at `/usr/local/cuda-12.8` | CUDA compile (`cuda-build`) |
| `hc-windows-x64-msvc` | Windows x86_64, MSVC 19.x, Git for Windows, FFmpeg, Ninja | Standalone Windows Release, Quality state Windows, CPU Reference Windows |
| `hc-macos-arm64` | macOS arm64 with Xcode CLT, FFmpeg, Ninja | Standalone macOS Release + ASan/UBSan, Quality state macOS |

Deliberately **not** routed to the pool:

- `publication-privacy` and `Installed privacy hooks (Windows)` — see §3.
- `inventory-gate-selftest` (CUDA negative controls) — synthetic-only and ~1 s;
  keeping it on a hosted image means a defect in the gate cannot be masked by
  the runner that the gate is supposed to police.
- `Workflow routing contract` — the gate that judges every other lane must not
  run on the pool it judges.
- `RCA160 Python compatibility` — 10-minute pure-Python matrix; pool capacity is
  better spent on decode lanes.

Two additional labels are **not** used and should not be added later without a
review: a bare `self-hosted` label (any host in the organisation can pick the
job up) and any `*-gpu`, `*-cuda`, `*-8k` label that encodes a corpus or GPU
generation rather than a contract. The verifier rejects a bare `self-hosted`
label outright (R12).

## 3. Protected-main-only trust boundary

A self-hosted runner is **not a sandbox**. A same-repository pull request
can be untrusted just as a fork can, and `workflow_dispatch` accepts an
operator-selected branch/ref (including a separate CUDA `checkout_ref`).
Neither mechanism is safe for executing code on a persistent pool.

The only pool-eligible event is an exact `push` to `refs/heads/main` after
independently verifying that the branch is protected. The expression also
requires `HYPERCINE_CI_SELF_HOSTED=1`. **All PRs (including same-repo PRs),
manual dispatches (even on main), feature pushes and tag pushes stay hosted.**
The static verifier checks these denied routes with the variable forcibly
enabled (R13), and checks that eligible protected-main lanes do route to
the pool (R14).

Self-hosted hosts must nevertheless be isolated and least-privileged:
no deployment credentials, no repository write grants, disposable workspaces,
and explicit trusted-host operators. These are operational requirements;
a `hc-*` label does **not** prove them. The mandatory publication-privacy
job and the routing-policy verifier stay on GitHub-hosted runners.

## 4. Toolkit pinning on a persistent host

The hosted CUDA image installs `cuda-toolkit-12-8=12.8.1-1` from a checksummed
keyring on every run. A persistent host cannot work that way: installing the
toolkit per job would need root, and trusting whatever is already there would
silently change the compiler behind a lane.

So the installer and the disk-cleanup step are skipped on a self-hosted host,
and a new step asserts the contract instead: `${CUDA_HOME}/bin/nvcc` must
report release `12.8`, otherwise the job fails with exit 2. A host with a
newer toolkit is not "close enough" — it is a different lane.

The same principle covers the platform lanes: every self-hosted job first runs
`ci/check_runner_capability.py`, which compares `platform.system()` and
`platform.machine()` against the capability label and fails closed on a
mismatch. A mislabelled host is the realistic failure mode here, and a wrongly
green platform lane is worse than a red one.

## 5. Static verification contract (new head not yet executed)

The corrected verifier models eight events, including manual dispatch on an
untrusted feature branch, and fourteen rules. It resolves actual workflow
expressions with the pool variable forcibly enabled for security checks.
**This correction has not been executed on a Python/Actions runner yet.**

```
$ python3 ci/workflow_event_matrix.py
# NOT RUN on corrected head; prior head's seven-event output is historical
```

Rules, and the failure each one exists for:

| Rule | Assertion |
| --- | --- |
| R1 | `publication-privacy` is scheduled for every branch push and every PR |
| R2 | `publication-privacy` never resolves to a self-hosted label and always has a github-hosted one |
| R4 | every self-hosted job is exactly `[self-hosted, <one hc-*>]` with a known capability |
| R5 | the capability label's platform matches the lane's platform |
| R6 | a fork PR never resolves to a self-hosted label |
| R7 | a docs-only PR does not start the five-job matrix |
| R8 | a source PR still starts exactly the five Standalone rows with unchanged names |
| R9 | a tag push starts neither the decode matrix nor the CUDA compile |
| R10 | a concurrency group is stable across two commits of the same PR |
| R11 | a self-hosted CUDA job verifies the pinned toolkit and does not install it |
| R12 | no bare `self-hosted` label, no `pull_request_target` toward the pool |
| R13 | with variable enabled, every PR, manual, feature and tag event remains hosted |
| R14 | with variable enabled, protected-main push still routes all eligible lanes to the pool |

The negative controls are the point. `ci/test_workflow_event_matrix.py` mutates a
deep copy of the real workflows, one change at a time, and requires the
specific rule to fire — including the two negative checks the maintainer asked
for: removing `push` from the privacy workflow must break R1, and moving
`publication-privacy` to the pool must break R2. It also runs the whole rule
set a second time with `HYPERCINE_CI_SELF_HOSTED=1`, so the enabled
configuration is verified, not just the default one.

```
$ python3 -m unittest ci.test_workflow_event_matrix ci.test_runner_capability
# NOT RUN on corrected head; execute all new negative controls before acceptance
```

`ci/check_runner_capability.py` has its own accepted/rejected matrix per
capability label. On this Linux x86_64 host, `--capability hc-linux-x64-cpu`
returns 0 and `--capability hc-macos-arm64` returns 2 with both a system and a
machine mismatch — the second call is a real execution, recorded here as
evidence that the check fails closed rather than only passing.

## 6. Activation and rollback

1. Verify that `main` is protected, including the mandatory
   `Publication privacy (all new commits)` status, required reviews,
   and restrictions on direct pushes; document the operator-controlled
   runner account and workspace isolation.
2. Register and qualify the individual hosts *out of band* against a
   pinned, trusted `main` SHA using the official test commands. Record
   toolchain, source SHA, test counts, sanitizer and workspace cleanup.
   Do not feed an arbitrary PR or manual `checkout_ref` to the pool.
3. Merge only a separately accepted, exact-head routing change while
   the variable is still unset. With it unset, all jobs stay hosted.
4. Once hardware qualification and review gates pass, set
   `HYPERCINE_CI_SELF_HOSTED=1`. The next eligible **protected-main push**
   will use the pool. Verify source-bound results, labels and the
   unchanged hosted privacy status.
5. **Do not use `workflow_dispatch` to qualify the pool**: even on
   `main` it must route hosted, to avoid the untrusted-ref/checkout
   loophole. PRs also stay hosted.

Rollback: delete the repository variable. Never relax the mandatory
privacy job or add broad labels as a workaround for a failing lane.

## 7. Evidence status

| Item | Status |
| --- | --- |
| Prior routing verifier (parent head only) | HISTORICAL — 7 events/12 rules, did not cover same-repo PR/manual-ref risk |
| Corrected verifier (8 events, 14 rules) | **NOT RUN** — requires exact-head Python execution |
| Previous 37 tests | HISTORICAL — parent head only; new routing negatives **NOT RUN** |
| Corrected pool-enabled event/label matrix | **NOT RUN** — main positive and PR/manual/feature negative controls pending |
| Fork PR hosted gate | HISTORICAL R6; expanded R13 still unexecuted |
| `check_runner_capability.py` on this host, matching label | RUN — exit 0 |
| `check_runner_capability.py` on this host, macOS label | RUN — exit 2, system + machine mismatch |
| Hosted CI for corrected diff | **NOT RUN** — commit uses `[skip ci]` to conserve hosted minutes |
| Any CTest/Standalone/CUDA build | **NOT RUN** |
| Runner registration, labels, service accounts, workspace cleanup | **NOT RUN / operator-attested** |
| Actual job execution on a self-hosted host | **NOT RUN** |

No runner tokens, hostnames, private paths or corpus locations appear in this
document or in the diff. Label names are the only pool-specific strings, and
they are contract names, not infrastructure addresses.
