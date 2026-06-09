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

Overall PSNR **48.86 dB**, LQOI **88.6%** of strictly-lossless QOI size. Speed
vs lossless QOI: **decode ~+7%** (326 vs 305 Mpx/s), **encode ~−7%** (203 vs
220 Mpx/s) — the smaller stream decodes faster, and channel-specialized encode
loops keep the per-pixel perceptual checks cheap enough to stay within ~7% on
encode. Max perceptual error **6/6** with **0** pixels exceeding the budget and
alpha exact — the encode→decode loop is verified correct. See
`results/RESULTS.md` for the full table.

> Note: the comprehensive benchmark's correctness gate caught a real
> `QOI_OP_LUMA` green-channel wraparound bug (a 1-level quantization error at the
> channel floor wrapped `0 → 255`), which has been fixed in `qoi.h`. The fix
> raised the green-channel PSNR from 44.6 → 49.8 dB at no size cost.
