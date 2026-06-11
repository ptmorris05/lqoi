/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT

LQOI - The "Lossy Quite OK Image" format 
(A perceptually quantized variant of QOI)

-- About

LQOI encodes and decodes images using the structural foundation of QOI but 
introduces perceptual quantization to heavily increase compression ratios 
with minimal performance overhead. It stays strictly single-pass.

Modifications in this variant:
1. Green-Weighted Manhattan Distance for perceptual error checks.
2. Chroma-Biased Lossy Runs with hysteresis: Micro-gradients are squashed
   into runs. A run starts only within the strict budget (QOI_RUN_START_T,
   default 6) but continues within a looser one (QOI_RUN_CONT_T, default 10),
   stretching runs over more pixels: fewer chunks, smaller files, and longer
   same-chunk stretches that keep both coders' branch predictors on track.
3. Lossy Indexing: Pixels snap to perceptually similar palette colors using a locality-sensitive hash (bottom 3 bits masked).
4. Base Pixel Hash Injection: State tracking uses substituted/quantized values.
5. Scaled QOI_OP_LUMA: Delta Green is bit-shifted, doubling the range to [-64, 63].
6. QOI_OP_LUMA1 (repurposes the QOI_OP_DIFF tag): a 1-byte luma step with an
   exact 4-bit green delta and a shared 2-bit chroma correction applied to r
   and b. The encoder uses it only when the total r+b error is at most
   QOI_LUMA1_T (default 1). It is tried before the palette, so it replaces
   both 2-byte LUMA chunks and lossier INDEX snaps -- smaller files AND
   higher PSNR than the exact DIFF op it replaces (which the lossy RUN had
   made nearly dead: <1% of chunks).

-- Synopsis

// Define `QOI_IMPLEMENTATION` in *one* C/C++ file before including this
// library to create the implementation.

#define QOI_IMPLEMENTATION
#include "qoi.h"

// Encode and store an RGBA buffer to the file system. The qoi_desc describes
// the input pixel data.
qoi_write("image_new.qoi", rgba_pixels, &(qoi_desc){
    .width = 1920,
    .height = 1080,
    .channels = 4,
    .colorspace = QOI_SRGB
});

// Load and decode a QOI image from the file system into a 32bbp RGBA buffer.
// The qoi_desc struct will be filled with the width, height, number of channels
// and colorspace read from the file header.
qoi_desc desc;
void *rgba_pixels = qoi_read("image.qoi", &desc, 4);

-- Data Format

A QOI file has a 14 byte header, followed by any number of data "chunks" and an
8-byte end marker.

struct qoi_header_t {
    char     magic[4];   // magic bytes "qoif"
    uint32_t width;      // image width in pixels (BE)
    uint32_t height;     // image height in pixels (BE)
    uint8_t  channels;   // 3 = RGB, 4 = RGBA
    uint8_t  colorspace; // 0 = sRGB with linear alpha, 1 = all channels linear
};

Pixels are encoded as
 - a run of pixels perceptually similar to the previous pixel
 - an index into an array of perceptually similar previously seen pixels
 - a luma step from the previous pixel: an exact green delta plus a chroma
   correction, in one byte (LUMA1) or two (LUMA)
 - full r,g,b or r,g,b,a values

.- QOI_OP_INDEX ----------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----------------|
|  0  0 |     index       |
`-------------------------`
2-bit tag b00
6-bit index into the color index array: 0..63

.- QOI_OP_LUMA1 ----------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----------+-----|
|  0  1 |    dg     |  c  |
`-------------------------`
2-bit tag b01 (repurposes the original QOI_OP_DIFF tag)
4-bit green channel difference from the previous pixel -8..7 (exact)
2-bit chroma correction -2..1, applied on top of dg to red and blue

The decoded values are
    r = prev.r + dg + c
    g = prev.g + dg
    b = prev.b + dg + c

All additions wrap around (mod 256). dg is stored with a bias of 8, c with a
bias of 2. Green is always reconstructed exactly; the encoder only emits this
chunk when the combined red+blue error is within its perceptual budget
(QOI_LUMA1_T, default 1).

.- QOI_OP_LUMA -------------------------------------.
|         Byte[0]         |         Byte[1]         |
|  7  6  5  4  3  2  1  0 |  7  6  5  4  3  2  1  0 |
|-------+-----------------+-------------+-----------|
|  1  0 |  green diff     |   dr - dg   |  db - dg  |
`---------------------------------------------------`
2-bit tag b10
6-bit green channel difference from the previous pixel (bit-shifted) -32..31
4-bit   red channel difference minus green channel difference -8..7
4-bit  blue channel difference minus green channel difference -8..7

The green channel difference is bit-shifted (divided by 2) during encoding, 
doubling its effective range to -64..63. It is stored with a bias of 32.
dr_dg and db_dg are calculated relative to the DECODED green difference.

.- QOI_OP_RUN ------------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----------------|
|  1  1 |       run       |
`-------------------------`
2-bit tag b11
6-bit run-length repeating the previous pixel (or perceptually similar): 1..62

The encoder starts a run only for pixels within QOI_RUN_START_T (default 6)
of the run's base pixel, but continues it within QOI_RUN_CONT_T (default 10).
The continuation budget is the loosest per-pixel error bound of the codec.

.- QOI_OP_RGB / RGBA ------.
(Identical to standard QOI format: b11111110 / b11111111 followed by raw bytes)

*/

/* -----------------------------------------------------------------------------
Header - Public functions */

#ifndef QOI_H
#define QOI_H

#ifdef __cplusplus
extern "C" {
#endif

#define QOI_SRGB   0
#define QOI_LINEAR 1

typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned char channels;
    unsigned char colorspace;
} qoi_desc;

#ifndef QOI_NO_STDIO
int qoi_write(const char *filename, const void *data, const qoi_desc *desc);
void *qoi_read(const char *filename, qoi_desc *desc, int channels);
#endif /* QOI_NO_STDIO */

void *qoi_encode(const void *data, const qoi_desc *desc, int *out_len);
void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels);

#ifdef __cplusplus
}
#endif
#endif /* QOI_H */

/* -----------------------------------------------------------------------------
Implementation */

#ifdef QOI_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>

#ifndef QOI_MALLOC
    #define QOI_MALLOC(sz) malloc(sz)
    #define QOI_FREE(p)    free(p)
#endif
#ifndef QOI_ZEROARR
    #define QOI_ZEROARR(a) memset((a),0,sizeof(a))
#endif

#define QOI_OP_INDEX  0x00 /* 00xxxxxx */
#define QOI_OP_LUMA1  0x40 /* 01xxxxxx */
#define QOI_OP_LUMA   0x80 /* 10xxxxxx */
#define QOI_OP_RUN    0xc0 /* 11xxxxxx */
#define QOI_OP_RGB    0xfe /* 11111110 */
#define QOI_OP_RGBA   0xff /* 11111111 */

#define QOI_MASK_2    0xc0 /* 11000000 */

/* Perceptual budget of QOI_OP_LUMA1: max combined |err_r| + |err_b| the
   encoder accepts for the 1-byte luma-step chunk (green is always exact).
   This is an encoder-only dial -- streams produced with any budget decode
   with the same decoder. On Kodak: 1 (default) improves BOTH size and PSNR
   over the old exact-DIFF codec; 0 allows only exact hits (highest PSNR,
   smaller size win); 2 trades ~0.5 dB for another ~4% of size. */
#ifndef QOI_LUMA1_T
    #define QOI_LUMA1_T 1
#endif

/* Lossy-run budgets (green-weighted Manhattan distance vs the run's base
   pixel). A run STARTS only within the strict budget, but may CONTINUE within
   a slightly larger one. This hysteresis stretches runs over more pixels, so
   the stream has fewer chunks: files get smaller AND both coders get faster
   (longer same-chunk stretches keep the branch predictors on known paths).
   Encoder-only dials; QOI_RUN_CONT_T is the codec's perceptual error bound. */
#ifndef QOI_RUN_START_T
    #define QOI_RUN_START_T 6
#endif
#ifndef QOI_RUN_CONT_T
    #define QOI_RUN_CONT_T 10
#endif

/* Updated to a Locality-Sensitive Hash: masks out the bottom 3 bits of RGB so similar colors collide */
#define QOI_COLOR_HASH(C) (((C.rgba.r & 0xf8)*3 + (C.rgba.g & 0xf8)*5 + (C.rgba.b & 0xf8)*7 + C.rgba.a*11))
#define QOI_MAGIC \
    (((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
     ((unsigned int)'i') <<  8 | ((unsigned int)'f'))
#define QOI_HEADER_SIZE 14

#define QOI_PIXELS_MAX ((unsigned int)400000000)

typedef union {
    struct { unsigned char r, g, b, a; } rgba;
    unsigned int v;
} qoi_rgba_t;

static const unsigned char qoi_padding[8] = {0,0,0,0,0,0,0,1};

static void qoi_write_32(unsigned char *bytes, int *p, unsigned int v) {
    bytes[(*p)++] = (0xff000000 & v) >> 24;
    bytes[(*p)++] = (0x00ff0000 & v) >> 16;
    bytes[(*p)++] = (0x0000ff00 & v) >> 8;
    bytes[(*p)++] = (0x000000ff & v);
}

static unsigned int qoi_read_32(const unsigned char *bytes, int *p) {
    unsigned int a = bytes[(*p)++];
    unsigned int b = bytes[(*p)++];
    unsigned int c = bytes[(*p)++];
    unsigned int d = bytes[(*p)++];
    return a << 24 | b << 16 | c << 8 | d;
}

void *qoi_encode(const void *data, const qoi_desc *desc, int *out_len) {
    int i, max_size, p, run;
    int px_len, px_end, px_pos, channels;
    unsigned char *bytes, *op;
    const unsigned char *pixels;
    qoi_rgba_t index[64];
    qoi_rgba_t px, px_prev;

    if (
        data == NULL || out_len == NULL || desc == NULL ||
        desc->width == 0 || desc->height == 0 ||
        desc->channels < 3 || desc->channels > 4 ||
        desc->colorspace > 1 ||
        desc->height >= QOI_PIXELS_MAX / desc->width
    ) {
        return NULL;
    }

    max_size =
        desc->width * desc->height * (desc->channels + 1) +
        QOI_HEADER_SIZE + sizeof(qoi_padding);

    p = 0;
    bytes = (unsigned char *) QOI_MALLOC(max_size);
    if (!bytes) {
        return NULL;
    }

    qoi_write_32(bytes, &p, QOI_MAGIC);
    qoi_write_32(bytes, &p, desc->width);
    qoi_write_32(bytes, &p, desc->height);
    bytes[p++] = desc->channels;
    bytes[p++] = desc->colorspace;

    pixels = (const unsigned char *)data;
    QOI_ZEROARR(index);

    run = 0;
    px_prev.rgba.r = 0;
    px_prev.rgba.g = 0;
    px_prev.rgba.b = 0;
    px_prev.rgba.a = 255;
    px = px_prev;

    px_len = desc->width * desc->height * desc->channels;
    px_end = px_len - desc->channels;
    channels = desc->channels;
    op = bytes + p;

    /* The per-pixel hot path is split into two channel-specialized loops. For
       RGB input the alpha never varies, so the RGB loop drops the per-pixel
       alpha load and the three alpha comparisons (run / index / diff gates),
       which measurably speeds up encoding. The two loops are otherwise
       identical in behaviour and emit a bitstream-identical stream -- keep them
       in sync if the encoding logic changes.

       All channel deltas are kept as mod-256 values in unsigned int form
       (d_r/u_vg/u_drdg/...) so the range gates compile to a single unsigned
       compare with no sign extension; each gate is OR-combined into one
       branch. The LUMA branch reconstructs r and b exactly and green as
       g - (dg & 1), so instead of re-hashing the substituted pixel it adjusts
       the green hash term in place (it moves by exactly -8*5 = -40 when and
       only when bit 3 of green flips). */
    if (channels == 4) {
        for (px_pos = 0; px_pos < px_len; px_pos += 4) {
            px.rgba.r = pixels[px_pos + 0];
            px.rgba.g = pixels[px_pos + 1];
            px.rgba.b = pixels[px_pos + 2];
            px.rgba.a = pixels[px_pos + 3];

            int s_r = (int)px.rgba.r - (int)px_prev.rgba.r;
            int s_g = (int)px.rgba.g - (int)px_prev.rgba.g;
            int s_b = (int)px.rgba.b - (int)px_prev.rgba.b;

            /* 1 & 2. Chroma-Biased Lossy Runs using Green-Weighted Manhattan Distance */
            int err_run = 2 * abs(s_g) + abs(s_r) + abs(s_b);

            if (px.rgba.a == px_prev.rgba.a &&
                err_run <= (run ? QOI_RUN_CONT_T : QOI_RUN_START_T)) {
                run++;
                if (run == 62 || px_pos == px_end) {
                    *op++ = QOI_OP_RUN | (run - 1);
                    run = 0;
                }
                continue; /* px_prev remains the base pixel */
            }

            if (run > 0) {
                *op++ = QOI_OP_RUN | (run - 1);
                run = 0;
            }

            /* 6. Chroma-Corrected Luma Step: 1-byte chunk with exact green and
               a shared 2-bit r/b correction. Tried before the palette: a hit
               here is at least as small as INDEX and far more accurate. */
            {
                unsigned int u_dg = (unsigned int)(s_g + 8) & 0xff;
                int dgv = (int)u_dg - 8;
                int x = (signed char)(s_r - dgv);    /* dr - dg, mod 256 */
                int y = (signed char)(s_b - dgv);    /* db - dg, mod 256 */

                /* The gate is shallow on purpose: the minimal r+b error for
                   the best in-range correction c is |x-y| plus twice the gap
                   between [min(x,y),max(x,y)] and [-2,1]; for small budgets it
                   reduces to pure range checks. Everything deeper (the chosen
                   correction, per-channel errors, wraparound) is only computed
                   on a hit, off the always-taken path. */
#if QOI_LUMA1_T == 0
                int hit1 = (px.rgba.a == px_prev.rgba.a) & (u_dg <= 15) &
                           (x == y) & ((unsigned int)(x + 2) <= 3);
#elif QOI_LUMA1_T == 1
                int hit1 = (px.rgba.a == px_prev.rgba.a) & (u_dg <= 15) &
                           ((unsigned int)(x - y + 1) <= 2) &
                           ((unsigned int)(x + y + 5) <= 8);
#else
                int lo1 = x < y ? x : y, hi1 = x < y ? y : x;
                int gap1 = lo1 > 1 ? lo1 - 1 : (hi1 < -2 ? -2 - hi1 : 0);
                int hit1 = (px.rgba.a == px_prev.rgba.a) & (u_dg <= 15) &
                           ((hi1 - lo1) + 2 * gap1 <= QOI_LUMA1_T);
#endif

                if (hit1) {
                    int c = x < -2 ? -2 : (x > 1 ? 1 : x);
                    int e_r = x - c, e_b = y - c;
                    int dec_r = (int)px.rgba.r - e_r;
                    int dec_b = (int)px.rgba.b - e_b;
                    /* the dropped error may not wrap r or b past the byte */
                    if (((unsigned int)dec_r <= 255) & ((unsigned int)dec_b <= 255)) {
                        *op++ = QOI_OP_LUMA1 | (u_dg << 2) | (c + 2);
                        px.rgba.r = (unsigned char)dec_r;
                        px.rgba.b = (unsigned char)dec_b;
                        index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
                        px_prev = px;
                        continue;
                    }
                }
            }

            int index_pos = QOI_COLOR_HASH(px) & (64 - 1);
            qoi_rgba_t pal_px = index[index_pos];

            /* 3. Lossy Indexing (Snap to Palette) */
            int err_idx = 2 * abs((int)px.rgba.g - (int)pal_px.rgba.g) +
                              abs((int)px.rgba.r - (int)pal_px.rgba.r) +
                              abs((int)px.rgba.b - (int)pal_px.rgba.b);

            if (px.rgba.a == pal_px.rgba.a && err_idx <= 6) {
                *op++ = QOI_OP_INDEX | index_pos;
                px = pal_px; /* 4. Base Pixel Hash Injection */
            }
            else {
                int store_pos = index_pos;

                if (px.rgba.a == px_prev.rgba.a) {
                    /* Scaled Luma (Doubled range via bitshift): u_vg = vg + 64 */
                    {
                        unsigned int u_vg = (unsigned int)(s_g + 64) & 0xff;

                        if (u_vg <= 127) {
                            unsigned int odd = u_vg & 1;        /* the dropped green LSB */
                            int dg2 = (int)u_vg - 64 - (int)odd; /* decoded green delta */
                            unsigned int u_drdg = (unsigned int)(s_r - dg2 + 8) & 0xff;
                            unsigned int u_dbdg = (unsigned int)(s_b - dg2 + 8) & 0xff;
                            int g_sub = (int)px.rgba.g - (int)odd; /* reconstructed green */

                            /* g_sub < 0 is the one case where dropping the green
                               LSB wraps the byte (0 -> 255); fall through to RGB. */
                            if ((u_drdg | u_dbdg) <= 15 && g_sub >= 0) {
                                *op++ = QOI_OP_LUMA | (u_vg >> 1);
                                *op++ = (u_drdg << 4) | u_dbdg;

                                /* r and b reconstruct exactly; only green moves (by odd) */
                                store_pos = (index_pos -
                                    (int)(((px.rgba.g ^ (unsigned int)g_sub) >> 3) & 1) * 40) & (64 - 1);
                                px.rgba.g = (unsigned char)g_sub;
                            }
                            else {
                                *op++ = QOI_OP_RGB;
                                *op++ = px.rgba.r;
                                *op++ = px.rgba.g;
                                *op++ = px.rgba.b;
                            }
                        }
                        else {
                            *op++ = QOI_OP_RGB;
                            *op++ = px.rgba.r;
                            *op++ = px.rgba.g;
                            *op++ = px.rgba.b;
                        }
                    }
                }
                else {
                    *op++ = QOI_OP_RGBA;
                    *op++ = px.rgba.r;
                    *op++ = px.rgba.g;
                    *op++ = px.rgba.b;
                    *op++ = px.rgba.a;
                }

                index[store_pos] = px;
            }

            px_prev = px;
        }
    }
    else {
        for (px_pos = 0; px_pos < px_len; px_pos += 3) {
            px.rgba.r = pixels[px_pos + 0];
            px.rgba.g = pixels[px_pos + 1];
            px.rgba.b = pixels[px_pos + 2];

            int s_r = (int)px.rgba.r - (int)px_prev.rgba.r;
            int s_g = (int)px.rgba.g - (int)px_prev.rgba.g;
            int s_b = (int)px.rgba.b - (int)px_prev.rgba.b;

            /* 1 & 2. Chroma-Biased Lossy Runs (alpha is constant for RGB) */
            int err_run = 2 * abs(s_g) + abs(s_r) + abs(s_b);

            if (err_run <= (run ? QOI_RUN_CONT_T : QOI_RUN_START_T)) {
                run++;
                if (run == 62 || px_pos == px_end) {
                    *op++ = QOI_OP_RUN | (run - 1);
                    run = 0;
                }
                continue; /* px_prev remains the base pixel */
            }

            if (run > 0) {
                *op++ = QOI_OP_RUN | (run - 1);
                run = 0;
            }

            /* 6. Chroma-Corrected Luma Step: 1-byte chunk with exact green and
               a shared 2-bit r/b correction. Tried before the palette: a hit
               here is at least as small as INDEX and far more accurate. */
            {
                unsigned int u_dg = (unsigned int)(s_g + 8) & 0xff;
                int dgv = (int)u_dg - 8;
                int x = (signed char)(s_r - dgv);    /* dr - dg, mod 256 */
                int y = (signed char)(s_b - dgv);    /* db - dg, mod 256 */

                /* The gate is shallow on purpose: the minimal r+b error for
                   the best in-range correction c is |x-y| plus twice the gap
                   between [min(x,y),max(x,y)] and [-2,1]; for small budgets it
                   reduces to pure range checks. Everything deeper (the chosen
                   correction, per-channel errors, wraparound) is only computed
                   on a hit, off the always-taken path. */
#if QOI_LUMA1_T == 0
                int hit1 = (u_dg <= 15) &
                           (x == y) & ((unsigned int)(x + 2) <= 3);
#elif QOI_LUMA1_T == 1
                int hit1 = (u_dg <= 15) &
                           ((unsigned int)(x - y + 1) <= 2) &
                           ((unsigned int)(x + y + 5) <= 8);
#else
                int lo1 = x < y ? x : y, hi1 = x < y ? y : x;
                int gap1 = lo1 > 1 ? lo1 - 1 : (hi1 < -2 ? -2 - hi1 : 0);
                int hit1 = (u_dg <= 15) &
                           ((hi1 - lo1) + 2 * gap1 <= QOI_LUMA1_T);
#endif

                if (hit1) {
                    int c = x < -2 ? -2 : (x > 1 ? 1 : x);
                    int e_r = x - c, e_b = y - c;
                    int dec_r = (int)px.rgba.r - e_r;
                    int dec_b = (int)px.rgba.b - e_b;
                    /* the dropped error may not wrap r or b past the byte */
                    if (((unsigned int)dec_r <= 255) & ((unsigned int)dec_b <= 255)) {
                        *op++ = QOI_OP_LUMA1 | (u_dg << 2) | (c + 2);
                        px.rgba.r = (unsigned char)dec_r;
                        px.rgba.b = (unsigned char)dec_b;
                        index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
                        px_prev = px;
                        continue;
                    }
                }
            }

            int index_pos = QOI_COLOR_HASH(px) & (64 - 1);
            qoi_rgba_t pal_px = index[index_pos];

            /* 3. Lossy Indexing (Snap to Palette) */
            int err_idx = 2 * abs((int)px.rgba.g - (int)pal_px.rgba.g) +
                              abs((int)px.rgba.r - (int)pal_px.rgba.r) +
                              abs((int)px.rgba.b - (int)pal_px.rgba.b);

            if (err_idx <= 6) {
                *op++ = QOI_OP_INDEX | index_pos;
                px = pal_px; /* 4. Base Pixel Hash Injection */
            }
            else {
                int store_pos = index_pos;

                /* Scaled Luma (Doubled range via bitshift): u_vg = vg + 64 */
                {
                    unsigned int u_vg = (unsigned int)(s_g + 64) & 0xff;

                    if (u_vg <= 127) {
                        unsigned int odd = u_vg & 1;        /* the dropped green LSB */
                        int dg2 = (int)u_vg - 64 - (int)odd; /* decoded green delta */
                        unsigned int u_drdg = (unsigned int)(s_r - dg2 + 8) & 0xff;
                        unsigned int u_dbdg = (unsigned int)(s_b - dg2 + 8) & 0xff;
                        int g_sub = (int)px.rgba.g - (int)odd; /* reconstructed green */

                        /* g_sub < 0 is the one case where dropping the green
                           LSB wraps the byte (0 -> 255); fall through to RGB.
                           r and b are exact mod 256 regardless; decoder unchanged. */
                        if ((u_drdg | u_dbdg) <= 15 && g_sub >= 0) {
                            *op++ = QOI_OP_LUMA | (u_vg >> 1);
                            *op++ = (u_drdg << 4) | u_dbdg;

                            /* r and b reconstruct exactly; only green moves (by odd) */
                            store_pos = (index_pos -
                                (int)(((px.rgba.g ^ (unsigned int)g_sub) >> 3) & 1) * 40) & (64 - 1);
                            px.rgba.g = (unsigned char)g_sub;
                        }
                        else {
                            *op++ = QOI_OP_RGB;
                            *op++ = px.rgba.r;
                            *op++ = px.rgba.g;
                            *op++ = px.rgba.b;
                        }
                    }
                    else {
                        *op++ = QOI_OP_RGB;
                        *op++ = px.rgba.r;
                        *op++ = px.rgba.g;
                        *op++ = px.rgba.b;
                    }
                }

                index[store_pos] = px;
            }

            px_prev = px;
        }
    }

    for (i = 0; i < (int)sizeof(qoi_padding); i++) {
        *op++ = qoi_padding[i];
    }

    *out_len = (int)(op - bytes);
    return bytes;
}

void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels) {
    const unsigned char *bytes;
    unsigned int header_magic;
    unsigned char *pixels;
    qoi_rgba_t index[64];
    qoi_rgba_t px;
    int px_len, chunks_len, px_pos;
    int p = 0, run = 0;

    if (
        data == NULL || desc == NULL ||
        (channels != 0 && channels != 3 && channels != 4) ||
        size < QOI_HEADER_SIZE + (int)sizeof(qoi_padding)
    ) {
        return NULL;
    }

    bytes = (const unsigned char *)data;

    header_magic = qoi_read_32(bytes, &p);
    desc->width = qoi_read_32(bytes, &p);
    desc->height = qoi_read_32(bytes, &p);
    desc->channels = bytes[p++];
    desc->colorspace = bytes[p++];

    if (
        desc->width == 0 || desc->height == 0 ||
        desc->channels < 3 || desc->channels > 4 ||
        desc->colorspace > 1 ||
        header_magic != QOI_MAGIC ||
        desc->height >= QOI_PIXELS_MAX / desc->width
    ) {
        return NULL;
    }

    if (channels == 0) {
        channels = desc->channels;
    }

    px_len = desc->width * desc->height * channels;
    /* one byte of slack so every pixel (RGB or RGBA) can be written with a
       single 4-byte store; for 3-channel output the 4th byte is overwritten
       by the next pixel and the final one lands in the slack byte */
    pixels = (unsigned char *) QOI_MALLOC(px_len + 1);
    if (!pixels) {
        return NULL;
    }

    QOI_ZEROARR(index);
    px.rgba.r = 0;
    px.rgba.g = 0;
    px.rgba.b = 0;
    px.rgba.a = 255;

    chunks_len = size - (int)sizeof(qoi_padding);
    for (px_pos = 0; px_pos < px_len; ) {
        if (p < chunks_len) {
            int b1 = bytes[p++];

            /* 4-way dispatch on the 2-bit tag; RGB/RGBA share the RUN tag
               bits, so that arm distinguishes them before assuming RUN.

               The palette re-store happens only in the arms that produce a
               new pixel value. Every palette entry satisfies the invariant
               hash(entry) == slot (both coders only ever store a value at
               its own hash), so after INDEX the store would be a no-op, and
               after RUN px is unchanged and already stored. */
            switch (b1 >> 6) {
            case 2: { /* QOI_OP_LUMA: Decode Scaled Luma Ranges */
                int b2 = bytes[p++];
                int encoded_dg = (b1 & 0x3f) - 32;
                int decoded_dg = encoded_dg << 1;

                px.rgba.r += decoded_dg - 8 + ((b2 >> 4) & 0x0f);
                px.rgba.g += decoded_dg;
                px.rgba.b += decoded_dg - 8 +  (b2       & 0x0f);
                index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
                break;
            }
            case 1: { /* QOI_OP_LUMA1: exact green delta + shared r/b correction */
                int dg1 = ((b1 >> 2) & 0x0f) - 8;
                int c1  = ( b1       & 0x03) - 2;
                px.rgba.r += dg1 + c1;
                px.rgba.g += dg1;
                px.rgba.b += dg1 + c1;
                index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
                break;
            }
            case 0: /* QOI_OP_INDEX */
                px = index[b1];
                break;
            default: /* QOI_OP_RUN / QOI_OP_RGB / QOI_OP_RGBA */
                if (b1 == QOI_OP_RGB) {
                    px.rgba.r = bytes[p++];
                    px.rgba.g = bytes[p++];
                    px.rgba.b = bytes[p++];
                    index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
                }
                else if (b1 == QOI_OP_RGBA) {
                    px.rgba.r = bytes[p++];
                    px.rgba.g = bytes[p++];
                    px.rgba.b = bytes[p++];
                    px.rgba.a = bytes[p++];
                    index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
                }
                else {
                    run = (b1 & 0x3f);
                }
                break;
            }
        }

        memcpy(pixels + px_pos, &px, 4);
        px_pos += channels;

        /* burst-write the rest of a run without re-entering the dispatch */
        for (; run > 0 && px_pos < px_len; run--) {
            memcpy(pixels + px_pos, &px, 4);
            px_pos += channels;
        }
    }

    return pixels;
}

#ifndef QOI_NO_STDIO
#include <stdio.h>

int qoi_write(const char *filename, const void *data, const qoi_desc *desc) {
    FILE *f = fopen(filename, "wb");
    int size, err;
    void *encoded;

    if (!f) {
        return 0;
    }

    encoded = qoi_encode(data, desc, &size);
    if (!encoded) {
        fclose(f);
        return 0;
    }

    fwrite(encoded, 1, size, f);
    fflush(f);
    err = ferror(f);
    fclose(f);

    QOI_FREE(encoded);
    return err ? 0 : size;
}

void *qoi_read(const char *filename, qoi_desc *desc, int channels) {
    FILE *f = fopen(filename, "rb");
    int size, bytes_read;
    void *pixels, *data;

    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    data = QOI_MALLOC(size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    bytes_read = fread(data, 1, size, f);
    fclose(f);
    pixels = (bytes_read != size) ? NULL : qoi_decode(data, bytes_read, desc, channels);
    QOI_FREE(data);
    return pixels;
}

#endif /* QOI_NO_STDIO */
#endif /* QOI_IMPLEMENTATION */
