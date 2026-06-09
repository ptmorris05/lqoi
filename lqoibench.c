/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT

LQOI comprehensive lossy benchmark
==================================

LQOI is a *lossy* fork of QOI. The original qoibench.c verifies the codec with
an exact `memcmp` of the decoded pixels against the source, which by definition
aborts on any lossy codec. This tool replaces that with an assessment suited to
a lossy format:

  Correctness (encode -> decode loop)
    - Every image is round-tripped (encode then decode) and the reconstruction
      is validated against the codec's own perceptual contract:
        * alpha must be reconstructed EXACTLY (LQOI mechanism #1), and
        * the per-pixel green-weighted Manhattan error
              2*|dG| + |dR| + |dB|
          must stay within the encoder's threshold.
      A decoder/encoder state mismatch (the failure mode LQOI mechanism #4
      "Base Pixel Hash Injection" exists to prevent) would blow far past this
      bound, so the check is a real correctness gate, not a formality.
    - Idempotency: decoding, re-encoding and decoding again should reproduce the
      same pixels. A lossy codec that keeps re-quantizing an already-quantized
      image would drift; we measure that drift.

  Quantitative error
    - PSNR (overall and per R/G/B channel), RMSE, MAE, max absolute error,
      fraction of pixels modified, and the distribution of the green-weighted
      perceptual error the encoder actually optimizes for.

  Compression
    - LQOI size vs raw, vs the source PNG, and vs strictly-lossless QOI
      (baseline compiled from upstream phoboslab/qoi; see run_benchmark.sh).

  Throughput
    - encode / decode megapixels-per-second, warmup + averaged over N runs.

  Qualitative
    - For each image we write the LQOI reconstruction and an amplified
      difference image into results/ so the error can be inspected by eye.

Requires "stb_image.h" and "stb_image_write.h" (fetched into vendor/ by
run_benchmark.sh). Build via `make lqoibench` or run_benchmark.sh.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <stdint.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define QOI_IMPLEMENTATION
#include "qoi.h"

/* Strictly-lossless QOI baseline. Compiled from the upstream reference
   implementation as a separate translation unit (vendor/qoi_ref.c) with its
   public symbols renamed. Used both as a size baseline and as a head-to-head
   encode/decode speed reference. Guarded so this tool still builds without it. */
#ifdef LQOI_WITH_REF
void *qoiref_encode(const void *data, const qoi_desc *desc, int *out_len);
void *qoiref_decode(const void *data, int size, qoi_desc *desc, int channels);
#endif

/* The perceptual error budget the LQOI encoder enforces (must mirror qoi.h).
   Green-weighted Manhattan: 2*|dG| + |dR| + |dB|. Runs and lossy-index snaps
   keep each emitted pixel within this distance of its reference, so a correct
   decoder must reproduce every pixel within this bound. LUMA only perturbs
   green by the dropped low bit, which is well inside it. */
#define LQOI_PERC_THRESHOLD 6

/* -----------------------------------------------------------------------------
   High resolution timer (from the original qoibench.c) */

#if defined(__APPLE__)
	#include <mach/mach_time.h>
#elif defined(__linux)
	#include <time.h>
	#ifdef CLOCK_MONOTONIC
		#define CLOCKID CLOCK_MONOTONIC
	#else
		#define CLOCKID CLOCK_REALTIME
	#endif
#elif defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#endif

static uint64_t ns() {
	static uint64_t is_init = 0;
#if defined(__APPLE__)
	static mach_timebase_info_data_t info;
	if (0 == is_init) { mach_timebase_info(&info); is_init = 1; }
	uint64_t now = mach_absolute_time();
	now *= info.numer;
	now /= info.denom;
	return now;
#elif defined(__linux)
	static struct timespec linux_rate;
	if (0 == is_init) { clock_getres(CLOCKID, &linux_rate); is_init = 1; }
	struct timespec spec;
	clock_gettime(CLOCKID, &spec);
	return (uint64_t)(spec.tv_sec * 1.0e9 + spec.tv_nsec);
#elif defined(_WIN32)
	static LARGE_INTEGER win_frequency;
	if (0 == is_init) { QueryPerformanceFrequency(&win_frequency); is_init = 1; }
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (uint64_t)((1e9 * now.QuadPart) / win_frequency.QuadPart);
#endif
}

#define ERROR(...) do { \
	printf("abort: " __VA_ARGS__); printf("\n"); exit(1); \
} while (0)

#define BENCHMARK_FN(NOWARMUP, RUNS, AVG_TIME, ...) \
	do { \
		uint64_t time = 0; \
		for (int _i = NOWARMUP; _i <= RUNS; _i++) { \
			uint64_t t0 = ns(); \
			__VA_ARGS__ \
			uint64_t t1 = ns(); \
			if (_i > 0) { time += t1 - t0; } \
		} \
		AVG_TIME = time / RUNS; \
	} while (0)

/* -----------------------------------------------------------------------------
   Per-image metrics */

typedef struct {
	char  name[256];
	int   w, h, channels;

	/* sizes (bytes) */
	uint64_t raw_size;     /* w*h*channels */
	uint64_t png_size;     /* source .png on disk */
	uint64_t qoi_size;     /* strictly-lossless QOI (0 if baseline absent) */
	uint64_t lqoi_size;    /* this codec */

	/* throughput (ns, averaged) */
	uint64_t enc_time;       /* LQOI encode */
	uint64_t dec_time;       /* LQOI decode */
	uint64_t qoi_enc_time;   /* lossless QOI encode (0 if baseline absent) */
	uint64_t qoi_dec_time;   /* lossless QOI decode (0 if baseline absent) */

	/* error (LQOI reconstruction vs source) */
	double mse_total;      /* mean squared error over R,G,B */
	double mse_ch[3];      /* per channel */
	double mae;            /* mean abs error over R,G,B */
	int    max_abs;        /* worst single-channel abs error */
	uint64_t modified_px;  /* pixels where any channel changed */
	uint64_t total_px;

	/* perceptual (the encoder's own metric) */
	double   gwm_sum;      /* sum of green-weighted Manhattan over all px */
	int      gwm_max;      /* worst per-pixel green-weighted Manhattan */
	uint64_t gwm_over;     /* pixels exceeding LQOI_PERC_THRESHOLD */
	uint64_t gwm_hist[8];  /* buckets: 0,1,2,3,4,5,6,>6 */

	/* correctness */
	int      alpha_exact;    /* 1 if every alpha reconstructed exactly */
	uint64_t idem_mismatch;  /* pixels differing on re-encode/decode */
	int      idem_max_abs;   /* worst channel drift on re-encode */
} image_result_t;

static double psnr_from_mse(double mse) {
	if (mse <= 0.0) return INFINITY;
	return 10.0 * log10((255.0 * 255.0) / mse);
}

/* Compute reconstruction error of `dec` vs `orig`, filling the error/perceptual
   fields of `r`. Both buffers are w*h*channels bytes. */
static void measure_error(image_result_t *r,
		const unsigned char *orig, const unsigned char *dec) {
	int ch = r->channels;
	uint64_t n = (uint64_t)r->w * r->h;
	double sq[3] = {0,0,0}, abssum = 0;
	int maxabs = 0;
	uint64_t modified = 0, gwm_over = 0;
	double gwm_sum = 0;
	int gwm_max = 0;
	int alpha_exact = 1;

	for (uint64_t i = 0; i < n; i++) {
		const unsigned char *o = orig + i * ch;
		const unsigned char *d = dec + i * ch;
		int dr = (int)d[0] - o[0];
		int dg = (int)d[1] - o[1];
		int db = (int)d[2] - o[2];
		int adr = abs(dr), adg = abs(dg), adb = abs(db);

		sq[0] += (double)dr * dr;
		sq[1] += (double)dg * dg;
		sq[2] += (double)db * db;
		abssum += adr + adg + adb;
		if (adr > maxabs) maxabs = adr;
		if (adg > maxabs) maxabs = adg;
		if (adb > maxabs) maxabs = adb;

		int changed = (adr | adg | adb);

		int gwm = 2 * adg + adr + adb;
		if (ch == 4) {
			int ada = abs((int)d[3] - o[3]);
			if (ada != 0) { alpha_exact = 0; changed = 1; }
		}
		gwm_sum += gwm;
		if (gwm > gwm_max) gwm_max = gwm;
		if (gwm > LQOI_PERC_THRESHOLD) gwm_over++;
		r->gwm_hist[gwm > 7 ? 7 : gwm]++;

		if (changed) modified++;
	}

	r->total_px = n;
	r->mse_ch[0] = sq[0] / n;
	r->mse_ch[1] = sq[1] / n;
	r->mse_ch[2] = sq[2] / n;
	r->mse_total = (sq[0] + sq[1] + sq[2]) / (n * 3.0);
	r->mae = abssum / (n * 3.0);
	r->max_abs = maxabs;
	r->modified_px = modified;
	r->gwm_sum = gwm_sum;
	r->gwm_max = gwm_max;
	r->gwm_over = gwm_over;
	r->alpha_exact = alpha_exact;
}

/* Write the reconstruction and an amplified difference image for visual
   inspection. The diff is |orig-dec| * AMP, clamped, so subtle error shows. */
#define DIFF_AMP 12
static void write_qualitative(const char *outdir, const image_result_t *r,
		const unsigned char *orig, const unsigned char *dec) {
	int ch = r->channels, w = r->w, h = r->h;
	uint64_t n = (uint64_t)w * h;
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s.lqoi_decoded.png", outdir, r->name);
	stbi_write_png(path, w, h, ch, dec, w * ch);

	unsigned char *diff = malloc(n * 3);
	if (diff) {
		for (uint64_t i = 0; i < n; i++) {
			for (int c = 0; c < 3; c++) {
				int e = abs((int)dec[i*ch+c] - orig[i*ch+c]) * DIFF_AMP;
				diff[i*3+c] = e > 255 ? 255 : e;
			}
		}
		snprintf(path, sizeof(path), "%s/%s.diff_x%d.png", outdir, r->name, DIFF_AMP);
		stbi_write_png(path, w, h, 3, diff, w * 3);
		free(diff);
	}
}

/* -----------------------------------------------------------------------------
   options */

static int opt_runs = 5;
static int opt_nowarmup = 0;
static int opt_noqual = 0;
static char outdir[512] = "results";

/* -----------------------------------------------------------------------------
   benchmark one image */

static int benchmark_image(const char *path, const char *display_name,
		image_result_t *r) {
	int w, h, in_channels;
	if (!stbi_info(path, &w, &h, &in_channels)) {
		printf("  ! skip (not a readable image): %s\n", path);
		return 0;
	}
	int channels = (in_channels == 3) ? 3 : 4;

	unsigned char *pixels = stbi_load(path, &w, &h, NULL, channels);
	if (!pixels) {
		printf("  ! skip (decode failed): %s\n", path);
		return 0;
	}

	memset(r, 0, sizeof(*r));
	snprintf(r->name, sizeof(r->name), "%s", display_name);
	r->w = w; r->h = h; r->channels = channels;
	r->raw_size = (uint64_t)w * h * channels;

	/* source PNG size on disk */
	FILE *f = fopen(path, "rb");
	if (f) { fseek(f, 0, SEEK_END); r->png_size = ftell(f); fclose(f); }

	qoi_desc desc = { .width = w, .height = h,
		.channels = channels, .colorspace = QOI_SRGB };

	/* ---- LQOI encode (timed) ---- */
	int lqoi_size = 0;
	void *enc = NULL;
	BENCHMARK_FN(opt_nowarmup, opt_runs, r->enc_time, {
		if (enc) free(enc);
		enc = qoi_encode(pixels, &desc, &lqoi_size);
	});
	if (!enc) { free(pixels); ERROR("LQOI encode failed for %s", path); }
	r->lqoi_size = lqoi_size;

	/* ---- LQOI decode (timed) ---- */
	unsigned char *dec = NULL;
	BENCHMARK_FN(opt_nowarmup, opt_runs, r->dec_time, {
		if (dec) free(dec);
		qoi_desc dd;
		dec = qoi_decode(enc, lqoi_size, &dd, channels);
	});
	if (!dec) { free(enc); free(pixels); ERROR("LQOI decode failed for %s", path); }

	/* ---- error metrics ---- */
	measure_error(r, pixels, dec);

	/* ---- idempotency: re-encode the decoded image, decode again ---- */
	int size2 = 0;
	void *enc2 = qoi_encode(dec, &desc, &size2);
	if (enc2) {
		qoi_desc dd2;
		unsigned char *dec2 = qoi_decode(enc2, size2, &dd2, channels);
		if (dec2) {
			uint64_t mism = 0; int mx = 0;
			for (uint64_t i = 0; i < (uint64_t)w*h; i++) {
				int diff = 0;
				for (int c = 0; c < channels; c++) {
					int e = abs((int)dec2[i*channels+c] - dec[i*channels+c]);
					if (e) { diff = 1; if (e > mx) mx = e; }
				}
				if (diff) mism++;
			}
			r->idem_mismatch = mism;
			r->idem_max_abs = mx;
			free(dec2);
		}
		free(enc2);
	}

	/* ---- lossless QOI baseline: size + head-to-head encode/decode speed ---- */
#ifdef LQOI_WITH_REF
	{
		int qsize = 0;
		void *qref = NULL;
		BENCHMARK_FN(opt_nowarmup, opt_runs, r->qoi_enc_time, {
			if (qref) free(qref);
			qref = qoiref_encode(pixels, &desc, &qsize);
		});
		if (qref) {
			r->qoi_size = qsize;
			unsigned char *qdec = NULL;
			BENCHMARK_FN(opt_nowarmup, opt_runs, r->qoi_dec_time, {
				if (qdec) free(qdec);
				qoi_desc qd;
				qdec = qoiref_decode(qref, qsize, &qd, channels);
			});
			if (qdec) free(qdec);
			free(qref);
		}
	}
#endif

	/* ---- qualitative artifacts ---- */
	if (!opt_noqual) write_qualitative(outdir, r, pixels, dec);

	free(enc);
	free(dec);
	free(pixels);
	return 1;
}

/* -----------------------------------------------------------------------------
   reporting */

static double mpps(uint64_t px, uint64_t ns_time) {
	return ns_time > 0 ? (double)px / ((double)ns_time / 1000.0) : 0.0;
}

static void print_image_row(FILE *out, const image_result_t *r) {
	double comp_raw = 100.0 * r->lqoi_size / (double)r->raw_size;
	double comp_png = r->png_size ? 100.0 * r->lqoi_size / (double)r->png_size : 0;
	double comp_qoi = r->qoi_size ? 100.0 * r->lqoi_size / (double)r->qoi_size : 0;
	double psnr = psnr_from_mse(r->mse_total);
	uint64_t px = (uint64_t)r->w * r->h;

	fprintf(out,
		"| %-14s | %4dx%-4d | %6.2f | %6.1f | %6.1f | %8.1f | %8.1f | %5.1f | %4.2f | %3d | %5.1f%% | %4.1f | %3d |\n",
		r->name, r->w, r->h,
		psnr,
		mpps(px, r->dec_time), mpps(px, r->enc_time),
		comp_raw, comp_png, comp_qoi == 0 ? 0 : comp_qoi,
		r->mae, r->max_abs,
		100.0 * r->modified_px / (double)r->total_px,
		r->gwm_sum / (double)r->total_px,
		r->gwm_max);
}

static void print_header(FILE *out) {
	fprintf(out,
		"| image          | size      | PSNR dB|dec mpps|enc mpps| %% raw  | %% png  |%% qoi| MAE  |max| %%px mod| gwm  |gwx|\n");
	fprintf(out,
		"|----------------|-----------|--------|--------|--------|--------|--------|-----|------|---|--------|------|---|\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: lqoibench <directory> [options]\n");
		printf("Options:\n");
		printf("    --runs N ....... timed runs per image (default 5)\n");
		printf("    --nowarmup ..... skip the discarded warmup run\n");
		printf("    --noqual ....... don't write decoded/diff PNGs\n");
		printf("    --outdir DIR ... output directory (default results)\n");
		return 1;
	}
	const char *dir_path = argv[1];
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--runs") && i+1 < argc) opt_runs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--nowarmup")) opt_nowarmup = 1;
		else if (!strcmp(argv[i], "--noqual")) opt_noqual = 1;
		else if (!strcmp(argv[i], "--outdir") && i+1 < argc)
			snprintf(outdir, sizeof(outdir), "%s", argv[++i]);
		else ERROR("Unknown option %s", argv[i]);
	}
	if (opt_runs <= 0) ERROR("Invalid --runs");

	DIR *dir = opendir(dir_path);
	if (!dir) ERROR("Couldn't open directory %s", dir_path);

	/* collect image file names (png/jpg/bmp/tga that stb can read) */
	char (*names)[256] = NULL;
	int n_files = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(dir)) != NULL) {
		const char *nm = e->d_name;
		size_t L = strlen(nm);
		if (L < 4) continue;
		const char *ext = nm + L - 4;
		if (strcasecmp(ext, ".png") && strcasecmp(ext, ".jpg") &&
		    strcasecmp(ext, ".bmp") && strcasecmp(ext, ".tga") &&
		    strcasecmp(nm + (L>=5?L-5:0), ".jpeg")) continue;
		if (n_files == cap) {
			cap = cap ? cap*2 : 16;
			names = realloc(names, cap * sizeof(*names));
		}
		snprintf(names[n_files++], 256, "%s", nm);
	}
	closedir(dir);
	if (n_files == 0) ERROR("No images found in %s", dir_path);

	/* sort names for stable output */
	for (int i = 0; i < n_files; i++)
		for (int j = i+1; j < n_files; j++)
			if (strcmp(names[i], names[j]) > 0) {
				char t[256]; strcpy(t, names[i]);
				strcpy(names[i], names[j]); strcpy(names[j], t);
			}

	/* ensure output dir exists (best effort) */
	{ char cmd[1100]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", outdir); int _=system(cmd); (void)_; }

	image_result_t *results = calloc(n_files, sizeof(image_result_t));
	int n_ok = 0;

	printf("# LQOI comprehensive benchmark\n\n");
	printf("dataset: %s   images: %d   runs: %d%s   baseline: %s\n\n",
		dir_path, n_files, opt_runs, opt_nowarmup ? " (no warmup)" : "",
#ifdef LQOI_WITH_REF
		"lossless QOI + PNG"
#else
		"PNG only"
#endif
	);

	for (int i = 0; i < n_files; i++) {
		char path[1100];
		snprintf(path, sizeof(path), "%s/%s", dir_path, names[i]);
		char disp[256]; snprintf(disp, sizeof(disp), "%s", names[i]);
		/* strip extension for display */
		char *dot = strrchr(disp, '.'); if (dot) *dot = 0;
		printf("[%2d/%2d] %s\n", i+1, n_files, names[i]);
		fflush(stdout);
		if (benchmark_image(path, disp, &results[n_ok])) n_ok++;
	}

	if (n_ok == 0) ERROR("No images successfully processed");

	/* ---- aggregate ---- */
	image_result_t tot; memset(&tot, 0, sizeof(tot));
	double agg_mse = 0, agg_mse_ch[3] = {0,0,0}, agg_mae = 0, agg_gwm = 0;
	uint64_t agg_px = 0;
	int correctness_fail = 0;
	for (int i = 0; i < n_ok; i++) {
		image_result_t *r = &results[i];
		uint64_t px = (uint64_t)r->w * r->h;
		tot.raw_size += r->raw_size;
		tot.png_size += r->png_size;
		tot.qoi_size += r->qoi_size;
		tot.lqoi_size += r->lqoi_size;
		tot.enc_time += r->enc_time;
		tot.dec_time += r->dec_time;
		tot.qoi_enc_time += r->qoi_enc_time;
		tot.qoi_dec_time += r->qoi_dec_time;
		tot.modified_px += r->modified_px;
		tot.total_px += r->total_px;
		tot.gwm_over += r->gwm_over;
		tot.idem_mismatch += r->idem_mismatch;
		if (r->gwm_max > tot.gwm_max) tot.gwm_max = r->gwm_max;
		if (r->max_abs > tot.max_abs) tot.max_abs = r->max_abs;
		if (r->idem_max_abs > tot.idem_max_abs) tot.idem_max_abs = r->idem_max_abs;
		for (int b = 0; b < 8; b++) tot.gwm_hist[b] += r->gwm_hist[b];
		/* pixel-weighted error accumulation */
		agg_mse     += r->mse_total * px;
		agg_mse_ch[0] += r->mse_ch[0] * px;
		agg_mse_ch[1] += r->mse_ch[1] * px;
		agg_mse_ch[2] += r->mse_ch[2] * px;
		agg_mae     += r->mae * px;
		agg_gwm     += r->gwm_sum;
		agg_px      += px;
		/* correctness gate */
		if (!r->alpha_exact) { correctness_fail = 1;
			printf("  !! ALPHA NOT EXACT: %s\n", r->name); }
		if (r->gwm_max > LQOI_PERC_THRESHOLD) { correctness_fail = 1;
			printf("  !! perceptual bound exceeded (gwm=%d>%d): %s\n",
				r->gwm_max, LQOI_PERC_THRESHOLD, r->name); }
	}
	double agg_psnr = psnr_from_mse(agg_mse / agg_px);

	/* ---- write CSV ---- */
	char csv_path[600]; snprintf(csv_path, sizeof(csv_path), "%s/results.csv", outdir);
	FILE *csv = fopen(csv_path, "w");
	if (csv) {
		fprintf(csv, "image,w,h,channels,raw_bytes,png_bytes,qoi_lossless_bytes,lqoi_bytes,"
			"lqoi_vs_raw_pct,lqoi_vs_png_pct,lqoi_vs_qoi_pct,"
			"lqoi_enc_mpps,lqoi_dec_mpps,qoi_enc_mpps,qoi_dec_mpps,"
			"psnr_db,psnr_r,psnr_g,psnr_b,rmse,mae,max_abs,pct_px_modified,"
			"mean_gwm,max_gwm,gwm_over_thresh,alpha_exact,idem_mismatch_px,idem_max_abs\n");
		for (int i = 0; i < n_ok; i++) {
			image_result_t *r = &results[i];
			uint64_t px = (uint64_t)r->w * r->h;
			fprintf(csv,
				"%s,%d,%d,%d,%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%d,%.4f,%.4f,%d,%llu,%d,%llu,%d\n",
				r->name, r->w, r->h, r->channels,
				(unsigned long long)r->raw_size, (unsigned long long)r->png_size,
				(unsigned long long)r->qoi_size, (unsigned long long)r->lqoi_size,
				100.0*r->lqoi_size/(double)r->raw_size,
				r->png_size?100.0*r->lqoi_size/(double)r->png_size:0,
				r->qoi_size?100.0*r->lqoi_size/(double)r->qoi_size:0,
				mpps(px, r->enc_time), mpps(px, r->dec_time),
				mpps(px, r->qoi_enc_time), mpps(px, r->qoi_dec_time),
				psnr_from_mse(r->mse_total),
				psnr_from_mse(r->mse_ch[0]), psnr_from_mse(r->mse_ch[1]),
				psnr_from_mse(r->mse_ch[2]),
				sqrt(r->mse_total), r->mae, r->max_abs,
				100.0*r->modified_px/(double)r->total_px,
				r->gwm_sum/(double)r->total_px, r->gwm_max,
				(unsigned long long)r->gwm_over, r->alpha_exact,
				(unsigned long long)r->idem_mismatch, r->idem_max_abs);
		}
		fclose(csv);
	}

	/* ---- write + print markdown ---- */
	char md_path[600]; snprintf(md_path, sizeof(md_path), "%s/RESULTS.md", outdir);
	FILE *md = fopen(md_path, "w");

	FILE *outs[2] = { stdout, md };
	for (int s = 0; s < 2; s++) {
		FILE *o = outs[s];
		if (!o) continue;
		fprintf(o, "\n# LQOI benchmark results\n\n");
		fprintf(o, "Dataset `%s` - %d images, %d timed run(s) per image.\n\n",
			dir_path, n_ok, opt_runs);
		fprintf(o, "Columns: PSNR (higher=better), dec/enc throughput (megapixels/s), "
			"size as %% of raw / source-PNG / lossless-QOI, mean abs error, max channel "
			"error, %% pixels modified, mean & max green-weighted Manhattan error "
			"(the encoder's own perceptual budget, threshold %d).\n\n", LQOI_PERC_THRESHOLD);
		print_header(o);
		for (int i = 0; i < n_ok; i++) print_image_row(o, &results[i]);

		/* totals row */
		double comp_raw = 100.0*tot.lqoi_size/(double)tot.raw_size;
		double comp_png = tot.png_size?100.0*tot.lqoi_size/(double)tot.png_size:0;
		double comp_qoi = tot.qoi_size?100.0*tot.lqoi_size/(double)tot.qoi_size:0;
		fprintf(o,
			"| **TOTAL**      |           | %6.2f | %6.1f | %6.1f | %6.1f | %6.1f | %5.1f | %4.2f | %3d | %5.1f%% | %4.1f | %3d |\n",
			agg_psnr,
			mpps(agg_px, tot.dec_time), mpps(agg_px, tot.enc_time),
			comp_raw, comp_png, comp_qoi,
			agg_mae/agg_px, tot.max_abs,
			100.0*tot.modified_px/(double)tot.total_px,
			agg_gwm/(double)tot.total_px, tot.gwm_max);

		fprintf(o, "\n## Aggregate\n\n");
		fprintf(o, "- **Compression**: LQOI is %.1f%% of raw", comp_raw);
		if (tot.png_size) fprintf(o, ", %.1f%% of source PNG", comp_png);
		if (tot.qoi_size) fprintf(o, ", **%.1f%% of strictly-lossless QOI** (%.2fx smaller)",
			comp_qoi, 100.0/comp_qoi);
		fprintf(o, ".\n");
		fprintf(o, "- **Fidelity**: overall PSNR %.2f dB", agg_psnr);
		fprintf(o, " (R %.2f / G %.2f / B %.2f dB), RMSE %.3f, mean abs err %.3f, worst channel err %d.\n",
			psnr_from_mse(agg_mse_ch[0]/agg_px),
			psnr_from_mse(agg_mse_ch[1]/agg_px),
			psnr_from_mse(agg_mse_ch[2]/agg_px),
			sqrt(agg_mse/agg_px), agg_mae/agg_px, tot.max_abs);
		fprintf(o, "- **Throughput (LQOI)**: %.1f Mpx/s decode, %.1f Mpx/s encode.\n",
			mpps(agg_px, tot.dec_time), mpps(agg_px, tot.enc_time));
		if (tot.qoi_enc_time && tot.qoi_dec_time) {
			double lq_e = mpps(agg_px, tot.enc_time), q_e = mpps(agg_px, tot.qoi_enc_time);
			double lq_d = mpps(agg_px, tot.dec_time), q_d = mpps(agg_px, tot.qoi_dec_time);
			fprintf(o, "- **Throughput (lossless QOI)**: %.1f Mpx/s decode, %.1f Mpx/s encode.\n",
				q_d, q_e);
			fprintf(o, "- **LQOI vs QOI speed**: encode %+.1f%% (%.2fx), decode %+.1f%% (%.2fx) "
				"relative to lossless QOI.\n",
				100.0*(lq_e/q_e - 1.0), lq_e/q_e,
				100.0*(lq_d/q_d - 1.0), lq_d/q_d);
		}
		fprintf(o, "- **Pixels modified**: %.1f%% of all pixels.\n",
			100.0*tot.modified_px/(double)tot.total_px);

		fprintf(o, "\n## Perceptual error distribution (green-weighted Manhattan)\n\n");
		fprintf(o, "| 2|dG|+|dR|+|dB| | pixels | %% |\n|---|---|---|\n");
		const char *lbl[8] = {"0 (exact)","1","2","3","4","5","6",">6"};
		for (int b = 0; b < 8; b++)
			fprintf(o, "| %s | %llu | %.2f%% |\n", lbl[b],
				(unsigned long long)tot.gwm_hist[b],
				100.0*tot.gwm_hist[b]/(double)tot.total_px);

		fprintf(o, "\n## Correctness (encode -> decode loop)\n\n");
		fprintf(o, "- Alpha reconstructed exactly on every image: **%s**\n",
			correctness_fail ? "see warnings above" : "YES");
		fprintf(o, "- Max per-pixel perceptual error across all images: **%d** (budget %d) -> **%s**\n",
			tot.gwm_max, LQOI_PERC_THRESHOLD,
			tot.gwm_max <= LQOI_PERC_THRESHOLD ? "within bound" : "EXCEEDS BOUND");
		fprintf(o, "- Pixels exceeding the perceptual budget: %llu / %llu (%.4f%%)\n",
			(unsigned long long)tot.gwm_over, (unsigned long long)tot.total_px,
			100.0*tot.gwm_over/(double)tot.total_px);
		fprintf(o, "- Idempotency (decode -> re-encode -> decode drift): %llu pixels changed total, max channel drift %d.\n",
			(unsigned long long)tot.idem_mismatch, tot.idem_max_abs);
		fprintf(o, "\n**Verdict: %s**\n",
			(!correctness_fail && tot.gwm_max <= LQOI_PERC_THRESHOLD)
			? "encode/decode loop is correct - every pixel reconstructed within the codec's perceptual contract, alpha exact."
			: "CORRECTNESS PROBLEM - reconstruction violates the codec's stated perceptual contract (see warnings).");
		if (!opt_noqual)
			fprintf(o, "\nQualitative artifacts (reconstruction + amplified diff x%d) written to `%s/`.\n",
				DIFF_AMP, outdir);
	}
	if (md) fclose(md);

	printf("\nWrote %s and %s\n", csv_path, md_path);

	free(results);
	free(names);
	return (!correctness_fail && tot.gwm_max <= LQOI_PERC_THRESHOLD) ? 0 : 2;
}
