# Security Policy

## Scope

HyperCine is an experimental, research-grade GPU-accelerated encoder derived from the open-source GoPro CineForm SDK. Because video codecs process untrusted media buffers and bitstreams, memory safety, integer overflow bounds, and buffer capacity checks (such as highpass VLC word bounds and asynchronous GPU error propagation) are treated as high-priority correctness issues.

## Reporting a Vulnerability

If you discover a potential security vulnerability (such as an out-of-bounds read/write in host or CUDA device memory, integer overflow in buffer sizing, or an unbounded loop triggered by crafted input):

1. **Prefer Private Disclosure First:** If GitHub Private Vulnerability Reporting (Security Advisories) is enabled on this repository, please submit a report through the repository's **Security -> Report a vulnerability** tab rather than opening a public issue with exploit details.
2. **Public Issue Fallback:** If private advisory reporting is unavailable and the issue is a non-exploitable bug or sanitizer finding, open a minimal GitHub issue describing the affected component and reproduction environment without attaching sensitive data.

## What to Include

- Exact git commit hash or tag (e.g., `phase3a2-release-candidate`).
- Build configuration, OS, compiler, and GPU/driver version (if CUDA-related).
- Minimal deterministic reproduction steps or test vector (or `compute-sanitizer` / AddressSanitizer output).
