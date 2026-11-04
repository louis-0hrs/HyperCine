# PR #352 production acceptance: prepared advisory trial

**Preparation only. No deployment, production activation, App expansion, branch
protection mutation, merge or paid hosted-CI restart is authorized by this file.**
The reporter context is not to become required during this trial.

## Review candidate

The portable controller is submitted in a separate stacked Draft PR against
#352's branch. Its parent is `61f8b095759bf178f46a6e2bd220c33a406df52a`.
The Draft PR body records the actual new Head SHA and exact-head test evidence.
Core recipes, codecs, `upstream/` and existing workflows are unchanged.

A proposed first *real HyperCine* advisory candidate is PR #352 itself. At
preparation time its public Head is
`61f8b095759bf178f46a6e2bd220c33a406df52a` and main Base is
`815be4718220644cf7f158bfd2418c00c0955200`. These are observations, not a future
approval. The operator must refresh both canonical refs immediately before any
trial. This document, a comment, or a label cannot approve a later commit.

The candidate is currently Draft. A future controlled Draft trial would require
explicit local `--allow-draft` in advisory mode; required mode rejects it. This
permits independent review without marking an unqualified PR ready for merge.

## Read-only preparation command

No local credentials/configuration values are committed. The checked-in example
is disabled and has null host/queue/installation/provider fields.

```sh
python3 -m ci.premerge_controller.admin \
  --config ci/premerge_controller/config.example.json \
  --preview 352 --allow-draft
```

This command reads GitHub metadata using the operator's existing CLI session.
It returns public candidate SHAs, tree/merge binding, advisory eligibility and
`mutation_performed=false`. It creates no approval, App check or private job.
Preview success is not a successful real advisory run.

## Future trial, only after independent review and explicit authorization

1. Review the complete new Draft diff, privacy scan, package tests and mutations.
   Preserve the existing #352 source diff and its native evidence separately.
2. Separately approve narrow Checks-write/Metadata-read App access to HyperCine
   and deployment of the reviewed package/provider on the trusted host. Verify
   App, source repository, installation, actual private workflow pin and provider
   hash locally. Set up a canonical service-owned 0700 approval directory under
   trusted ancestors; existing ledger/lock files must be owned regular 0600
   single-link files. The controller rejects unsafe directories and symlinks
   before approval/cycle; see the package deployment boundary. None of these
   authorization changes is performed by this PR.
3. Configure an ignored local file with the actual host/queue/provider values.
   Keep `required_gate=false`; leave main-push delegation off. Do not modify any
   required check, ruleset, bypass actor or branch setting for this advisory test.
4. Refresh PR/main refs and run a read-only preview. The local operator then
   explicitly approves exactly those Head/Base values, with `--allow-draft` only
   if deliberately testing the Draft candidate. Approval persists locally and
   does not itself dispatch anything.
5. Only after separate execution approval enable the local advisory controller
   and reporter. One deliberate `--cycle --execute` (or the reviewed worker hook)
   creates a pending App check before dispatch. Existing disposable-VM machinery
   runs the pinned private workflow; no runner is registered on public HyperCine.
6. Observe actual three-lane job conclusions. Success requires correct workflow,
   actor, nonce, Run ID, Attempt1 and all expected successful jobs on the approved
   source. Record the public check's App/source identity and binding, plus local
   redacted provenance and cleanup/usage receipts. Do not infer success from a
   runner process exit or an unrelated/skipped Actions job.
7. On a separately approved real-PR change, verify that a changed Head or Base
   invalidates the stored approval and does not transfer success. Do not create a
   production change solely to manufacture this negative case without permission.
   The current synthetic fixture already supplies enforced Head/Base controls.
8. Stop advisory execution and return the local enable flags to false. Keep
   native results and invalidation evidence separate from synthetic fixture and
   unit results. Independently review before any later required-gate migration.

During advisory mode the App check is informational. Existing publication privacy
remains required; the skipped #352 Head has no eligible production privacy PASS.
No App success or fixture privacy result substitutes for that mandatory gate.

## Existing synthetic evidence, not a production result

The prior isolated fixture recorded eleven blocked and two accepted GitHub merges,
including stale Head/Base checks, real Reporter/Actions identities and exact
Tree/parent verification. The first failed test-merge-SHA profile is explicitly
retained as failure. Those results are linked from #352 and qualify the synthetic
policy; they do not qualify this not-yet-deployed production package.

## Local validation receipt

[Sanitized test and read-only preview evidence](validation/premerge-controller-review-20261104.json)
records the original package suites, mutations, historical native full CTest
rerun and real-PR read-only preview. The focused owner-private state correction
has its own [validation receipt](validation/premerge-state-trust-review-20261104.json).
Historical native/fixture evidence is not a rerun on the corrected controller. The Draft PR records the exact
new Head SHA and controller rerun on that committed version. No raw VM/build
logs, host paths, private queue identifiers or signing material are published.
