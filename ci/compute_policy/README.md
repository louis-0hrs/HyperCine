# Mac-only admission — disabled review package

Owner clarification6655103747 supersedes the permanent whole-workflow pause policy.
**Only macOS migrates.** Windows, Linux CPU/sanitizers and CUDA retain their hosted
triggers, rows, steps and recipes. The GitHub adapter is GET-only, including owner
mode changes. No watch/gate/CLI path can enable/disable a workflow or dispatch a job.

| Local mode | Hosted non-Mac | Private Mac | Hosted Mac |
| --- | --- | --- | --- |
| normal | existing jobs continue | fresh owner-bound health/session | never automatic |
| paused/host-unavailable | existing jobs continue | denied | never automatic |
| recovering | existing jobs continue | denied | never automatic |
| owner-authorized-hosted | existing jobs continue | denied | exact candidate grant plus separate explicit owner manual request |

Automatic main/feature pushes, same-repo/fork PRs and ordinary manual requests
exclude the three Mac matrix rows. Missing/legacy variables cannot enable paid
Mac. The original two Standalone and one Quality Mac recipes remain available
only through manual typed `mac_hosted_fallback=true`, actor ID248477380 AND
triggering actor `louis-0hrs`, and `mac_candidate_sha` equal to the selected run's
exact github.sha. Non-owner reruns, missing acknowledgment and stale refs deny
Mac. Fallback adds Mac rows without removing non-Mac rows. No fallback is executed
by this package; it changes local admission state only.

## Reporting and required-check semantics

Absent matrix rows are **NOT RUN**, never green Mac validation. Their individual
Actions check names will not appear automatically. GitHub can accept skipped jobs;
do not add a dummy skipped/success job as a substitute Mac gate. The separate
`HyperCI / Mac relay` aggregate is Reporter App5612385 only, using the existing
Head/Base/tree/test-merge, nonce/workflow/actor/run/attempt and three successful
lane proofs in `premerge_controller`. Its protocol/recipes remain unchanged.
Public Actions15368 statuses cannot impersonate that aggregate.

Reporter is still optional/disabled and no branch rule changes here. Before
production cutover, independently approve/qualify the additive App-pinned aggregate
while preserving required Publication privacy Actions15368. Until that occurs
this remains a Draft source candidate, not production. A new candidate with no
qualified Mac result must remain pending/failing, never success. Fork code is not
automatically sent to a private VM; existing owner/repository approval guards apply.

A manual hosted Mac result is a real Actions result, not automatically an App
aggregate success. The private-run verifier must not silently accept a different
public workflow as proof. If fallback must satisfy a future required aggregate,
separately review a source-bound hosted-result verifier/Reporter adapter first;
until qualified, the aggregate remains pending/blocking. No copied green check.

## Owner decisions, storage and recovery

Schema2 records Mac mode, immutable expiring plans, generation/audit, health epoch,
exact fallback SHA and read-only workflow/security snapshots. Schema1 global-policy
journals reject; no silent import, rollback or stale approval reuse. Keep old audit
records separately and initialize fresh owner-inspected state. All hosted workflow
families must stay active; external disable/drift latches Mac attention and never
triggers automatic remote repair. Unrelated hosted jobs do not block Mac pause.

Owner/admin identity is authenticated. No workflow-write grant is needed/requested
for this GET-only runtime. Health/API/relay loss or epoch change latches Mac pause;
healthy return never resumes. Plans expire after five minutes, generations fence
old commands/errors. Trusted health must be fresh within60 seconds (five seconds
future skew max), with32-hex availability epoch rotated on outage/relay or collector
restart. Authenticated independent collector, trusted installed package and actual
private queue adapter are separate deployment prerequisites. Same-UID/root trusted.

## Owner pause versus private queue submission

`gate.admit_local` uses watch only as a generation fence. It reloads current state,
live inventory/security and health/session under the canonical owner-transition
OS lock, holding it through bounded queue acknowledgment only. Owner pause first
means zero submissions; submission lock first permits that in-flight request before
the pause completes. Pre-existing accepted work may execute later. No lock through
VM/job lifetime. Barrier tests exercise both orders against actual flock.

Trusted `dispatch_private(*, timeout)` must enforce the supplied ten-second network
deadline synchronously, return on queue acknowledgment, and never wait for a VM,
detach/retry, reenter owner commands or use another destination. A keyword cannot
sandbox trusted Python code; review the installed adapter before activation.
Uncertain acknowledgment pauses without retry; reconcile nonce/run before another
request. An old worker error cannot overwrite a newer owner generation.

## Temporary cost window is separate

A separately owner-authorized audited pause of four workflow families may reduce
one qualification to privacy-only jobs. That is **temporary repo-wide cost control**,
not Mac runtime. This adapter cannot perform it. Explicit owner restore/readback of
previously active families is required before normal non-Mac CI resumes. Failure
means visible attention/manual reconciliation, never automatic restore or false
normal-state claims. See the [runbook](../../docs/FAIL_CLOSED_CI_RUNBOOK.md) and
[integration window](../../docs/INTEGRATION_PRIVACY_WINDOW.md).

## Reproduction

POSIX Python3.9+, PyYAML6.0.2, CMake and OpenSSL. TEST_PYTHON must contain PyYAML:

```sh
cmake -S ci/premerge_controller -B build/premerge-controller -DPython3_EXECUTABLE="$TEST_PYTHON"
ctest --test-dir build/premerge-controller -C Release --output-on-failure
python3 -m unittest ci.test_workflow_event_matrix ci.test_runner_capability
```

CTest retains the complete premerge/protocol/storage suite and mutations, plus
Mac state/CLI/transport/admission/event tests. Non-Mac rows and full job-step hashes
compare with frozen parent4ceab03. GitHub is mocked; actual POSIX locks are exercised.
Old assertions demanding all workflows OFF in normal mode are retired because they
contradict the corrected scope, not counted as new passing tests. Historical receipts
remain bound to their original source. No service, credentials or host paths included.
