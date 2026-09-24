# CineForm Bitstream & Format Compatibility

This document details the format compatibility guarantees, supported color representations, resolution limits, and precision specifications for HyperCine.

---

## 1. Bitstream Compatibility Standard

HyperCine adheres strictly to:
1. **The Official GoPro CineForm SDK Bitstream Specification** (`CFHD` FourCC).
2. **SMPTE VC-5 Standard Suite** (SMPTE ST 2073-1: Elementary Bitstream, ST 2073-2: Conformance, ST 2073-7: Color Difference Video Formats).
3. **Ecosystem Interoperability:** Any bitstream encoded by HyperCine must decode seamlessly in:
   - Adobe Premiere Pro / After Effects (via official CineForm importer or QuickTime/AVI).
   - DaVinci Resolve Studio (native CineForm decoder).
   - Apple Final Cut Pro (QuickTime CineForm).
   - FFmpeg (`cfhd` decoder and demuxer).
   - Upstream GoPro CineForm SDK (`upstream/Example/TestCFHD`).

Conversely, HyperCine decodes any valid `CFHD` bitstream produced by cameras (GoPro HERO series with Protune, Silicon Imaging SI-2K), editing systems, or FFmpeg.

---

## 2. Supported Pixel & Encoded Formats

| Pixel Format FourCC | Memory Layout | Bit Depth | Encoded Format | Description |
|---------------------|---------------|-----------|----------------|-------------|
| `CFHD_PIXEL_FORMAT_YUY2` (`'YUY2'`) | Packed Y0 U0 Y1 V0 | 8-bit / component | `CFHD_ENCODED_FORMAT_YUV_422` | Standard Windows YUV 4:2:2 video broadcast format |
| `CFHD_PIXEL_FORMAT_2VUY` (`'2vuy'`) | Packed U0 Y0 V0 Y1 | 8-bit / component | `CFHD_ENCODED_FORMAT_YUV_422` | Standard macOS / QuickTime YUV 4:2:2 |
| `CFHD_PIXEL_FORMAT_BGRA` (`'BGRA'`) | Packed B, G, R, A | 8-bit / component | `CFHD_ENCODED_FORMAT_RGB_444` / `YUV_422` | Windows 32-bit graphics format (Direct3D surface format) |
| `CFHD_PIXEL_FORMAT_RGBA` (`'RGBA'`) | Packed R, G, B, A | 8-bit / component | `CFHD_ENCODED_FORMAT_RGB_444` / `YUV_422` | OpenGL / Vulkan standard 32-bit graphics format |
| `CFHD_PIXEL_FORMAT_RGB48` (`'R48 '`) | Packed R, G, B | 16-bit / component | `CFHD_ENCODED_FORMAT_RGB_444` | High dynamic range 16-bit uncompressed RGB |
| `CFHD_PIXEL_FORMAT_B64A` (`'b64a'`) | Packed B, G, R, A | 16-bit / component | `CFHD_ENCODED_FORMAT_RGBA_4444` | 16-bit HDR with alpha channel |
| `CFHD_PIXEL_FORMAT_BYR2` / `BYR4` | Planar Bayer Mosaic | 10-bit / 12-bit raw | `CFHD_ENCODED_FORMAT_BAYER` | CineForm RAW sensor capture |

---

## 3. Encoding Quality Presets

HyperCine maps directly to the official CineForm quality definitions:

| Quality Preset | Value | Quantization Scaling | Typical Bitrate (4K 24fps) | Recommended Use Case |
|----------------|-------|----------------------|---------------------------|----------------------|
| `CFHD_ENCODING_QUALITY_FILMSCAN3` | FilmScan 3 | Minimum (Near Lossless) | ~600 - 800 Mbps | Master archiving, VFX plates, multi-generation compositing |
| `CFHD_ENCODING_QUALITY_FILMSCAN2` | FilmScan 2 | Very Low | ~450 - 600 Mbps | High-end grading and finishing |
| `CFHD_ENCODING_QUALITY_FILMSCAN1` | FilmScan 1 | Low | ~300 - 450 Mbps | Production mastering |
| `CFHD_ENCODING_QUALITY_HIGH` | High | Moderate | ~180 - 250 Mbps | Online editing and high-quality preview |
| `CFHD_ENCODING_QUALITY_MEDIUM` | Medium | Medium | ~100 - 150 Mbps | Real-time multicam editing |
| `CFHD_ENCODING_QUALITY_LOW` | Low | High | ~50 - 80 Mbps | Fast proxy workflows |

---

## 4. Resolution and Aspect Ratio Support

- **Spatial Resolutions:**
  - Standard Definition (SD 480p, 576p)
  - High Definition (HD 720p, 1080p, 1080i)
  - Digital Cinema 2K ($2048 \times 1080$)
  - Ultra High Definition 4K UHD ($3840 \times 2160$) & 4K DCI ($4096 \times 2160$)
  - Ultra High Definition 8K UHD ($7680 \times 4320$) & 8K DCI ($8192 \times 4320$)
  - Arbitrary custom dimensions (width must be multiple of 16, height must be multiple of 8 or 16 depending on wavelet decomposition depth).
- **Scan Modes:**
  - Progressive (`CFHD_ENCODED_FORMAT_PROGRESSIVE`)
  - Interlaced Upper Field First (`CFHD_ENCODED_FORMAT_UPPER_FIELD_FIRST`)
  - Interlaced Lower Field First (`CFHD_ENCODED_FORMAT_LOWER_FIELD_FIRST`)

---

## 5. Mathematical Precision & Determinism

### Wavelet Integer Arithmetic
- All 2-6 biorthogonal wavelet transforms compute using exact 16-bit and 32-bit integer arithmetic with symmetric rounding offsets (e.g. $+8$ before right-shift by 4 in highpass analysis).
- Floating-point calculations are strictly forbidden in the core wavelet pipeline to prevent divergence between CPU and GPU architectures.

### Video Levels vs. Full Range (Studio Pedestal)
- Standard ITU-R BT.601 and BT.709 video signals use Studio Swing:
  - Luma ($Y$): 16 to 235 (footroom and headroom preserved).
  - Chroma ($U, V$): 16 to 240, centered at 128.
- CineForm handles pedestal removal and re-insertion during color transformation. In tests using full-range patterns (values $0 \dots 15$), pedestal clamping removes the lower 16 values, resulting in an expected PSNR lower-bound of $\approx 24\text{ dB}$ on constant low-pedestal black fields.
- Full computer RGB ($0 \dots 255$) preserves full dynamic range when encoded in `RGB_444` or `FILMSCAN3` mode.
