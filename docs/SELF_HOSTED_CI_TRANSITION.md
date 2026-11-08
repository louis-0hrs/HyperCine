# Mac CI through a private disposable-runner relay

This is the **disabled candidate policy**, not a statement that production has
been installed or activated. The owner Mac-only scope correction supersedes the
former main-push-only variable design. See [deployment qualification](PRIVATE_RELAY_DEPLOYMENT_QUALIFICATION.md),
[owner pause/resume](FAIL_CLOSED_CI_RUNBOOK.md), and the separate
[temporary privacy window](INTEGRATION_PRIVACY_WINDOW.md).

## Scope and routing

Only these three macOS lanes may enter the private relay:

| Lane | Coverage |
| --- | --- |
| `standalone-release` | Full SDK-free Standalone CTest, architecture and required FFmpeg external decode gate |
| `standalone-sanitizers` | Same suite with ASan/UBSan and existing Apple leak-sanitizer exclusion |
| `quality-state` | Full portable quality-state CTest |

The private execution capability is **Darwin/arm64**. Windows, Linux CPU and
sanitizers, CUDA, privacy/security, compatibility and generic build jobs must
never enter this relay. Their existing GitHub-hosted labels, triggers and recipes
remain intact, independently of Mac availability. No repository-scoped
self-hosted runner may be attached to public HyperCine.

In the candidate workflow definitions, automatic main/feature pushes, same-repo
and fork PRs, and ordinary manual dispatches omit all three hosted Mac rows.
Other event/path/branch filters still apply; this does not introduce new triggers.
Missing or legacy `HYPERCINE_CI_LOCAL_MAC`/`HYPERCINE_CI_SELF_HOSTED` variables
cannot turn hosted Mac rows back on. Deleting a variable is **not rollback**.
An omitted Mac row is **NOT RUN**, never a successful Mac result.

Public events do not authorize private execution. The separately approved
candidate must pass exact Head/Base/tree/test-merge and private workflow/actor/
nonce/run/attempt checks before local admission. A fork or manual request is not
an automatic private queue submission. All three approved Mac lanes must then
actually succeed. Only Reporter App5612385 may report the bound aggregate
`HyperCI / Mac relay`; mandatory `Publication privacy (all new commits)` remains
owned by Actions15368. Reporter is still optional until separately approved rules
and cutover qualification; this document does not make it required.

## Fail-closed operation and explicit fallback

A qualified deployment must enforce the immutable three-lane/capability allowlist
before queue submission **and before VM provisioning/runner registration**, as
well as in the pinned private workflow. Unknown lanes, workflows, OS/architecture
or extra labels are denied. The installed legacy worker has not passed this
new gate; see the qualification report before using this as an operating runbook.

Mac/API/relay health loss latches local pause. Healthy return never resumes
admission automatically: a fresh owner decision and current health/session proof
are required. Normal runtime cannot disable or enable GitHub workflow families.
Windows/Linux/CUDA scheduling continues. There is no automatic paid Mac fallback.

The candidate retains original hosted Mac recipes only for an explicit
`workflow_dispatch` request with all four guards: `mac_hosted_fallback=true`,
original owner actor ID248477380, triggering actor `louis-0hrs`, and
`mac_candidate_sha == github.sha`. The selected ref must contain these guarded
workflow definitions. An ordinary manual request does not add Mac jobs. Owner
hosted mode alone does not dispatch anything. Non-Mac jobs remain in this manual
run too; its budget needs separate approval.

A hosted fallback result does **not** automatically satisfy the private Reporter
aggregate. Its source-bound result adapter remains unqualified. Initial local-only
cutover may leave that path unused; absent qualified Mac evidence stays pending
or failing. Never copy Actions success or create dummy/skipped Mac jobs to satisfy
the App gate.

## Isolation and result boundaries

Both hosted Mac and private relay recipes invoke `ci/run_macos_lane.py` with the
same flags, tests and fatal sanitizer settings. Build parallelism is two. Each
private lane requires a fresh unprivileged disposable VM and ephemeral private
runner, with cleanup on failures. Host credentials, privileged mounts, clipboard,
SSH agent and private network access must not enter the VM.

The private workflow is pinned and rechecked. Its fixed matrix and Darwin/arm64
guest checks are necessary but do not replace host-side pre-provisioning checks.
Missing, duplicated, cancelled, skipped, neutral or failed lane evidence cannot
become aggregate success. A runner process exit is not a test result. Private
logs, queue identities, machine paths and usage details remain private.

These tests do not qualify CUDA, original SDK, production Metal algorithms or
release quality. Virtual Metal availability is not evidence for an unexecuted
GPU algorithm.

## Validation and controlled cutover

Use the exact candidate and the configured test interpreter with PyYAML:

```sh
ctest --test-dir build/premerge-controller --output-on-failure
python3 ci/workflow_event_matrix.py
python3 -m unittest ci.test_workflow_event_matrix ci.test_runner_capability
```

The source model covers main/feature push, same-repo/fork PR and manual events;
R13 forbids public self-hosted routing and R14 checks automatic Mac omission plus
owner/SHA-bound manual fallback. Existing non-Mac rows and full step hashes are
compared with the frozen baseline. This is not actual hosted scheduling evidence.
Native lane execution requires a separate approved isolated VM trial; do not
start one merely to inspect the policy.

Before cutover: independently accept the exact installed adapter/collector/package
and negative tests, then obtain genuine Actions15368 privacy evidence on the
owner-authorized integration candidate. Preserve existing required Privacy and
separately qualify any additive App-pinned gate. Installation, rules, activation,
workflow toggles and merge each remain outside this source-review handoff.

Operational rollback means **pause local Mac admission and retain visible missing
Mac evidence**. Do not remove Privacy, reset variables to restart hosted Macs, or
disable mixed workflows. Track already-submitted work honestly. A full source
rollback or explicit paid hosted fallback requires its own reviewed owner action.
The temporary four-workflow privacy qualification window is a separate audited
operation with explicit owner restoration; it is not steady-state routing.
