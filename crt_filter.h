/* crt_filter.h - CRT look for the presented picture (CPU version).
 *
 * An original, simple CRT model written for this project:
 *   - each source line is a light beam with a gaussian vertical profile;
 *     brighter colors make a wider beam (bright lines "bloom" into the gap)
 *   - pixel-art friendly horizontal sampling (sharp, with a short blend)
 *   - an RGB stripe mask, one stripe per output column
 *   - a faint soft glow from a blurred copy of the picture
 * The GPU version (crt_gpu.c) uses the same constants and math, so both
 * draw the same picture and crt_map_point maps the mouse for either.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The look. Linear-light math; beam widths are in source lines. */
#define CRT_GAMMA        2.2f   /* source and output gamma */
#define CRT_SHARP        2.5f   /* horizontal edge sharpness (1 = plain linear blend) */
#define CRT_BEAM_MIN     0.28f  /* beam sigma for black */
#define CRT_BEAM_MAX     0.46f  /* beam sigma for full brightness */
#define CRT_MASK_DIM     0.70f  /* the two dimmed stripes of each mask triad */
#define CRT_GAIN         1.30f  /* brightness compensation for beam gaps + mask */
#define CRT_GLOW         0.10f  /* soft glow amount */

/* Render src (sw x sh ARGB, square pixels) into dst (dw x dh ARGB), the
 * screen fitted to the src aspect and centered, black outside. `curved` is
 * accepted for compatibility and ignored (flat screen only). */
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

/* Screen rectangle for the GPU version. */
typedef struct {
    int cx0, cy0, cw, ch;      /* screen rect in the dw x dh picture */
} CrtParams;
void crt_params(int dw, int dh, int sw, int sh, CrtParams *out);
