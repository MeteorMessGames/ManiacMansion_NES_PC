/* crt_filter.h - CRT look for the presented picture (CPU, multithreaded).
 *
 * A CPU port of libretro's crt-royale-fast (slang-shaders,
 * presets/crt-royale-fast/crt-royale-fast-rgb-aperture.slangp, shaders in
 * crt/shaders/crt-royale/src-fast) with its default parameters: CRT gamma
 * linearize, generalized-gaussian scanline beams sampled per output row,
 * quintic horizontal resampling, the Royale aperture-grille mask texture
 * resized to 3-px triads, the brightpass + 17-tap bloom that puts back the
 * light the mask takes away, and (CRT Curved) crt-geom style curvature with
 * Royale's corner mask. SDL's renderer cannot run slang shaders, so this runs
 * on the CPU at the window's resolution, split across worker threads.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Render src (sw x sh ARGB, square pixels) into dst (dw x dh ARGB), the
 * screen fitted to the src aspect and centered, black outside. */
void crt_render(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, bool curved);

/* Picture size: 0 = fill dst (default), k > 0 = exactly k x src, centered,
 * when it fits (else fill). Applies to crt_render, crt_map_point,
 * crt_params (so the GPU version too) and crt_fit_rect. */
void crt_set_scale(int k);
int  crt_get_scale(void);
/* The screen rectangle the filter draws src into, in dst pixels. */
void crt_fit_rect(int dw, int dh, int sw, int sh, int *x0, int *y0, int *w, int *h);

/* Map an output point (ox, oy in dst pixels) to src coordinates for the same
 * geometry; false if the point is off the picture. */
bool crt_map_point(int dw, int dh, int sw, int sh, bool curved, float ox, float oy, float *sx, float *sy);

/* The derived constants and screen rectangle the CPU filter uses, for the
 * GPU version (crt_gpu.c) so both render the same picture. */
typedef struct {
    float mask[3][3];          /* grille: [column % 3][r, g, b] */
    float undim, center_weight, contrast;
    float bloom_w[5];          /* normalized gaussian taps 0..4 */
    float stretch[3];          /* crt-geom maxscale() */
    float conv_y[3];
    float beam_min, beam_max, beam_power;
    int cx0, cy0, cw, ch;      /* screen rect in the dw x dh picture */
} CrtParams;
void crt_params(int dw, int dh, int sw, int sh, CrtParams *out);
