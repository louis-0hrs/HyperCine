# Contributing to HyperCine

Thank you for your interest in contributing to **HyperCine**. To keep the repository reproducible, auditable, and strictly compatible with the CineForm (`CFHD` / SMPTE VC-5) specification, all contributions must follow these core engineering rules.

---

## 1. Preserve Upstream Immutability
- The entire [`upstream/`](upstream/) directory contains the pristine GoPro CineForm SDK reference baseline (`11574d0295771edccadd17af14af74a539f924c7`).
- **Never** modify, reformat, patch, or modernize any file under `upstream/`.
- `git status --porcelain upstream/` must always be empty.
- Handle any upstream build or runtime quirks in the root `CMakeLists.txt` or HyperCine wrapper layers, and document them in [`docs/UPSTREAM.md`](docs/UPSTREAM.md).

## 2. Correctness Before Optimization
- Every encoder, wavelet, quantization, or entropy change must pass the full CTest verification suite (`ctest --test-dir build -C Release --output-on-failure`) before any performance claim is considered.
- Do not trade away bitstream validity, deterministic rate-control behavior, or capacity safety bounds for higher FPS.
- All new source files must carry the standard MIT / Apache-2.0 dual-license header.

## 3. New Codec Paths Require Bitstream Compatibility Tests
- Any new pixel format conversion, wavelet path, quantization mode, or entropy kernel must include deterministic tests under `tests/` verifying:
  - Coefficient and subband payload equivalence against the upstream reference (or documented mathematical bounds where applicable).
  - Successful decode verification using the pristine upstream decoder (`CFHD_DecodeSample`).
- Both CPU-only (`-DHYPERCINE_ENABLE_CUDA=OFF`) and CUDA-enabled (`-DHYPERCINE_ENABLE_CUDA=ON`) builds must remain green.

## 4. Benchmark Changes Require Methodology Disclosure
- Public throughput claims must use the canonical `ProductionEndToEnd` contract (`HostPageableEndToEnd` as the primary desktop headline) with at least 5 trials and median FPS as the headline metric.
- Always disclose the exact baseline being compared against (e.g., pristine single-threaded `CFHD_EncodeSample` with `_THREADED_ENCODER=0`), hardware/clock telemetry, slot count, and input residency mode (`HostPageable`, `HostPinned`, or `DeviceInput`).
- Never present `DeviceInput` (GPU-resident input) as conventional file-transcode throughput.
- Canonical benchmark JSON artifacts must be generated from a clean git working tree without `--allow-dirty`.
