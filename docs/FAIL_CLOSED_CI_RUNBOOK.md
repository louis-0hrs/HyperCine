# Mac-only pause/recovery — review only

Owner correction6655103747 supersedes the global compute-pause runtime. Windows,
Linux CPU and CUDA remain hosted. Runtime GitHub calls are GET-only; owner choices
write only the protected local Mac journal. Nothing is installed/activated here.

Install only an independently reviewed exact package/manifest on a trusted control
plane. Canonical shared state0700; owned regular single-link config/health/journal/
lock0600; no unsafe ancestors or links. Keep example enabled=false until separate
activation approval. Bind POLICY_CONFIG/POLICY_STATE privately. Schema2 rejects
old schema1 global journals: preserve old evidence separately, never import old
approvals into live Mac state. Authenticate/qualify independent health collector
and availability epochs; missing/stale health denies local admission.

Qualify the installed queue adapter's ten-second synchronous network deadline,
nonce/run reconciliation, no detached retry/reentrancy/VM wait/hosted fallback.
The canonical owner lock covers final revalidation and bounded queue acknowledgment,
not VM lifetime. Pause first denies later work; already submitted work may run after
pause. Do not claim cancellation/exactly-once from a timeout keyword.

## Inspect and owner Mac choices

Use trusted installed source, not a PR checkout. No command below mutates GitHub:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" inspect
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  plan --generation "$GENERATION" --target paused/host-unavailable --reason owner-mac-pause
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  execute --generation "$GENERATION" --plan "$PLAN_ID" --execute
```

Use fresh generation/plan output, within five minutes. Enabled config and explicit
execution are required for local state changes. Pausing Mac leaves all hosted
families active; unrelated hosted jobs do not require a quiet window or cancellation.
API/health/signal/process uncertainty never resumes Mac. Healthy return needs a new
owner action. CLI denial exits3; other errors2. allowed=false is never permission.

External workflow drift is visible and denies Mac. This runtime never fixes/toggles
workflows automatically. For a fresh local pause snapshot use:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  plan --generation "$GENERATION" --target paused/host-unavailable --reconcile --reason owner-recovery
```

Execute that new plan; handle external GitHub settings under separate owner control.
Use target recovering while repairing, then a new normal plan/execution only with
fresh stable authenticated health and all hosted families/security readback intact.
Old generation/session approvals and old worker errors cannot override later owner.

## Explicit paid Mac fallback — not run during this Draft

Bind local owner choice to one exact source SHA:

```sh
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  plan --generation "$GENERATION" --target owner-authorized-hosted \
  --candidate-sha "$SOURCE_SHA" --reason owner-mac-hosted-budget
python3 -m ci.compute_policy.admin --config "$POLICY_CONFIG" --state "$POLICY_STATE" \
  execute --generation "$GENERATION" --plan "$PLAN_ID" --execute --allow-hosted
```

This launches nothing. Paid execution additionally requires explicit owner manual
Standalone/Quality dispatch with typed mac_hosted_fallback=true and mac_candidate_sha
matching that run's github.sha, original actor ID and triggering-owner guards. A
normal manual dispatch runs non-Mac only. Approved fallback adds original Mac recipes;
no mode/health/variable can cause automatic events to launch paid Mac. Stale/new SHA
needs new approval. The CLI's hosted admission query also requires --candidate-sha.

A genuine Actions Mac result does not automatically satisfy the App5612385 aggregate.
Before using hosted fallback to meet that future required gate, independently qualify
a hosted-result verifier/Reporter adapter binding Head/Base/tree/workflow/actor/nonce/
run/attempt and all three successful recipes. Until then aggregate pending/blocking;
never copy status names or synthesize success. Private Reporter proof is unchanged.

## Checks and separate temporary window

Excluded Mac rows are NOT RUN and emit no individual Actions check. Before production
cutover, separately qualify/approve additive HyperCI / Mac relay pinned to App5612385,
while retaining required Publication privacy Actions15368. Reporter remains optional,
configs disabled, rules unchanged in this review. Fork candidates still need existing
owner/repo approval and cannot implicitly reach a private host.

The [integration window](INTEGRATION_PRIVACY_WINDOW.md) is separately authorized:
immutable four-ID/original-state snapshot, unrelated-run inventory, coordinated quiet
window, narrow verified writes and independent readback. No Mac CLI here implements
it. Explicit audited owner restore of hosted families is required afterward for normal
non-Mac operation; no timer/finally/health-based restore. Partial failure means visible
attention and manual reconciliation, never automatic fallback or false resumed CI.
