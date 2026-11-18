/* rom_picker.c - the small window that asks which Maniac Mansion ROM to play, drawn like the pause menu: the game's own
 * letters and colours, a bordered panel, an arrow beside the selected row.
 * Runs before the game starts (called from the release build's ROM finder), so it has its own SDL window and no game state. */
#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "menu_render.h"

#define MAX_ROWS 16
#define LW 256            /* the picker is laid out on the game's 256-wide grid */
#define C_OLIVE  0xB8D808u
#define C_ORANGE 0xD86000u
#define C_WHITE  0xFCFCFCu
#define C_DIM    0x707828u

typedef struct { uint32_t *d; int w, h, s; } Canvas;

static void px_rect(const Canvas *c, int x, int y, int w, int h, uint32_t rgb) {
    mr_rect(c->d, c->w, c->h, x * c->s, y * c->s, w * c->s, h * c->s, rgb, 255);
}
static void px_box(const Canvas *c, int x, int y, int w, int h, uint32_t rgb) {
    px_rect(c, x, y, w, 1, rgb); px_rect(c, x, y + h - 1, w, 1, rgb);
    px_rect(c, x, y, 1, h, rgb); px_rect(c, x + w - 1, y, 1, h, rgb);
}
static void px_text(const Canvas *c, int x, int y, uint32_t rgb, const char *s, int pitch) {
    mr_glyph_text(c->d, c->w, c->h, x * c->s, y * c->s, c->s, rgb, s, pitch, 0, c->w);
}
static void px_text_c(const Canvas *c, int cx, int y, uint32_t rgb, const char *s, int pitch) {
    px_text(c, cx - mr_glyph_width(s, pitch) / 2 - 1, y, rgb, s, pitch);
}

/* titles[0..n-1] are the ROMs found in the folder. Returns the chosen index, or -1 to quit. */
int mm_rom_picker(const char *const *titles, int n, int *remember) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return n > 0 ? 0 : -1;
    const char *labels[MAX_ROWS];
    int rows = 0, quit_row;
    for (int i = 0; i < n && rows < MAX_ROWS - 1; i++) labels[rows++] = titles[i];
    n = rows;
    quit_row = rows; labels[rows++] = "Quit";
    const int row_h = 16, top = 40;
    int lh = top + rows * row_h + 14;                 /* logical height of the panel = the window */
    int s = lh * 3 <= 900 ? 3 : 2;
    int pw = LW * s, ph = lh * s;
    SDL_Window *win = SDL_CreateWindow("Maniac Mansion NES PC", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, pw, ph, SDL_WINDOW_SHOWN);
    if (!win) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return n > 0 ? 0 : -1; }
    SDL_Surface *ws = SDL_GetWindowSurface(win);
    uint32_t *buf = (uint32_t *)malloc((size_t)pw * ph * 4);
    Canvas c = { buf, pw, ph, s };
    int sel = 0, hover = -1, result = -2;
    while (result == -2) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) result = -1;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) result = -1;
                else if (k == SDLK_DOWN) sel = (sel + 1) % rows;
                else if (k == SDLK_UP) sel = (sel + rows - 1) % rows;
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                    if (sel == quit_row) result = -1;
                    else result = sel;
                }
            } else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                int my = (e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y) / s;
                int mx = (e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x) / s;
                hover = -1;
                for (int i = 0; i < rows; i++) {
                    int y0 = top + i * row_h;
                    if (my >= y0 && my < y0 + row_h && mx >= 4 && mx < LW - 4) hover = i;
                }
                if (hover >= 0) sel = hover;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && hover >= 0) {
                    if (hover == quit_row) result = -1;
                    else result = hover;
                }
            }
        }
        /* the same look as the pause menu: black panel, olive and orange double border, arrow on the selected row */
        px_rect(&c, 0, 0, LW, lh, 0x000000);
        px_box(&c, 0, 0, LW, lh, C_OLIVE);
        px_box(&c, 2, 2, LW - 4, lh - 4, C_ORANGE);
        px_text_c(&c, LW / 2, 7, C_OLIVE, "Maniac Mansion", 8);
        px_text_c(&c, LW / 2, 17, C_DIM, "Choose a version to play", 8);
        px_rect(&c, 8, 28, LW - 16, 1, C_ORANGE);
        for (int i = 0; i < rows; i++) {
            int y0 = top + i * row_h, ty = y0 + (row_h - 7) / 2;
            bool is_sel = i == sel;
            if (is_sel) for (int k = 0; k < 4; k++) px_rect(&c, 8 + k, ty + k, 1, 7 - 2 * k, C_WHITE);
            char lb[64];
            snprintf(lb, sizeof lb, "%s", labels[i]);
            int pitch = mr_glyph_width(lb, 8) <= LW - 28 ? 8 : 7;
            while (strlen(lb) > 4 && mr_glyph_width(lb, pitch) > LW - 28) lb[strlen(lb) - 1] = 0;
            px_text(&c, 18, ty, is_sel ? C_WHITE : C_OLIVE, lb, pitch);
        }
        ws = SDL_GetWindowSurface(win);
        if (ws) {
            SDL_Surface *src = SDL_CreateRGBSurfaceWithFormatFrom(buf, pw, ph, 32, pw * 4, SDL_PIXELFORMAT_ARGB8888);
            if (src) { SDL_BlitSurface(src, NULL, ws, NULL); SDL_FreeSurface(src); }
            SDL_UpdateWindowSurface(win);
        }
        SDL_Delay(16);
    }
    free(buf);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    (void)remember;
    return result;   /* -1 quit, 0..n-1 the ROM */
}
