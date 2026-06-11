# LQOI - The “Lossy Quite OK Image Format”

**Notice:** This is an experimental fork of the original [QOI (Quite OK Image)](https://github.com/phoboslab/qoi) format by Dominic Szablewski.

LQOI introduces perceptual quantization into the QOI algorithm to heavily increase compression ratios while maintaining the extreme encode/decode throughput of the original single-pass architecture.

Single-file MIT licensed library for C/C++

See `qoi.h` for the documentation and format specification.

## Why LQOI?

The original QOI format is strictly lossless, which limits its compression capabilities on noisy images, micro-gradients, or digital photography.

**LQOI makes QOI lossy.** By applying targeted, human-vision-biased quantization during encoding, LQOI massively increases the hit-rate of QOI's most efficient opcodes (`QOI_OP_RUN` and `QOI_OP_INDEX`).

The primary design constraint of LQOI is performance: the modifications require **no dynamic state synchronization** in the decoder. All perceptual checks are confined to the encode pass, and the encoder uses channel-specialized hot loops with branchless, shallow perceptual gates to keep them cheap. Both encode and decode stay within ~10% of lossless QOI's throughput while producing files **1.24× smaller than lossless QOI** at ~49 dB PSNR (see benchmarks below).

### Benchmark Results

Measured on the [Kodak True Color suite](http://r0k.us/graphics/kodak/) (24 photographic images, 768×512), Apple M5, clang -O3:

| Codec | Compression | Size vs raw | Fidelity (PSNR) | Decode | Encode |
|-------|-------------|-------------|-----------------|--------|--------|
| QOI (lossless) | 1.72× | 58.3% | ∞ (lossless) | 306 Mpx/s | 218 Mpx/s |
| **LQOI (lossy)** | **2.13×** | **46.9%** | **49.15 dB** | **274 Mpx/s (−11%)** | **196 Mpx/s (−10%)** |

LQOI produces files **19.5% smaller than lossless QOI** (1.24×) while reconstructing every pixel within its perceptual budget (green-weighted error ≤ 6, alpha exact) — ~49 dB PSNR is visually near-lossless. The throughput cost vs lossless QOI is ~10% in both directions; most of it is not extra arithmetic but branch entropy: the better-compressed stream has a flatter chunk-type distribution, which costs branch mispredictions in both coders.

The strength of the 1-byte `QOI_OP_LUMA1` mechanism is a compile-time dial (`QOI_LUMA1_T`, an encoder-only setting — all settings produce streams readable by the same decoder): `0` = exact hits only (highest PSNR, 84.7% of QOI size), `1` (default, above), `2` = 76.9% of QOI size at 48.67 dB.

See [`BENCHMARKING.md`](BENCHMARKING.md) to reproduce (`./run_benchmark.sh`).

### The Six Mechanisms of LQOI

1. **Green-Weighted Manhattan Distance:** Replaces exact-match checks with a branchless L1 norm error threshold ($2|\Delta G| + |\Delta R| + |\Delta B| \le 6$). Alpha must remain exact.
2. **Chroma-Biased Lossy Runs:** Micro-gradients are squashed into `QOI_OP_RUN` chunks. The run continues as long as the current pixel remains within the perceptual threshold of the *first* pixel in the run, preventing compounding drift.
3. **Lossy Indexing (Snap to Palette):** Pixels evaluate a locality-sensitive version of the standard QOI hash. If the stored pixel is perceptually close (within the threshold), the pixel "snaps" to the palette color via `QOI_OP_INDEX`.
4. **Base Pixel Hash Injection:** When a pixel is quantized, the encoder injects the *substituted* value into the hash array, standardizing the palette and eliminating decoder mismatch.
5. **Scaled `QOI_OP_LUMA`:** The Green channel payload is bit-shifted (`val >> 1`), doubling its effective reach to `[-64, 63]` and allowing medium-contrast edges to be captured in 2 bytes instead of 4.
6. **Chroma-Corrected Luma Step (`QOI_OP_LUMA1`):** Repurposes the `QOI_OP_DIFF` tag, which the lossy runs had left nearly dead (<1% of chunks), as a 1-byte chunk: an exact 4-bit green delta plus a shared 2-bit correction applied to red and blue. It is tried before the palette, so it converts both 2-byte `LUMA` chunks and lossy `INDEX` snaps into 1 byte with at most ±1 total error on red+blue — on Kodak this alone makes files ~9% smaller *and* raises PSNR.

## Example Usage

Usage remains identical to the original QOI library.

```c
#define QOI_IMPLEMENTATION
#include "qoi.h"

// Encode and store an RGBA buffer to the file system. 
// LQOI applies perceptual quantization automatically during this step.
qoi_write("image_new.lqoi", rgba_pixels, &(qoi_desc){
    .width = 1920,
    .height = 1080,
    .channels = 4,
    .colorspace = QOI_SRGB
});

// Load and decode an LQOI image from the file system.
qoi_desc desc;
void *rgba_pixels = qoi_read("image.lqoi", &desc, 4);

```

## Compatibility & File Extension

Because LQOI repurposes the payload meanings of `QOI_OP_DIFF` and `QOI_OP_LUMA`, **LQOI files are not decodable by standard QOI decoders** (they will appear corrupted).

For this reason, the recommended file extension for LQOI images is `.lqoi` and the recommended MIME type is `image/lqoi`.

## Limitations

Like the original QOI implementation, this library is limited to images with a maximum size of 400 million pixels and is not a streaming en-/decoder. It loads the whole image file into RAM before doing any work.

Additionally, LQOI is strictly a **lossy** format. It is highly optimized for human perception, but it should not be used for normal maps, depth passes, scientific data, or pixel-art where strict 1:1 color accuracy is required.

## Acknowledgments

LQOI is built entirely on the brilliant structural foundation of [QOI by phoboslab](https://github.com/phoboslab/qoi). If you require lossless compression, or want to explore the massive ecosystem of native ports, viewers, and plugins for this format, please visit the original repository.
