<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Self-hosted CI transition: routing, isolation and verification

Status: **proposal, not enabled.** Every lane in this document is still on its
github-hosted label on `main`. No runner is registered, no workflow has been
dispatched, and nothing here has been executed on hardware.

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

## 3. Isolation of untrusted pull-request code

A self-hosted runner is a machine, not a sandbox. Two properties of GitHub
Actions make that dangerous, and both must be addressed by design, not by a
label:

1. **`pull_request` from a fork** runs the *merge commit* of untrusted code.
   Fork pull requests therefore never select the pool: the `runs-on`
   expression includes `github.event.pull_request.head.repo.fork != true`, so a
   fork PR falls back to its github-hosted label even with the variable enabled.
   This is checked by rule R6 against a real fork scenario, and the first
   version of this diff failed that rule — the guard exists because it was
   missing.
2. **Same-repository branches are not automatically trusted.** A contributor
   with write access, or a compromised token, is enough. The pool therefore
   runs with these invariants:

   - No repository, organisation or environment secret is exposed to a
     `pull_request` event (`secrets.*` are already empty for that event; no lane
     in this repository reads one).
   - The runner service account has no write access to the repository and no
     deployment credentials; it can read and execute, nothing else.
   - Each job runs in a workspace that is created for the job and deleted
     afterwards, not a shared directory reused across jobs.
   - No job has `actions: write`, `packages: write` or `id-token: write`; the
     existing `permissions: contents: read` is kept as-is.
   - `pull_request_target` is not used anywhere, and the verifier fails if a
     `pull_request_target` workflow ever targets the pool (R12).

   These are properties of the runner configuration, not of the YAML, so they
   cannot be proven by this repository. They are listed as **NOT RUN /
   operator-attested** in §7 and must be confirmed before the variable is
   enabled.

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

## 5. Local verification (executed)

`ci/workflow_event_matrix.py` parses the real workflow files, applies seven
synthetic events (PR with sources, docs-only PR, feature-branch push, `main`
push, tag push, manual dispatch, fork PR), resolves `if:` and `runs-on:` the
way Actions documents them, and checks twelve rules. It never dispatches
anything.

```
$ python3 ci/workflow_event_matrix.py
OK: 7 workflows, 7 events, 80 job instances, 12 rules
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

The negative controls are the point. `ci/test_workflow_event_matrix.py` mutates a
deep copy of the real workflows, one change at a time, and requires the
specific rule to fire — including the two negative checks the maintainer asked
for: removing `push` from the privacy workflow must break R1, and moving
`publication-privacy` to the pool must break R2. It also runs the whole rule
set a second time with `HYPERCINE_CI_SELF_HOSTED=1`, so the enabled
configuration is verified, not just the default one.

```
$ python3 -m unittest ci.test_workflow_event_matrix ci.test_runner_capability
Ran 37 tests in 0.16s
OK
```

`ci/check_runner_capability.py` has its own accepted/rejected matrix per
capability label. On this Linux x86_64 host, `--capability hc-linux-x64-cpu`
returns 0 and `--capability hc-macos-arm64` returns 2 with both a system and a
machine mismatch — the second call is a real execution, recorded here as
evidence that the check fails closed rather than only passing.

## 6. Activation and rollback

Activation is a variable, not a commit:

1. Register the hosts and qualify them out of band (§7). Attach exactly the
   `hc-*` label that was qualified; do not attach several.
2. Run one manually dispatched qualification per capability with the variable
   still unset, so the hosted baseline is recorded for the same SHA.
3. Set `HYPERCINE_CI_SELF_HOSTED=1` for the repository.
4. Open a PR that changes nothing but a comment, and confirm every lane picks
   the expected label and that the required privacy check still reports on a
   hosted runner.
5. Only then allow a real PR to be judged by the pool.

Rollback is deleting the variable. No commit is needed, every lane returns to
its github-hosted label on the next event, and no branch protection rule has to
be touched. If a specific capability is unreliable, remove that one label from
the affected hosts: those lanes stop being schedulable and the job stays
queued — which is visible — instead of silently passing somewhere else.

Do not "fix" a stuck queue by adding a second, broader label to a host. That is
how a lane ends up on the wrong hardware.

## 7. Evidence status

| Item | Status |
| --- | --- |
| `workflow_event_matrix.py` on current `main` | RUN — `OK: 7 workflows, 7 events, 80 job instances, 12 rules` |
| Negative controls and invariant tests (27 routing + 10 capability) | RUN — 37 tests OK |
| Routing with `HYPERCINE_CI_SELF_HOSTED=1` (static) | RUN — all pool lanes resolve to the matching `hc-*` label, privacy stays hosted |
| Fork PR falls back to hosted labels | RUN (static, R6) |
| `check_runner_capability.py` on this host, matching label | RUN — exit 0 |
| `check_runner_capability.py` on this host, macOS label | RUN — exit 2, system + machine mismatch |
| Hosted CI for this diff | **NOT RUN** — single `[skip ci]` commit, per the maintainer's instruction not to spend hosted minutes |
| Any CTest/Standalone/CUDA build | **NOT RUN** |
| Runner registration, labels, service accounts, workspace cleanup | **NOT RUN / operator-attested** |
| Actual job execution on a self-hosted host | **NOT RUN** |

No runner tokens, hostnames, private paths or corpus locations appear in this
document or in the diff. Label names are the only pool-specific strings, and
they are contract names, not infrastructure addresses.
