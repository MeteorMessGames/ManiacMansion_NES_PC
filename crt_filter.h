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
#ifndef CRT_GAMMA
#define CRT_GAMMA        2.2f   /* source and output gamma */
#endif
#ifndef CRT_SHARP
#define CRT_SHARP        2.5f   /* horizontal edge sharpness (1 = plain linear blend) */
#endif
#ifndef CRT_MASK_DIM
#define CRT_MASK_DIM     0.70f  /* the two dimmed stripes of each mask triad */
#endif
/* The two looks F9 cycles through: beam sigma for black and for full
 * brightness (in source lines), brightness compensation for the gaps and the
 * mask, and the glow amount. */
typedef struct {
    float beam_min, beam_max, gain, glow;
} CrtLook;
enum { CRT_LOOK_NORMAL, CRT_LOOK_STRONG, CRT_LOOK_COUNT };
void crt_set_look(int look);
const CrtLook *crt_look(void);

/* Render src (sw x sh ARGB, square pixels) into dst (dw x dh ARGB), the
 * screen fitted to the src aspect and centered, black outside. `curved` is
 * accepted for compatibility and ignored (flat screen only). */
void crt_render(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, bool curved);

/* Style: the CRT model, or SHARP = plain pixels resampled "sharp bilinear"
 * (whole pixels, a one-output-pixel blend only where two source pixels meet;
 * used for the Sharp filter at a non-square pixel aspect). */
enum { CRT_STYLE_CRT, CRT_STYLE_SHARP };
void crt_set_style(int style);
int  crt_get_style(void);
/* Pixel aspect (width / height of one source pixel): 1 = square, 8/7 = how
 * a NES drew on a TV (4:3 picture). integer: when filling (scale 0), snap to
 * the largest whole vertical scale, as the Sharp filter does. */
#define CRT_PAR_TV (8.0f / 7.0f)
void crt_set_aspect(float par);
void crt_set_integer(bool on);

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
