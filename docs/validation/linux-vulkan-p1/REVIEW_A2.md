# Linux Vulkan P1 - independent A2 review validation (PR #61, issue #31)

Independent re-validation of the exact PR #61 head `05b889a` on the Linux A2
lane, requested by the #62 dispatch board. Scope: build with Vulkan OFF, build
and parity with Vulkan ON, resource/queue/command-buffer ownership review, the
GPU-required fail-closed path, and - newly available here - a real run under the
Khronos validation layer. This document records only what was actually executed.

Machine-readable summary: `docs/validation/linux-vulkan-p1/validation.json`.

## Environment

| Item | Value |
|------|-------|
| GPU | NVIDIA A2, compute capability 8.6, driver 550.163.01 |
| Vulkan instance | 1.4.309 |
| Physical-device API | 1.3.277 (NVIDIA ICD) |
| Validation layer | `VK_LAYER_KHRONOS_validation` (Chromium-bundled runtime, user-level manifest) |
| shaderc runtime | `libshaderc1` 2025.2-1 |
| Compiler | GCC 14.2.0 |

Shader build fingerprint is unchanged from the original record
(`color_cg709` 1968 words / `b382a2254df10ccb`, `wavelet_h26` 2024 /
`2c3e3eae4d8e5abc`, `wavelet_v26` 2022 / `40fc48953eed6386`).

## 1. Vulkan OFF - portable standalone untouched

Configuration `HYPERCINE_BUILD_STANDALONE=ON`, `UPSTREAM=OFF`, `CUDA=OFF`,
`VULKAN=OFF`:

```
100% tests passed, 0 tests failed out of 27
Total Test time (real) = 516.54 sec
```

No new target and no new link-time dependency are introduced when the option is
OFF. The default-`OFF` contract holds.

## 2. Vulkan ON - build, parity and full CTest

Configuration `HYPERCINE_BUILD_STANDALONE=ON`, `VULKAN=ON`:

- Build: PASS (SPIR-V embedded at build time via `dlopen`ed shaderc runtime).
- `test_vulkan_filmscan3 --gpu-required`: `RESULT: PASS (all samples bit-exact)`,
  133 compared cases (color 36, wavelet_h 42, wavelet_v 42, forward_level 12
  across 128/256/1920x1088, padded and unpadded strides).
- Full `ctest --output-on-failure`:

```
100% tests passed, 0 tests failed out of 28
Label Time Summary:
gpu-required    =  45.93 sec*proc (1 test)
vulkan          =  45.93 sec*proc (1 test)
Total Test time (real) = 560.44 sec
```

## 3. GPU-required fail-closed path

With `VK_ICD_FILENAMES` pointed at a non-existent ICD (no usable device):

| Run | Exit | Output |
|-----|------|--------|
| `--gpu-required` | **1** | `GPU REQUIRED run: FAIL (no Vulkan device)` |
| default (optional) | **77** | `SKIP (no Vulkan device; not required)` |

The GPU path fails closed and never silently falls back to the CPU.

## 4. Resource / queue / command-buffer ownership (static review at head)

- Every device resource (`VkBuffer`/`VkDeviceMemory`, `VkShaderModule`,
  `VkDescriptorSetLayout`, `VkPipelineLayout`, `VkPipeline`) is created per call
  and destroyed on **both** the success and exception paths.
- Per-dispatch `VkDescriptorSet` and `VkCommandBuffer` are allocated from pools
  with a single queue, then freed after `vkQueueWaitIdle`, so the pools are
  reused across the long run instead of exhausting (descriptor pool `maxSets`
  256 with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`).
- `vkCmdResetQueryPool` is recorded **between** `vkBeginCommandBuffer` and
  `vkEndCommandBuffer` (the bring-up defect 1 fix); timestamps are gated on
  `timestampValidBits` and optional entry-point availability.
- Destructor order: query pool -> descriptor pool -> command pool -> device ->
  instance; no use-after-free of the device-bound pools.
- Device selection prefers a discrete GPU, then integrated, then any compute
  queue; a CPU-type device is only used when no GPU exposes a compute queue.

## 5. Khronos validation layer - now actually run

The original record honestly stated the validation layers were not installed.
On this host the layer **runtime** ships inside the Chromium bundle
(`/usr/lib/chromium/libVkLayer_khronos_validation.so`) but has no loader
manifest, so it was invisible. A **user-level manifest** was added under the
per-user Vulkan `explicit_layer.d` directory and exposed via `VK_LAYER_PATH`;
the layer was then forced into the run with `VK_INSTANCE_LAYERS`. This required
no root, no system change and no repository code change.

Loader debug confirms real insertion:

```
INFO | LAYER:   Insert instance layer "VK_LAYER_KHRONOS_validation"
```

Full parity run under the forced layer, core validation enabled:

```
PASS lines: 133
RESULT: PASS (all samples bit-exact)
VALIDATION ERRORS:   0
VALIDATION WARNINGS: 0
```

Scope note recorded honestly: Vulkan **core** validation covered the whole run
with zero errors. GPU-assisted (shader-instrumentation) validation self-disabled
with a layer-internal error under this loader/layer version combination; that is
a layer limitation, not an application or codec defect, and is **not** claimed as
coverage.

## 6. Explicitly not claimed

- No CFHD bitstream compatibility, no production encoder claim.
- No speed claim: P1 uses host-visible buffers, so wall time is transfer-bound.
- No L2/L3, quantization, entropy or framing.
- No GPU-assisted validation coverage.
- No merge: PR #61 stays Draft; promotion is a maintainer decision.
