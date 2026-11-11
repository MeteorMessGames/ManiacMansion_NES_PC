/* crt_filter.c - see crt_filter.h. CPU version of the project's own CRT
 * model, split across worker threads by output rows.
 *
 * Per output pixel (x, y) inside the screen rect:
 *   sx, sy  = the source position (pixel centers at .5)
 *   row j   = the source line above sy - 0.5, t = distance down to it
 *   c_j     = that line's color at sx (sharp horizontal blend, linear light)
 *   out     = sum over lines j, j+1 of c * exp(-d^2 / (2 sigma(c)^2)),
 *             sigma(c) = mix(BEAM_MIN, BEAM_MAX, c) per channel
 *   out     = out * mask(x % 3) * GAIN + GLOW * blurred(sx, sy)
 * The per-line beam weights depend only on the row and the channel value,
 * so they are tabled once per output row.
 */
#include "crt_filter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef CYC_WITH_SDL
#include <SDL.h>
#define CRT_THREADS 1
#endif

#pragma GCC optimize("O3")

/* ---- geometry ---- */

/* 0 = fill the output (fractional fit); k > 0 = exactly k x the source,
 * centered, when that fits (else fill). */
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

void crt_fit_rect(int dw, int dh, int sw, int sh, int *x0, int *y0, int *w, int *h) {
    fit_rect(dw, dh, sw, sh, x0, y0, w, h);
}

bool crt_map_point(int dw, int dh, int sw, int sh, bool curved, float ox, float oy, float *sx, float *sy) {
    (void)curved;
    int x0, y0, w, h;
    fit_rect(dw, dh, sw, sh, &x0, &y0, &w, &h);
    double a = (ox - x0) / (double)w, b = (oy - y0) / (double)h;
    *sx = (float)(a * sw);
    *sy = (float)(b * sh);
    return a >= 0 && a < 1 && b >= 0 && b < 1;
}

void crt_params(int dw, int dh, int sw, int sh, CrtParams *p) {
    fit_rect(dw, dh, sw, sh, &p->cx0, &p->cy0, &p->cw, &p->ch);
}

/* ---- tables ---- */

static bool  s_init;
static float s_lin[256];          /* 8-bit -> linear light */
#define ENC_N 4096
static uint8_t s_enc[ENC_N + 1];  /* linear 0..1 -> 8-bit */

static void init_once(void) {
    for (int i = 0; i < 256; i++) s_lin[i] = powf(i / 255.0f, CRT_GAMMA);
    for (int i = 0; i <= ENC_N; i++) s_enc[i] = (uint8_t)(powf((float)i / ENC_N, 1.0f / CRT_GAMMA) * 255.0f + 0.5f);
    s_init = true;
}

static inline uint8_t encode(float v) {
    if (v <= 0) return 0;
    if (v >= 1) return 255;
    return s_enc[(int)(v * ENC_N + 0.5f)];
}

static inline float beam_sigma(float c) { return CRT_BEAM_MIN + (CRT_BEAM_MAX - CRT_BEAM_MIN) * c; }

/* ---- per-frame state shared by the workers ---- */

typedef struct {
    uint32_t *dst;
    int dw, dh;
    int sw, sh;
    int x0, y0, w, h;
    float *lin;             /* src in linear light, sw*sh*3 */
    float *glow;            /* blurred lin, sw*sh*3 */
    float *colx;            /* per output column: source x (pixel units) */
    float mask[3][3];       /* [x % 3][channel] */
} Frame;

static Frame s_f;

/* 3x3 box blur of the linear picture (the glow source) */
static void build_glow(void) {
    int sw = s_f.sw, sh = s_f.sh;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            for (int c = 0; c < 3; c++) {
                float acc = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    int yy = y + dy < 0 ? 0 : y + dy >= sh ? sh - 1 : y + dy;
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx < 0 ? 0 : x + dx >= sw ? sw - 1 : x + dx;
                        acc += s_f.lin[((size_t)yy * sw + xx) * 3 + c];
                    }
                }
                s_f.glow[((size_t)y * sw + x) * 3 + c] = acc / 9.0f;
            }
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* sharp horizontal sample of source line j at source x position px (pixel units) */
static inline void line_color(int j, float px, float out[3]) {
    int sw = s_f.sw;
    float p = px - 0.5f;
    int i = (int)floorf(p);
    float f = p - i;
    f = (f - 0.5f) * CRT_SHARP + 0.5f;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    const float *a = &s_f.lin[((size_t)j * sw + clampi(i, 0, sw - 1)) * 3];
    const float *b = &s_f.lin[((size_t)j * sw + clampi(i + 1, 0, sw - 1)) * 3];
    for (int c = 0; c < 3; c++) out[c] = a[c] + (b[c] - a[c]) * f;
}

/* bilinear sample of the glow picture */
static inline void glow_color(float px, float py, float out[3]) {
    int sw = s_f.sw, sh = s_f.sh;
    float x = px - 0.5f, y = py - 0.5f;
    int i = (int)floorf(x), j = (int)floorf(y);
    float fx = x - i, fy = y - j;
    int i0 = clampi(i, 0, sw - 1), i1 = clampi(i + 1, 0, sw - 1);
    int j0 = clampi(j, 0, sh - 1), j1 = clampi(j + 1, 0, sh - 1);
    for (int c = 0; c < 3; c++) {
        float a = s_f.glow[((size_t)j0 * sw + i0) * 3 + c], b = s_f.glow[((size_t)j0 * sw + i1) * 3 + c];
        float d = s_f.glow[((size_t)j1 * sw + i0) * 3 + c], e = s_f.glow[((size_t)j1 * sw + i1) * 3 + c];
        float top = a + (b - a) * fx, bot = d + (e - d) * fx;
        out[c] = top + (bot - top) * fy;
    }
}

#define WT_N 256   /* beam weight table resolution over the channel value */

static void render_rows(int r0, int r1) {
    float wt0[WT_N + 1], wt1[WT_N + 1];
    for (int y = r0; y < r1; y++) {
        uint32_t *row = s_f.dst + (size_t)y * s_f.dw;
        int ry = y - s_f.y0;
        if (ry < 0 || ry >= s_f.h) { memset(row, 0, (size_t)s_f.dw * 4); continue; }
        memset(row, 0, (size_t)s_f.x0 * 4);
        memset(row + s_f.x0 + s_f.w, 0, (size_t)(s_f.dw - s_f.x0 - s_f.w) * 4);

        float sy = (ry + 0.5f) * s_f.sh / (float)s_f.h;
        float p = sy - 0.5f;
        int j = (int)floorf(p);
        float t = p - j;                       /* distance below line j; 1 - t above line j + 1 */
        int j0 = clampi(j, 0, s_f.sh - 1), j1 = clampi(j + 1, 0, s_f.sh - 1);
        for (int k = 0; k <= WT_N; k++) {
            float s = beam_sigma((float)k / WT_N);
            wt0[k] = expf(-(t * t) / (2 * s * s));
            wt1[k] = expf(-((1 - t) * (1 - t)) / (2 * s * s));
        }
        for (int rx = 0; rx < s_f.w; rx++) {
            float px = s_f.colx[rx];
            float a[3], b[3], g[3];
            line_color(j0, px, a);
            line_color(j1, px, b);
            glow_color(px, sy, g);
            const float *m = s_f.mask[(s_f.x0 + rx) % 3];
            uint8_t o[3];
            for (int c = 0; c < 3; c++) {
                float ca = a[c] < 0 ? 0 : a[c] > 1 ? 1 : a[c];
                float cb = b[c] < 0 ? 0 : b[c] > 1 ? 1 : b[c];
                float v = ca * wt0[(int)(ca * WT_N + 0.5f)] + cb * wt1[(int)(cb * WT_N + 0.5f)];
                v = v * m[c] * CRT_GAIN + g[c] * CRT_GLOW;
                o[c] = encode(v);
            }
            row[s_f.x0 + rx] = 0xFF000000u | ((uint32_t)o[0] << 16) | ((uint32_t)o[1] << 8) | o[2];
        }
    }
}

#ifdef CRT_THREADS
typedef struct { int r0, r1; } Job;
static int worker(void *arg) {
    Job *jb = (Job *)arg;
    render_rows(jb->r0, jb->r1);
    return 0;
}
#endif

static bool ensure(void **p, size_t *cap, size_t need) {
    if (*cap >= need) return true;
    void *n = realloc(*p, need);
    if (!n) return false;
    *p = n;
    *cap = need;
    return true;
}

void crt_render(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, bool curved) {
    (void)curved;
    if (!s_init) init_once();
    static void *lin = NULL, *glow = NULL, *colx = NULL;
    static size_t lin_cap = 0, glow_cap = 0, colx_cap = 0;
    Frame *f = &s_f;
    f->dst = dst; f->dw = dw; f->dh = dh; f->sw = sw; f->sh = sh;
    fit_rect(dw, dh, sw, sh, &f->x0, &f->y0, &f->w, &f->h);
    size_t n = (size_t)sw * sh * 3 * sizeof(float);
    if (sw < 2 || sh < 2 || f->w < 2 || f->h < 2 ||
        !ensure(&lin, &lin_cap, n) || !ensure(&glow, &glow_cap, n) ||
        !ensure(&colx, &colx_cap, (size_t)f->w * sizeof(float))) {
        memset(dst, 0, (size_t)dw * dh * 4);
        return;
    }
    f->lin = (float *)lin; f->glow = (float *)glow; f->colx = (float *)colx;
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
        uint32_t px = src[i];
        f->lin[i * 3 + 0] = s_lin[(px >> 16) & 255];
        f->lin[i * 3 + 1] = s_lin[(px >> 8) & 255];
        f->lin[i * 3 + 2] = s_lin[px & 255];
    }
    build_glow();
    for (int rx = 0; rx < f->w; rx++) f->colx[rx] = (rx + 0.5f) * sw / (float)f->w;
    for (int s = 0; s < 3; s++)
        for (int c = 0; c < 3; c++) f->mask[s][c] = s == c ? 1.0f : CRT_MASK_DIM;

#ifdef CRT_THREADS
    int nt = SDL_GetCPUCount();
    nt = nt < 1 ? 1 : nt > 16 ? 16 : nt;
    Job jobs[16];
    SDL_Thread *th[16] = { 0 };
    for (int i = 0; i < nt; i++) {
        jobs[i].r0 = dh * i / nt;
        jobs[i].r1 = dh * (i + 1) / nt;
    }
    for (int i = 1; i < nt; i++) {
        th[i] = SDL_CreateThread(worker, "crt", &jobs[i]);
        if (!th[i]) render_rows(jobs[i].r0, jobs[i].r1);
    }
    render_rows(jobs[0].r0, jobs[0].r1);
    for (int i = 1; i < nt; i++) if (th[i]) SDL_WaitThread(th[i], NULL);
#else
    render_rows(0, dh);
#endif
}
