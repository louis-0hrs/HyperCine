# Output ownership and failure preservation

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

Tracking: [#367](https://github.com/louis-0hrs/HyperCine/issues/367),
fundamental audit [#364](https://github.com/louis-0hrs/HyperCine/issues/364).

## Problem and corrected contract

The previous FilmScan3 command opened `trace.json` before validating the first
input. A missing input could therefore destroy a previous trace. The P6 parity
tool reused work paths and recursively removed a profile's sample directory;
selecting the corpus itself as the work root could delete retained evidence.
Both defects were reproduced with disposable synthetic files.

Every invocation now owns a **new run directory**. Existing files, directories,
empty output directories, dangling links and completed runs are refused. There
is no overwrite option and no recursive deletion to obtain a clean run.

### FilmScan3 CLI

`OUTPUT_DIR` must not exist, and its parent must already exist. Output must be
outside the parent directories of all selected frame and explicit metadata
inputs. Keep input files in an input directory and outputs in a separate run
directory. Ordinary parent aliases, including the macOS system temporary-path
alias, are canonicalized and pinned; aliases into retained input roots are
rejected using physical directory identity.

All frame inputs and explicit metadata are opened and checked before output
creation. Their handles stay open while the run is encoded, so replacing an
input pathname cannot silently select another file. Metadata must contain
exactly 45 bytes per requested frame. Frame inputs must have the exact BGRA
storage size for the admitted geometry. Invalid options, shapes or input
storage do not truncate earlier output.

The CLI writes into an exclusively created `.hypercine-incomplete-*` staging
directory. Generated files are created exclusively through a pinned directory,
and writes, flushes, closes and trace-stream failures are checked. Publication
occurs only after every frame and the complete JSON trace succeed, using a
native atomic directory rename that refuses an occupied destination. A second
process choosing the same final name cannot replace the first process's run.

Linux uses `renameat2(RENAME_NOREPLACE)`, macOS uses
`renameatx_np(RENAME_EXCL)`, and Windows uses handle-based `FileRenameInfo`
without replacement. There is no unsafe ordinary-rename fallback. Windows
directories are held against rename/deletion; parent identity uses the full
128-bit file ID and 64-bit volume serial. POSIX verifies pinned parent and
staging identities before publication and the resulting directory identity
after publication.

Failed or interrupted staging directories remain available for inspection.
The tool does not recursively clean a path that another actor might have
replaced or populated. On POSIX, new run directories use mode `0700` and files
use `0600`; existing permissions are not changed.

### P6 writer parity tool

`--work` names a new directory under an existing parent. Corpus and work roots
must be disjoint. Profiles, geometry selectors, frame indices, inventory paths,
hashes and sizes are validated before creating the work directory. The exact
input bytes copied into the run are verified using pinned directory/file
handles. The layout is:

```text
RUN/PROFILE/inputs/input_0000.bgra
RUN/PROFILE/samples/frame_0000.cfhd
RUN/PROFILE/samples/trace.json
RUN/report.json
```

An explicit `--report` must be a new file outside the corpus. It can be a
top-level file inside the new work directory, or reside in a separate existing
parent. Parent aliases and physical ancestry are checked without trusting only
path-string comparisons. A report is fully staged in a private directory,
then published without replacing a concurrent or existing destination.

A completed comparison that finds packet/table divergence publishes an
explicit `status: "FAIL"` report and exits nonzero. Incomplete input, encoder,
copy or report-write failures do not publish a final comparison. Encoder
stderr is not copied into the tool's public diagnostic messages. Partial owned
work and small report staging directories are retained, rather than deleted.

The optional real-corpus CTest uses `tests/run_p6_writer_parity.py` to choose a
new run name for every invocation. Repeating CTest no longer reuses prior
evidence or requires a destructive cleanup step.

### Bounded input reads

The P6 follow-up also bounds the actual read operation. A descriptor-size check
alone is insufficient when a producer grows a file after that check. Reads now
use a remaining byte budget, at most 64 KiB per request, and one overflow-probe
byte that is never copied into owned output. Short reads accumulate correctly;
an early EOF or growth beyond a pinned extent fails. Hashing and interpretation
use the same admitted snapshot. This also applies to the compatibility helpers
imported by the SDK gates.

| Input to this tool | Maximum |
| --- | ---: |
| BGRA frame | 33,177,600 bytes, the existing 3840 × 2160 × 4 input budget |
| SDK or writer packet | 128 MiB |
| Transfer index | 8 MiB |
| Individual profile manifest | 4 MiB |
| Encoder trace | 4 MiB |

These are tool resource limits, not general CFHD/SDK format maxima or permission
to encode a larger geometry. The packet allowance has headroom above the current
writer's enclosing chunk and framing. A known manifest length is additionally
an exact bound. Zero-byte pins remain accepted only by the existing generic
`require_pinned` compatibility helper; P6 frames and metadata must be nonempty.

The follow-up passed 43 ownership/reader cases in each Python mode, including
17 new cases. Three isolated old-source controls failed the intended read-budget
assertions using only small synthetic files. Its complete strict Linux gate
passed **133/133 CTests**, with zero failure/error/skip. The original execution
receipt below remains historical; the new source-bound follow-up receipt is
[`P6_BOUNDED_READS_20261112.json`](validation/P6_BOUNDED_READS_20261112.json).

## Examples

Create the parent `runs` once; keep it separate from the input/corpus directory.
Use the actual configured executable path for `ENCODER` below.

```sh
ENCODER --geometry 1920x1080 runs/encode-001 inputs/frame_0000.bgra

python tools/p6_writer_parity.py \
  --corpus "$HYPERCINE_GOLDEN_DIR" --encoder ENCODER \
  --profiles filmscan3 --max-frames 16 \
  --work runs/parity-001 --report runs/parity-001/report.json
```

For another run choose `encode-002` / `parity-002`. Do not move inputs into an
output run or remove retained evidence to make an occupied name available.

## Validation and limits

The mandatory standalone inventory includes real CLI preservation tests,
native directory-transaction tests, Python parity ownership tests in normal
and optimized modes, a real two-frame CLI/P6 interoperability test, and an
actual CPU CLI-to-4K-FFmpeg gate. The latter also fixes a pre-existing helper
error: the real trace reports lowercase `filmscan3`; the old mock fixtures
and validator incorrectly required `FilmScan3`.

The two-frame interoperability corpus is explicitly self-generated synthetic
test data. Its SDK-shaped schema does **not** establish original GoPro SDK
provenance, independent packet equivalence or native GPU qualification.

The final Linux execution receipt and source fingerprints are recorded in
`docs/validation/OUTPUT_OWNERSHIP_367_20261112.json`. Native Windows/macOS and
filesystem-specific qualification remain required. Unsupported identity or
atomic-publication operations safely refuse a run. POSIX report publication
requires a supported same-filesystem hardlink operation. Windows identity
queries require the modern `FileIdInfo` interface; no shortened-ID fallback is
accepted. FAT/ReFS/SMB behavior is not inferred from Linux tests.

This is ordinary-error atomic visibility and preservation, not a claim of
power-loss durability or a sandbox against a malicious encoder, privileged
actor, or arbitrary same-user filesystem mutation. One open handle is retained
per CLI input argument; exceptionally long invocations can hit the platform
descriptor limit and fail before output creation. Core codec arithmetic,
packet format, public codec APIs and frozen `upstream/` are not changed.

Microsoft API contracts: [FILE_ID_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info)
and [FILE_RENAME_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info).
