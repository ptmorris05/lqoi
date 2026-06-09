# LQOI - The “Lossy Quite OK Image Format”

**Notice:** This is an experimental fork of the original [QOI (Quite OK Image)](https://github.com/phoboslab/qoi) format by Dominic Szablewski.

LQOI introduces perceptual quantization into the QOI algorithm to heavily increase compression ratios while maintaining the extreme encode/decode throughput of the original single-pass architecture.

Single-file MIT licensed library for C/C++

See `qoi.h` for the documentation and format specification.

## Why LQOI?

The original QOI format is strictly lossless, which limits its compression capabilities on noisy images, micro-gradients, or digital photography.

**LQOI makes QOI lossy.** By applying targeted, human-vision-biased quantization during encoding, LQOI massively increases the hit-rate of QOI's most efficient opcodes (`QOI_OP_RUN` and `QOI_OP_INDEX`).

The primary design constraint of LQOI is performance: the modifications require **no branching** and **no dynamic state synchronization** in the decoder. All perceptual checks are done strictly during the encode pass using fast bitwise operations. This keeps LQOI well within a 10% latency margin of the original QOI's blistering speeds, while achieving significantly smaller file sizes.

### The Five Mechanisms of LQOI

1. **Green-Weighted Manhattan Distance:** Replaces exact-match checks with a branchless L1 norm error threshold ($2|\Delta G| + |\Delta R| + |\Delta B| \le 6$). Alpha must remain exact.
2. **Chroma-Biased Lossy Runs:** Micro-gradients are squashed into `QOI_OP_RUN` chunks. The run continues as long as the current pixel remains within the perceptual threshold of the *first* pixel in the run, preventing compounding drift.
3. **Lossy Indexing (Snap to Palette):** Pixels evaluate a locality-sensitive version of the standard QOI hash. If the stored pixel is perceptually close (within the threshold), the pixel "snaps" to the palette color via `QOI_OP_INDEX`.
4. **Base Pixel Hash Injection:** When a pixel is quantized, the encoder injects the *substituted* value into the hash array, standardizing the palette and eliminating decoder mismatch.
5. **Scaled `QOI_OP_LUMA`:** The Green channel payload is bit-shifted (`val >> 1`), doubling its effective reach to `[-64, 63]` and allowing medium-contrast edges to be captured in 2 bytes instead of 4.

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
