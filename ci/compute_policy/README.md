# Cost fail-closed CI policy — disabled review package

This code is not installed, scheduled or activated by the repository. The example
configuration is OFF. No import, unit test or mutation contacts GitHub. The four
public compute workflows currently retain their existing production settings until
a separately authorized deployment. This package does not pretend they are already
paused.

## Modes and invariants

| Mode | Public CPU/CUDA/Standalone/Quality workflows | Private Mac admission | Hosted admission |
| --- | --- | --- | --- |
| `normal` | all paused | fresh trusted owner-bound health session only | denied |
| `paused/host-unavailable` | desired OFF; actual readback shown separately | denied | denied |
| `recovering` | all paused | denied | denied |
| `owner-authorized-hosted` | only explicitly selected, owner-approved workflows | denied | explicit selected scope only |

`normal` means local Mac operation, never hosted fallback. The combined public
workflows contain hosted rows, so they stay OFF even while the Mac is healthy.
The private relay is a separate queue and uses `gate.admit_local` before private dispatch.
This seam must be installed in a trusted deployment, not imported from a candidate
checkout. There is no dispatch/rerun/cancel/check-status API in this package.
Enabling hosted workflows can allow future repository events to run them; the
owner's explicit hosted command authorizes that ongoing mode until paused again.

A lost/expired health observation, changed health session or private relay error
latches a pause. Healthy observations alone cannot resume it. No finally block,
signal handler, timer or recovery path enables compute. Compensation only attempts
to disable. Existing running/queued jobs are inventoried, not silently cancelled.
A repository-wide workflow disable is non-atomic and does not cancel existing runs.

## Modules and authority

- `policy.py`: owner plans, four modes, generation fencing, immutable snapshots,
  hash-chained audit, quiet-window preflight, durable intents, independent readback,
  OFF-only compensation, expiry and admission.
- `api.py`: GitHub.com-only CLI boundary. Writes can touch only the four reviewed
  compute workflow IDs. Watcher transport cannot enable even if asked. Privacy,
  rules, statuses, runners and dispatch endpoints are unwritable.
- `admin.py`: manual inspect/plan/execute/reconcile commands and one watchdog tick.
  No service or timer is installed. Mutations require both enabled local config
  and explicit `--execute`. Hosted resume also requires `--allow-hosted` and an
  explicit workflow selection in the owner plan.
- `gate.py`: complete fail-closed local-worker seam. Disabled/uncertain policy
  denies admission rather than passing through. It never substitutes hosted work.
- Existing hardened `premerge_controller.state_store` provides canonical owned
  0700 directories, owned regular single-link 0600 files, no-follow descriptor
  access, locking, exclusive temporary files and durable atomic replacement.
  Separate fixed journal/lock names prevent cross-controller file mixing.

V1 verifies the authenticated public owner's ID/login, repository admin permission
and actual `X-OAuth-Scopes` grant (`repo` and `workflow`) through the existing CLI.
Accepted endpoint scopes are not a credential grant. Opaque fine-grained/App grants
are unsupported and fail closed until an independently reviewed permission adapter
exists. No credentials are bundled or written to journal/logs. No new token or wider
grant is requested by installation of this source.

All owner decisions serialize through one canonical state directory on one trusted
control plane. Every command uses a current generation, exact immutable plan ID and
fresh live inventory. Plans expire after five minutes. Snapshots are never silently
reused to enable workflows after a newer decision. Originally disabled workflows
stay disabled on restore; an explicitly selected `--new-hosted-grant` is a new owner
authorization, not restoration. A fresh pause reconciliation rotates the baseline,
retaining the old snapshot in audit history. Old worker failures cannot overwrite a
newer owner's generation.

See the admission ordering contract below; a watch snapshot alone is never a
queue authorization.

The local UID/root and the reviewed installed package are trusted. Hash chains are
corruption detection, not signatures against a compromised same-UID writer. Do not
roll back the entire journal over a live one. If journal freshness is lost, start
in uninitialized paused/attention mode with a fresh independent inventory; old plan
IDs cannot be replayed. GitHub workflow state changes have no compare-and-swap
transaction across four workflows. Do not claim atomic protection against direct UI
changes between requests. Use the coordinated quiet window and route all owner
changes through the control plane; detected external drift stops the operation.

## Owner pause versus private queue submission

`gate.admit_local` treats the initial watch result only as an expected generation.
It reacquires the **same canonical OS lock** used by owner plan/execute/reconcile,
loads fresh journal state, reads live inventory/security and checks fresh health,
session, verification age and that expected generation. It holds this lock through
one synchronous **queue submission**, then releases it before any VM/job execution.

The lock defines the order. If an owner's pause/other transition commits first,
the stale worker cannot submit. If final admission/submission owns the lock first,
the concurrent pause waits for that bounded request to return/fail, then commits;
that already submitted work may start or finish after the pause. A pause does not
cancel pre-existing/in-flight queue submissions or running jobs. A pause request
is not complete merely because its caller has started waiting for the lock.
No new submission may linearize after a completed pause until a new owner resume.

The injected trusted adapter has a revised interface:
`dispatch_private(*, timeout)`; the gate supplies ten seconds. The adapter **must**
enforce that network deadline synchronously, submit only the queue request and
return on acknowledgment. It must not wait for VM allocation/job completion,
spawn a background request, retry an uncertain request, reenter owner/policy
commands, or try another destination. A zero-argument legacy callback is rejected.
This package supplies the gate, not a production queue transport: enforce/review
that contract in the installed private adapter before deployment. Passing a timeout
keyword is not a sandbox for arbitrary same-UID Python code. GitHub metadata reads
also use the transport's bounded per-request timeout; stalled reads deny admission.

A timeout/error releases the lock and latches pause only if its generation is still
current; a later owner decision is never overwritten. An uncertain remote queue
acknowledgment may still represent already accepted work. Do not automatically
retry it. The premerge controller's existing nonce/run reconciliation remains the
job identity mechanism; this gate does not create an exactly-once queue protocol.

Barrier tests prove owner-first => zero dispatch, submission-first => owner blocked
on the actual flock until acknowledgment, and no lock retained for job lifetime.
Designated mutations remove final state reload, final admission or OS serialization.

## Mandatory privacy and security

Privacy guard stays active, including its Ubuntu publication scan and Windows
installed-hook tests. The source blob for its complete workflow is pinned; a
changed definition requires review. Required PR/privacy rules, App15368 provenance
and no-bypass applicable rulesets are checked before and after transitions. Unknown
required contexts, missing readback or unverified write capability prevent start.
If static routing is required, its workflow must also be active. No paused or unrun
check is posted as successful; missing required checks block merges.

The whole optional CUDA workflow includes the currently non-required inventory
self-test. Pausing that workflow also leaves that advisory self-test **NOT RUN**;
it is not called green. No workflow/job definition is removed here. If that or any
other compute-contained check becomes mandatory, preflight refuses the unknown
required context: split the mandatory job into an always-active security workflow
and review the policy before enabling this operating mode. Do not silently classify
a newly required security job as optional to make the pause succeed.

## Independent health/control plane

A watchdog on the failed Mac cannot pause remote CI. Deploy the serialized control
plane/watchdog on an independent trusted host, with a reviewed authenticated health
collector, before claiming live enforcement. No host location or credential is
included here. Normal local mode already holds public hosted compute OFF, so a Mac
outage cannot cause those workflows to become a fallback even if the observer fails.

`host_status` is the owned 0600 local output of that trusted collector, not raw
untrusted network JSON, a label or a PR comment. It contains:

```json
{"healthy": true, "observed_at": 0, "session_id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
```

The timestamp is illustrative and deliberately stale. A valid observation must be
within 60 seconds (at most five seconds future clock skew). The collector's 32-hex
session identifies an availability epoch, changed on every observed outage, relay
restart or collector restart. The owner binds that epoch during `normal` resume;
newly healthy epochs still need another owner action. The collector must preserve
that contract even if the control-plane process was temporarily stopped. A missing
collector defaults to unhealthy. Admission additionally needs a current generation,
fresh policy verification and independent live workflow/security facts.

API loss can prevent a requested remote disable. The journal then records the
unresolved write, desired pause and owner attention; inspection shows actual
remaining active workflows. This is not falsely advertised as verified remote OFF.
Already explicitly authorized hosted work may continue until GitHub can accept the
owner's recovery request. No algorithm can make unavailable remote APIs atomic.

## Reproduction

POSIX Python3.9+, CMake and OpenSSL, repository root:

```sh
cmake -S ci/premerge_controller -B build/premerge-controller
ctest --test-dir build/premerge-controller -C Release --output-on-failure
```

The four CTests cover both complete public packages and both designated mutation
suites. Failure tests inject errors before/after each disable, during each manual
resume, during compensation/readback, on signals and after process loss. All GitHub
calls are mocked. Actual production workflow states remain untouched.

See [manual operating/recovery runbook](../../docs/FAIL_CLOSED_CI_RUNBOOK.md) and
[revised integration privacy window](../../docs/INTEGRATION_PRIVACY_WINDOW.md).
