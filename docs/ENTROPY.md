# CineForm Highpass Entropy

Phase 2B copies the stock highpass coder. The reference is `EncodeQuantizedBand` in the pristine `upstream/` tree, through the active `EncodeQuantLongRuns` path (`encoder.c`, the `#if 1` body). HyperCine does not edit that tree. `tests/test_phase2b` checks the staged CPU coder and the GPU coder against `EncodeOneExternalBand`, which calls the same upstream function.

FilmScan 3, GOP 1, progressive is the measured path. `SetCodingFlags` selects codebook 1 and `peaks = 0` for those highpass bands. A band with peaks coding stays on the CPU. Lowpass is not this coder: `EncodeLowPassBand` writes a lowpass header and raw coefficient pairs.

## Sequence

1. **Active codebook.** `SetCodingFlags(encoder, subband)` returns the codebook index, the peaks flag, and the coding flags stored in the band header. The runbook is `encoder->codebook_runbook[codebook]`. The value book is `encoder->valuebook[codebook]`. The band-end code is `encoder->band_end_code[codebook]` / `band_end_size[codebook]`.

2. **Zero runs.** Coefficients are visited in row-major order. A row is `width` samples. The encoder then adds `pitch_pixels - width` implicit zeros and does not read the padding. Runs cross rows. The active FilmScan buffers use `pitch_pixels == width`.

3. **Run symbols.** For a zero count `n`, the emitted index is `min(n, runbook_length - 1)`. The code covers `rlc[index].count` zeros, which is not always `index`. The count is subtracted and the step repeats until the run is gone. Each step is one run token.

4. **Value symbols.** A nonzero coefficient is clamped to ±((`VALUE_TABLE_LENGTH` / 2) - 1). With `VALUE_TABLE_SIZE` 11 that half-range is 1024, so the stored extremes are ±1023. The value-book index is `value` when positive and `VALUE_TABLE_LENGTH + value` when negative.

5. **Codeword.** Run entries store `bits` and `size` separately. Value entries are packed VLE words: `codeword = entry & 0x7FFFFFF`, `size = entry >> 27`. Sizes used by the table build are 1..31. The band-end code is one more symbol after the last run or value.

6. **Bit packing.** `PutBits` writes MSB-first into a 32-bit buffer. While the code fits in the free bits it is shifted in. Otherwise the buffer is flushed with `PutLong(SwapInt32)` and the remainder stays. A code that fills the buffer exactly leaves `nBitsFree == 0`; the next `PutBits` or `FlushStream` emits that word. On this Win64 build the C path is active (`_ASMOPT` is 0).

7. **Band framing.** `PutVideoBandHeader` writes the tag, size, subband, dimensions, quantization, scale, and coding flags. The coefficient bytes are appended only when the header leaves the stream on a 32-bit boundary (`nBitsFree == 32`). `PutVideoBandTrailer` calls `PadBits32`, which zero-fills to a word, then `FlushStream`, then the trailer tags and `SizeTagPop`. The coefficient payload, including the band-end code and the pad, is a whole number of big-endian 32-bit words.

`src/cpu/entropy_stages.cpp` follows that order. `FinishFromTokens` and `FinishFromCodewords` are the host codebook, prefix, and pack steps. The GPU kernels in `src/cuda/entropy_gpu.cu` produce the same tokens, codewords, exclusive bit offsets, and coefficient bytes. Block-local nonzero scans are merged by a carry of the zeros that cross a 256-wide CUDA block. Packing assigns one thread to each output word and ORs the bits that land in that word, so a codeword that crosses a byte, a 32-bit word, or a work-item boundary has one writer.

## What is checked

For every vector below, the test stores nothing on disk. It recomputes the payload and requires:

- the same token index sequence
- the same codeword and bit length
- exclusive offsets, with offset 0 equal to 0 and the final sum equal to the CPU bit count
- the same coefficient bytes
- the same full band payload as `EncodeOneExternalBand`

The vectors are an all-zero band, one positive coefficient, one trailing negative coefficient, alternating values, short runs, a nonzero on a CUDA block boundary, ±1023 and the clamped ±1024 inputs, an LCG pattern seeded with `0x1337BEEF`, every luma highpass band of the canonical 128×128 frame, and the three luma level-1 bands (LH, HL, HH) of the canonical 4K frame. The 1080p canonical frame is also assembled as a full sample and decoded with the stock decoder.

GPU paths that stop after tokens, or after codewords, finish the remaining steps on the CPU and must still match those coefficient bytes.

Hashes printed by `test_phase2b` are FNV-1a 64-bit. `coeff` is the tight int16 band. `payload` is the full band after the header and trailer. These values are from the Release run that also decoded the 1080p sample at 44.47 dB.

| Vector | Tokens | Bits | Payload bytes | Coefficient hash | Payload hash |
| :--- | ---: | ---: | ---: | :--- | :--- |
| zero | 2 | 60 | 56 | `0x7da144b97d054b25` | `0x470296b9e16eb13c` |
| single +1 | 3 | 62 | 56 | `0xc01cd06959834324` | `0xfcf2916e57b2cca7` |
| single −1 at end | 3 | 62 | 56 | `0x7cd8b5b97c5ada9b` | `0x1980d7c7215db92a` |
| alternating | 256 | 538 | 116 | `0x35e68edaff0c2725` | `0xc841660dcda29954` |
| short runs | 103 | 549 | 120 | `0xd213ad3d875702d6` | `0x28aeea93f39b1856` |
| block boundary | 4 | 68 | 60 | `0x0ca7d885c757ef51` | `0xf4eb29458e5eab95` |
| ±1023 / ±1024 | 5 | 160 | 68 | `0xaf8a74caa57295e2` | `0xa7ad3fa33ca519f6` |
| LCG `0x1337BEEF` | 121 | 744 | 144 | `0x1af081c389d9590f` | `0x03c1c1f07d2fd4bb` |
| 128 Y L3 LH | 39 | 243 | 80 | `0xd49aeb23504f31c6` | `0x5c0fb7446478157f` |
| 128 Y L3 HL | 37 | 227 | 80 | `0xdd74243f0648c740` | `0xd3140891eb7125cb` |
| 128 Y L3 HH | 168 | 494 | 112 | `0xf53d481e0a1166eb` | `0xf15d109eda1063a9` |
| 128 Y L2 LH | 577 | 1756 | 268 | `0x717b61c3de5cdd01` | `0x651c0fbf47349f3e` |
| 128 Y L2 HL | 547 | 1705 | 264 | `0x5ed84a6ba251c72f` | `0x4b18c7569fc9a25b` |
| 128 Y L2 HH | 823 | 2467 | 360 | `0xb28bba578d0fbfbe` | `0xb16b76513685537e` |
| 128 Y L1 LH | 2457 | 7321 | 964 | `0x6c55bd6863a163f6` | `0x58a4f1f3367be416` |
| 128 Y L1 HL | 2471 | 7338 | 968 | `0xaef22117943dcd41` | `0x4f294bd41866442b` |
| 128 Y L1 HH | 1570 | 5550 | 744 | `0x42747c5817a4cfc5` | `0x4cc67a60e906599b` |
| 4K Y L1 LH | 1211518 | 3615581 | 451996 | `0x2423f3eef9da0d28` | `0x112b6f69289267cd` |
| 4K Y L1 HL | 1256628 | 3712560 | 464120 | `0x48ea57665ad4a95b` | `0xadd7cc3c19061815` |
| 4K Y L1 HH | 726773 | 2612812 | 326652 | `0x8544daf4b4fb8e58` | `0x319c0e1f6ef1fcd9` |

Band numbers follow the upstream enum: 1 is LH, 2 is HL, 3 is HH. The test prints the shorter hex form when the high nibble is zero; the table restores the full 16 digits. The 1080p canonical sample is 900396 bytes. Its highpass coefficient download is 768100 bytes and its lowpass download is 129600 bytes.
