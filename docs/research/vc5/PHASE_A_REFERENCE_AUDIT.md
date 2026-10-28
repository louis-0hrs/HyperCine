# VC-5 reference and conformance audit — Phase A (docs-only)

Status: **Phase A inventory only. No conformance claim. No code, build, test or
`upstream/` change.** Phase B is explicitly *not* authorized by this document.

Scope owner: issue [#303](https://github.com/louis-0hrs/HyperCine/issues/303).
Research lane; it does not gate #259, #120, #144, #160 or #161.

---

## 0. What this document can and cannot conclude

**Can conclude**

- Exactly which SMPTE publications and which repository revision were
  inspected, with byte counts and SHA-256, so a reader can re-obtain the same
  bytes.
- Whether the pinned reference repository contains what its own README, install
  notes and RP 2073-2 Annex A/E claim. Two of those claims are false at the
  pinned revision (§2.1, §2.2, §6).
- That the reference codec does not build from the pinned snapshot on a current
  POSIX toolchain without local out-of-tree workarounds, and which workarounds
  are required (§5).
- That a **normative prescription in ST 2073-1:2017 contradicts the reference
  implementation and every reference bitstream** on one header field (§4.1).
- That a VC-5/ST 2073-1 elementary bitstream and a legacy GoPro CineForm
  bitstream are distinguishable at byte 0, and which shape HyperCine's
  SDK-free standalone encoder emits (§4).

**Cannot conclude**

- That any HyperCine bitstream is or is not VC-5-conformant. RP 2073-2 §9.2/§9.3
  defines conformance by running the *release build of the reference decoder*
  over the reference bitstreams and comparing output images byte for byte. No
  reference decoder binary was built or run here.
- That legacy CineForm/CFHD output is "non-conformant VC-5". Different formats,
  different framing (§4). Failing a VC-5 check is not a defect in a CFHD file.
- That any decoder is correct. #11 already measured FFmpeg-vs-GoPro decoder
  differences; a difference is not a verdict.
- Anything about pixels, coefficient values, or image equality for HyperCine.

---

## 1. Pinned publications

Fetched over HTTPS from `pub.smpte.org` on 2026-10-28; sizes and hashes computed
locally over the received bytes.

| Document | Path under `https://pub.smpte.org/latest/` | Bytes | SHA-256 (first 16) |
| --- | --- | --- | --- |
| RP 2073-2:2022 (Conformance) | `rp2073-2/rp2073-2-2022.pdf` | 748,625 | `9e5d7beb0fa54ef1` |
| ST 2073-1:2017 (Elementary Bitstream) | `st2073-1/st2073-1-2017.pdf` | 986,432 | `6fe18da50e84264b` |
| ST 2073-3:2015 (Image Formats) | `st2073-3/st2073-3-2015.pdf` | 943,694 | `4a694a3f41f7ff28` |
| ST 2073-4:2015 (Subsampled Colour) | `st2073-4/st2073-4-2015.pdf` | 626,666 | `d1be8f58c2202f45` |
| ST 2073-5:2015 (Layers) | `st2073-5/st2073-5-2015.pdf` | 224,324 | `a5fac1dac52ec1cc` |
| ST 2073-6:2015 (Sections) | `st2073-6/st2073-6-2015.pdf` | 226,238 | `c7c39a7120c4bc59` |
| ST 2073-7:2022 (Metadata) | `st2073-7/st2073-7-2022.pdf` | 646,384 | `d43c97b3af6e353f` |

RP 2073-2:2022 is a revision of RP 2073-2:2017, approved 2022-02-08, 71 pages.
The SMPTE ST 2073 landing page lists exactly the six parts above as current
(checked 2026-10-28); `st2073-1-2020`, `-2022`, `-2023` all return `403`, i.e.
they do not exist. So ST 2073-1:2017 is the current edition, which matters for
§4.1.

**There is no "VC-5 Part 2" codec part.** RP 2073-2 Table 1: enabling part 2
"has no effect as there is no code associated with the conformance
specification". Part 2 is the RP. Do not write "Part 2 decoder".

**Not verified:** errata, and whether `pub.smpte.org/latest/` serves the same
bytes as the purchasable edition. `latest` is a moving alias; the hashes above
pin what was served on 2026-10-28 and nothing more.

---

## 2. Pinned reference repository

```
https://github.com/SMPTE/rp2073-2-a
default branch main = 217a6a5191fad13d777b94c6432440fd726a6e1b
committed            = 2024-06-04T04:53:52Z
version.yaml         = 5.0.0      common/include/version.h = 5.0.0.0
commits on main      = 7
files at that commit = 1,880 (1,291 under `media/`)
```

### 2.1 Annex E's tag does not exist

RP 2073-2:2022 Annex E.1:

> The software version corresponding to this document revision is tagged as
> `SMPTE-RP-2073-2-2022`.

Measured against the live repository:

- `GET /git/refs` → exactly one ref, `refs/heads/main`.
- `GET /git/ref/tags/SMPTE-RP-2073-2-2022` → **404**. Same for
  `SMPTE-RP-2073-2`, `v5.0.0`, `release-5.0`.
- `GET /releases` → empty. `git ls-remote --tags` → empty.

**Finding A-1.** The normative pointer resolves to nothing. Identity between
"revision 5.0.0" and "current main" cannot be proven from the repository. The
only reproducible pin is the commit SHA. Never cite "the latest release" of
this repo as a conformance basis.

### 2.2 The documented build system is not in the repository

`README.md`, `notes/install.md` and RP 2073-2 §10 all document `make` from
`$(ROOT)`, plus CMake projects under `$(ROOT)/{encoder,decoder,converter,
comparer}/build/cmake/`. At the pinned commit:

| Documented path | Present at `217a6a5` |
| --- | --- |
| `Makefile` (root) | **absent** |
| `encoder/build/cmake/CMakeLists.txt` | **absent** |
| `decoder/build/cmake/CMakeLists.txt` | **absent** |
| `converter/build/cmake/CMakeLists.txt` | **absent** |
| `comparer/build/cmake/CMakeLists.txt` | **absent** |
| `scripts/requirements.txt` | **absent** (referenced by `notes/install.md`) |
| `tables/table17.inc` | present |
| `scripts/testmedia.json` | present |

The repository's own `.gitignore` ignores `Makefile` (line 237) and
`/CMakeLists.txt` (line 338). The build entry points were never committed.

**Finding A-2.** `make` / `make test` per RP 2073-2 §10 is not reproducible from
the pinned public tree. Any Phase B harness must supply its own build
description and say so.

### 2.3 Test-material volume and availability

Walking the pinned tree:

- 1,291 files under `media/`, of which **964 are Git LFS pointers** totalling
  **858,373,400 bytes** (0.80 GiB) of payload.
- Only `…/release/` configurations: **330 pointers, 353,985,740 bytes**
  (0.33 GiB) covering both bitstreams and decoded images; the 152 release
  `.vc5` pointers alone are 10,410,124 bytes.
- Part 1 release alone: 76 pointers, 71,081,260 bytes.
- The other 327 `media/` files are text: `README.md` and per-test `.log` files.

LFS payload access tested **anonymously** (no credentials) on five release
artefacts; every downloaded byte stream hashed to the OID in its pointer:

| Artefact (repo-relative, `media/…/release/`) | Bytes | SHA-256 == OID |
| --- | --- | --- |
| `solid/…/part1/…/solid-48x48-0000-part1-48x48-rg48.vc5` | 740 | yes |
| `solid/…/part1/…/solid-48x48-0000-part1-48x48-b64a.vc5` | 960 | yes |
| `solid/…/part1/…/solid-97x97-0000-part1-97x97-byr4.vc5` | 1,288 | yes |
| `solid/…/part3/…/solid-96x96-0000-part3-96x96-byr4.vc5` | 960 | yes |
| `boxes/…/part3/…/boxes-640x480-0000-part3-640x480-rg48.vc5` | 72,984 | yes |
| `boxes/…/part4/…/boxes-640x480-0000-part4-640x480-nv12.vc5` | 42,432 | yes |
| `boxes/…/part6/…/boxes-640x480-0000-part6-640x480-rg48.vc5` | 73,804 | yes |

**Finding A-3.** Availability is **AVAILABLE**, not **BLOCKED**. The AWS S3
path in `notes/install.md` and `scripts/getmedia.py` is legacy and was not used;
plain Git LFS works. Availability is not permission — see §3.

---

## 3. Licence and redistribution constraints

`LICENSE.md` is the bespoke "VC-5 Test Materials License" between SMPTE and
GoPro, not an open-source licence. GitHub's detector reports `NOASSERTION`.
Clauses that bind this repository:

- §1 test materials are **jointly copyrighted** by SMPTE and GoPro;
- §2 "GoPro retains rights to the test materials";
- §8 a purchaser may build commercial derivative works but
  **"may not redistribute the test materials in their original form"**;
- §9 possible patent-pool licensing.

`notes/install.md` "Restrictions" adds that the release is provided "for the
sole purpose of **evaluating the source code prior to purchase**". `PATENTS.md`
is a non-assertion disclaimer, not a grant. Source headers read
"(c) 2013-2021 Society of Motion Picture & Television Engineers LLC and Woodman
Labs, Inc." — Woodman Labs is GoPro.

**Finding A-4.** Nothing from that repository — code, headers, tag tables,
bitstreams, decoded images, XML metadata, or a table *transcribed* from them —
may enter HyperCine, which is MIT OR Apache-2.0. Phase B must therefore be an
**out-of-tree, locally configured** harness: a `HYPERCINE_REFERENCE_DIR`-style
variable whose *name* is published and whose *value* is not; no vendored
payload; no committed excerpt; published records limited to hashes, counts,
verbatim normative text and verdicts.

Two external dependencies are separately licensed and are *not* test materials:
Mini-XML (Apache-2.0) and Expat (MIT variant), per `licenses/licenses.txt`.
Argtable2 (LGPL-2) is used only by the CLI utilities.

---

## 4. Format contract: legacy CFHD vs ST 2073-1 vs HyperCine standalone

### 4.1 A normative contradiction in ST 2073-1:2017 Table B.1

This was not the expected result of the audit and it is the most consequential
item here, so the evidence is spelled out.

ST 2073-1:2017 §B.1.10 defines the `PrescaleShift` field. Verbatim:

> The value of PrescaleShift[i] shall be in the range [0, 15].
> The prescale shifts shall be represented in the tag-value pair for
> PrescaleShift using a packed representation listed in Table B.1 with the bit
> positions numbered from right to left in the 16-bit unsigned value.

Table B.1 — Packed representation of the prescale shifts:

| Bit positions | Prescale shift |
| --- | --- |
| 3–0 | PrescaleShift[0] |
| 7–4 | PrescaleShift[1] |
| 11–8 | PrescaleShift[2] |
| 15–12 | 0 |

So the standard says: **4-bit fields at bits 0, 4, 8**.

The reference implementation does something else. `common/src/syntax.c`:

```c
TAGWORD PackTransformPrescale(TRANSFORM *transform)
{
	TAGWORD packed_prescale = 0;
	int i;
	for (i = 0; i < MAX_WAVELET_COUNT; i++)
	{
		assert((transform->prescale[i] & ~0x03) == 0);
		packed_prescale += transform->prescale[i] << (14 - i * 2);
	}
	return packed_prescale;
}
```

and `common/src/codec.c` decodes it symmetrically:

```c
int prescale_value = (value >> (14 - wavelet_index * 2)) & 0x03;
```

That is **2-bit fields at bits 14, 12, 10** (the `assert` even caps values at
2 bits, contradicting the standard's stated range `[0,15]`).

Every reference bitstream measured carries `PrescaleShift = 10240 = 0x2800`:

| Bitstream (part) | `PrescaleShift` |
| --- | --- |
| `solid-48x48-0000-part1-48x48-rg48` (1) | `0x2800` |
| `solid-48x48-0000-part1-48x48-b64a` (1) | `0x2800` |
| `solid-97x97-0000-part1-97x97-byr4` (1) | `0x2800` |
| `solid-96x96-0000-part3-96x96-byr4` (3) | `0x2800` |
| `boxes-640x480-0000-part4-640x480-nv12` (4) | `0x2800` |
| `boxes-640x480-0000-part6-640x480-rg48` (6) | `0x2800` |

Decoding `0x2800` both ways:

| Interpretation | PrescaleShift[0..2] |
| --- | --- |
| ST 2073-1 Table B.1 (4-bit @ 0,4,8) | `[0, 0, 8]` — **invalid**, 8 exceeds any sane shift |
| reference code (2-bit @ 14,12,10) | `[0, 2, 2]` — matches `SetTransformPrescale` for 10/12-bit |

`common/src/wavelet.c` sets `{0, 2, 2, …}` for precision 10 and 12 specifically
to prevent wavelet overflow, and `{0,2,0}` appears nowhere. So the intent is
unambiguous: `[0,2,2]`.

**Finding A-5 (normative contradiction).** For a 10- or 12-bit stream, a
decoder that implements ST 2073-1:2017 Table B.1 literally reads
`PrescaleShift = [0,0,8]` from an official reference bitstream — outside the
standard's own `[0,15]`-per-shift intent, and in an arithmetic-right-shift
context that is nonsensical. The reference decoder and sample encoder share the
2-bit packing and therefore agree with each other while disagreeing with the
published table. This is a defect in one of the two, and it is not resolvable
from the public materials: the text and the code contradict each other, and no
errata or newer edition exists (§1).

Consequences, all of which constrain Phase B:

1. "The reference decoder accepts it" cannot be used as evidence that a
   prescale field is *standard*-correct, only that it is *reference*-correct.
2. Any HyperCine VC-5 framing work must pick a side **explicitly** and record
   it; silently following the code reproduces a deviation from the normative
   table.
3. This is the one item here that is a plausible SMPTE errata report, because a
   minimal reproducer is trivial: quote §B.1.10 + Table B.1, quote the two
   functions, and show `0x2800` in a published reference bitstream. It is
   reported here as a *finding*, not as a claim of a standard defect — the
   alternative reading (the table is authoritative and the reference codec is
   wrong) is equally consistent with the evidence.

Explicitly **not** claimed: that HyperCine's own `PRESCALE_TABLE` value is
wrong. See §4.3 item 4.

### 4.2 ST 2073-1:2017 §8 framing vs legacy GoPro framing

Normative requirements:

- §8.5(2) "The first segment in the bitstream shall be a start marker segment."
- §8.6 `StartMarkerSegment` = the four bytes `0x56 0x43 0x2D 0x35` (`"VC-5"`).
- §8.5(3) the bitstream header follows immediately after the start marker.
- §8.3.1 a tag-value pair is one 32-bit segment, tag in the high 16 bits,
  **network order**.
- §8.3.3 optional tag-value pairs use the two's-complement negation of the tag.
- §8.4.3 small chunk header = 16-bit tag + 16-bit segment count, tag
  `0x4000..0x40FF`.
- §8.4.4 large chunk header = 8-bit tag + 24-bit segment count, tag `0x60..0x6F`
  (`0x60` = `LargeCodeblockTag`, `0x61..0x6F` reserved).
- §8.7(2) the header "shall not contain a tag-value pair for a header parameter
  that is not defined in this standard (Table B.2)".
- Table B.2 header parameters: `ImageWidth` 20, `ImageHeight` 21,
  `BitsPerComponent` 101, `ChannelCount` 12, `SubbandCount` 14,
  `ChannelNumber` 62, `SubbandNumber` 48, `LowpassPrecision` 35,
  `Quantization` 53, `PrescaleShift` 109, `ChannelWidth` 104, `ChannelHeight`
  105. ST 2073-3 Table A.1 adds `PatternWidth` 106, `PatternHeight` 107,
  `ComponentsPerSample` 108, `ImageFormat` 84, `MaxBitsPerComponent` 102.

**Official reference bitstream**, `solid-48x48-0000-part1-48x48-rg48.vc5`
(740 bytes, SHA-256 `caf2a78e…`):

```
offset  bytes        reading
0       56 43 2D 35  StartMarkerSegment (§8.6)
4       00 0C 00 03  tag 12  ChannelCount         = 3
8       00 14 00 30  tag 20  ImageWidth           = 48
12      00 15 00 30  tag 21  ImageHeight          = 48
16      00 54 00 01  tag 84  ImageFormat          = 1
20      00 6A 00 01  tag 106 PatternWidth         = 1
24      00 6B 00 01  tag 107 PatternHeight        = 1
28      00 6C 00 03  tag 108 ComponentsPerSample  = 3
32      00 66 00 0C  tag 102 MaxBitsPerComponent  = 12
36      00 6D 28 00  tag 109 PrescaleShift        = 10240
40      A0 04 00 09  optional small chunk 0x4004, 9 segments
```

Every field is a legal Table B.2 / ST 2073-3 Table A.1 parameter. Note `0x6A`,
`0x6B`, `0x6C`, `0x66` in the header: these are inside the *large-chunk* numeric
range `0x60..0x6F`, which is why a walker must treat the header region as
tag-value pairs and only the codeblock region as chunk elements. A naive
chunk-only parser mis-reads `PatternWidth = 1` as a 1 GB chunk.

**Legacy GoPro CineForm**, `sample_canon_422.cfhd` (900,396 bytes):

```
0       00 01 00 09   tag 1 = SAMPLE,  value 9
4       00 02 00 03   tag 2 = INDEX,   value 3
8       00 07 79 B0   tag 0, value 31152    <- index payload, not a tagged field
12      00 04 B9 68   tag 4 = MARKER, value 47464
16      00 01 87 24   tag 1 = SAMPLE,  value 34596
20      00 0A 00 00   tag 10 = TRANSFORM_TYPE
```

No start marker. The `SAMPLE`/`INDEX`/`MARKER` prologue and the
`0x2000`/`0x2100`/`0x2200` sized-chunk family are legacy CineForm container
conventions, enumerated in HyperCine's own frozen `upstream/Codec/codec.h`
(`CODEC_TAG_SAMPLE` 1, `_INDEX` 2, `_ENTRY` 3, `_MARKER` 4,
`_SUBBAND_SIZE` 0x2000, `_LEVEL_SIZE` 0x2100, `_SAMPLE_SIZE` 0x2200).

**The discriminator is byte 0..3.** `56 43 2D 35` = VC-5; anything else = not
VC-5 framing. `0x2000` is *also* the low end of the ST 2073-1 large-chunk range,
so the dialects collide numerically exactly where a parser must commit to one
grammar.

RP 2073-2 §5 and `vc5codec.org` describe the divergence qualitatively (tags,
endianness, chunk/codeblock representation). What is measured here is the
concrete byte-level test.

### 4.3 What HyperCine's SDK-free standalone encoder emits

Build: `-DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_STANDALONE=ON`, GCC
14.2.0, Release, out-of-tree. Input: `hypercine_encode_smoke flat` canonical
pattern, 128×128. Output 3,444 bytes. First 92 bytes:

```
00 01 00 09   tag  1 SAMPLE              = 9
00 02 00 03   tag  2 INDEX               = 3   (3 raw size words follow)
00 00 05 18   raw word 1304
00 00 03 F4   raw word 1012
00 00 03 F4   raw word 1012
00 0A 00 00   tag 10 TRANSFORM_TYPE      = 0
00 0B 00 01   tag 11 NUM_FRAMES          = 1
00 0C 00 03   tag 12 NUM_CHANNELS        = 3
00 54 00 01   tag 84 ENCODED_FORMAT      = 1
FF A5 00 02   tag 91 ENCODED_COLORSPACE = 2   (optional: -91)
00 0D 00 03   tag 13 NUM_WAVELETS        = 3
00 0E 00 0A   tag 14 NUM_SUBBANDS        = 10
00 0F 00 02   tag 15 NUM_SPATIAL         = 2
00 10 00 03   tag 16 FIRST_WAVELET       = 3
00 14 00 80   tag 20 FRAME_WIDTH         = 128
00 15 00 80   tag 21 FRAME_HEIGHT        = 128
FF BB 00 00   tag 69 FRAME_NUMBER        = 0   (optional: -69)
00 46 00 0A   tag 70 PRECISION           = 10
FF AB 00 80   tag 85 DISPLAY_HEIGHT      = 128 (optional: -85)
FF B0 00 06   tag 80 QUALITY_L           = 6   (optional: -80)
FF AF 00 00   tag 81 QUALITY_H           = 0   (optional: -81)
00 53 20 00   tag 83 PRESCALE_TABLE      = 8192 = 0x2000
DE 00 03 46   tag 0x2200 SAMPLE_SIZE    = 838 segments (optional: -8704)
```

Read against `src/core/standalone_encoder.cpp` (`WireTag`, `EncodeBgra`):

1. **No `StartMarkerSegment`.** `grep -rn 56432D35 src include` finds nothing;
   the encoder never writes it. ST 2073-1 §8.5(2) is violated by construction.
   Framing statement only — nothing about coefficients.
2. **The framing is legacy CineForm, matching §4.2**, not §4.1: leading
   `SAMPLE`/`INDEX`, then the legacy sized-chunk family, with `SAMPLE_SIZE` emitted in its
   optional form (`DE 00 03 46` = `-0x2200` with 838 segments). `0x2200` is legal
   legacy framing and **structurally impossible** in ST 2073-1, where `0x22` is
   neither a small-chunk tag (`0x4000..0x40FF`) nor a large-chunk tag
   (`0x60..0x6F`).
3. **The three words after `INDEX` are the channel sizes**, and they are
   correct. Source:
   `for(int i=0;i<3;++i) s.Tag(kIndexEntry,i);` reserves three *tag-shaped*
   placeholders, then `s.Patch32(index+4*c, …)` overwrites each with the raw
   byte length of that channel. The real GoPro sample does the same thing with
   `00 07 79 B0` / `00 04 B9 68` / `00 01 87 24` — raw numbers, tag field
   meaningless. HyperCine writes `1304 + 1012 + 1012 = 3328`, and
   `3444 − 3328 = 116` bytes of remaining header/trailer, which is consistent.
   **This is correct legacy behaviour, not a defect.** (An earlier draft of this
   audit called it a bug; the comparison against the real GoPro file disproved
   that.)
4. **`tag 83 = 8192 = 0x2000` as the `PRESCALE_TABLE` value.** Source:
   `s.Tag(kPrescale,kSubbandSize); // 10-bit prescale 0,2,0`. Decoded with the
   reference's 2-bit packing, `0x2000` gives `[0,2,0]`; with ST 2073-1 Table B.1
   it gives `[0,0,0]`. The official reference bitstreams all carry `0x2800`
   (`[0,2,2]`). So HyperCine is self-consistent with its own comment and with
   the reference packing, and differs from the reference bitstreams in the
   *third* level only. Whether that is correct depends on the encoder's actual
   prescale, which this audit did not measure. Recorded as an open item, not a
   defect.
5. Tags 13, 15, 16, 69, 80, 81, 83, 85, 91 are not in ST 2073-1 Table B.2. They
   are legacy assignments, and the encoder's own comment says "Wire tags from
   frozen `Codec/codec.h`"; `upstream/Codec/codec.h` numbers `PRECISION` at 70
   and `FRAME_NUMBER` at 69, matching the emitted bytes. So the encoder is
   self-consistent with the legacy enum it claims to target. The point is only
   that this enum is not ST 2073-1's.

**Finding A-6 (the actionable one).** HyperCine's public prose claims
"SMPTE VC-5 / CFHD" (`README.md` line 9, `docs/ARCHITECTURE.md` line 58,
`docs/PHASE3A_SEQUENCE_CORRECTNESS.md`) without distinguishing the two dialects.
Measured at byte 0, the standalone path produces **legacy CineForm framing**, and
the VC-5/ST 2073-1 framing constraint (§8.5(2), start marker) is not met. That
may be entirely intended — bit-exactness against the GoPro SDK is the project's
stated oracle, and `AGENTS.md` §4 says "SMPTE VC-5 / CineForm `CFHD`", treating
them as one thing. But the two claims cannot both be made about the same bytes.
Either the docs name the dialect, or the standalone path gains an opt-in VC-5
framing mode. **This document does not decide which**; it records that the
current wording is undecidable for a reader, and that a 4-byte check settles it
per artefact.

Not claimed: that legacy framing is wrong, that any decoder rejects the
HyperCine stream, or that its coefficients, quantisation or entropy coding
deviate from anything. None of that was tested.

---

## 5. Does the reference codec build on a current POSIX toolchain?

The reference decoder is the oracle in RP 2073-2 §9.2/§9.3, so buildability is a
precondition for any Phase B verdict. Measured with GCC 14.2.0 on Linux x86_64,
`-fsyntax-only` per translation unit (no link, no run, no media).

### 5.1 As shipped (`config.h`: parts 1,3,4,5,6,7; C99)

Fails before reaching codec logic:

| Error | Cause |
| --- | --- |
| `macros.h:85` "type of 'a' defaults to 'int'" ×2 | K&R `inline static int min(a, b)`; implicit `int` is an error in modern dialects |
| `filelist.h:42` `PATH_MAX` undeclared | POSIX name, no `limits.h` |
| `parameters.h:23` `_MAX_PATH` undeclared | the fallback is Win32-only |
| `headers.h:69` `mxml.h` not found | Mini-XML is a mandatory include whenever part 7 is enabled |
| `timer.h` `#error Timing code is only available for Windows` | `_TIMING` defaults on; non-Win32 branch is a hard `#error` |

### 5.2 With documented part selection only (`-D_PARTS=61`, part 7 off)

Still fails on `mxml.h`, `PATH_MAX`/`_MAX_PATH`, `min()`. Part 7 cannot simply be
turned off to escape the XML dependency, because `parameters.h` unconditionally
needs `PATH_MAX`.

### 5.3 With local out-of-tree workarounds

Applied **outside** the tree, measurement only: `-std=gnu89` (tolerates K&R),
`-D_PRODUCTION` (disables the Windows-only timer), `-DPARTS=61`,
`-DPATH_MAX=4096`, an empty `mxml.h` stub on the include path,
`-Itables -Imetadata/common/include -Imetadata/dumper/include`.

Decoder side, 35 translation units: **32 compile, 3 do not.**

| Failing unit | Reason |
| --- | --- |
| `common/src/frame.c` | calls `PackBufferRowsToBYR3`/`BYR4` with wrong arity; uses `ENCODED_FORMAT_YUV_422`/`_RGB_444`/`_BAYER` undeclared in a decode-only build. Real source drift. |
| `common/src/syntax.c` | live `PutVideoGroupExtension(ENCODER *, …)` and 3 siblings; `ENCODER` is defined only in `encoder/include/encoder.h`. A decoder-only build of the shared source cannot work as laid out. |
| `common/src/utilities.c` | `fileno` implicit declaration (needs `_POSIX_C_SOURCE`) plus `min()` |

Encoder side, with encoder headers and `-D_PARTS=125`: **2 of 35 compile.** The
blocker is one file — `metadata/common/include/metadata.h` uses a C99
`for (int i = …)`, so `-std=gnu89` (needed for the codec's K&R `min()`) and
`-std=c99` (needed by the metadata headers) are mutually exclusive within one
translation unit. Genuine internal inconsistency in the pinned release.

**Finding A-7.** The reference decoder does not compile from the pinned public
snapshot on a current POSIX toolchain without local workarounds, and with them
3 of 35 units still fail for source-level reasons. A Phase B "decoder
conformance" verdict cannot come from `make`. It needs either the authors'
tested toolchain (macOS Catalina / Windows + MSVC) or a maintained local port
that is explicitly *not* the pinned tree. Any published verdict must name which
build produced it, because a patched reference decoder is a modified oracle —
hence the `UNQUALIFIED` verdict category in §8.2.

**Finding A-8 (dead-code caveat).** 24 tag identifiers appear in the reference
sources with no definition in the pinned headers (`CODEC_TAG_MARKER`, `_INDEX`,
`_ENTRY`, `_LAYER_COUNT`, `_PEAK_LEVEL`, `_SUBBAND_SIZE`, …). After preprocessing
under the documented part masks, **none survive** — every live use sits inside
an `#if 0` or `#ifdef`-disabled block. The snapshot is self-consistent when
compiled, but a naive text grep produces a false "undefined identifier" report.
Phase B tooling must preprocess, not grep.

---

## 6. Annex A vs the repository: one real documentation defect

Annex A is normative and enumerates reference bitstreams by name and path.
Parsed from the PDF text and compared with the pinned tree:

| Annex A section | Names listed | In a `release/` dir |
| --- | --- | --- |
| A.2 VC-5 Part 1 | 38 | 38 |
| A.3 VC-5 Part 3 | 38 | 38 |
| A.4 VC-5 Part 4 | 16 | 16 |
| A.5 VC-5 Part 5 | 19 | 19 |
| A.6.2 Part 6 | 38 | 38 |
| A.6.3 Image sections | 2 (`boxes.vc5`, `mixed.vc5`) | 2 |
| A.6.4 Image section layers | 1 | **0** |
| **total unique** | **152** | **151** |

The mismatch, quoted from RP 2073-2 §A.6.4:

```
$(ROOT)/media/sections/layers/boxes-section-layers-rg48.vc5
```

does not exist there. The repository has instead:

```
media/sections/master/layers/release/layers.vc5
media/sections/master/layers/release/boxes-1280x720-0000-vc5.rg48   (decoded image)
media/sections/master/layers/release/boxes-640x480-0000-vc5.rg48
media/sections/master/layers/release/boxes-640x480-0001-vc5.rg48
```

**Finding A-9.** The single Annex A bitstream exercising nested image sections
with layers (Part 6's hardest case) is named differently by the standard than by
the materials it normatively references — both directory (`sections/layers/` vs
`sections/master/layers/release/`) and filename (`boxes-section-layers-rg48.vc5`
vs `layers.vc5`). A runner taking Annex A literally records a missing input. It
must therefore treat "input absent" as **BLOCKED**, never as PASS, and must
record the name it looked for. That `layers.vc5` is the intended artefact is a
reasonable inference, not a citation.

This is the same failure mode #160/#307 already guard against on the HyperCine
side (`skipUnless` turning missing evidence into a CTest pass).

---

## 7. Lineage: why "SMPTE reference" and "GoPro SDK" are not two oracles

`LICENSE.md` §1: test materials "will be jointly copyrighted by SMPTE and
GoPro"; §2 "GoPro retains rights to the test materials"; §11 updates by GoPro
are provided to SMPTE. Source headers name both SMPTE LLC and Woodman Labs
(GoPro). RP 2073-2 §6.2 describes one code base serving encoder and decoder with
compile-time part masks, not two independent implementations.

Structural evidence of shared ancestry: the reference tree and HyperCine's
frozen `upstream/Codec/` share the same decomposition (`codec.c`, `syntax.c`,
`wavelet.c`, `vlc.c`, `bandfile.c`, `unpack.c`, `transperm.c`, `table17.inc`),
the same lowpass/highpass/band/zerotree state machine, and the same
`SPECIAL_MARKER_BAND_END` special codeword. Only the tag numbering differs — the
documented CineForm-vs-VC-5 divergence, not evidence of independent derivation.
And §4.1 shows both sides of *that* code base agreeing on a prescale packing
that contradicts the standard.

**Classification:** SHARED_CODE for entropy coder, wavelet, codebook and file
layout; SIMILAR_ALGORITHM or IDENTICAL for the transform mathematics; UNKNOWN
for anything not comparable without breaching the licence.

**Consequence for Phase B.** Agreement between the SMPTE reference decoder and
the GoPro SDK is *not* independent corroboration of a HyperCine bitstream, and
disagreement between them is *not* automatically a HyperCine or a standard
defect. Independent evidence must come from a third implementation family
(FFmpeg's `cfhd`, partly characterised in #11) and from normative byte-level
checks derived from the ST text, not from another copy of the same code.

---

## 8. What Phase B would have to be, and what it may not be

Design only. **Not authorized by this issue.**

### 8.1 Minimum check set, no new codec backend

1. **Framing discriminator** — for every bitstream HyperCine emits, compare
   bytes 0..3 against `56 43 2D 35` and record the answer next to the dialect the
   producer claims. Needs no reference material; settles Finding A-6 per
   artefact.
2. **Header-parameter legality** — for streams that pass (1), walk the header
   region as tag-value pairs, assert every tag is in ST 2073-1 Table B.2 or the
   ST 2073-3 Table A.1 additions, assert at-most-once per parameter and the
   required set present (§8.7). Must *not* treat `0x60..0x6F` as chunks in this
   region (§4.2).
3. **Prescale readout** — print the `PrescaleShift` value decoded both ways
   (§4.1) and its `[0..2]` vector. Cheap, and it makes the contradiction visible
   per artefact instead of per audit.
4. **Reference-decoder run** — per RP 2073-2 §9.2/§9.3, on a licence-permitted
   local checkout of the pinned commit, release configuration: decode each
   reference bitstream, compare output images byte-for-byte. Blocked on
   Finding A-7 (build) and A-9 (input naming); must record which build produced
   the verdict.
5. **Metadata equivalence** — part 7 only, and only if someone accepts the
   Mini-XML/Expat dependency. §9.4.4's "last duplicate tuple wins" is the only
   subtlety and is cheap on its own.

### 8.2 Data protocol

One record per run; a missing record is `BLOCKED`, never a pass.

```
implementation        HyperCine | SMPTE reference decoder | FFmpeg cfhd | other
version / build SHA   repo commit SHA + dirty flag
reference pin         rp2073-2-a commit SHA (never "latest release")
normative clause      e.g. ST 2073-1:2017 §8.5(2)
dialect               vc5 | legacy-cfhd | unknown
input identity        SHA-256 of the exact input bytes
output identity       SHA-256 of the exact output bytes, or n/a
enabled features      parts mask, compile-time and runtime
decoder return code   integer + warnings/errors class
expected result       PASS | FAIL | BLOCKED | NOT_APPLICABLE | UNQUALIFIED
observed result       same vocabulary
platform / toolchain  OS family, compiler + version, build type
verdict               one of the five, plus a one-line reason
negative controls     cases designed to fail, and whether they did
```

`UNQUALIFIED` means "ran, but the evidence does not support the claim" — e.g. a
locally patched reference decoder, or a verdict against a tag interpretation
that contradicts the normative table. It is not a synonym for `FAIL`.

### 8.3 Explicit prohibitions

- No vendoring, no committed excerpt, no re-hosted bitstream or image. Reference
  material stays in an external, locally configured directory whose *name* is
  published and whose *value* is not.
- No claim of the form "VC-5 compliant", "conformant", "certified" or
  "SMPTE-validated". A bounded verdict is: "the reference decoder at <build>
  decoded this input without errors and produced byte-identical output on
  <platform>".
- No change to the shared writer, and no "fixing" legacy CineForm to match VC-5,
  without a separate product/format decision. Finding A-6 is a documentation or
  an opt-in-flag decision, not a silent rewrite.
- No report to SMPTE unless a minimal independent reproducer shows a likely
  error in the referenced materials or a normative ambiguity. Two candidates
  exist and both are documentation-level, reproducible with §9's commands:
  Finding A-1 (missing tag) and Finding A-5 (Table B.1 vs reference packing),
  with A-9 (misnamed Annex A bitstream) as a third. Reported here as findings,
  not as claims of standard defects.
- No GPU work, no oracle re-run, no corpus re-export: the reference bitstreams
  are already published, and regenerating GoPro goldens is out of scope per #303
  and the #120/#161 hardware windows.

### 8.4 Smallest first slice

Items 1–3 of §8.1, as a standalone tool with no external dependency, no
reference checkout and no media: a 4-byte framing check, a header-parameter
legality walk, and a prescale readout, over HyperCine's own output. It converts
Findings A-6 and the prescale question from prose into a pass/fail line per
emitted artefact, needs nothing from SMPTE or GoPro, and cannot breach the
licence.

---

## 9. Reproduction

All measurements used public read paths and local hashing only.

```bash
# 1. Publications (sizes + SHA-256 pinned in section 1)
curl -sSL -o rp2073-2-2022.pdf https://pub.smpte.org/latest/rp2073-2/rp2073-2-2022.pdf
sha256sum rp2073-2-2022.pdf
# 403 for st2073-1-2020/-2022/-2023 confirms 2017 is the current edition

# 2. Repository pin, and the missing tag
git ls-remote https://github.com/SMPTE/rp2073-2-a.git             # one ref, no tags
gh api repos/SMPTE/rp2073-2-a/git/refs --jq '.[].ref'
gh api repos/SMPTE/rp2073-2-a/git/ref/tags/SMPTE-RP-2073-2-2022   # 404

# 3. Absent build system (run inside an external checkout at 217a6a5)
for p in Makefile encoder/build/cmake/CMakeLists.txt scripts/requirements.txt; do
  test -e "$p" && echo "PRESENT $p" || echo "MISSING $p"
done

# 4. Media inventory: pointer count and payload bytes
python3 - <<'PY'
import os, re
tot = n = 0
for root, _, files in os.walk("media"):
    for f in files:
        head = open(os.path.join(root, f), "rb").read(200)
        m = re.search(rb"oid sha256:([0-9a-f]{64})\nsize (\d+)", head)
        if m:
            n += 1; tot += int(m.group(2))
print(n, tot)          # 964 858373400
PY

# 5. The four-byte discriminator
printf 'vc5    : '; od -An -tx1 -N4 solid-48x48-0000-part1-48x48-rg48.vc5   # 56 43 2d 35
printf 'legacy : '; od -An -tx1 -N4 sample_canon_422.cfhd                 # 00 01 00 09

# 6. PrescaleShift across reference bitstreams (expect 0x2800 = 10240 everywhere)
python3 - <<'PY'
import glob
for p in sorted(glob.glob("media/**/release/*.vc5", recursive=True))[:6]:
    d = open(p, "rb").read()
    pos = 4
    while pos + 4 <= min(len(d), 600):
        t = int.from_bytes(d[pos:pos+2], "big", signed=True)
        v = int.from_bytes(d[pos+2:pos+4], "big")
        if abs(t) == 109:
            print(p.split("/")[-1], hex(v), "ref-pack", [(v >> (14-i*2)) & 3 for i in range(3)],
                  "st-B.1", [(v >> (4*i)) & 0xF for i in range(3)])
            break
        pos += 4
        if abs(t) >= 0x4000: break
PY

# 7. Reference-decoder compile probe (decoder side, part 7 off, local workarounds)
#    flags in section 5.3; expect 32 of 35 units to compile.
```

Toolchain for §5: GCC 14.2.0, C/gnu89 and C99, Linux x86_64, no CUDA, no GPU.
No media was decoded and no image was compared.
