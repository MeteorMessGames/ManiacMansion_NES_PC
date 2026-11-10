/* menu_render.c - drawing helpers for the Escape menu (mouse_cursor_hook.c).
 *
 * Everything draws into a caller-owned ARGB8888 buffer at window
 * resolution (the hook presents that buffer instead of the 256x240 frame
 * while the menu is open), so text is anti-aliased TrueType rather than the
 * runner's 3x5 overlay font. Font: DejaVu Sans Bold, embedded (menu_font.h).
 */
#include "menu_render.h"

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "menu_font.h"

static stbtt_fontinfo s_fallback;
static int s_font_ok = -1;

static float glyph_px(const stbtt_fontinfo *f, float px) { (void)f; return px; }

static int font_ready(void) {
    if (s_font_ok < 0)
        s_font_ok = stbtt_InitFont(&s_fallback, menu_font_ttf, stbtt_GetFontOffsetForIndex(menu_font_ttf, 0)) ? 1 : 0;
    return s_font_ok;
}

static const stbtt_fontinfo *font_for(int c) { (void)c; return &s_fallback; }

static inline uint32_t mix(uint32_t dst, uint32_t rgb, int a) {
    if (a <= 0) return dst;
    if (a >= 255) return 0xFF000000u | (rgb & 0xFFFFFF);
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    uint32_t r = (dr * (255 - a) + sr * a) / 255;
    uint32_t g = (dg * (255 - a) + sg * a) / 255;
    uint32_t b = (db * (255 - a) + sb * a) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void mr_scale_nearest(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh, int dim) {
    for (int y = 0; y < dh; y++) {
        const uint32_t *row = src + (size_t)(y * sh / dh) * sw;
        uint32_t *out = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            uint32_t p = row[x * sw / dw];
            uint32_t r = ((p >> 16) & 0xFF) * dim / 255, g = ((p >> 8) & 0xFF) * dim / 255, b = (p & 0xFF) * dim / 255;
            out[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void mr_blit_scaled(uint32_t *dst, int dw, int dh, int x, int y, int w, int h,
                    const uint32_t *src, int sw, int sh) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int ty = y + j;
        if (ty < 0 || ty >= dh) continue;
        const uint32_t *row = src + (size_t)(j * sh / h) * sw;
        uint32_t *out = dst + (size_t)ty * dw;
        for (int i = 0; i < w; i++) {
            int tx = x + i;
            if (tx < 0 || tx >= dw) continue;
            out[tx] = 0xFF000000u | row[i * sw / w];
        }
    }
}

void mr_rect(uint32_t *d, int w, int h, int x, int y, int rw, int rh, uint32_t rgb, int alpha) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + rw > w ? w : x + rw, y1 = y + rh > h ? h : y + rh;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) d[(size_t)j * w + i] = mix(d[(size_t)j * w + i], rgb, alpha);
}

void mr_round_rect(uint32_t *d, int w, int h, int x, int y, int rw, int rh, int radius, uint32_t rgb, int alpha) {
    if (radius * 2 > rh) radius = rh / 2;
    if (radius * 2 > rw) radius = rw / 2;
    for (int j = 0; j < rh; j++) {
        int py = y + j;
        if (py < 0 || py >= h) continue;
        for (int i = 0; i < rw; i++) {
            int px = x + i;
            if (px < 0 || px >= w) continue;
            int a = alpha;
            int cx = i < radius ? radius - i : (i >= rw - radius ? i - (rw - radius - 1) : 0);
            int cy = j < radius ? radius - j : (j >= rh - radius ? j - (rh - radius - 1) : 0);
            if (cx && cy) {
                /* soft corner: coverage from distance to the corner circle */
                float dx = (float)cx - 0.5f, dy = (float)cy - 0.5f;
                float dist = dx * dx + dy * dy, r = (float)radius;
                if (dist > (r + 0.5f) * (r + 0.5f)) continue;
                if (dist > (r - 0.5f) * (r - 0.5f)) a = alpha / 2;
            }
            d[(size_t)py * w + px] = mix(d[(size_t)py * w + px], rgb, a);
        }
    }
}

int mr_text_width(float px, const char *s) {
    if (!font_ready() || !s) return 0;
    float x = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const stbtt_fontinfo *f = font_for(*p);
        float scale = stbtt_ScaleForPixelHeight(f, glyph_px(f, px));
        int adv, lsb;
        stbtt_GetCodepointHMetrics(f, *p, &adv, &lsb);
        x += adv * scale;
        if (p[1] && font_for(p[1]) == f) x += scale * stbtt_GetCodepointKernAdvance(f, *p, p[1]);
    }
    return (int)(x + 0.5f);
}

int mr_text(uint32_t *d, int w, int h, int x, int y, float px, uint32_t rgb, int alpha, const char *s) {
    if (!font_ready() || !s) return x;
    /* one baseline for the line, from the primary font's ascent */
    const stbtt_fontinfo *base_font = &s_fallback;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(base_font, &ascent, &descent, &gap);
    int baseline = y + (int)(ascent * stbtt_ScaleForPixelHeight(base_font, glyph_px(base_font, px)) + 0.5f);
    float pen = (float)x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const stbtt_fontinfo *f = font_for(*p);
        float scale = stbtt_ScaleForPixelHeight(f, glyph_px(f, px));
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(f, *p, &adv, &lsb);
        float sub = pen - (float)(int)pen;
        stbtt_GetCodepointBitmapBoxSubpixel(f, *p, scale, scale, sub, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *bmp = (unsigned char *)malloc((size_t)gw * gh);
            if (bmp) {
                stbtt_MakeCodepointBitmapSubpixel(f, bmp, gw, gh, gw, scale, scale, sub, 0, *p);
                int ox = (int)pen + x0, oy = baseline + y0;
                for (int j = 0; j < gh; j++) {
                    int py = oy + j;
                    if (py < 0 || py >= h) continue;
                    for (int i = 0; i < gw; i++) {
                        int pxx = ox + i;
                        if (pxx < 0 || pxx >= w) continue;
                        int a = bmp[j * gw + i] * alpha / 255;
                        d[(size_t)py * w + pxx] = mix(d[(size_t)py * w + pxx], rgb, a);
                    }
                }
                free(bmp);
            }
        }
        pen += adv * scale;
        if (p[1] && font_for(p[1]) == f) pen += scale * stbtt_GetCodepointKernAdvance(f, *p, p[1]);
    }
    return (int)pen;
}
