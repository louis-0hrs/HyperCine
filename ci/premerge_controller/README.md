# Reviewed premerge controller — disabled deployment package

This package prepares production review for PR #352. It imports no host settings,
registers no runner and changes no branch protection. Importing it has no side
effects. It is not installed by CMake, a workflow, or a Git hook.

The public package replaces machine-specific worker/key-store code with reviewed
interfaces. The complete implementation, protocol tests, adapters, mutation
runner and disabled configuration are available here; there is no hidden success
predicate. The operator still owns the existing disposable-VM worker and a
separately reviewed signing-key provider. No VM image, machine path, private queue
name, installation identifier or signing material is bundled.

## Modules

- `controller.py`: local approval, durable pending check before dispatch, private
  run/job provenance, invalidation and App result aggregation.
- `candidate_binding.py`: independently resolve protected main and source branch
  refs; verify exact expected Head/Base, merge parents, ancestry and tree equality.
  Cached PR metadata cannot silently preserve an old approval.
- `checks_reporter.py`: repo/App/installation-scoped Checks transport. Production
  reporting is disabled unless explicitly configured; the default key provider
  fails closed. Public output contains only public candidate SHAs and a binding
  digest, not the private queue URL or raw private run identity.
- `bridge.py`: private-workflow pin verification. No worker internals are imported.
- `state_store.py`: owner-private, descriptor-relative approval ledger and lock;
  exclusive temporary files and durable atomic replacement.
- `worker_hook.py`: complete worker seam; missing/disabled configuration returns
  without API calls. The worker injects its approved API and reporter interfaces.
- `adapters.py`: optional scoped GitHub CLI and owner/hash-verified key-reader
  adapters. The CLI adapter cannot merge, alter rules, or register runners.
- `admin.py`: explicit local preview/approval/cycle commands. No public labels,
  comments or PR actors automatically approve work. A real cycle needs
  `--execute`, explicit state, enabled controller/reporter and a configured key
  adapter. The disabled example is not an operational configuration.

## Reproduce all package tests

On a POSIX host (macOS/Linux), from the repository root, using Python 3.9+
and OpenSSL. Windows service deployment is not implemented by this package:

```sh
cmake -S ci/premerge_controller -B build/premerge-controller
ctest --test-dir build/premerge-controller -C Release --output-on-failure
```

CTest runs the complete portable package suite and all controlled mutations.
The mutation runner uses isolated temporary copies, verifies a passing baseline,
then removes one security property per copy. A kill requires failure of the
specifically designated regression test; syntax/import errors and unrelated
failures are not counted as evidence. Test-only RSA keys are generated locally,
never taken from a real GitHub credential. No unit/mutation test contacts GitHub,
executes a real key provider or starts a VM/private dispatch.

Direct commands:

```sh
python3 -m unittest discover -s ci/premerge_controller/tests -v
python3 -m ci.premerge_controller.mutation_suite --output build/premerge-mutations.json
```

## Worker integration interface

After separate deployment approval, the existing trusted worker may call:

```python
from ci.premerge_controller.worker_hook import poll
poll(api, worker_config, state_directory, reporter_factory=approved_reporter_factory)
```

`api` is the host's reviewed GitHub boundary. The existing worker retains VM,
network-isolation, queue, usage and cleanup responsibilities. `reporter_factory`
provides a `ProductionReporter` with a local signing-key reader. Private values
belong in the operator's ignored local config, never in this repository. This
hook does not enable itself or replace the current worker. Deploy only the
independently reviewed controller version into a trusted host location; never
import the controller from an untrusted candidate checkout or auto-pull updates
from a public branch into the privileged host execution path.

## Approval-state deployment boundary

The state directory is security-sensitive: its ledger authorizes private dispatch
and binds the public check. Deploy it outside candidate checkouts, under the
trusted service UID, with mode **0700**. Ledger and lock must be owned regular
single-link files with mode **0600**. Do not use a shared directory. Root and all
processes running as that service UID are trusted; this is not protection against
a compromised service UID, nor cryptographic detection of owner-written forgeries.

Supply a canonical path with no symlink components (including operating-system
path aliases). The controller validates every ancestor using pinned directory
file descriptors and no-follow opens. Ancestors must belong to root/service UID
and must not be group/world writable; a root-owned sticky temporary parent is the
only writable-ancestor exception. Existing unsafe permissions are rejected, never
silently repaired. Approval may create the final missing directory as 0700;
parents must already exist and be trusted. Cycle never creates a missing state
directory. Disabled controllers remain no-ops.

Both approval and enabled cycles validate the directory, ledger and lock before
GitHub requests, signing or dispatch. Symlinks (including dangling links), FIFOs,
directories, hardlinked files, malformed/duplicate-key JSON and inconsistent
record bindings fail closed. The old predictable `premerge.json.tmp` must be
absent; any leftover entry requires operator investigation, not automatic
recovery. An absent ledger in an existing safe directory means no approved work.

All ledger reads/writes and lock acquisition are relative to the validated
state-directory descriptor. Persistence creates a random temporary name with
`O_CREAT|O_EXCL|O_NOFOLLOW`, flushes and fsyncs that file, atomically replaces the
ledger, then fsyncs the directory. Failed replacement keeps the previous ledger
and cleans only the temporary file created by that operation. A colliding name
is never truncated or followed. Revalidate the deployment's filesystem guarantees
and effective access controls; use local storage with POSIX locking and atomic
rename semantics. Windows and network-filesystem service deployment are outside
this package's qualified scope.

## Advisory versus required enforcement

The initial real-PR trial is **advisory only** (`required_gate=false`). It checks
that existing main protection still requires a PR and Actions-owned publication
privacy. It needs no new ruleset or strict-base setting, makes no protection
writes, and never claims that its App check is an enforced merge gate.

Advisory success is a point-in-time attestation: the controller revalidates the
current Head/Base immediately before publishing and invalidates an old approval
on its next poll after either ref changes. Without a mandatory strict App gate,
the GitHub server does not enforce that advisory result. Privacy remains the
independent unchanged required check. Advisory permission for a Draft PR requires
an explicit local `--allow-draft`; it cannot authorize required-mode Draft work.

A later required-mode migration is out of scope. It would separately need an
active, applicable, no-bypass strict-base ruleset and an App-pinned required
context. The package refuses to claim required-mode readiness without those
conditions. It never creates such a rule. Changing modes requires renewed local
approval; a stored approval is not silently promoted.

See [the non-executing real-PR advisory plan](../../docs/PREMERGE_ADVISORY_REVIEW.md)
and the exact-head validation receipt linked from the Draft PR.
