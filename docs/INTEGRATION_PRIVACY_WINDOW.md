# #352/#353 privacy-only qualification under persistent pause policy

Supersedes the restore-on-exit assumption in the earlier plan/comment6645154714.
Owner policy6645766280 requires persistent compute pause after host/relay failure
and explicit authenticated owner resumption. This file authorizes no live action.

## Candidate binding and current scope

This policy revision changes the #353 controller/security package. The old unpublished
empty candidate `fc6c39b022af9db76c35afbef60d9fd9b924ba86` / old tree
`cefd557952fbdef35e5a025992595f2f9f7f969b` is **superseded**, not an eligible candidate
for this new code. No replacement non-skipped H is created by this revision.
The PR body/follow-up records the actual new skipped source Head and source manifest.
The original #352 source `61f8b095759bf178f46a6e2bd220c33a406df52a` and native inputs
remain unchanged; previous real advisory results bind only to that original source.

After independent review of the new exact source, separately authorize one combined
integration branch/PR with that reviewed tree. Seed at its skipped reviewed tip;
create a Draft with no CI. Only then prepare a deliberately authorized empty,
non-skipped child H preserving the **new** reviewed tree. Recalculate complete
file/blob/SHA256 equivalence and actual main Base B; do not reuse old approvals,
checks, trees or tables. No private host/queue/key material belongs in that PR.

## Narrow one-time window

1. Use the independently installed reviewed control plane and an explicit owner
   privacy-only decision. Read exact four workflow states, grants, active unrelated
   runs, mandatory privacy and no-bypass protection. Coordinate a quiet window.
   Unknown required checks or any failed permission/inventory read means no start.
2. Capture a fresh generation-bound pause plan using the manual runbook. Execute
   only after separate authorization. No workflow enable, dispatch or timer occurs.
   The four optional build workflows are paused persistently; mandatory Privacy,
   its Windows security tests and independent static routing remain active.
3. Push only the separately approved empty H on the seeded branch. A known-empty
   two-dot push runs the two Privacy jobs. The PR's full three-dot stack comparison
   runs those two Privacy jobs and static routing: **five predicted small jobs**
   (three Ubuntu/two Windows), no macOS/CUDA/codec build matrix. No skip directive
   may be placed on H. These are counts, not minute/duration guarantees.
4. Require actual completed/success for the two Privacy runs and routing. The push
   scanner covers only the metadata tip; it is insufficient to qualify the stack.
   The PR workflow must scan the whole introduced range detailed below.
5. Read back all four **still-paused** states and unchanged required privacy. Keep
   the paused journal/audit. Do not auto-restore original compute states at the end,
   on error, on a signal, after timeout, after a host restart or when health recovers.
   A partial disable/readback failure is visible uncertainty and owner attention;
   an explicit fresh manual pause-reconcile is the recovery path.
6. Return source-bound H/B/current test-merge S, actual check/run identities and
   privacy evidence for independent acceptance. No merge or production activation.
   If the owner later wants Mac or hosted compute, use a fresh authenticated resume
   plan, not the historical snapshot or a previously saved enable command.

The persistent operating policy and this one-time qualification are distinct. No
watchdog/service or live mode is installed by the source change. Cost pause itself
must never disable a required privacy/security workflow. The currently non-required
CUDA inventory self-test shares a paused optional workflow and remains NOT RUN;
if it becomes required, preflight refuses until it is split/reviewed.

## Actual privacy workflow: checkout S and scan B..S

The workflow remains byte-for-byte unchanged. Its pinned blob is enforced by policy.
It has `pull_request` and all-branch `push` triggers, no `workflow_dispatch`.
The Ubuntu `publication-privacy` job runs `actions/checkout@v4` with `fetch-depth:0`.
For a PR event, default checkout uses the virtual test-merge commit S, not a manual
checkout of the old source branch. `PR_BASE_SHA` is the event's Base B. The script
sets `GIT_NO_REPLACE_OBJECTS=1`, verifies B is a commit, then executes:

```sh
python ci/privacy_guard.py --commits "$base..HEAD"
```

Thus the required **PR** qualification is **B..S**, including all original introduced
commits, the new reviewed policy commits, H and S. An empty tip does not exempt the
stack's earlier added text or commit envelopes. On push C..H only the tip is scanned;
that push PASS alone is not the full-stack acceptance evidence.

For the actual future run, record the checkout commit from checkout logs, event B,
current PR H/Base/test-merge S, run event and Head, actual check `head_sha`, name,
App15368/status/conclusion and the PR check rollup. GitHub may evaluate S rather
than H; do not equate workflow `head_sha`, checkout SHA and evaluated check SHA
without reading them. Verify the actual current evaluated SHA before declaring the
required gate satisfied. Use genuine push/PR checks, not manual dispatch, a local
scanner, same-name commit status or Reporter5612385. Missing/pending required
privacy blocks merge; no rules are edited and no success is forged.

Current official behavior and workflow evidence:
- [Skip instructions leave required checks pending](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/skip-workflow-runs).
- [Required-check events and Head/test-merge selection](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks).
- [PR event merge SHA](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#pull_request).
- [Checkout pull-request merge default](https://github.com/actions/checkout).
- [Disabling does not cancel existing runs](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/disable-and-enable-workflows).

## Final landing is a separate decision

After genuine H/S privacy acceptance, obtain explicit merge/tree/identity approval.
Historical source commits contain skip markers; a normal merge/rebase can suppress
main push workflows because GitHub considers every newly pushed commit message.
A clean-message squash is a reviewable tree-preserving option; it changes main's
ancestry, so preserve original source refs as provenance and obtain that choice
explicitly. Final GitHub-created F is not assigned a fictitious SHA in advance.
Verify its parent/tree, then genuine post-merge privacy on F. No skipped main merge
or transferred green check substitutes for this.

If the owner deliberately keeps the four optional workflows paused, a later clean
eligible main push predicts three cheap jobs (two Privacy plus static routing),
eight total with qualification. **No automatic resume is part of that estimate.**
If the owner explicitly resumes hosted compute before landing, current restored
triggers predict fourteen main jobs, nineteen with qualification. Re-inventory
triggers/states at execution time; neither budget/merge is authorized here.
Main Mac routing and mandatory Reporter activation remain separate reviewed work.
