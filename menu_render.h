/* menu_render.h - window-resolution drawing for the Escape menu. */
#pragma once
#include <stdint.h>

/* Nearest-neighbour scale src (sw x sh) into dst (dw x dh), multiplying
 * every channel by dim/255 (255 = unchanged). */
void mr_scale_nearest(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, int dim);
/* Alpha-blended rectangle / rounded rectangle (alpha 0..255). */
void mr_rect(uint32_t *d, int w, int h, int x, int y, int rw, int rh, uint32_t rgb, int alpha);
void mr_round_rect(uint32_t *d, int w, int h, int x, int y, int rw, int rh, int radius, uint32_t rgb, int alpha);
/* Anti-aliased text, top-left at (x, y), `px` pixels tall. Returns the pen x. */
int  mr_text(uint32_t *d, int w, int h, int x, int y, float px, uint32_t rgb, int alpha, const char *s);
int  mr_text_width(float px, const char *s);
