# HyperCine Source Tree

This directory contains the planned native and accelerated backend implementations for HyperCine:

- `src/cpu/`: Future optimized CPU implementation and integration glue. In Phase 0, the upstream reference in `upstream/` is the sole authoritative CPU implementation.
- `src/cuda/`: NVIDIA CUDA GPU backend (Phase 1+).
- `src/vulkan/`: Vulkan compute backend for cross-vendor GPU acceleration (AMD, Intel, NVIDIA).
- `src/metal/`: Apple Metal compute backend (macOS / iOS).
- `src/core/`: Common data structures, dispatch interfaces, and bitstream definitions.
