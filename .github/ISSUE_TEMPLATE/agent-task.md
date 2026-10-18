---
name: Agent implementation task (public-safe)
about: Plan a Windows, macOS or Linux agent task without exposing local infrastructure
title: "[Agent] "
labels: []
assignees: []
---

<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

## Goal and scope
<!-- Describe public product behavior and the smallest intended change. -->

## Platform, ownership, and branch
- Platform / backend:
- Allowed source directories:
- Forbidden source directories / cross-agent dependencies:
- Intended base branch:
- Draft PR expected: yes

## Public-safe reference material
- Repository-relative fixtures:
- External corpus variable (name ONLY, not its local value): HYPERCINE_GOLDEN_DIR
- Expected hashes / frame counts / deterministic test cases:

Do not place real private IP addresses, NAS/UNC/SMB/NFS shares,
internal DNS names, personal home-directory paths or credentials in
issues, comments, logs or PRs. Pass external paths to the agent by
local environment configuration outside this public repository.

## Acceptance and evidence
- [ ] Source changes limited to approved scope; upstream/ unmodified
- [ ] Build and CTest results identified for the relevant platform
- [ ] Exactness/compatibility and regression evidence included
- [ ] Benchmark claims backed by reproducible measurements (if applicable)
- [ ] All public validation files sanitized and manually reviewed
- [ ] Privacy guard run before commit **and before push**
- [ ] Draft PR only; no merge, force-push or branch rewrite unless authorized

## Hand-off
<!-- Note remaining blockers without pasting local environment values. -->
