# Safe publication protocol for HyperCine

HyperCine is a **public** repository. A pushed commit is public immediately,
including commits on draft-PR branches. Removing a line in a later commit does
not remove it from Git history. This applies equally to Windows, Linux and macOS.

## Before publishing

1. Keep NAS/scratch corpora, full system logs, generated CMake outputs and
   unredacted machine logs **outside** the repository tree.
2. Pass external golden reference locations with the environment variable
   `HYPERCINE_GOLDEN_DIR`. In public docs, show only the variable name
   and a repository-relative example; never disclose its local value.
3. Keep meaningful evidence: deterministic hashes, frame counts,
   bit-exactness, timings, sanitizer results, GPU model and tool versions.
   Replace machine-specific full paths with repository-relative paths or
   descriptions such as "external corpus (read-only)".
4. Check the full staged diff manually. Run:
   `python ci/privacy_guard.py --staged`
5. Configure local hooks per clone or worktree (Git does not auto-activate hooks).
   For **separate per-worktree settings** enable `extensions.worktreeConfig=true`
   in the shared Git configuration first (only after confirming every worktree
   supports this Git feature), then in the intended worktree run:
   `git config --worktree core.hooksPath .githooks`
   Without this setup, `git config --local` writes to the *shared* config and
   changes hook behavior for every linked worktree. Every affected worktree
   must contain the hooks and `ci/privacy_guard.py` at the expected paths.
   Git for Windows may expose an unusable `python3` Store alias; hooks now
   verify that the interpreter actually runs Python 3.8+, trying `python3`,
   then `python`, then Windows `py -3`, and fail closed if none works.
   Hook files require executable permissions on Unix-like systems.
6. The pre-push hook scans every outgoing commit. Always push through a
   configured remote name (e.g. `git push origin my-branch`), not a raw URL;
   new branches exclude only commits already known on that destination
   remote. Never bypass the hook by using `--no-verify`, removing it or
   suppressing scanner findings.
7. Check PR titles, issue bodies and comments separately; Git diff scanners
   cannot inspect public conversation text.

## What the guard checks

The repository scanner is Python 3 standard-library-only. It checks additions
in the staging area or each selected commit for RFC1918 IPv4 addresses, private
DNS suffixes, UNC/SMB/NFS-style shares, user-specific Windows/Unix home paths,
tilde-relative home paths and likely secrets. It also scans new path names and
commit messages. Staged scans also inspect the effective author and committer
identities using `git var`, including `GIT_AUTHOR_*` / `GIT_COMMITTER_*`
overrides. Commit and range scans inspect the actual author and committer
name/email stored in every selected Git object, without mailmap rewriting.
The same text rules apply to those fields; single-label email domains and
Git's inferred `(none)` domain are also rejected as likely host-derived
identities. Missing/unreadable identity data blocks the scan.

Use a deliberately configured identity with a public domain, a chosen GitHub
noreply address, or a neutral agent address such as
`hypercine-agent@example.invalid`. Configure it only in your own worktree
with `git config --worktree user.name`, `git config --worktree user.email`,
and `git config --worktree user.useConfigOnly true` after the per-worktree
setup above. These settings do not override explicit `GIT_*` environment
values; review overrides locally without publishing raw identity output.
The guard cannot infer whether an arbitrary name or public email domain
identifies private infrastructure, so manual metadata review remains required.

Diagnostics report only a file or identity-field marker, line and rule,
with sensitive filenames redacted. They do not output the matched value.

It is a **defense in depth**, not proof of privacy. It cannot identify every
internal hostname, embedded binary metadata, every credential format or leaks
in files already committed before this policy. Do not add blanket exceptions
to silence results; use explicit sanitized data instead.

## CI gate and its limitation

The independent `Privacy guard / publication-privacy` GitHub Actions job
scans all commits introduced by a PR, including content later removed in the
same PR. It additionally runs on pushes to **every branch**, checking commits
since the previous push or the verified merge base with `main` on first push.
Maintainers should
make the check required in branch protection/rulesets after it is verified.
GitHub CI begins **after pushing**, so it cannot undo initial public exposure.

## If a public disclosure is discovered

1. Stop pushing new copies. Report only file/PR/commit references and the type
   of exposed data, not the data itself.
2. Determine whether the content is merely a topology hint, personally
   identifying metadata or an actual credential. Rotate/revoke real secrets
   as appropriate; changing a path in Git is **not** a security revocation.
3. Coordinate with owners of all active branches before modifying any branch.
4. Prefer sanitized follow-up commits for public visible files. Evaluate any
   history rewrite, force push, cache cleanup or GitHub support request
   separately; rewriting public history is disruptive and never guarantees
   previously fetched content disappears.
5. Keep checksums, reproducibility reports and codec test results intact.

Tracking: issue #54. No active feature branch history is rewritten by this
hardening change.

## Combined hardening and Git identity policy (issue #54)

Six overlapping PRs are integrated together: #74 (`+++` hunk content),
#78 (disable textconv), #79 (quoted paths), #81 (binary/empty/rename-only
destination path scanning), #85 (fixed Git diff prefixes), and #87 (raw
author/committer identity scanning). The scanner inspects added text and a
NUL-delimited destination-path inventory in staged, commit, and range modes.

**Pre-commit:** configure `user.name` and `user.email` explicitly. Absent
configuration fails closed rather than trusting an auto-inferred hostname.
Agent-named identities also require `user.useConfigOnly=true`. For agents
use a deliberate `agent@example.invalid` style alias or a verified GitHub
`users.noreply.github.com` address. Human contributors may retain explicitly
configured public identities; the scanner cannot prove their provenance.

**Commit/pre-push:** raw author and committer envelope fields are checked,
including public-looking multi-label dynamic/reverse-DNS host patterns that
the earlier suffix-only implementation missed. This heuristic cannot detect
*every* machine-owned public domain: Git objects have no explicit record
of whether their metadata came from host inference. Manual author/committer
review remains mandatory for imported public branches.

The scanner must never print matched sensitive values. Historic Git objects,
previous issue revisions, clones and caches remain public independently of
forward-only redaction; no history rewrite/force-push is authorized here.
