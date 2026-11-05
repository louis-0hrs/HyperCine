# Manual cost-policy operation and recovery

**Review package only. No source change in this PR installs a service, disables a
workflow, publishes an eligible integration head or authorizes a merge.** The owner
policy in #353 comment6645766280 supersedes unconditional automatic restoration.

## Pre-deployment prerequisites

Use the exact independently reviewed commit/package manifest on a trusted control
plane independent of the Mac. Preserve the durable state on that control plane;
all owners/operators use the same canonical state and lock. State directory0700,
owned regular config/health/journal/lock0600; no symlinks/hardlinks/shared writable
ancestors. Root/service UID and the health collector are trusted. Do not auto-pull
public PR code into that host. The live private VM worker is not modified here.

Copy the disabled example into an ignored private local config and fix permissions.
Bind `POLICY_CONFIG` and `POLICY_STATE` locally; never publish their values. A reviewed
collector supplies the protected health file with authenticated availability epochs.
Default null health blocks local resume. Keep `enabled=false` until deployment and
execution are separately authorized. All write commands fail before API mutation
without an enabled config, explicit execution and verified owner write grants.

The four IDs/path pairs are fixed in code. Preflight reads each live state/version,
privacy active state, pinned privacy definition, applicable PR/privacy rules and
no-bypass rulesets. It inventories queued/in-progress/waiting/pending/requested runs
with bounded complete pagination. Unknown required checks, unavailable permission
proof, wrong owner/repo/workflow, any failed state read or drift means **NO START**.
Do not test write permission by briefly enabling a workflow.

## Read-only inspection and new owner plans

These commands run on the trusted installed package, not a candidate checkout:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" inspect
```

Read generation, mode, owner attention, unresolved intent, exact live workflow
inventory and `all_compute_verified_paused`. Desired mode alone is not proof that
all remote workflows are disabled. A new uninitialized journal starts desired
paused with attention, not a claim of already paused production.

Owner-coordinated quiet window: no concurrent repo pushes, merges, policy edits or
workflow-setting edits. Inspect unrelated active runs; do not cancel their work.
Default plans refuse any such active run. Obtain the owner's bounded decision,
then make a fresh plan with the generation just read:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  plan --generation "$GENERATION" --target paused/host-unavailable --reason owner-pause
```

The output's immutable `plan_id`, original states and generation are the approval
binding. Do not replace them with a stale saved command. Within five minutes:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  execute --generation "$GENERATION" --plan "$PLAN_ID" --execute
```

Each write has a durable preceding intent and an independent readback. Every exit
path keeps or attempts a pause. A caught signal/API error may compensate only by
disabling; it never re-enables original states. Pause is persistent: no timer or
finally restore. Status/audit is the operator notification; integrate nonzero exits
and `attention` into an independent owner's alert channel before unattended use.
This package never sends messages or installs a notifier itself.

## Manual recovery after partial transition, signal or host/process loss

1. Keep compute paused. Do not replay an old resume token or blindly enable all IDs.
   `SIGKILL`, power loss and network loss cannot be reliably handled by a trap.
   Inspect the journal and independently read all four current states and required
   privacy/rules through the command above. Pending intent is retained after loss.
2. Verify owner credentials/grants and canonical current journal. If another owner
   has advanced its generation, the old snapshot is invalid. Never overwrite that
   journal with a backup. After total journal loss, start fresh in paused/attention
   mode and require a new authenticated owner decision; do not import old plans.
3. Make an explicitly reconciled **pause**, binding the fresh current inventory and
   current generation. It rotates the baseline so old disabled states are not
   unintentionally overwritten later:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  plan --generation "$GENERATION" --target paused/host-unavailable \
  --reconcile --reason owner-recovery
```

   If work is already queued/running and the owner needs to stop future starts,
   add explicit `--emergency-pause`. This flag is pause-only and does not cancel
   those runs. Keep their IDs in the audit and let the owner decide their handling.
4. Execute that new plan with its generation/ID and `--execute`. Inspect again.
   Required: all four independently verified non-active, no unresolved intent,
   unchanged Actions15368 privacy definition/source/rules and no hidden policy
   bypass. If a disable still fails, retain visible attention/partial inventory,
   exit nonzero and stop. Never represent partial failure as a successful pause.
5. Only after repair/owner choice create another **new** plan for either local or
   hosted resumption. No automatic restore command is scheduled after recovery.

## Authenticated local resume

An owner may first use `--target recovering` with a fresh plan/execution to keep
all public compute OFF while repairing. Healthy probes do not exit recovering.
When the trusted collector has a fresh stable availability session:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  plan --generation "$GENERATION" --target normal --reason local-repair-confirmed
```

Execute the resulting fresh plan as above. Public build workflows stay paused;
only the separate private Mac gate can admit work. Its admission denies on policy,
API, health, generation or session uncertainty. The worker must use `gate.admit_local`
and have no alternative hosted dispatch path. The gate is supplied, not installed.

### Queue submission and pause ordering

The initial watch snapshot cannot authorize dispatch. The gate re-locks the same
canonical journal as owner transitions, reloads current generation/mode and live
inventory/security, then checks fresh health/session immediately before submission.
Keep the lock only through the bounded queue acknowledgment; VM execution is outside
it. A completed owner pause wins against every later admission. If submission owns
the lock first, the owner's command waits, and the already submitted job may run
later. That is pre-existing work, not a post-pause admission. Inspect/reconcile such
work; this gate does not cancel it or falsely promise revocation of accepted jobs.

Install only a separately reviewed synchronous queue adapter implementing
`dispatch_private(*, timeout)` and enforcing the supplied ten-second network deadline.
Do not wrap a full VM lifecycle, detached request, automatic retry or reentrant owner
command in that callback. The gate is not a sandbox for trusted same-UID code; the
actual private transport must be reviewed before activation. No queue transport or
private worker is installed by this source correction. Old zero-argument callbacks
fail closed. On uncertain acknowledgment, retain owner attention, reconcile the
existing nonce/run before resumption, and never retry automatically or fall back.

## Authenticated hosted choice / manual restore

Enabling public workflows can allow later pushes/PR events to incur hosted compute.
It is an ongoing explicit owner mode, not a fallback or a timed restoration. In a
quiet window, inspect fresh generation and choose the exact IDs:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  plan --generation "$GENERATION" --target owner-authorized-hosted \
  --workflow-id 373401385 --workflow-id 395280516 \
  --workflow-id 385851595 --workflow-id 386265774 --reason owner-hosted-budget
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  execute --generation "$GENERATION" --plan "$PLAN_ID" --execute --allow-hosted
```

Use the actual generation/plan ID emitted by this new plan. Without a new grant,
only originally active selected workflows can be restored; omitted or originally
disabled ones stay OFF. After a fresh reconciliation or intentional change to that
baseline, an owner who consciously wants a **new** enable authorization must add
`--new-hosted-grant` to the plan and explicitly select each workflow. That is audited
as a new decision, never an automatic override of a later owner's disabled choice.

On any partial resume failure the controller attempts OFF-only compensation, retains
attention and independent readback, then stops. No automatic retry of enable and
no global hosted fallback exists. Already authorized hosted mode does not depend
on Mac health; the owner explicitly chose it. Pausing it is another owner action.

## Scope and hard limits

GitHub's workflow API is not an atomic four-resource transaction. The control plane
serializes its own decisions and detects observable external drift, but cannot
prevent direct UI changes racing between individual requests. Keep the coordinated
window; do not claim global compare-and-swap or guaranteed remote OFF during an API
outage. The readback makes uncertainty visible. Disabling also does not cancel
existing runs. No required privacy/security workflow, status or branch rule can be
written by this adapter. Unknown required compute/security contexts stop preflight.

The optional CUDA workflow's advisory inventory self-test is also NOT RUN while
that workflow is paused. It is not presently required. If made mandatory, split it
out and review the policy before deployment; the engine refuses an unknown required
context. Required checks that cannot run remain pending/missing and block merge;
never mark them green via skip or the Reporter App.

The CLI `admit` exits3 on denial; errors exit2. JSON `allowed=false` must never be
interpreted as permission. No CLI command creates a GitHub check or labels an unrun
job successful. API grants that cannot be verified by the V1 classic-token adapter
are rejected, not probed using writes. Tokens/keys/host paths are never printed.
