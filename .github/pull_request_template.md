<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

## Scope
<!-- What changed, on which backend, and which issue tracks it? -->

## Validation evidence (public-safe summary)
<!-- Report test counts, checksum comparisons, CI status, GPU model,
     compiler/CUDA/Metal versions and timings; not full machine logs. -->

## Privacy and publishing checklist
- [ ] I manually reviewed the staged diff and all new validation artifacts.
- [ ] I ran the privacy guard against staged changes and every outgoing
      commit (or confirmed both repository hooks were enabled).
- [ ] No private IPs, internal hostnames, UNC/NAS mounts, personal absolute
      paths, credentials or raw system logs were newly published.
- [ ] Local corpus locations are referenced by environment variable name
      (e.g. HYPERCINE_GOLDEN_DIR), never by its machine-specific value.
- [ ] Any unresolved privacy finding is clearly identified as a blocker,
      without reproducing its sensitive value.
- [ ] Frozen upstream/ code is unchanged.
- [ ] No unrelated active agent branch was rewritten or merged.

## Outstanding risks and maintainer decision
<!-- This draft PR does not imply approval to merge. -->
