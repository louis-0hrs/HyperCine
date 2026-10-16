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
5. Configure local hooks per clone or worktree (Git does not auto-activate hooks):
   `git config --local core.hooksPath .githooks`
   The hook files require executable permissions on Unix-like systems.
   Where hooks cannot execute, agents must invoke the scanner explicitly.
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
commit messages. It reports file/line/rule only, with sensitive filenames
redacted. It does not output the matched value.

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
