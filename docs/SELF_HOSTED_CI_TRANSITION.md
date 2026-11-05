# Mac CI through a private disposable-runner relay

The public HyperCine repository must never have a repository-scoped self-hosted
runner attached. Event guards in a reviewed workflow cannot prevent another
workflow from requesting an online public runner. Fork exclusion alone, and even
main-only routing expressions, do not enforce that repository-wide boundary.

## Scope and routing

`HYPERCINE_CI_LOCAL_MAC=1` affects only **pushes to protected `main`**. It removes
the two hosted Mac Standalone rows and the Mac quality-state row from those
runs. A separately enabled host controller sends the same exact public source
SHA to a private relay repository. The controller retains intermediate main
pushes while the VM is busy; activation does not backfill old hosted history.
That relay uses
`[self-hosted, macOS, ARM64, hyperci-macos15-arm64]` on an unprivileged disposable
macOS VM, one job per VM. No runner is registered on public HyperCine.

The old `HYPERCINE_CI_SELF_HOSTED` variable has no routing effect. Windows,
Linux and CUDA keep their hosted labels and test commands. Publication privacy,
Windows privacy hooks, Python compatibility and the routing gate remain hosted.
All PRs (including same-repo PRs), feature/tag pushes and manual dispatches retain
their hosted Mac behavior. With the new variable unset, all five Standalone
matrix rows and all three quality-state rows retain their existing hosted labels.

Both hosted Mac and private relay jobs invoke `ci/run_macos_lane.py`:

| Lane | Result context | Coverage |
| --- | --- | --- |
| `standalone-release` | `Standalone full decode (macOS arm64 Release)` | Full SDK-free Standalone CTest, architecture and required FFmpeg external decode gate |
| `standalone-sanitizers` | `Standalone full decode (macOS arm64 ASan UBSan)` | Same suite with ASan/UBSan, existing Apple leak-sanitizer exclusion |
| `quality-state` | `Quality state (macOS arm64)` | Full portable quality-state CTest |

The recipe retains all build flags, test inventories, decode requirements and
sanitizer fatal-error settings. Build parallelism is two to fit the VM budget.
It does not mount private reference corpora or qualify CUDA, original SDK,
production Metal algorithms, or release quality. Virtual Metal availability is
an infrastructure property, not evidence for an unexecuted GPU algorithm.

## Enforcement outside public workflow code

The controller has two independent opt-ins: a local configuration and the public
Mac variable. It verifies protected default branch `main`, its required PR rule
and the mandatory Actions-owned publication privacy check, then resolves its full
40-character SHA, and dispatches only that SHA automatically. Qualification of
a Draft requires an explicitly pinned local SHA; arbitrary PR/manual refs are
never automatically accepted.

The private relay definition is pinned to an operator-reviewed commit. Its
visibility and default-branch commit are rechecked before dispatch and result
acceptance. The private queue accepts only its pinned workflow, trusted actor,
and manual-dispatch event. Untrusted users cannot enqueue jobs there.

The relay validates the SHA and request nonce, checks guest identity and macOS
protections, and fetches the public source by SHA without Git credentials.
No host OAuth token, privileged host mount, clipboard, SSH agent or private
network access enters the VM. Host-side network rules enforce isolation.
Each job gets a fresh cloned disk; the runner registration and VM are removed
afterward, including failure paths. No cross-job build workspace is retained.

The controller retains a private durable dispatch ledger with source SHA,
request nonce, relay run and per-lane conclusions. It accepts only the pinned
relay definition and matching nonce/actor. It posts the contexts in the table
on the exact public source commit. Missing, cancelled, skipped, timed-out or
failed jobs cannot become success. A runner process exit code is not a test
result. Public status descriptions contain only a short result; raw logs,
resource samples and private run details remain private/local.

## Validation

Run in an isolated review VM from the exact candidate checkout:

```sh
python3 ci/workflow_event_matrix.py
python3 -m unittest ci.test_workflow_event_matrix ci.test_runner_capability
python3 ci/workflow_event_matrix.py --list
python3 ci/workflow_event_matrix.py --list --local-mac
python3 ci/run_macos_lane.py --lane standalone-release
# Use a fresh workspace for each following lane.
python3 ci/run_macos_lane.py --lane standalone-sanitizers
python3 ci/run_macos_lane.py --lane quality-state
```

The verifier simulates same-repo/fork PRs, feature/main/tag pushes and manual
main/feature dispatches. R13 forbids public self-hosted routing even on main;
R14 verifies that activation delegates exactly the Mac main-push rows. Mutation
controls must fail the corresponding rule. R1/R2 preserve mandatory hosted
`Publication privacy (all new commits)`, and R8 retains all five PR rows.

## Activation and rollback

1. Keep the public variable unset. Independently review this exact head and
   retain protected-main rules and the mandatory privacy status unchanged.
2. Qualify the pinned candidate through the private relay: all three full lanes
   must complete on disposable VMs; verify source SHA, conclusions and cleanup.
   A local script pass alone does not qualify the Actions relay lifecycle.
3. Accept and merge the reviewed change only with the required publication
   privacy gate satisfied. This Draft does not authorize a main merge.
4. With private relay and host controller qualified and enabled, set only
   `HYPERCINE_CI_LOCAL_MAC=1`. Verify source-bound Mac statuses on the next main
   push and the unchanged Windows/Linux/CUDA/privacy checks. No public runner
   registration or broad global pool switch is part of activation.
5. Roll back by deleting the Mac variable: subsequent runs use hosted Macs.
   Already-dispatched local work remains tracked and its real result reported.

The private relay recipe and host controller live outside this public repo.
No token or private relay credential is added as a public-repo secret. Private
Actions logs and local usage history provide diagnosis without publishing
machine paths or environment dumps. See the PR receipt for executed evidence;
anything not executed remains NOT RUN.
