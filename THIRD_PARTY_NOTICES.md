# Third-Party Notices and Attribution

**HyperCine** is an independent open-source project implementing and accelerating CineForm-compatible (`CFHD` / SMPTE ST 2073 VC-5) encoding. HyperCine is not affiliated with, sponsored by, or endorsed by GoPro, Inc.

All original HyperCine source files (under `include/hypercine/`, `src/`, `tests/`, `benchmarks/`, and `cmake/`) are dual-licensed under the **MIT License** ([`LICENSE-MIT`](LICENSE-MIT)) or the **Apache License, Version 2.0** ([`LICENSE-APACHE`](LICENSE-APACHE)), at the recipient's option.

---

## 1. GoPro CineForm SDK (`upstream/`)

HyperCine incorporates the open-source **GoPro CineForm SDK** as an immutable reference baseline and verification oracle inside the [`upstream/`](upstream/) directory.

- **Upstream Project:** GoPro CineForm SDK
- **Upstream Source URL:** `https://github.com/gopro/cineform-sdk`
- **Imported Git Commit:** `11574d0295771edccadd17af14af74a539f924c7`
- **Repository Location:** [`upstream/`](upstream/)
- **Immutability Statement:** The entire [`upstream/`](upstream/) directory is preserved 100% unmodified from upstream commit `11574d0295771edccadd17af14af74a539f924c7` (`git status --porcelain upstream/` is empty). All original upstream copyright headers and license files ([`upstream/LICENSE.txt`](upstream/LICENSE.txt), [`upstream/LICENSE-MIT`](upstream/LICENSE-MIT), [`upstream/LICENSE-APACHE`](upstream/LICENSE-APACHE)) are retained intact.
- **Original Copyright:** `Copyright 2016-2017 GoPro, Inc.`
- **Original License:** Dual-licensed under the **Apache License, Version 2.0** or the **MIT License**.

### GoPro MIT License Notice

```text
Copyright 2017 GoPro, Inc.

Permission is hereby granted, free of charge, to any person obtaining a 
copy of this software and associated documentation files (the "Software"), 
to deal in the Software without restriction, including without limitation 
the rights to use, copy, modify, merge, publish, distribute, sublicense, 
and/or sell copies of the Software, and to permit persons to whom the 
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included 
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL 
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
DEALINGS IN THE SOFTWARE.
```

### GoPro Apache License 2.0 Notice

```text
Copyright 2016-2017 GoPro, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

## 2. Third-Party Code Included Within `upstream/`

The pristine GoPro CineForm SDK tree in [`upstream/`](upstream/) includes the following third-party components:

### 2.1 `readavi` (AVI Reader Utility in `upstream/Example/`)
- **Files:** `upstream/Example/avi.h`, `upstream/Example/fileio.cpp`, `upstream/Example/fileio.h`, `upstream/Example/readavi.cpp`
- **Copyright:** `Copyright 2004-2013 by Michael Kohn <mike@mikekohn.net>` (`http://www.mikekohn.net/`)
- **License:** BSD License (as stated in the file headers within `upstream/Example/`).
- **Usage in HyperCine:** Used only by the upstream `TestCFHD` example and the `test_avi_sample` test harness to parse the sample AVI container (`upstream/Example/videos/CFHDsample422.AVI`). Not linked into the core HyperCine library.

### 2.2 `ucm.cmake` (Useful CMake Macros in `upstream/ucm.cmake`)
- **File:** `upstream/ucm.cmake`
- **Upstream URL:** `https://github.com/onqtam/ucm`
- **Copyright:** `Copyright (c) 2016 Viktor Kirilov`
- **License:** MIT Software License.
- **Usage in HyperCine:** Invoked by `upstream/CMakeLists.txt` when building the pristine upstream reference targets.

---

## 3. External Build / Platform Dependencies (Not Bundled)

HyperCine bundles **zero** external third-party libraries outside `upstream/`. When built with GPU acceleration enabled (`-DHYPERCINE_ENABLE_CUDA=ON`), HyperCine links against the system-installed **NVIDIA CUDA Runtime** (`cudart`) and optionally loads the **NVIDIA Management Library** (`nvml.dll`) dynamically at runtime for benchmark telemetry.
