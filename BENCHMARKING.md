# LQOI Benchmarking

Because LQOI is **lossy**, the original `qoibench` correctness check (an exact
`memcmp` of the decoded pixels against the source) is meaningless — it aborts on
the first quantized pixel. The tooling here verifies and measures the codec the
way a lossy format should be assessed.

## Quick start

```sh
./run_benchmark.sh
```

This fetches the build-time dependencies and a dataset into a gitignored
`vendor/` and `images/` directory, builds `lqoibench`, runs it on the Kodak
suite, and writes results to `results/`.

To benchmark your own images:

```sh
./run_benchmark.sh path/to/images
```

## What gets measured

`lqoibench` round-trips every image (encode → decode) and reports:

**Correctness (encode → decode loop)**
- **Alpha exactness** — LQOI must reconstruct alpha exactly; any deviation fails.
- **Perceptual bound** — every reconstructed pixel must lie within the encoder's
  own budget: green-weighted Manhattan error `2·|ΔG| + |ΔR| + |ΔB| ≤ 6`. A
  decoder/encoder desync blows far past this, so it is a real correctness gate.
- **Idempotency** — decoding, re-encoding and decoding again should not drift
  beyond the same budget.

**Quantitative error**
- PSNR (overall and per R/G/B channel), RMSE, MAE, max absolute channel error,
  fraction of pixels modified, and the full distribution of the perceptual error
  the encoder optimizes for.

**Compression**
- LQOI size vs raw, vs the source PNG, and vs strictly-lossless QOI (baseline
  compiled from upstream `phoboslab/qoi`, never vendored in this repo).

**Throughput (head-to-head vs lossless QOI)**
- Encode / decode megapixels per second for both LQOI and the lossless QOI
  reference (warmup + averaged), plus the relative speed-up/slow-down.

**Qualitative**
- For each image, `results/<name>.lqoi_decoded.png` (the reconstruction) and
  `results/<name>.diff_x12.png` (absolute error amplified 12× for visual
  inspection of where error concentrates).

Outputs: `results/RESULTS.md` (human summary), `results/results.csv`
(per-image machine-readable), plus the per-image PNGs above.

## Dataset

The default dataset is the **Kodak True Color Suite** (24 photographic images,
768×512) — the canonical benchmark for lossy image-compression quality. It is
representative of LQOI's target domain (digital photography, micro-gradients).

## Tools

| target | purpose |
|---|---|
| `make lbench` / `lqoibench` | comprehensive lossy benchmark (above) |
| `make bench` / `qoibench`   | original benchmark, now with a lossy-aware verify + per-image PSNR |
| `make conv` / `qoiconv`     | PNG ↔ LQOI conversion |

All third-party material (`stb_image*.h`, the upstream reference QOI, build
artifacts, datasets, results) is gitignored under `vendor/`, `images/` and
`results/`.

## Latest results (Kodak, 24 images)

Overall PSNR **49.15 dB**, LQOI **80.5%** of strictly-lossless QOI size (1.24×
smaller). Speed vs lossless QOI: **encode ~−10%** (196 vs 218 Mpx/s), **decode
~−11%** (274 vs 306 Mpx/s). Max perceptual error **6/6** with **0** pixels
exceeding the budget and alpha exact — the encode→decode loop is verified
correct. See `results/RESULTS.md` for the full table.

The `QOI_OP_LUMA1` strength dial (`-DQOI_LUMA1_T=N`, encoder-only, all values
decode with the same decoder) measured on the same setup:

| `QOI_LUMA1_T` | size vs QOI | PSNR | encode |
|---|---|---|---|
| 0 (exact only) | 84.7% | 49.20 dB | 184 Mpx/s |
| **1 (default)** | **80.5%** | **49.15 dB** | **196 Mpx/s** |
| 2 | 76.9% | 48.67 dB | 180 Mpx/s |

History of findings made with this benchmark:

> The correctness gate caught a real `QOI_OP_LUMA` green-channel wraparound bug
> (a 1-level quantization error at the channel floor wrapped `0 → 255`), fixed
> in `qoi.h`; the fix raised green-channel PSNR from 44.6 → 49.8 dB at no size
> cost.

> Throughput measurements drove the encoder's branchless restructuring (mod-256
> unsigned gates, OR-combined range checks, pointer emission, incremental hash
> updates): bitstream-identical, encode 205 → 227 Mpx/s, briefly making LQOI
> encode ~4% *faster* than lossless QOI before that headroom was spent on
> `QOI_OP_LUMA1`.

> The chunk census (LUMA pairs = 68% of stream bytes, `QOI_OP_DIFF` < 1% of
> chunks) motivated repurposing the DIFF tag as the 1-byte `QOI_OP_LUMA1`,
> which cut file size ~9% while *raising* PSNR. Encoder-side, naive early-out
> gating of the new chunk cost 23% encode throughput in branch mispredictions;
> the shipped shallow branchless gate recovers nearly all of it.
