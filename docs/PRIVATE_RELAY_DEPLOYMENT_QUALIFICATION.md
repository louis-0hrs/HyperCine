# Private Mac relay deployment qualification handoff

> **Private implementation follow-up:** review6656448538 is now addressed by a
> separate disabled, uninstalled private Draft candidate. Private source
> `a961b799b8105bffec8ec7f6d8fd0dc56c30a776`, tree
> `f2aca6d6b44621ec7bf204a0989746a654918234`, ref
> `review/private-mac-integration-20261105`; private repository identity and host
> code are intentionally omitted here. [Redacted source-bound receipt](validation/private-candidate-20261105.json).
> The historical installed audit below remains accurate: those installed bytes
> were not changed. Candidate tests must not be used to relabel installed failures.
>
> Candidate CTest **3/3**, **64 integrated tests**, **24 designated mutation kills**,
> **30 lock-order stress scenarios** PASS. The same nine forbidden payloads now
> reject at the candidate worker entrypoint with zero queue/VM/registration/check
> port calls; three valid Mac lanes pass a simulated lifecycle. Signed capability
> binds candidate/nonce/run/job/attempt/runner and rejects non-Mac-labeled results.
> Real local signatures, a separate producer process/Unix socket and the actual
> subprocess transport with a sleeping fake executable were exercised. See the
> receipt for the measured under-ten-second timeout and all mutation identities.
>
> This is **isolated implementation evidence, not deployed or native acceptance**.
> No actual GitHub test request, VM, registration or App check ran. Native Tart/JIT
> behavior, distinct-UID collector launcher/ACLs and real host monitoring,
> production controller poll/pending/invalidation wiring, durable capability
> receipt recovery after restart, and real Reporter/Actions advisory remain
> unqualified. Missing capability receipts fail closed. No production enablement,
> workflow/rule mutation, eligible H or merge. Next: independent review of the
> exact private candidate and its explicit remaining integration gates.

**HOLD — installed deployment is not qualified.** This follows focused source
review6655968595 (PASS) and owner boundary6656003569 on PR #353. The accepted
runtime source is C=`693a87b14426ef6771d889bcf65fb730ed217b7d`,
tree=`ba30556b09091339140796669b2b9c89d038bdc9`, stacked on unchanged
#352=`61f8b095759bf178f46a6e2bd220c33a406df52a`.
This follow-up changes documentation/evidence only; it does not fix or install a
private adapter. Any subsequent runtime change requires new source-bound review.

The [redacted inventory and isolated probes](validation/relay-deployment-audit-20261105.json)
binds the actual installed worker/legacy relay/telemetry bytes and every reviewed
controller/policy Python file by SHA256. The private workflow was fetched read-only
at its installed commit pin; only its content hash is public. No credentials,
private repository identity, host paths or raw logs are included. The private
reproducer is retained locally with its hash in the receipt; independent reviewers
need authorized access to the matching private inputs to reproduce installed
behavior. This is agent-generated evidence, not independent deployment acceptance.

## Observed installed state

| Boundary | Observation | Qualification |
| --- | --- | --- |
| Installed package | LaunchAgent points to legacy worker; no reviewed controller/policy package or premerge/compute config | NOT INSTALLED; no manifest equality or trusted deployment claim |
| Existing relay | Legacy main relay flag false; private workflow pinned | Remains disabled; no activation |
| Queue deadline | Legacy worker API uses a 60-second subprocess timeout; reviewed gate expects a 10-second queue-only callback, which is not wired into the worker | NOT QUALIFIED; no end-to-end deadline/ack bound |
| Nonce recovery | Legacy source saves intent before POST and refuses automatic duplicate dispatch; matching run lookup exists | Source observation only; new gate/adapter crash, timeout and ambiguous-ack reconciliation NOT RUN |
| Health collector | No configured authenticated collector/epoch input or new policy consumer in this installed worker | NOT QUALIFIED; no claim that filename search proves absence system-wide |
| Private workflow | Fixed three-lane matrix, fixed Mac ARM labels, Darwin/arm64 guest assertions, no lane-selector input | Source inspection PASS; guest assertions occur after provisioning |
| Host queue eligibility | Pin/workflow/actor controls exist; strict job-name and complete capability allowlists absent | New pre-provisioning boundary FAIL in isolated predicate probes |

The private workflow cannot normally generate the forged payloads below from its
fixed matrix. Nevertheless the requested independent host boundary is absent;
workflow pinning alone is not evidence of that second enforcement point.

## Executed isolated negative probes

Only the exact installed `eligible` function AST was compiled with its `re`
dependency. Worker imports/main, network, VM provisioning and registration were
never invoked. Trusted run/workflow/actor/pin fields were retained; only job fields
were changed. The six controls passed: all three valid Mac lane names admitted,
and unknown workflow, wrong pin and wrong actor denied.

**Nine required rejection cases failed:** Windows, Linux, CUDA, privacy, generic
build, forged fourth lane, wrong architecture, extra label, and missing capability
labels. Each retained the configured worker label and was admitted by this pure
predicate. This is not a live exploit or proof of an actual forbidden dispatch.
Zero external dispatches/registrations/VMs/hosted jobs/check writes occurred because
the harness did not call those facilities, **not because installed guards rejected
these payloads**. Do not report these probes as nine successful negative tests.

At exact C, `controller.outcome` correctly rejects missing lanes, extra or renamed
non-Mac jobs and skipped Mac results; all three valid successes produce success.
A synthetic three-job set retaining Mac names but changing labels to Linux/X64
still yields success in this isolated evaluator. Full controller acceptance also
checks pinned run/workflow/actor/nonce provenance, so this is not a demonstrated
end-to-end Reporter forgery. However the evaluator does not itself authenticate
OS capability. The requested integrated capability/result binding remains open.
No real Reporter App call was made.

The exact-C policy was exercised against a temporary private journal and mocked
GitHub identity/inventory: disabled refused; default paused; explicit owner resume
admitted; health loss paused; healthy return stayed paused; fresh owner resume
admitted again. No API writes or queue calls. This proves isolated state behavior,
not authentication of an installed collector or a real owner credential.

## Preserved source evidence and limitations

The full component suite is rerun for this docs/evidence revision: controller108,
Mac policy/workflow57, routing/capability44, designated mutations43+29, and privacy158.
Results are recorded in the handoff comment with the resulting exact head. The
runtime manifest must remain byte-identical to C. Existing tests cover stale,
future/NaN health, epoch changes, private file ownership/symlinks, owner-lock races,
nonce/provenance and persistent pause, using explicitly synthetic health/API inputs.
They do not qualify an independently authenticated installed health producer.

The committed 15-case Mac-only event matrix retains hosted Windows/Linux/CUDA and
privacy plans for main, PR, fork, feature and owner-manual paths. It models actual
public YAML and preserves full recipe hashes. It does not prove live GitHub
scheduling or absence of cross-repository dispatch in an uninstalled adapter.
Integrated forbidden-payload mutation kills are **NOT RUN/NOT QUALIFIED**: the
baseline host predicate already fails those assertions, so mutation credit would
be meaningless. Historical72 public kills remain scoped to public code only.

## Next bounded implementation and acceptance package

Prepare an **uninstalled**, disabled private candidate for separate review:

1. One manifest-bound queue adapter and worker integration that accepts exactly
   `standalone-release`, `standalone-sanitizers`, `quality-state` with the approved
   Darwin/arm64 capability/labels and workflow pin. Deny all unknown job names,
   workflows, OS/architecture and labels before any VM or runner side effect.
   Keep unrelated repositories' worker policies separate; never widen HyperCine's
   private plane to their generic jobs.
2. Enforce the gate's hard queue-only deadline in the actual adapter. Hold the
   canonical owner lock through acknowledgment only. No detached retry, reentry,
   VM/job wait or hosted fallback. Persist nonce intent and reconcile uncertain
   responses without resubmitting; distinguish missing/duplicate/late ack.
3. Supply an authenticated independent health producer, ownership/access policy,
   restart/outage epoch rotation and freshness/replay proof. Bind its identity and
   executable bytes to the deployment manifest; mere owner-writable JSON is not
   independent collector authentication. Deny bad/missing/replayed health and
   retain pause until an explicit fresh owner decision.
4. Bind reviewed controller, policy, hook, real adapter, health producer, private
   workflow and configuration to one candidate manifest. Test isolated package
   lifecycle, interrupted/ambiguous submissions, owner races, all forbidden
   payloads and result-capability spoofing. Require zero forbidden queue/VM/runner
   effects. Only after baseline guards pass, kill designated removed-guard mutants.
5. Independently review redacted source-bound evidence. Actual installation,
   activation, additive App5612385 rules and eligible H publication remain separate
   owner actions. Obtain genuine Actions15368 B..S evidence on that future H.

No eligible H, production installation/enablement, workflow toggle, paid CI,
branch-rule change or merge is part of this handoff. Optional hosted-result
Reporter adapter can remain unused for initial local-only cutover. #259/#46 stay
open and `upstream/` remains frozen.
