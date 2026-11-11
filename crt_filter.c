/* crt_filter.c - CPU port of crt-royale-fast (see crt_filter.h).
 *
 * Pass map (slang file -> function here), default parameters of
 * crt-royale-fast-rgb-aperture.slangp / bind-shader-params.h:
 *   first-pass-linearize-crt-gamma        s_lin8 (crt_gamma 2.4, 240p: no interlace)
 *   scanlines-vertical-interlacing        vpass (generalized gaussian, beta 2,
 *                                         sigma 0.055..0.2 ^0.38, 3-sample AA,
 *                                         convergence y r/g/b +0.05/-0.05/+0.05,
 *                                         levels_autodim_temp 0.5)
 *   mask-resize-vertical/horizontal       build_mask (grille texture -> 3 px triads)
 *   scanlines-horizontal-apply-mask       mask_row (quintic horizontal filter, mask,
 *                                         curvature + corner when curved)
 *   brightpass                            mask_row (levels_contrast 0.671875,
 *                                         bloom_underestimate_levels 1, excess 0)
 *   bloom-vertical / horizontal-reconstitute  rpass (sigma from triad size)
 * mask_row..rpass are fused per output row (a 9-row ring per thread) so no
 * full-screen intermediate is written.
 *   lcd_gamma 2.4 out.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")
#endif
#include "crt_filter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef CYC_WITH_SDL
#include <SDL.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- crt-royale-fast parameters (defaults) ---- */
#define CRT_GAMMA        2.4
#define LCD_GAMMA        2.4
#define LEVELS_CONTRAST  0.671875f
#define AUTODIM          0.5f
#define BEAM_MIN_SIGMA   0.055
#define BEAM_MAX_SIGMA   0.2
#define BEAM_SPOT_POWER  0.38
#define BLOOM_UNDER      1.0f
#define TRIAD_SIZE       3          /* mask_triad_size_desired */
#define MASK_AVG         (53.0f / 255.0f)   /* mask_grille15_avg_color */
#define GEOM_R           4.0
#define GEOM_D           1.5
#define GEOM_ASPECT      0.75       /* GLSL comma-expression quirk: vec2(0.75) */
#define GEOM_CORNERSIZE  0.006
#define GEOM_CORNERSMOOTH 400.0
static const float k_conv_y[3] = { 0.05f, -0.05f, 0.05f };

/* One 8-px period of TileableLinearApertureGrille15Wide8And5d5SpacingResizeTo64BGR.png
 * (column averages, linear 0..255; the 64-px tile is 8 identical triads). */
static const uint8_t k_grille[8][3] = {
    {0, 0, 109}, {8, 0, 255}, {0, 9, 80}, {7, 210, 0}, {0, 210, 7}, {80, 9, 0}, {255, 0, 8}, {108, 0, 0},
};

#define OUT_STEPS 4096

static float   s_lin8[256];
static uint8_t s_out[OUT_STEPS + 1];
#define BR 4   /* bloom kernel radius (sigma ~1.56 px: the 5th tap is 0.6%) */
static float   s_undim, s_center_weight, s_bloom_w[BR + 1];   /* normalized */
static float   s_mask[TRIAD_SIZE][3];
static bool    s_init = false;

/* beam contribution table: signed distance d in [D_MIN, D_MAX] by 1/D_RES, x 256 input levels */
#define D_MIN  (-1.0)
#define D_MAX  3.0
#define D_RES  128
#define D_N    ((int)((D_MAX - D_MIN) * D_RES) + 1)
static float  *s_contrib = NULL;    /* [D_N][256] */
static double  s_contrib_ph = -1;

static double beam_contrib(double d, double color, double ph) {
    if (color <= 0) return 0;
    double sigma = BEAM_MIN_SIGMA + (BEAM_MAX_SIGMA - BEAM_MIN_SIGMA) * pow(color, BEAM_SPOT_POWER);
    double alpha = sqrt(2.0) * sigma;
    /* beta = 2: scale = color * beta * 0.5 / alpha / gamma(1/2) */
    double scale = color / (alpha * sqrt(M_PI));
    double off = ph / 3.0, d2 = d + off, d3 = fabs(d - off);
    double w = exp(-(d / alpha) * (d / alpha)) + exp(-(d2 / alpha) * (d2 / alpha)) + exp(-(d3 / alpha) * (d3 / alpha));
    return scale / 3.0 * w;
}

static void build_contrib(double ph) {
    if (!s_contrib) s_contrib = malloc(sizeof(float) * D_N * 256);
    for (int i = 0; i < D_N; i++) {
        double d = D_MIN + (double)i / D_RES;
        for (int c = 0; c < 256; c++) s_contrib[i * 256 + c] = (float)(beam_contrib(d, s_lin8[c], ph) * AUTODIM);
    }
    s_contrib_ph = ph;
}

static inline const float *contrib_row(float d) {
    int i = (int)lrintf((d - (float)D_MIN) * D_RES);
    if (i < 0) i = 0;
    if (i >= D_N) i = D_N - 1;
    return s_contrib + (size_t)i * 256;
}

/* Lanczos-2 resize of the 8-px grille period to TRIAD_SIZE px (the
 * mask-resize passes; a downscale, so the kernel is widened by 8/TRIAD). */
static void build_mask(void) {
    double scale = 8.0 / TRIAD_SIZE;
    for (int x = 0; x < TRIAD_SIZE; x++) {
        double center = (x + 0.5) * scale - 0.5, acc[3] = {0, 0, 0}, wsum = 0;
        for (int k = -8; k <= 16; k++) {
            double t = (k - center) / scale;
            if (fabs(t) >= 2.0) continue;
            double w = t == 0 ? 1.0 : (sin(M_PI * t) / (M_PI * t)) * (sin(M_PI * t / 2) / (M_PI * t / 2));
            int s = ((k % 8) + 8) % 8;
            for (int c = 0; c < 3; c++) acc[c] += w * k_grille[s][c] / 255.0;
            wsum += w;
        }
        for (int c = 0; c < 3; c++) {
            double v = acc[c] / wsum;
            s_mask[x][c] = (float)(v < 0 ? 0 : v);
        }
    }
}

static void init_once(void) {
    for (int i = 0; i < 256; i++) s_lin8[i] = (float)pow(i / 255.0, CRT_GAMMA);
    for (int i = 0; i <= OUT_STEPS; i++) s_out[i] = (uint8_t)lrint(pow((double)i / OUT_STEPS, 1.0 / LCD_GAMMA) * 255.0);
    build_mask();
    s_undim = (1.0f / AUTODIM) * (1.0f / MASK_AVG) * LEVELS_CONTRAST;
    /* bloom sigma: get_min_sigma_to_blur_triad(tile_x / triads_per_tile, 1/256) */
    double triad = TRIAD_SIZE, thresh = 1.0 / 256.0;
    double sigma = -0.05168 + 0.6113 * triad - 1.122 * triad * sqrt(0.000416 + thresh);
    double wsi = fmin(exp(exp(0.348348412457428 / (sigma - 0.0860587260734721))), 0.399334576340352 / sigma);
    s_center_weight = (float)wsi;
    double w[BR + 1], sum = 0;
    for (int k = 0; k <= BR; k++) { w[k] = exp(-(double)(k * k) / (2 * sigma * sigma)); sum += k ? 2 * w[k] : w[k]; }
    for (int k = 0; k <= BR; k++) s_bloom_w[k] = (float)(w[k] / sum);
    s_init = true;
}

/* ---- crt-geom curvature (tilt 0, overscan 100%, center 0) ---- */
static double s_stretch_x, s_stretch_y, s_stretch_z;

static void bkwtrans(double x, double y, double *ox, double *oy) {
    double d2 = GEOM_D * GEOM_D;
    double A = x * x + y * y + d2;
    double B = 2.0 * (GEOM_R * (0.0 - GEOM_D) - d2);
    double C = d2 + 2.0 * GEOM_R * GEOM_D;
    double disc = B * B - 4.0 * A * C;
    double c = (-B - sqrt(disc > 0 ? disc : 0)) / (2.0 * A);
    double px = c * x / GEOM_R, py = c * y / GEOM_R;
    double pp = px * px + py * py;
    double a = sqrt(pp < 1 ? 1 - pp : 0);
    double r = fmax(fabs(GEOM_R * acos(a)), 1e-5);
    double s = r / sin(r / GEOM_R);
    *ox = px * s;
    *oy = py * s;
}

static void fwtrans(double x, double y, double *ox, double *oy) {
    double r = fmax(sqrt(x * x + y * y), 1e-5);
    double k = sin(r / GEOM_R) / r;
    x *= k; y *= k;
    double xx = 1.0 - cos(r / GEOM_R);
    double D = GEOM_D / GEOM_R + xx;
    *ox = GEOM_D * x / D;
    *oy = GEOM_D * y / D;
}

static void geom_init(void) {
    double cx, cy;
    bkwtrans(0, 0, &cx, &cy);
    double a = 0.5 * GEOM_ASPECT, t;
    double lo_x, lo_y, hi_x, hi_y;
    fwtrans(-a, cy, &lo_x, &t);
    fwtrans(cx, -a, &t, &lo_y);
    fwtrans(+a, cy, &hi_x, &t);
    fwtrans(cx, +a, &t, &hi_y);
    lo_x /= GEOM_ASPECT; lo_y /= GEOM_ASPECT; hi_x /= GEOM_ASPECT; hi_y /= GEOM_ASPECT;
    s_stretch_x = (hi_x + lo_x) * GEOM_ASPECT * 0.5;
    s_stretch_y = (hi_y + lo_y) * GEOM_ASPECT * 0.5;
    s_stretch_z = fmax(hi_x - lo_x, hi_y - lo_y);
}

/* screen uv (0..1) -> picture uv; cval = corner coverage */
static void geom_transform(double u, double v, double *ou, double *ov, double *cval) {
    double x = (u - 0.5) * GEOM_ASPECT * s_stretch_z + s_stretch_x;
    double y = (v - 0.5) * GEOM_ASPECT * s_stretch_z + s_stretch_y;
    double bx, by;
    bkwtrans(x, y, &bx, &by);
    *ou = bx / GEOM_ASPECT + 0.5;
    *ov = by / GEOM_ASPECT + 0.5;
    if (cval) {
        double qx = fmin(*ou, 1 - *ou) * GEOM_ASPECT, qy = fmin(*ov, 1 - *ov) * GEOM_ASPECT;
        double cd = GEOM_CORNERSIZE;
        qx = cd - fmin(qx, cd);
        qy = cd - fmin(qy, cd);
        double dist = sqrt(qx * qx + qy * qy);
        double cv = (cd - dist) * GEOM_CORNERSMOOTH;
        *cval = cv < 0 ? 0 : cv > 1 ? 1 : cv;
    }
}

/* ---- geometry cache ---- */
typedef struct {
    int dw, dh, sw, sh;
    bool curved;
    int scale;             /* s_fit_scale it was built for */
    int cx0, cy0, cw, ch;
    int16_t *hi;           /* [cw] flat: horizontal texel */
    float   *hw;           /* [cw] flat: quintic weight toward hi+1 */
    int16_t *ox;           /* [cw] flat: ORIG column */
    uint16_t *mu, *mv;     /* [cw*ch] curved: picture uv * 65534, 0xFFFF = off */
    uint8_t  *mc;          /* [cw*ch] curved: corner 0..255 */
} Geo;
static Geo s_geo;

/* 0 = fill the output (fractional fit); k > 0 = exactly k x the source,
 * centered, when that fits (else fill). Set by crt_set_scale. */
static int s_fit_scale = 0;
void crt_set_scale(int k) { s_fit_scale = k > 0 ? k : 0; }
int crt_get_scale(void) { return s_fit_scale; }

static void fit_rect(int dw, int dh, int sw, int sh, int *x0, int *y0, int *w, int *h) {
    double sc = fmin((double)dw / sw, (double)dh / sh);
    if (s_fit_scale > 0 && s_fit_scale <= sc) sc = s_fit_scale;
    *w = (int)(sw * sc + 0.5);
    *h = (int)(sh * sc + 0.5);
    if (*w > dw) *w = dw;
    if (*h > dh) *h = dh;
    *x0 = (dw - *w) / 2;
    *y0 = (dh - *h) / 2;
}

/* sample_single_scanline_horizontal, beam_horiz_filter 0: quintic */
static inline void horiz_tap(float u, int sw, int *i, float *w) {
    float curr = u * sw;
    int ii = (int)floorf(curr - 0.4995f);
    float t = curr - (ii + 0.5f);
    if (t < 0) t = 0; else if (t > 1) t = 1;
    *w = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    if (ii < 0) { ii = 0; *w = 0; }
    if (ii > sw - 2) { ii = sw - 2; if (ii < 0) ii = 0; }
    *i = ii;
}

static void geo_build(int dw, int dh, int sw, int sh, bool curved) {
    Geo *g = &s_geo;
    free(g->hi); free(g->hw); free(g->ox); free(g->mu); free(g->mv); free(g->mc);
    memset(g, 0, sizeof *g);
    g->dw = dw; g->dh = dh; g->sw = sw; g->sh = sh; g->curved = curved; g->scale = s_fit_scale;
    fit_rect(dw, dh, sw, sh, &g->cx0, &g->cy0, &g->cw, &g->ch);
    int cw = g->cw, ch = g->ch;
    g->hi = malloc(sizeof(int16_t) * cw);
    g->hw = malloc(sizeof(float) * cw);
    g->ox = malloc(sizeof(int16_t) * cw);
    for (int x = 0; x < cw; x++) {
        int i; float w;
        float u = (x + 0.5f) / cw;
        horiz_tap(u, sw, &i, &w);
        g->hi[x] = (int16_t)i;
        g->hw[x] = w;
        int o = (int)(u * sw);
        g->ox[x] = (int16_t)(o < sw ? o : sw - 1);
    }
    double ph = (double)sh / ch;
    if (fabs(ph - s_contrib_ph) > 1e-9) build_contrib(ph);
    if (!curved) return;
    geom_init();
    size_t n = (size_t)cw * ch;
    g->mu = malloc(sizeof(uint16_t) * n);
    g->mv = malloc(sizeof(uint16_t) * n);
    g->mc = malloc(n);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            size_t k = (size_t)y * cw + x;
            double u, v, cv;
            geom_transform((x + 0.5) / cw, (y + 0.5) / ch, &u, &v, &cv);
            if (u < 0 || u >= 1 || v < 0 || v >= 1 || cv <= 0) { g->mu[k] = 0xFFFF; g->mv[k] = 0; g->mc[k] = 0; continue; }
            g->mu[k] = (uint16_t)(u * 65534.0);
            g->mv[k] = (uint16_t)(v * 65534.0);
            g->mc[k] = (uint8_t)lrint(cv * 255.0);
        }
}

/* ---- buffers ---- */
static float    *s_vs = NULL;      /* [ch][sw][3] vertical scanlines */
static size_t    s_vs_cap = 0;
static float    *s_mrow = NULL;    /* [cw*3] mask pattern for a row */
static size_t    s_mrow_cap = 0;
static const uint32_t *s_src;

static void ensure_bufs(void) {
    const Geo *g = &s_geo;
    size_t vn = (size_t)g->ch * g->sw * 3;
    if (vn > s_vs_cap) { free(s_vs); s_vs = malloc(vn * sizeof(float)); s_vs_cap = vn; }
    size_t mn = (size_t)g->cw * 3;
    if (mn > s_mrow_cap) { free(s_mrow); s_mrow = malloc(mn * sizeof(float)); s_mrow_cap = mn; }
    for (int x = 0, ph = 0; x < g->cw; x++) {
        for (int c = 0; c < 3; c++) s_mrow[x * 3 + c] = s_mask[ph][c];
        if (++ph == TRIAD_SIZE) ph = 0;
    }
}

/* ---- thread pool ---- */
typedef void (*RowFn)(int y0, int y1, void *ctx);

#ifdef CYC_WITH_SDL
#define MAX_WORKERS 16
static int        s_nworkers = -1;
static SDL_sem   *s_go[MAX_WORKERS], *s_done;
static RowFn      s_fn;
static void      *s_ctx;
static int        s_rows;

static int worker_main(void *arg) {
    int k = (int)(intptr_t)arg;
    for (;;) {
        SDL_SemWait(s_go[k]);
        int parts = s_nworkers + 1;
        int y0 = s_rows * (k + 1) / parts, y1 = s_rows * (k + 2) / parts;
        s_fn(y0, y1, s_ctx);
        SDL_SemPost(s_done);
    }
    return 0;
}

static void pool_init(void) {
    int n = SDL_GetCPUCount() - 1;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    if (n < 0) n = 0;
    s_done = SDL_CreateSemaphore(0);
    s_nworkers = 0;
    if (!s_done) return;
    for (int k = 0; k < n; k++) {
        s_go[k] = SDL_CreateSemaphore(0);
        if (!s_go[k]) break;
        SDL_Thread *t = SDL_CreateThread(worker_main, "crt", (void *)(intptr_t)k);
        if (!t) { SDL_DestroySemaphore(s_go[k]); break; }
        SDL_DetachThread(t);
        s_nworkers++;
    }
}

static void parallel_rows(RowFn fn, int rows, void *ctx) {
    if (s_nworkers < 0) pool_init();
    if (s_nworkers == 0 || rows < 64) { fn(0, rows, ctx); return; }
    s_fn = fn; s_ctx = ctx; s_rows = rows;
    for (int k = 0; k < s_nworkers; k++) SDL_SemPost(s_go[k]);
    fn(0, rows / (s_nworkers + 1), ctx);
    for (int k = 0; k < s_nworkers; k++) SDL_SemWait(s_done);
}
#else
static void parallel_rows(RowFn fn, int rows, void *ctx) { fn(0, rows, ctx); }
#endif

/* ---- passes ---- */

/* scanlines-vertical-interlacing: [ch][sw] at viewport height */
static void vpass(int y0, int y1, void *ctx) {
    (void)ctx;
    const Geo *g = &s_geo;
    for (int y = y0; y < y1; y++) {
        float curr = (y + 0.5f) * g->sh / g->ch;
        int idx = (int)floorf(curr - 0.4995f);
        float dist = curr - (idx + 0.5f);
        int dround = dist >= 0.5f;
        int lines[3] = { idx, idx + 1, dround ? idx + 2 : idx - 1 };
        const float *rows[3][3];
        for (int c = 0; c < 3; c++) {
            float d2 = dist - k_conv_y[c];
            rows[0][c] = contrib_row(d2);
            rows[1][c] = contrib_row(fabsf(1.0f - d2));
            rows[2][c] = contrib_row(dround ? 2.0f - d2 : d2 + 1.0f);
        }
        float *o = s_vs + (size_t)y * g->sw * 3;
        for (int x = 0; x < g->sw; x++, o += 3) {
            o[0] = o[1] = o[2] = 0;
            for (int l = 0; l < 3; l++) {
                int ln = lines[l];
                if (ln < 0 || ln >= g->sh) continue;
                uint32_t p = s_src[(size_t)ln * g->sw + x];
                o[0] += rows[l][0][(p >> 16) & 0xFF];
                o[1] += rows[l][1][(p >> 8) & 0xFF];
                o[2] += rows[l][2][p & 0xFF];
            }
        }
    }
}

/* scanlines-horizontal-apply-mask + brightpass for content row r:
 * K = dim - brightpass (what stays put), B = brightpass (what gets bloomed). */
static void mask_row(int r, float *restrict K, float *restrict B) {
    const Geo *g = &s_geo;
    const int n = g->cw * 3;
    const float inv_cw1 = 1.0f / (s_center_weight - 1.0f);
    float col[4096 * 3], orig[4096 * 3];
    if (!g->curved) {
        const float *vs = s_vs + (size_t)r * g->sw * 3;
        int oy = (int)((r + 0.5f) * g->sh / g->ch);
        if (oy >= g->sh) oy = g->sh - 1;
        const uint32_t *srow = s_src + (size_t)oy * g->sw;
        for (int x = 0; x < g->cw; x++) {
            const float *a = vs + g->hi[x] * 3, *b = a + (g->hi[x] + 1 < g->sw ? 3 : 0);
            float w = g->hw[x];
            uint32_t op = srow[g->ox[x]];
            for (int c = 0; c < 3; c++) {
                col[x * 3 + c] = a[c] + (b[c] - a[c]) * w;
                orig[x * 3 + c] = LEVELS_CONTRAST * s_lin8[(op >> (16 - 8 * c)) & 0xFF];
            }
        }
    } else {
        size_t k = (size_t)r * g->cw;
        for (int x = 0; x < g->cw; x++, k++) {
            if (g->mu[k] == 0xFFFF) {
                for (int c = 0; c < 3; c++) { col[x * 3 + c] = 0; orig[x * 3 + c] = 0; }
                continue;
            }
            float u = g->mu[k] / 65534.0f, v = g->mv[k] / 65534.0f, w;
            int i;
            horiz_tap(u, g->sw, &i, &w);
            int ox = (int)(u * g->sw), row = (int)(v * g->ch), oy = (int)(v * g->sh);
            if (ox >= g->sw) ox = g->sw - 1;
            if (oy >= g->sh) oy = g->sh - 1;
            if (row >= g->ch) row = g->ch - 1;
            const float *a = s_vs + ((size_t)row * g->sw + i) * 3, *b = a + (i + 1 < g->sw ? 3 : 0);
            float cv = g->mc[k] / 255.0f;
            uint32_t op = s_src[(size_t)oy * g->sw + ox];
            for (int c = 0; c < 3; c++) {
                col[x * 3 + c] = (a[c] + (b[c] - a[c]) * w) * cv;
                orig[x * 3 + c] = LEVELS_CONTRAST * s_lin8[(op >> (16 - 8 * c)) & 0xFF];
            }
        }
    }
    const float *restrict m = s_mrow;
    for (int i = 0; i < n; i++) {
        float dim = col[i] * m[i];
        dim = dim > 0 ? dim : 0;
        float inten = dim * s_undim;
        float area = orig[i] - s_center_weight * inten;
        area = area > 0 ? area : 0;
        float safe = inten > 1e-20f ? inten : 1e-20f;
        float ratio = ((1.0f - BLOOM_UNDER * area) / (BLOOM_UNDER * safe) - 1.0f) * inv_cw1;
        ratio = ratio < 0 ? 0 : ratio > 1 ? 1 : ratio;
        float bp = dim * ratio;
        B[i] = bp;
        K[i] = dim - bp;
    }
}

typedef struct { uint32_t *dst; } FinalCtx;

/* per-thread ring of mask_row results for the 9-row bloom window */
#define RING (2 * BR + 1)
#if defined(_MSC_VER)
#define TLS __declspec(thread)
#else
#define TLS __thread
#endif
static TLS float *tl_ring = NULL;
static TLS size_t tl_ring_cap = 0;

/* mask + brightpass + bloom-vertical + bloom-horizontal-reconstitute + lcd
 * gamma, for output rows y0..y1, fused so nothing full-screen is stored. */
static void rpass(int y0, int y1, void *vctx) {
    FinalCtx *fc = (FinalCtx *)vctx;
    const Geo *g = &s_geo;
    const int n = g->cw * 3;
    size_t need = (size_t)RING * 2 * n;
    if (need > tl_ring_cap) { free(tl_ring); tl_ring = malloc(need * sizeof(float)); tl_ring_cap = need; }
    float *Kr = tl_ring, *Br = tl_ring + (size_t)RING * n;
    const float w0 = s_bloom_w[0], w1 = s_bloom_w[1], w2 = s_bloom_w[2], w3 = s_bloom_w[3], w4 = s_bloom_w[4];
    const float k = s_undim * OUT_STEPS;
    float pad[4096 * 3 + 2 * BR * 3];
    int ring_row[RING];
    for (int i = 0; i < RING; i++) ring_row[i] = -1;
    for (int y = y0; y < y1; y++) {
        uint32_t *o = fc->dst + (size_t)y * g->dw;
        int cy = y - g->cy0;
        if (cy < 0 || cy >= g->ch) { for (int x = 0; x < g->dw; x++) o[x] = 0xFF000000u; continue; }
        for (int x = 0; x < g->cx0; x++) o[x] = 0xFF000000u;
        for (int x = g->cx0 + g->cw; x < g->dw; x++) o[x] = 0xFF000000u;
        /* make sure rows cy-BR..cy+BR (edge-clamped) are in the ring */
        const float *bk[RING];
        const float *krow = NULL;
        for (int t = -BR; t <= BR; t++) {
            int r = cy + t;
            r = r < 0 ? 0 : r >= g->ch ? g->ch - 1 : r;
            int slot = r % RING;
            if (ring_row[slot] != r) {
                mask_row(r, Kr + (size_t)slot * n, Br + (size_t)slot * n);
                ring_row[slot] = r;
            }
            bk[t + BR] = Br + (size_t)slot * n;
            if (t == 0) krow = Kr + (size_t)slot * n;
        }
        /* vertical bloom into the padded row */
        float *restrict p = pad + BR * 3;
        const float *restrict a0 = bk[0], *restrict a1 = bk[1], *restrict a2 = bk[2], *restrict a3 = bk[3],
                    *restrict a4 = bk[4], *restrict a5 = bk[5], *restrict a6 = bk[6], *restrict a7 = bk[7],
                    *restrict a8 = bk[8];
        for (int i = 0; i < n; i++)
            p[i] = w0 * a4[i] + w1 * (a3[i] + a5[i]) + w2 * (a2[i] + a6[i]) + w3 * (a1[i] + a7[i]) + w4 * (a0[i] + a8[i]);
        for (int e = 1; e <= BR; e++)
            for (int c = 0; c < 3; c++) { p[-e * 3 + c] = p[c]; p[n - 3 + e * 3 + c] = p[n - 3 + c]; }
        /* horizontal bloom + reconstitute, in place into col-free buffer */
        uint32_t *row = o + g->cx0;
        const float *restrict K = krow;
        for (int x = 0; x < g->cw; x++) {
            uint32_t px = 0xFF000000u;
            for (int c = 0; c < 3; c++) {
                int i = x * 3 + c;
                float bl = w0 * p[i] + w1 * (p[i - 3] + p[i + 3]) + w2 * (p[i - 6] + p[i + 6]) +
                           w3 * (p[i - 9] + p[i + 9]) + w4 * (p[i - 12] + p[i + 12]);
                float v = (K[i] + bl) * k;
                v = v < 0 ? 0 : v > OUT_STEPS ? OUT_STEPS : v;
                px |= (uint32_t)s_out[(int)v] << (16 - 8 * c);
            }
            row[x] = px;
        }
    }
}

#ifdef CRT_PROFILE
double crt_prof[4];
#endif
void crt_render(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, bool curved) {
    if (!s_init) init_once();
    Geo *g = &s_geo;
    if (g->dw != dw || g->dh != dh || g->sw != sw || g->sh != sh || g->curved != curved || g->scale != s_fit_scale || !g->hi)
        geo_build(dw, dh, sw, sh, curved);
    if (g->cw < 2 || g->ch < 2 || sw < 2) { memset(dst, 0, (size_t)dw * dh * 4); return; }
    ensure_bufs();
    s_src = src;
#ifdef CRT_PROFILE
    Uint64 t0 = SDL_GetPerformanceCounter();
#define PT(name) do { Uint64 t1 = SDL_GetPerformanceCounter(); crt_prof[name] += (double)(t1 - t0) * 1000.0 / SDL_GetPerformanceFrequency(); t0 = t1; } while (0)
#else
#define PT(name) do { } while (0)
#endif
    parallel_rows(vpass, g->ch, NULL);
    PT(0);
    FinalCtx fc = { dst };
    parallel_rows(rpass, dh, &fc);
    PT(3);
}

void crt_fit_rect(int dw, int dh, int sw, int sh, int *x0, int *y0, int *w, int *h) {
    fit_rect(dw, dh, sw, sh, x0, y0, w, h);
}

bool crt_map_point(int dw, int dh, int sw, int sh, bool curved, float ox, float oy, float *sx, float *sy) {
    int x0, y0, w, h;
    fit_rect(dw, dh, sw, sh, &x0, &y0, &w, &h);
    double a = (ox - x0) / (double)w, b = (oy - y0) / (double)h;
    if (curved) {
        if (!s_init) init_once();
        geom_init();
        double u, v;
        geom_transform(a, b, &u, &v, NULL);
        a = u;
        b = v;
    }
    *sx = (float)(a * sw);
    *sy = (float)(b * sh);
    return a >= 0 && a < 1 && b >= 0 && b < 1;
}

void crt_params(int dw, int dh, int sw, int sh, CrtParams *p) {
    if (!s_init) init_once();
    geom_init();
    for (int m = 0; m < TRIAD_SIZE; m++)
        for (int c = 0; c < 3; c++) p->mask[m][c] = s_mask[m][c];
    p->undim = s_undim;
    p->center_weight = s_center_weight;
    p->contrast = LEVELS_CONTRAST;
    for (int k = 0; k <= BR; k++) p->bloom_w[k] = s_bloom_w[k];
    p->stretch[0] = (float)s_stretch_x;
    p->stretch[1] = (float)s_stretch_y;
    p->stretch[2] = (float)s_stretch_z;
    for (int c = 0; c < 3; c++) p->conv_y[c] = k_conv_y[c];
    p->beam_min = (float)BEAM_MIN_SIGMA;
    p->beam_max = (float)BEAM_MAX_SIGMA;
    p->beam_power = (float)BEAM_SPOT_POWER;
    fit_rect(dw, dh, sw, sh, &p->cx0, &p->cy0, &p->cw, &p->ch);
}
