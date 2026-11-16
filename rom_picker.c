/* rom_picker.c - the small window that asks which Maniac Mansion ROM to play, drawn with the same look as the Esc menu.
 * Runs before the game starts (called from the release build's ROM finder), so it has its own SDL window and no game state. */
#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "menu_render.h"

#define PW 560
#define MAX_ROWS 16

/* titles[0..n-1] are the ROMs found in the folder. Returns the chosen index, or -1 to quit. */
int mm_rom_picker(const char *const *titles, int n, int *remember) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return n > 0 ? 0 : -1;
    const char *labels[MAX_ROWS];
    int rows = 0, quit_row;
    for (int i = 0; i < n && rows < MAX_ROWS - 1; i++) labels[rows++] = titles[i];
    n = rows;
    quit_row = rows; labels[rows++] = "Quit";
    const int row_h = 34, gap = 6, top = 96, pad = 28;
    int ph = top + rows * (row_h + gap) + 20;
    SDL_Window *win = SDL_CreateWindow("Maniac Mansion NES PC", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, PW, ph, SDL_WINDOW_SHOWN);
    if (!win) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return n > 0 ? 0 : -1; }
    SDL_Surface *ws = SDL_GetWindowSurface(win);
    uint32_t *buf = (uint32_t *)malloc((size_t)PW * ph * 4);
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
                int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y, mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                hover = -1;
                for (int i = 0; i < rows; i++) {
                    int y0 = top + i * (row_h + gap);
                    if (my >= y0 && my < y0 + row_h && mx >= pad / 2 && mx < PW - pad / 2) hover = i;
                }
                if (hover >= 0) sel = hover;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && hover >= 0) {
                    if (hover == quit_row) result = -1;
                    else result = hover;
                }
            }
        }
        /* same look as the Esc menu: dark panel, white title, grey subtitle, blue selection */
        mr_rect(buf, PW, ph, 0, 0, PW, ph, 0x14161F, 255);
        mr_text(buf, PW, ph, pad, 20, 26.0f, 0xFFFFFF, 255, "Maniac Mansion");
        mr_text(buf, PW, ph, pad, 54, 14.0f, 0x9AA0B4, 255, "Choose a version to play");
        mr_rect(buf, PW, ph, pad, 80, PW - 2 * pad, 1, 0x3A3D4C, 255);
        for (int i = 0; i < rows; i++) {
            int y0 = top + i * (row_h + gap);
            bool s = i == sel;
            if (s) mr_round_rect(buf, PW, ph, pad / 2, y0, PW - pad, row_h, 6, 0x2C6FE0, 215);
            else mr_round_rect(buf, PW, ph, pad / 2, y0, PW - pad, row_h, 6, 0xFFFFFF, 20);
            uint32_t fg = s ? 0xFFFFFF : 0xE6E9F4;
            if (i == quit_row) mr_text(buf, PW, ph, pad, y0 + 8, 16.0f, s ? 0xFFFFFF : 0xB4B9CC, 255, labels[i]);
            else mr_text(buf, PW, ph, pad, y0 + 8, 16.0f, fg, 255, labels[i]);
        }
        ws = SDL_GetWindowSurface(win);
        if (ws) {
            SDL_Surface *src = SDL_CreateRGBSurfaceWithFormatFrom(buf, PW, ph, 32, PW * 4, SDL_PIXELFORMAT_ARGB8888);
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
