# FilmScan3 CPU geometry P4 — design and qualification

Source baseline: `2a5e94ff6120a7053621c6ab4dcb020c7934e6d4`; issue #44.

## Audit and design (before implementation)

The legacy `ValidateGeometry` and fixed-Q2 `Sample` budget stay at 2,097,152
pixels / 16,793,600 bytes. Raising that global constant would also silently
expand legacy allocations. Instead, CPU FilmScan3 gets an explicit per-session
geometry constructor and its own 8,294,400-pixel budget. Existing constructors
retain 1920×1080 semantics and encoder object layout. Session geometry is fixed;
Reset resets rate state only. The new constructor selects CPU only.

| Boundary | Existing behavior | Planned bound / proof |
| --- | --- | --- |
| Frame dimensions | width / height <=4096; width %128, height %8, height >=24 | Same legal wire geometry; independent FS3 pixel budget; 8K rejected |
| Plane constructor | padded stride times height <= legacy pixel cap | Separate 8,294,400-element allocation cap; <=16,588,800 bytes per plane |
| Forward / frontend validation | shared legacy plane cap | Same plane cap; prescale 0,2,0 and six-tap minimum unchanged |
| BGRA storage | aligned, positive padded pitch; checked last row extent | Preserve checked `(SIZE_MAX-row)/(height-1)` guard; bottom-up CG709 Y/Cr/Cb |
| PackHighpass | 4*(N+1) reserve; <=26 bits/coefficient and end/pad | Separate plane cap; <=33,177,604 bytes reserve; unsigned run <=8,294,400 |
| FS3 Writer::Check | 8*legacy pixels+16384 | Session `8*pixels+16384` <=66,371,584 bytes; below 24-bit chunk limit |
| Writer::End | 24-bit word count | Subtraction-based patch bounds; reject >0xffffff words |
| Writer::Record | 24-bit size, fixed small metadata | Explicit size guard; checked bounded padding |
| Writer::Patch / channel index | uint32 bytes | Check before narrowing; whole sample <=66,371,584 < UINT32_MAX |
| NextLimiter | fixed 1920*1080*3*10/8 | Session pixels, integer division then float conversion/division, double 1.5 then float |

All products are bounded before use: dimensions <=4096, padded plane stride
<=4096, sample/entropy sizes <2^27, indices <8,294,400. These fit even 32-bit
size_t and signed int indexing. No global 8K allowance or new chunk variant.
`upstream/Codec/quantize.c` computes the integer 3-channel 10-bit numerator
before float division and then compound division by double 1.5. Preserve that
rounding order; 1080p original packets are the regression oracle, not runtime
rate-control input. GUID/DATE/TIME/TIMC/UFRM and all coefficient/table records
remain constructed exactly as before. State commits only after packet assembly.

Conservative live CPU allocation estimate: <=8P bytes color+pyramid/output
bands, <=4P temporary transform buffers, <=4P entropy, <=16P writer capacity
plus <=4P caller BGRA and allocator overhead: <36P+1MiB, about 286 MiB at 4K.
The budget concerns one encode, not arbitrary caller-retained output history or
unbounded concurrent encoders. Measure process peak RSS separately on the
16GB M2 Pro. Allocation failures propagate without committing frame state.

## Qualification status

Implementation and measured evidence will be added after testing. 4K SDK
byte matching requires a genuine original-GoPro corpus with identical inputs,
parameters, state and metadata. 8K remains PENDING_8K until independent SDK
samples, chunk feasibility and host resource qualification exist.
