/* menu_render.h - window-resolution drawing for the Escape menu. */
#pragma once
#include <stdint.h>

/* Nearest-neighbour scale src (sw x sh) into dst (dw x dh), multiplying
 * every channel by dim/255 (255 = unchanged). */
void mr_scale_nearest(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, int dim);
/* Nearest-neighbour scale src (sw x sh) into the rectangle x,y,w,h of dst (clipped). */
void mr_blit_scaled(uint32_t *dst, int dw, int dh, int x, int y, int w, int h,
                    const uint32_t *src, int sw, int sh);
/* Alpha-blended rectangle / rounded rectangle (alpha 0..255). */
void mr_rect(uint32_t *d, int w, int h, int x, int y, int rw, int rh, uint32_t rgb, int alpha);
void mr_round_rect(uint32_t *d, int w, int h, int x, int y, int rw, int rh, int radius, uint32_t rgb, int alpha);
/* Anti-aliased text, top-left at (x, y), `px` pixels tall. Returns the pen x. */
int  mr_text(uint32_t *d, int w, int h, int x, int y, float px, uint32_t rgb, int alpha, const char *s);
int  mr_text_width(float px, const char *s);
/* The game's own 6x7 letters (menu_font8.h), upper-cased. (x, y) = top-left of the first 8x8 cell in device pixels, s = device
 * pixels per font pixel, pitch = font pixels between cells (8 = the game's spacing, 7 = a tighter line). Pixels outside
 * [clip0, clip1) in x are not drawn (pass 0, w for no clip). Returns the drawn width in font pixels (n * pitch - 2). */
int mr_glyph_text(uint32_t *d, int w, int h, int x, int y, int s, uint32_t rgb, const char *str, int pitch, int clip0, int clip1);
int mr_glyph_width(const char *str, int pitch);
