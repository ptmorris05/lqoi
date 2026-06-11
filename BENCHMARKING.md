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

Overall PSNR **47.06 dB**, LQOI **74.3%** of strictly-lossless QOI size (1.35×
smaller). Speed vs lossless QOI: **decode +6%** (311 vs 293 Mpx/s), **encode
−1%** (214 vs 217 Mpx/s). Max perceptual error **10/10** (the run-continuation
budget) with **0** pixels exceeding it and alpha exact — the encode→decode
loop is verified correct. See `results/RESULTS.md` for the full table.

The run-hysteresis dial (`-DQOI_RUN_CONT_T=N`, encoder-only, all values decode
with the same decoder) measured on the same setup:

| `QOI_RUN_CONT_T` | size vs QOI | PSNR | encode | decode |
|---|---|---|---|---|
| 6 (no hysteresis) | 80.5% | 49.15 dB | 196 Mpx/s | 274 Mpx/s |
| 8 | 77.3% | 48.20 dB | 202 Mpx/s | 297 Mpx/s |
| **10 (default)** | **74.3%** | **47.06 dB** | **214 Mpx/s** | **311 Mpx/s** |

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

> The throughput cost of the richer chunk mix was diagnosed as branch entropy,
> which led to the run hysteresis (start ≤ `QOI_RUN_START_T`, continue within
> the looser `QOI_RUN_CONT_T`): longer runs mean
> fewer chunks and longer same-chunk stretches, recovering decode to lossless-
> QOI speed while *also* shrinking files ~4%. The decoder additionally
> burst-writes runs, emits pixels as single 4-byte stores, and skips palette
> re-stores that the invariant hash(entry)==slot proves are no-ops (this also
> fixed a latent seed-pixel palette desync on streams that begin with a run).
> A "sticky LUMA1" variant (budget +1 inside unbroken LUMA1 stretches) was
> evaluated and rejected: the loop-carried budget state serialized the encoder
> (−11% encode) for only −1.8% size.
