/* mouse_cursor_hook.c -- full reimplementation of Maniac Mansion's
 * joypad-poll routine, hooked at its one real JSR entry ($FB68), so a
 * "handled" return's RTS-style stack pop (cyc_hooks.c's
 * return_from_routine()) is correct: it pulls the return address the JSR
 * at $831F just pushed, same as the original routine's own RTS would.
 *
 * (Earlier attempt hooked $FB7F, the LDA $4016,X instruction 12 bytes into
 * the loop body -- wrong, because that address is reached by a BNE
 * loop-back, not a JSR, so "handled" there popped the stale OUTER caller's
 * return address and aborted the whole 2-controller/8-bit loop after one
 * bit read. See mmresume.md and game.toml's history comment.)
 *
 * Original routine, decoded from raw PRG bytes (see mmresume.md):
 *   LDX #$01
 *   loop (X=1 then X=0, i.e. pad2 then pad1):
 *     A = $02,X ; $04,X = A        ; stash last frame's byte for edge-detect
 *     $7221 = 8                    ; bit counter
 *     strobe $4016 (write 1 then 0)
 *     repeat 8x: ASL $02,X ; A = ($4016,X) & 1 ; $02,X |= A ; --$7221
 *     DEX ; loop while X >= 0
 * Result: $02 = pad1 byte, $03 = pad2 byte, standard NES bit order
 * (bit7=A, bit6=B, bit5=Select, bit4=Start, bit3=Up, bit2=Down, bit1=Left,
 * bit0=Right) -- first bit shifted in (A) ends up at bit7 after 7 more
 * ASLs, last bit shifted in (Right) stays at bit0.
 *
 * This replacement skips the real $4016/$4017 strobe+read entirely (we are
 * now the sole source of button state) and writes $02-$05 directly.
 *
 * REAL INPUT WIRED (2026-11-06): the windowed-mods gap is closed (SDL
 * confirmed linked and the hook confirmed firing in a real windowed run --
 * see mmresume.md). LMB -> BTN_A (the verb-grid "select" button in Maniac
 * Mansion's interface); RMB -> BTN_B (see below).
 *
 * ABSOLUTE CURSOR POSITIONING (2026-11-06, ScummVM-style, supersedes the
 * earlier relative-motion-to-d-pad-taps approximation): found the verb
 * cursor's live screen position by scripting --input d-pad holds against a
 * quiet static screen (the NES soft-reset safety menu, no idle animation to
 * pollute the RAM diff) and diffing full zero-page+OAM state between a
 * RIGHT-held run and a LEFT-held run -- see re/menu_{right,left,up,down}.txt
 * and re/*.jsonl. The verb cursor is sprite OAM entry 1 ($0200-based table):
 *   $0204 = cursor Y (sprite Y, clamped ~35-222 on that screen)
 *   $0205 = cursor sprite tile index (unchanged by movement, left alone)
 *   $0206 = cursor sprite attribute byte (unchanged by movement, left alone)
 *   $0207 = cursor X (sprite X, clamped ~16-239 on that screen)
 * Writing these two bytes directly from the real absolute mouse position
 * every frame bypasses the ROM's own digital fixed-step-per-frame cursor
 * movement code entirely -- the thing that made the earlier d-pad-tap
 * approach structurally incapable of 1:1 tracking no matter how clean the
 * input was (see mmresume.md's pivot note). This is the same trick ScummVM
 * itself doesn't need (it has a real SCUMM interpreter with native mouse
 * support) but is the closest equivalent inside a ROM we're not
 * rewriting: drive the position register directly instead of emulating
 * the original controller.
 * Window->NES-pixel mapping: SDL_GetMouseFocus() + SDL_GetWindowSize() give
 * the focused window's pixel size without needing cyc_sdl.c to hand us a
 * window handle (global SDL call, no host-extras plumbing needed); the
 * window is letterboxed by SDL_RenderSetLogicalSize(256,240) (stock
 * non-widescreen aspect -- this game never calls nes_video_set_aspect_mode)
 * so the inverse letterbox math (scale = min(w/256,h/240), center-offset)
 * maps window pixels back to the 256x240 logical frame the ROM draws into.
 * Clamped to [0,255]x[0,239] -- NOT the narrower ~16-239/35-222 box observed
 * on the reset-menu screen, which is that specific screen's own UI-box
 * bound enforced by the (now-bypassed) movement code, not a hardware limit;
 * gameplay screens may want their own clamp if the cursor can visually
 * leave the playfield under some verb-grid layout -- not yet confirmed
 * either way, watch for it when next tested live.
 * The OS cursor is hidden (SDL_ShowCursor(SDL_DISABLE)) since the game
 * draws its own cursor sprite from these same bytes.
 *
 * RIGHT-CLICK / DOTT-style default-action: SCOPED BACK, not implemented
 * (see mmresume.md's "Right-click" section -- this NES port's classic
 * 15-verb grid interface, not DOTT's 2-button one, makes a true
 * "default verb for the object under the cursor" feature a real
 * reverse-engineering project on its own, not attempted here).
 *
 * CUTSCENE SKIP (2026-11-06, reverted back to this after a brief A+B-combo
 * experiment in commit 0caf0d5 turned out broken): RMB -> BTN_A | BTN_START
 * unconditionally. A right-click both performs the game's normal
 * context action (A) and skips any active cutscene (Start) in the same
 * press.
 */
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_host_extras.h"
#include "cyc_mod.h"
#include "cyc_state.h"
#include "cyc_session.h"
#include "cyc_overlay.h"
#include "menu_render.h"
#include "crt_filter.h"
#include "crt_gpu.h"
#include "cyc_render.h"
#include "cyc_video.h"
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CYC_WITH_SDL
#include <SDL.h>
#endif

/* We don't use cyc_host_extras's own feature surface (window/options/view
 * modes) -- only the [[mod_function_hook]] mechanism above -- but this file
 * is the project's HOST_EXTRAS source, so it must still provide a strong
 * definition here. Without it, cyc_host_extras_none.c's weak fallback is
 * supposed to supply one, but MinGW's COFF linker does not reliably
 * resolve that fallback (see that file's own comment) -- confirmed here:
 * the Windows cross-build failed with "undefined reference to
 * cyc_host_extras" until this was added; the native Linux build never
 * needed it (ELF weak-symbol resolution just worked there). */
/* 2026-11-09: now a real definition (the Escape save/load menu needs
 * present/frame_end/event) -- see "Escape menu" below; defined at the end
 * of the file. */

#define HOOK_ID "maniac-mansion.mouse-cursor.joypad-read"
#define HOOK_ID_BANK0 "maniac-mansion.mouse-cursor.joypad-read-bank0"
#define HOOK_ID_GAME_SAVE "maniac-mansion.game-save-observer"
#define HOOK_ID_UNCLAMP "maniac-mansion.cursor-unclamp"

/* Standard NES button bits, matching the assembled byte's layout. */
#define BTN_A      0x80
#define BTN_B      0x40
#define BTN_SELECT 0x20
#define BTN_START  0x10
#define BTN_UP     0x08
#define BTN_DOWN   0x04
#define BTN_LEFT   0x02
#define BTN_RIGHT  0x01

/* Cursor sprite OAM entry (RE'd 2026-11-06 -- see the file header).
 * NOT a fixed slot: the reset-menu screen (quiet, few sprites) the RE was
 * done on happened to always place the cursor at OAM entry 1 ($0204-$0207),
 * but a live-test report ("clicks logged, no movement") during real
 * gameplay -- with far more active sprites (characters, scenery, objects)
 * -- combined with the debug log proving $0204/$0207 writes land cleanly
 * and uncontested every frame, means the game just isn't reading slot 1 as
 * the cursor there: this NES engine evidently allocates OAM slots
 * dynamically per frame based on which sprites are active, not a fixed
 * table. Cursor tile index $FA was constant across every RE capture
 * (menu AND gameplay), so instead of a fixed address this scans all 64 OAM
 * entries each frame for tile==$FA and writes position to whichever slot
 * currently holds it -- correct regardless of where the engine puts it. */
#define CURSOR_TILE 0xFA

/* THE REAL FIX (2026-11-06, Mesen Lua RE via Wine on the Linux dev box, per
 * user's "Test on the Linux box with wine" -- see mmresume.md): direct OAM writes
 * above were silently overwritten every frame -- confirmed by actual
 * before/after screenshots, not just RAM-diff logs (which only prove a
 * write landed, not that it survived to render). Root cause: this engine's
 * own per-frame routine (disassembled at $9539-$9545) re-copies the cursor
 * position from cartridge WRAM into OAM every single frame:
 *   LDA $7116 ; STA $0207   (X: WRAM -> OAM)
 *   LDA $7118 ; STA $0204   (Y: WRAM -> OAM)
 * so writing OAM directly just gets clobbered by this copy one frame later.
 * $7116/$7118 are the actual authoritative position -- write there instead
 * and let the game's own routine do the OAM copy (which also means we no
 * longer need to scan OAM for the cursor's slot before positioning it). */
#define CURSOR_SRC_X 0x7116
#define CURSOR_SRC_Y 0x7118

static uint8_t *ram_byte(uint16_t addr) {
    return (uint8_t *)&cyc_cpu_ram()[addr & 0x7FF];
}

/* Cartridge WRAM ($6000-$7FFF), NOT the 2KB console RAM ram_byte() above
 * targets -- a completely different backing array in NESRecomp (hw_cart.wram
 * via cyc_cart_ram()). Needed because the authoritative cursor position
 * lives here, not in OAM -- see CURSOR_SRC_X/Y below. Returns NULL if addr
 * is outside the cartridge's actual WRAM window. */
static uint8_t *cart_ram_byte(uint16_t addr) {
    if (addr < 0x6000) return NULL;
    size_t len = 0;
    const uint8_t *wram = cyc_cart_ram(&len);
    size_t off = (size_t)(addr - 0x6000);
    if (!wram || off >= len) return NULL;
    return (uint8_t *)&wram[off];
}

static bool find_cursor_oam(uint16_t *y_addr, uint16_t *x_addr) {
    for (uint16_t base = 0x0200; base < 0x0300; base += 4) {
        if (*ram_byte(base + 1) == CURSOR_TILE) {
            *y_addr = base;
            *x_addr = base + 3;
            return true;
        }
    }
    return false;
}

/* File log (2026-11-06, user request: "track it yourself" instead of
 * relaying console text by hand): writes next to the exe so a developer
 * can pull it over SSH after a play session instead of the player having
 * to transcribe console output live. Opened lazily on first use, line-
 * buffered via fflush after every write so a force-closed game (Alt+F4,
 * crash) doesn't lose the tail of the session. */
static FILE *s_dbg_log = NULL;
static int   s_dbg_log_state = -1;

static FILE *dbg_log(void) {
    if (s_dbg_log_state < 0) {
        s_dbg_log = fopen("mouse_debug.log", "a");
        s_dbg_log_state = s_dbg_log ? 1 : 0;
        if (s_dbg_log) fprintf(s_dbg_log, "=== session start ===\n");
    }
    return s_dbg_log_state == 1 ? s_dbg_log : NULL;
}

#ifdef CYC_WITH_SDL
static bool s_cursor_hidden = false;

/* SDL_GetMouseFocus()/SDL_GetMouseState() require the window to actually
 * hold OS mouse-focus (hover + active) -- if the game launches behind/
 * alongside a console window and the player never clicks into the SDL
 * window specifically, focus-gated queries silently return null/zero
 * forever: frozen cursor AND dead buttons, exactly the 2026-11-06 report.
 * SDL_GetGlobalMouseState() is focus-independent (real desktop cursor
 * position + buttons, works whether or not this window is focused) so it's
 * used for everything here instead. The one thing it can't give us is
 * *which* window to measure against -- SDL_GetMouseFocus() is tried first
 * since it's accurate when available, falling back to SDL_GetWindowFromID
 * (window id 1 -- nes_game creates exactly one window) when focus hasn't
 * been established yet, so cursor tracking and clicks work from frame one
 * without requiring an extra focusing click. */
static SDL_Window *find_game_window(void) {
    SDL_Window *win = SDL_GetMouseFocus();
    if (win) return win;
    return SDL_GetWindowFromID(1);
}

/* 2026-11-06 report: "enter is picked up by console" -- run.bat launches
 * nes_game.exe directly inside the same cmd window it opened in (no
 * `start`), and on this user's machine the console appears to keep OS
 * input focus even after the SDL window is created and shown, so neither
 * keyboard input nor mouse hover ever reaches the game window. A tested
 * Wine+Xvfb run of this exact exe confirmed the hook logic itself is
 * correct (cursor tracked, click and Start both produced real RAM
 * changes) once the window actually has focus -- this is purely a
 * focus-stealing problem on that machine, not a logic bug. SDL_RaiseWindow
 * asks the OS to bring the window to the front and give it focus; done
 * once, the first time we see a real window, so it doesn't fight the user
 * if they deliberately switch away afterward. */
static bool s_raised_once = false;
static void raise_once(SDL_Window *win) {
    if (!win || s_raised_once) return;
    SDL_RaiseWindow(win);
    s_raised_once = true;
}

/* Without a grab, SDL_ShowCursor(SDL_DISABLE) only swaps the cursor while
 * it's inside the window -- the instant it strays past the edge Windows
 * shows the normal arrow again and the user can wander off into other
 * windows (2026-11-06 report: "mouse stays behind screen and moves around
 * in windows"). SDL_SetWindowGrab confines the OS cursor to the window
 * while it has focus, same as any other windowed game's mouse capture.
 *
 * Must track focus every call, not just set-once: an unconditional grab
 * (and the warp in apply_absolute_cursor below) blocks Alt+Tab entirely --
 * confirmed by 2026-11-06 report "can not alt tab out of game and mouse
 * wont extend out from window". Releasing grab the instant the window
 * loses focus lets the OS give focus to whatever's being switched to. */
static bool s_grabbed = false;
static void sync_grab(SDL_Window *win) {
    bool focused = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
    if (focused == s_grabbed) return;
    SDL_SetWindowGrab(win, focused ? SDL_TRUE : SDL_FALSE);
    s_grabbed = focused;
}

enum { DI_MUSIC, DI_SFX, DI_FULLSCREEN, DI_FILTER, DI_SCALE, DI_SHAPE, DI_BACK, DI_COUNT };
/* 2026-11-10 (user): only Sharp and CRT. Sharp is pixel-perfect: the largest
 * whole-number scale that fits (6x on 1440p, 4x on 1080p). Smooth and CRT
 * Curved were removed; config values 1 (Smooth) / 3 (CRT Curved) load as
 * Sharp / CRT. Saved as 0 = Sharp, 2 = CRT (the old numbers), 4 = CRT Strong
 * (added 2026-11-10: thinner beams, darker gaps; see crt_filter.c k_looks). */
enum { FILTER_SHARP, FILTER_CRT, FILTER_CRT_STRONG, FILTER_COUNT };
static const char *const k_filter_names[FILTER_COUNT] = { "Sharp", "CRT", "CRT Strong" };
static int  s_filter = FILTER_SHARP;
static int  s_music_vol = 100, s_sfx_vol = 100;   /* percent, steps of 5 */
static int  s_vol_drag = -1;                      /* volume row being dragged */
static int  s_vol_req_row = -1, s_vol_req_value = 0;   /* queued from events */
static bool s_display_inited = false;
static bool s_req_filter_cycle = false;
/* 2026-11-10 (user): Sharp scale choice. 0 = Auto (largest whole scale that
 * fits), else a fixed 2x..6x; a fixed scale that doesn't fit the screen uses
 * Auto's. Saved as [Game] Scale. CRT: Auto fills the screen (fractional);
 * a fixed scale draws the CRT screen at exactly k x 256 by k x 240. */
static int  s_scale = 0;
static int  s_scale_req_dir = 0;                  /* queued from events */
/* 2026-11-10 (user): picture shape. 0 = square pixels, 1 = TV (4:3): NES
 * pixels 8:7 wide, as on a real TV. With Sharp at TV shape the picture goes
 * through the filter module's sharp-bilinear style (crt_set_style) instead
 * of the host's integer scaling. Saved as [Game] Aspect. */
static int  s_aspect = 0;
static bool s_shape_req = false;                  /* toggle queued from events */
static bool host_draws_sharp(void);
/* what the last present handed the host, for mapping the mouse back */
enum { PRES_HOST, PRES_CANVAS, PRES_CRT, PRES_MENU };
static int  s_pres_kind = PRES_HOST;
static int  s_pres_w = 256, s_pres_h = 240;    /* the picture (logical) size */
static int  s_pres_cw = 256;                   /* the game canvas width under it */

/* Manual letterbox math (scale = min(w/256,h/240), center offset) was
 * wrong in practice (2026-11-06 report: "cursor moves but not accurately")
 * -- SDL_WINDOW_ALLOW_HIGHDPI means SDL_GetWindowSize() returns logical
 * points while the renderer's actual output is in physical pixels, which
 * can differ by the OS DPI scale factor, and SDL_RenderSetIntegerScale
 * (if the user has that option on) clamps the real scale to a whole
 * number our fractional math didn't know about -- either one throws off
 * the magnitude (not direction) of the mapping, matching the symptom.
 * SDL_RenderWindowToLogical (SDL >= 2.0.18, present in this project's
 * bundled headers) does this conversion internally using the renderer's
 * own actual state, so it's used instead of hand-rolled math. */
/* 2026-11-10: keep the mouse inside the game picture (user: "only go to
 * edge of graphics window, not beyond"): the black bars around the picture
 * (fullscreen / resized window) are out of bounds. Window coordinates of
 * the presented picture's game area, applied with SDL_SetWindowMouseRect
 * while the game owns the mouse; cleared while the Esc menu is open. */
static SDL_Rect s_confine = { 0, 0, 0, 0 };
static bool     s_confined = false;

static void confine_clear(SDL_Window *win) {
    if (s_confined && win) SDL_SetWindowMouseRect(win, NULL);
    s_confined = false;
}

static void confine_mouse(SDL_Window *win) {
    if (!win) return;
    SDL_Renderer *ren = SDL_GetRenderer(win);
    if (!ren || !s_grabbed || s_pres_kind == PRES_MENU) { confine_clear(win); return; }
    float x0 = 0, y0 = 0, w = (float)s_pres_w, h = (float)s_pres_h;
    if (s_pres_kind == PRES_CRT) {
        /* the CRT filter's own screen rect (fill, or a fixed scale) */
        int rx, ry, rw, rh;
        crt_fit_rect(s_pres_w, s_pres_h, s_pres_cw, 240, &rx, &ry, &rw, &rh);
        x0 = (float)rx; y0 = (float)ry; w = (float)rw; h = (float)rh;
    }
    int ax, ay, bx, by;
    SDL_RenderLogicalToWindow(ren, x0, y0, &ax, &ay);
    SDL_RenderLogicalToWindow(ren, x0 + w, y0 + h, &bx, &by);
    SDL_Rect r = { ax, ay, bx - ax, by - ay };
    if (r.w <= 0 || r.h <= 0) { confine_clear(win); return; }
    /* Windows drops the cursor clip on some events (seen on the Windows test PC: the
     * pointer reached x 3765, the next monitor), so re-apply it every 30
     * calls even when the rect is unchanged */
    static int refresh = 0;
    bool same = s_confined && r.x == s_confine.x && r.y == s_confine.y && r.w == s_confine.w && r.h == s_confine.h;
    if (same && ++refresh < 30) return;
    refresh = 0;
    if (SDL_SetWindowMouseRect(win, &r) == 0) { s_confine = r; s_confined = true; }
}

/* Backstop, every frame: if the pointer is outside the picture anyway, put
 * it back on the nearest edge (global coordinates, so it also catches a
 * pointer that already left the window). */
static void confine_warp_back(SDL_Window *win) {
    if (!win || !s_confined || !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) return;
    int wx = 0, wy = 0, gx = 0, gy = 0;
    SDL_GetWindowPosition(win, &wx, &wy);
    SDL_GetGlobalMouseState(&gx, &gy);
    int x0 = wx + s_confine.x, y0 = wy + s_confine.y;
    int x1 = x0 + s_confine.w - 1, y1 = y0 + s_confine.h - 1;
    int nx = gx < x0 ? x0 : gx > x1 ? x1 : gx;
    int ny = gy < y0 ? y0 : gy > y1 ? y1 : gy;
    if (nx != gx || ny != gy) SDL_WarpMouseGlobal(nx, ny);
}

/* 2026-11-10: the room's drawn extent on screen. At the end of a scrolling
 * room the view runs past the room (the yard at its right end shows one blank
 * 8-px column), so the cursor must not go there (user: mouse went past the
 * play window on scrolling screens, side depending on direction). From the
 * frame the PPU drew: room-band lines (nametable rows 4-19, the lower
 * nametables folded back, vertical mirroring), their scroll X, and the room
 * width ($63B5; room column s = nametable column s+2). */
static uint8_t wram(uint16_t addr);
static bool room_extent(int *y0, int *y1, int *x0, int *x1) {
    if (!wram(0x6D41)) return false;               /* no room/UI split: not a room */
    int width = wram(0x63B5);
    if (width < 1 || width > 62) return false;
    int first = -1, last = -1, sx = 0;
    bool clip = false;
    for (int y = 0; y < 240; y++) {
        uint8_t mask = cyc_render_line_mask(y);
        if (!(mask & 0x08)) continue;
        int sy = cyc_render_line_scroll_y(y);
        if (sy >= 240) sy -= 240;
        if (sy >= 240 || (sy >> 3) < 4 || (sy >> 3) > 19) continue;
        if (first < 0) { first = y; sx = cyc_render_line_scroll_x(y); clip = !(mask & 0x02); }
        last = y;
    }
    if (first < 0) return false;
    int l = 16 - sx, r = (width + 2) * 8 - sx;
    if (clip && l < 8) l = 8;
    *x0 = l < 0 ? 0 : l;
    *x1 = (r > 256 ? 256 : r) - 1;
    *y0 = first;
    *y1 = last;
    return *x1 > *x0;
}

static uint8_t s_unclamp_x, s_unclamp_y;
static bool    s_unclamp_on = false;   /* mouse is driving the cursor */

static void apply_absolute_cursor(SDL_Window *win) {
    if (!s_cursor_hidden) {
        SDL_ShowCursor(SDL_DISABLE); /* the game draws its own cursor sprite */
        s_cursor_hidden = true;
    }
    if (!win) return; /* no window found at all yet: leave cursor where it was */
    SDL_Renderer *ren = SDL_GetRenderer(win);
    if (!ren) return;
    /* 2026-11-06: writing $7116/$7118 unconditionally every frame (no gate)
     * stomped WRAM during non-cursor screens (transitions, cutscenes) where
     * this engine evidently reuses that address for something else entirely
     * -- caused a hang into a black screen after picking a kid at the
     * character-select screen. find_cursor_oam() (still scanning OAM for
     * tile $FA, same heuristic the old OAM-direct-write approach used) is
     * restored here as a cheap "is a live cursor actually on screen this
     * frame" gate: only touch cartridge WRAM when it still is. */
    uint16_t y_oam, x_oam;
    if (!find_cursor_oam(&y_oam, &x_oam)) return;
    uint8_t *src_x = cart_ram_byte(CURSOR_SRC_X);
    uint8_t *src_y = cart_ram_byte(CURSOR_SRC_Y);
    if (!src_x || !src_y) return; /* WRAM not mapped this build/frame */

    /* 2026-11-06: computing window-relative coords as
     * (SDL_GetGlobalMouseState() - SDL_GetWindowPosition()) never tracked
     * at all in testing -- global position and window position can live in
     * different coordinate spaces on a mixed-DPI/multi-monitor Windows
     * setup (SDL_GetGlobalMouseState goes through GetCursorPos; window
     * position/size under SDL_WINDOW_ALLOW_HIGHDPI can be reported in a
     * different scale), so the subtraction landed thousands of px outside
     * the window every single frame regardless of where the real cursor
     * was on screen. SDL_GetMouseState() already returns window-relative
     * coordinates directly (no subtraction, no cross-space mismatch
     * possible) when this window has mouse focus -- this is what the very
     * first build used, and it tracked (just needed the scale fix below).
     * Only update while actually hovering this window; otherwise leave the
     * cursor where it was rather than guess from a global position that
     * may not correspond to this window at all. */
    if (SDL_GetMouseFocus() != win) return;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);

    /* 2026-11-06: this write ran unconditionally every frame regardless of
     * D-pad input, so it permanently fought (and always won against) any
     * d-pad-driven cursor movement the ROM's own code tried to do -- the
     * game would update its internal position in response to a D-pad
     * press, then this immediately stomped it back to the stale mouse
     * position on the very next frame, net result: zero visible movement.
     * Confirmed on the character-select screen (arrow keys never moved
     * the portrait highlight at all under the old unconditional write).
     * Fix: only overwrite the position when the mouse has actually moved
     * since the last frame -- an idle mouse no longer re-asserts a stale
     * position over whatever the D-pad is doing. */
    static int s_last_mx = -1, s_last_my = -1;
    bool mouse_moved = (mx != s_last_mx) || (my != s_last_my);
    s_last_mx = mx;
    s_last_my = my;
    if (!mouse_moved) return;

    float lx = 0.0f, ly = 0.0f;
    SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly);
    /* 2026-11-10: logical space is the presented picture: the game canvas
     * (wide when widescreen is on), or the CRT filter's window-resolution
     * picture. Map back to the canvas, then to native columns. */
    if (s_pres_kind == PRES_CRT) {
        float cx = 0, cy = 0;
        crt_map_point(s_pres_w, s_pres_h, s_pres_cw, 240, false, lx, ly, &cx, &cy);
        lx = cx;
        ly = cy;
    }
    if (s_pres_cw > 256) {
        int ly_i = (int)ly;
        (void)ly_i;
        lx -= (float)((s_pres_cw - 256) / 2);
    }
    /* 2026-11-10: full screen reach (user). The game clamps the cursor to
     * X 16..239 / Y 35..222 every frame in cursor_move_dpad; the
     * cursor-unclamp hook at $9539 restores these values right after.
     * Y stops at 238: a sprite at 239+ is hidden. */
    if (lx < 0) lx = 0; else if (lx > 255) lx = 255;
    if (ly < 0) ly = 0; else if (ly > 238) ly = 238;
    {   /* over the room: stay on the room's drawn columns */
        int ry0, ry1, rx0, rx1;
        if (room_extent(&ry0, &ry1, &rx0, &rx1)) {
            /* 2026-11-10 (user): not into the text lines above the room
             * (dialogue / messages); stop at the room's first line */
            if (ly < ry0) ly = (float)ry0;
            if (ly >= ry0 && ly <= ry1) {
                if (lx < rx0) lx = (float)rx0;
                if (lx > rx1) lx = (float)rx1;
            }
        }
    }
    *src_x = (uint8_t)lx;
    *src_y = (uint8_t)ly;
    s_unclamp_x = (uint8_t)lx;
    s_unclamp_y = (uint8_t)ly;
    s_unclamp_on = true;
}

/* ------------------------------------------------------------------------
 * 2026-11-09: Escape menu (SAVE GAME / LOAD GAME / RESUME).
 *
 * This build has no runtime menu (recomp-ui isn't compiled in), so Escape
 * did nothing in the host. The menu is ours, drawn through
 * CycHostExtras.present over a frozen copy of the frame:
 *   - Opening takes an in-memory save state (cyc_state_save). The machine
 *     keeps running behind the menu with all input withheld; RESUME (or
 *     Escape) restores the snapshot, so the game is effectively paused.
 *   - Save State / Load State open a slot list (10 slots since 2026-11-10, scrolling;
 *     with screenshots); see the slot notes below.
 * A save state belongs to the exact executable that wrote it (cyc_state.h):
 * a different build refuses it, reported as "different version". The
 * game's own Start-menu battery save is unaffected and survives updates.
 * ---------------------------------------------------------------------- */
static uint8_t wram(uint16_t addr);   /* defined with the context-RMB code */
#define MENU_W 256
#define MENU_H 240
/* PCSX2-style pause sidebar (user choice): translucent panel down the left,
 * game lightly dimmed on the right, anti-aliased TrueType text rendered at
 * the window's real resolution (menu_render.c). */
enum { MI_RESUME, MI_SAVE, MI_LOAD, MI_DISPLAY, MI_EXIT, MI_COUNT };
static const char *const k_menu_rows[MI_COUNT] = { "Resume Game", "Save State", "Load State", "Options", "Exit Game" };

/* 2026-11-10: display options (Display sub-menu, F9 filter, F10 widescreen,
 * F11 fullscreen via the host). Filters: Sharp = nearest + integer scale,
 * Smooth = linear, CRT / CRT Curved = crt_filter.c at window resolution. */


/* 2026-11-09: multiple save slots (user request), slots 1..6 (1..10 since
 * 2026-11-10, with a scrolling list). The game's
 * own Start-menu save also writes the most recently saved slot
 * (follow_game_save; Slot 1 if none yet). Each slot = <exe dir>/ManiacMansion.<name>.state (cyc_state file)
 * plus a .thumb sidecar (screenshot shown in the slot list). The old
 * single-file ManiacMansion.state is no longer used (any snapshot only
 * loads in the exe that wrote it anyway). */
enum { MODE_MAIN, MODE_SAVE, MODE_LOAD, MODE_DISPLAY };
#define NUM_SLOTS 10
#define SLOT_ROWS_VISIBLE 7   /* slot list rows on screen; the rest scroll (wheel, Up/Down) */
#define THUMB_W 128
#define THUMB_H 120
typedef struct {
    bool     used;
    time_t   mtime;
    bool     thumb_ok;
    uint32_t thumb[THUMB_W * THUMB_H];
} SlotInfo;
static SlotInfo s_slots[NUM_SLOTS + 1];

static int      s_game_saved_frames = -1;  /* -1 idle, else frames since the game's own save */
static int      s_toast_frames = 0;        /* "SAVED TO SLOT N" banner countdown */
static char     s_toast_text[32] = "";
static int      s_game_thumb_frames = -1;  /* frames until the game-save slot's screenshot is taken */
static int      s_game_thumb_slot = 0;
static bool     s_menu_open = false;
static bool     s_menu_guard = false;          /* swallow input until all released */
static int      s_menu_mode = MODE_MAIN;
static int      s_menu_sel = 0;
static int      s_menu_hover = -1;             /* row under the mouse, or -1 */
static int      s_menu_scroll = 0;             /* first row shown in the slot lists */
static int      s_confirm_slot = -1;           /* occupied slot armed for overwrite */
static uint8_t *s_menu_snap = NULL;
static size_t   s_menu_snap_len = 0;
static uint32_t s_menu_frozen[CYC_VIDEO_MAX_WIDTH * MENU_H];   /* game canvas at open */
static int      s_frozen_w = MENU_W;
static uint64_t s_frozen_gen = 0;              /* bumps when s_menu_frozen changes */
static uint32_t s_menu_native[MENU_W * MENU_H];  /* native frame at open (thumbnails) */
static uint32_t s_wide_copy[CYC_VIDEO_MAX_WIDTH * MENU_H];     /* canvas + toast */
static uint32_t *s_crt_pic = NULL;             /* CRT output at window resolution */
static size_t   s_crt_cap = 0;
static uint32_t *s_hi_pic = NULL;              /* window-resolution menu picture */
static int      s_hi_w = 0, s_hi_h = 0;        /* its size = the presented size */
static char     s_menu_status[96] = "";
static uint32_t s_menu_status_rgb = 0x7CE08A;
/* queued from the event callback, run in frame_end (a frame boundary) */
static bool     s_menu_req_toggle = false;
static bool     s_menu_req_back = false;
static int      s_menu_req_activate = -1;

static void slot_file(int slot, const char *ext, char *out, size_t n) {
    char *base = SDL_GetBasePath();
    snprintf(out, n, "%sManiacMansion.slot%d.%s", base ? base : "", slot, ext);
    if (base) SDL_free(base);
}

static void slot_name(int slot, char *out, size_t n) {
    snprintf(out, n, "Slot %d", slot);
}

/* 256x240 frame -> 128x120 thumbnail (2x2 average). */
static void make_thumb(const uint32_t *f, uint32_t *t) {
    for (int y = 0; y < THUMB_H; y++)
        for (int x = 0; x < THUMB_W; x++) {
            const uint32_t *p = f + (size_t)(y * 2) * MENU_W + x * 2;
            uint32_t q[4] = { p[0], p[1], p[MENU_W], p[MENU_W + 1] };
            uint32_t r = 0, g = 0, b = 0;
            for (int i = 0; i < 4; i++) { r += (q[i] >> 16) & 0xFF; g += (q[i] >> 8) & 0xFF; b += q[i] & 0xFF; }
            t[y * THUMB_W + x] = 0xFF000000u | ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
        }
}

static void write_thumb(int slot, const uint32_t *frame) {
    static uint32_t t[THUMB_W * THUMB_H];
    char path[1100];
    if (!frame) return;
    make_thumb(frame, t);
    slot_file(slot, "thumb", path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite("MMT1", 1, 4, f);
    fwrite(t, sizeof t, 1, f);
    fclose(f);
}

static void refresh_slots(void) {
    for (int s = 0; s <= NUM_SLOTS; s++) {
        SlotInfo *si = &s_slots[s];
        char path[1100];
        struct stat st;
        slot_file(s, "state", path, sizeof path);
        si->used = stat(path, &st) == 0;
        si->mtime = si->used ? st.st_mtime : 0;
        si->thumb_ok = false;
        if (!si->used) continue;
        slot_file(s, "thumb", path, sizeof path);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char magic[4];
        si->thumb_ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "MMT1", 4) &&
                       fread(si->thumb, sizeof si->thumb, 1, f) == 1;
        fclose(f);
    }
}

static void slot_time_text(int slot, char *out, size_t n) {
    if (!s_slots[slot].used) { snprintf(out, n, "Empty"); return; }
    struct tm *tm = localtime(&s_slots[slot].mtime);
    if (!tm) { snprintf(out, n, "Saved"); return; }
    strftime(out, n, "%b %d, %H:%M", tm);
}

/* Rows per mode: slots 1..NUM_SLOTS + Back. */
static int menu_row_count(void) {
    if (s_menu_mode == MODE_DISPLAY) return DI_COUNT;
    if (s_menu_mode != MODE_MAIN) return NUM_SLOTS + 1;
    return MI_COUNT;
}
/* Rows drawn at once: the slot lists scroll, the other menus fit. */
static int menu_visible_rows(void) {
    int n = menu_row_count();
    return (s_menu_mode == MODE_SAVE || s_menu_mode == MODE_LOAD) && n > SLOT_ROWS_VISIBLE ? SLOT_ROWS_VISIBLE : n;
}
static void menu_scroll_clamp(void) {
    int max = menu_row_count() - menu_visible_rows();
    if (s_menu_scroll > max) s_menu_scroll = max;
    if (s_menu_scroll < 0) s_menu_scroll = 0;
}
/* scroll so the selected row is on screen */
static void menu_keep_visible(void) {
    int vis = menu_visible_rows();
    if (s_menu_sel < s_menu_scroll) s_menu_scroll = s_menu_sel;
    if (s_menu_sel >= s_menu_scroll + vis) s_menu_scroll = s_menu_sel - vis + 1;
    menu_scroll_clamp();
}
/* slot for a row in the slot lists, -1 = Back */
static int row_slot(int row) {
    int slot = 1 + row;
    return slot <= NUM_SLOTS ? slot : -1;
}

/* Sidebar geometry for a presented size (all derived from the height). */
typedef struct { int sw, pad, item_y0, item_h, item_gap, title_y, sub_y, sep_y; float u; } MenuLayout;
static MenuLayout menu_layout(int w, int h) {
    MenuLayout L;
    L.u = (float)h / 240.0f;
    L.sw = (int)(w * 0.40f);
    {   /* on a wide picture, keep the sidebar the width it has at 256:240 */
        int cap = (int)(h * 256.0f / 240.0f * 0.40f);
        if (L.sw > cap) L.sw = cap;
    }
    L.pad = (int)(10 * L.u);
    L.title_y = (int)(16 * L.u);
    L.sub_y = (int)(33 * L.u);
    L.sep_y = (int)(46 * L.u);
    if (s_menu_mode == MODE_MAIN || s_menu_mode == MODE_DISPLAY) {
        L.item_y0 = (int)(58 * L.u);
        /* Options has 7 rows: a little tighter so they clear the footer */
        L.item_h = (int)((s_menu_mode == MODE_DISPLAY ? 15 : 17) * L.u);
        L.item_gap = (int)((s_menu_mode == MODE_DISPLAY ? 2 : 3) * L.u);
    } else {
        L.item_y0 = (int)(54 * L.u);
        L.item_h = (int)(14 * L.u);
        L.item_gap = (int)(2 * L.u);
    }
    return L;
}

static int menu_row_at(int x, int y) {
    if (s_hi_w <= 0) return -1;
    MenuLayout L = menu_layout(s_hi_w, s_hi_h);
    if (x < L.pad / 2 || x >= L.sw - L.pad / 2) return -1;
    int vis = menu_visible_rows();
    for (int k = 0; k < vis; k++) {
        int y0 = L.item_y0 + k * (L.item_h + L.item_gap);
        if (y >= y0 && y < y0 + L.item_h) return s_menu_scroll + k;
    }
    return -1;
}

static void menu_log(const char *what, const char *detail) {
    FILE *log = dbg_log();
    if (log) { fprintf(log, "[menu] %s%s%s\n", what, detail ? ": " : "", detail ? detail : ""); fflush(log); }
}

static void menu_os_cursor(bool on);

static void menu_close(void) {
    free(s_menu_snap);
    s_menu_snap = NULL;
    s_menu_snap_len = 0;
    s_menu_open = false;
    s_menu_guard = true;
    menu_os_cursor(false);
}

/* After any state load: the hook's preview tracking must match the RAM the
 * state brought back (a snapshot taken while hovering holds a preview verb). */
static void after_state_load(void);

static bool restore_snapshot(char *err, size_t n) {
    if (!s_menu_snap) { snprintf(err, n, "no snapshot"); return false; }
    if (!cyc_state_load(s_menu_snap, s_menu_snap_len, err, n)) return false;
    cyc_session_state_loaded();
    after_state_load();
    return true;
}

static void menu_status(const char *msg, uint32_t rgb) {
    snprintf(s_menu_status, sizeof s_menu_status, "%s", msg);
    s_menu_status_rgb = rgb;
}

static void menu_set_mode(int mode) {
    s_menu_mode = mode;
    s_menu_hover = -1;
    s_menu_scroll = 0;
    s_confirm_slot = -1;
    s_menu_status[0] = 0;
    if (mode == MODE_MAIN) return;
    if (mode == MODE_DISPLAY) { s_menu_sel = 0; return; }
    refresh_slots();
    /* start on the newest save (load) / first empty slot (save) */
    int best = -1;
    for (int s = 1; s <= NUM_SLOTS; s++) {
        if (mode == MODE_SAVE && !s_slots[s].used) { best = s; break; }
        if (mode == MODE_LOAD && s_slots[s].used && (best < 0 || s_slots[s].mtime > s_slots[best].mtime)) best = s;
    }
    if (best < 0) best = 1;
    s_menu_sel = best - 1;
    menu_keep_visible();
}

/* The composed game picture (wide when widescreen is on) behind the menu. */
static void grab_frozen(void) {
    int fw = 256, fh = 240;
    const uint32_t *p = cyc_render_present(&fw, &fh);
    if (!p || fw < 256 || fw > CYC_VIDEO_MAX_WIDTH || fh != MENU_H) { fw = 256; p = cyc_frame_argb(); }
    if (p) memcpy(s_menu_frozen, p, (size_t)fw * MENU_H * 4);
    else memset(s_menu_frozen, 0, (size_t)fw * MENU_H * 4);
    s_frozen_w = fw;
    s_frozen_gen++;
}

static void menu_open_now(void) {
    char err[160];
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!cyc_state_save(&buf, &len, err, sizeof err)) { menu_log("open failed", err); return; }
    s_menu_snap = buf;
    s_menu_snap_len = len;
    grab_frozen();
    const uint32_t *f = cyc_frame_argb();
    if (f) memcpy(s_menu_native, f, sizeof s_menu_native);
    else memset(s_menu_native, 0, sizeof s_menu_native);
    s_menu_open = true;
    menu_set_mode(MODE_MAIN);
    refresh_slots();
    s_menu_sel = MI_RESUME;
    menu_os_cursor(true);
    menu_log("open", NULL);
}

static void slot_activate(int slot) {
    char err[160] = "", path[1100], name[32], msg[96];
    slot_name(slot, name, sizeof name);
    slot_file(slot, "state", path, sizeof path);
    if (s_menu_mode == MODE_SAVE) {
        if (s_slots[slot].used && s_confirm_slot != slot) {
            s_confirm_slot = slot;
            snprintf(msg, sizeof msg, "Click again to overwrite %s", name);
            menu_status(msg, 0xF0C060);
            return;
        }
        s_confirm_slot = -1;
        /* rewind to the paused moment, then write it */
        if (restore_snapshot(err, sizeof err) && cyc_state_save_file(path, err, sizeof err)) {
            write_thumb(slot, s_menu_native);
            menu_set_mode(MODE_MAIN);
            s_menu_sel = MI_RESUME;
            snprintf(msg, sizeof msg, "Saved to %s", name);
            menu_status(msg, 0x7CE08A);
            menu_log("saved", path);
        } else {
            menu_status("Save failed", 0xF07070);
            menu_log("save failed", err);
        }
        refresh_slots();
    } else {
        if (!s_slots[slot].used) {
            snprintf(msg, sizeof msg, "%s is empty", name);
            menu_status(msg, 0xF0C060);
            return;
        }
        if (cyc_state_load_file(path, err, sizeof err)) {
            cyc_session_state_loaded();
            after_state_load();
            menu_log("loaded", path);
            menu_set_mode(MODE_MAIN);
            menu_close();
        } else {
            snprintf(msg, sizeof msg, "%s is from a different version", name);
            menu_status(msg, 0xF07070);
            menu_log("load failed", err);
        }
    }
}

static void menu_back(void);
static void display_activate(int item);

static void menu_activate(int item) {
    char err[160] = "";
    if (s_menu_mode == MODE_DISPLAY) {
        if (item == DI_BACK) menu_back();
        else display_activate(item);
        return;
    }
    if (s_menu_mode != MODE_MAIN) {
        int slot = row_slot(item);
        if (slot < 0) { menu_back(); return; }
        slot_activate(slot);
        return;
    }
    if (item == MI_SAVE) {
        menu_set_mode(MODE_SAVE);
    } else if (item == MI_LOAD) {
        menu_set_mode(MODE_LOAD);
    } else if (item == MI_DISPLAY) {
        menu_set_mode(MODE_DISPLAY);
    } else if (item == MI_EXIT) {
        /* back to the paused moment first (the battery save the host writes
         * on exit then matches what was on screen), then a normal quit */
        if (!restore_snapshot(err, sizeof err)) menu_log("exit restore failed", err);
        menu_log("exit", NULL);
        menu_close();
        SDL_Event q;
        memset(&q, 0, sizeof q);
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    } else {
        if (!restore_snapshot(err, sizeof err)) menu_log("resume restore failed", err);
        menu_log("resume", NULL);
        menu_close();
    }
}

/* Leave a slot list (Esc / right-click / Back): back to the main rows. */
static void menu_back(void) {
    int from = s_menu_mode;
    menu_set_mode(MODE_MAIN);
    s_menu_sel = from == MODE_SAVE ? MI_SAVE : from == MODE_LOAD ? MI_LOAD : MI_DISPLAY;
}

/* "Last saved" line: the newest slot. */
static void last_saved_text(char *out, size_t n) {
    int best = -1;
    for (int s = 0; s <= NUM_SLOTS; s++)
        if (s_slots[s].used && (best < 0 || s_slots[s].mtime > s_slots[best].mtime)) best = s;
    if (best < 0) { snprintf(out, n, "No saves yet"); return; }
    char when[48], name[32];
    slot_time_text(best, when, sizeof when);
    slot_name(best, name, sizeof name);
    snprintf(out, n, "Last saved %s (%s)", when, name);
}

/* Size to present the menu at: the window's real drawable, letterboxed to
 * the game picture's aspect, so the texture maps ~1:1 and text is crisp. */
static void drawable_size(int *w, int *h) {
    int ow = 0, oh = 0;
    SDL_Window *win = find_game_window();
    SDL_Renderer *ren = win ? SDL_GetRenderer(win) : NULL;
    if (ren) SDL_GetRendererOutputSize(ren, &ow, &oh);
    *w = ow;
    *h = oh;
}

static void menu_target_size(int *w, int *h) {
    int ow = 0, oh = 0, fw = s_frozen_w;
    drawable_size(&ow, &oh);
    if (ow < fw || oh < MENU_H) { ow = fw * 3; oh = MENU_H * 3; }
    float sx = (float)ow / fw, sy = (float)oh / MENU_H, sc = sx < sy ? sx : sy;
    int tw = (int)(fw * sc + 0.5f), th = (int)(MENU_H * sc + 0.5f);
    if (tw > 4096) { th = th * 4096 / tw; tw = 4096; }
    if (th > 4096) { tw = tw * 4096 / th; th = 4096; }
    *w = tw;
    *h = th;
}

/* Font size for s at up to px, shrunk so it fits in avail pixels. */
static float fit_px(float px, int avail, const char *s) {
    int tw = mr_text_width(px, s);
    return (tw > avail && tw > 0) ? px * avail / tw : px;
}

/* Right-hand preview in the slot lists: the selected slot's screenshot. */
static void draw_slot_preview(uint32_t *d, int pw, int ph, const MenuLayout *L) {
    float u = L->u;
    int slot = row_slot(s_menu_sel);
    int area_x = L->sw, area_w = pw - L->sw;
    int bw = (int)(area_w * 0.80f), bh = bw * MENU_H / MENU_W;
    int bx = area_x + (area_w - bw) / 2, by = L->item_y0;
    if (by + bh > ph - (int)(30 * u)) { bh = ph - (int)(30 * u) - by; bw = bh * MENU_W / MENU_H; bx = area_x + (area_w - bw) / 2; }
    if (slot < 0) return;
    int b = (int)u > 1 ? (int)u : 1;
    mr_rect(d, pw, ph, bx - 2 * b, by - 2 * b, bw + 4 * b, bh + 4 * b, 0x15161D, 230);
    mr_rect(d, pw, ph, bx - b, by - b, bw + 2 * b, bh + 2 * b, 0x2C6FE0, 220);
    char name[32], when[48], cap[96];
    slot_name(slot, name, sizeof name);
    slot_time_text(slot, when, sizeof when);
    if (s_slots[slot].used && s_slots[slot].thumb_ok) {
        mr_blit_scaled(d, pw, ph, bx, by, bw, bh, s_slots[slot].thumb, THUMB_W, THUMB_H);
    } else {
        mr_rect(d, pw, ph, bx, by, bw, bh, 0x0C0D12, 255);
        const char *t = s_slots[slot].used ? "No preview" : "Empty slot";
        float px = 9.0f * u;
        mr_text(d, pw, ph, bx + (bw - mr_text_width(px, t)) / 2, by + (bh - (int)px) / 2, px, 0x7D8296, 255, t);
    }
    snprintf(cap, sizeof cap, "%s   %s", name, when);
    mr_text(d, pw, ph, bx, by + bh + (int)(6 * u), fit_px(7.0f * u, bw, cap), 0xFFFFFF, 255, cap);
}

/* Gameplay picture: the host's own (NULL), the canvas with a toast on it,
 * or the CRT filter's at window resolution. */
static const uint32_t *display_present(int *w, int *h) {
    bool crt = !host_draws_sharp();
    int cw = 256, ch = 240;
    /* no window yet: the host sizes the new window from this first picture,
     * so give it the plain one */
    SDL_Window *win = find_game_window();
    if (!win || !SDL_GetRenderer(win)) crt = false;
    if (!crt && s_toast_frames <= 0) {
        s_pres_kind = PRES_HOST;
        s_pres_cw = s_pres_w = cyc_video_width();
        s_pres_h = 240;
        return NULL;
    }
    const uint32_t *pic = cyc_render_present(&cw, &ch);
    if (!pic || cw > CYC_VIDEO_MAX_WIDTH || ch != 240) return NULL;
    if (s_toast_frames > 0) {
        memcpy(s_wide_copy, pic, (size_t)cw * ch * 4);
        cyc_overlay_toast(s_wide_copy, cw, ch, s_toast_text, "");
        pic = s_wide_copy;
    }
    if (!crt) {
        s_pres_kind = PRES_CANVAS;
        s_pres_cw = s_pres_w = cw;
        s_pres_h = ch;
        *w = cw;
        *h = ch;
        return pic;
    }
    int dw = 0, dh = 0;
    drawable_size(&dw, &dh);
    if (dw < cw || dh < ch) { dw = cw * 3; dh = ch * 3; }
    /* The filter runs on the CPU inside the frame: render at most s_crt_cap_h
     * lines and let the GPU scale the rest (linear, see apply_filter). The
     * cap adapts: lowered while the filter averages over ~11 ms a frame,
     * raised again while it has room. */
    static int s_crt_cap_h = 1440;
    if (dh > s_crt_cap_h) { dw = (int)((long)dw * s_crt_cap_h / dh); dh = s_crt_cap_h; }
    if (dw > 4096) { dh = dh * 4096 / dw; dw = 4096; }
    size_t need = (size_t)dw * dh;
    if (need > s_crt_cap) {
        uint32_t *np = (uint32_t *)realloc(s_crt_pic, need * 4);
        if (!np) return NULL;
        s_crt_pic = np;
        s_crt_cap = need;
    }
    Uint64 t0 = SDL_GetPerformanceCounter();
    crt_render(s_crt_pic, dw, dh, pic, cw, ch, false);
    {
        static double acc = 0;
        static int n = 0;
        acc += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        if (++n == 30) {
            double avg = acc / n;
            int old = s_crt_cap_h;
            if (avg > 11.0 && s_crt_cap_h > 720) s_crt_cap_h -= 180;
            else if (avg < 6.0 && s_crt_cap_h < 1440 && dh >= s_crt_cap_h) s_crt_cap_h += 180;
            if (old != s_crt_cap_h) {
                char msg[64];
                snprintf(msg, sizeof msg, "%.1f ms -> %d lines", avg, s_crt_cap_h);
                menu_log("crt cap", msg);
            }
            acc = 0;
            n = 0;
        }
    }
    s_pres_kind = PRES_CRT;
    s_pres_cw = cw;
    s_pres_w = dw;
    s_pres_h = dh;
    *w = dw;
    *h = dh;
    return s_crt_pic;
}

static const char *const k_display_rows[DI_COUNT] = { "Music", "Sound FX", "Fullscreen", "Filter", "Scale", "Shape", "Back" };

/* The slider of a volume row in the Options list (false: not a volume row). */
static bool volume_bar(const MenuLayout *L, int row, int *bx, int *by, int *bw, int *bh) {
    if (row != DI_MUSIC && row != DI_SFX) return false;
    /* full-width bar along the bottom of the row, label and value above */
    float u = L->u;
    int y0 = L->item_y0 + row * (L->item_h + L->item_gap);
    *bx = L->pad + (int)(6 * u);
    *bw = L->sw - L->pad / 2 - (int)(6 * u) - *bx;
    *bh = (int)(3 * u) > 2 ? (int)(3 * u) : 2;
    *by = y0 + L->item_h - *bh - (int)(2.5f * u);
    return *bw > 4;
}

/* 2026-11-10: CRT on the GPU (user: the CPU filter used too much CPU,
 * CRT Curved especially). The host presents the texture crt_gpu renders;
 * NULL (menu open, other filters, no OpenGL) falls back to present. */
static void *display_present_texture(void *ctx, void *renderer, int *w, int *h) {
    (void)ctx;
    if (s_menu_open || crt_gpu_failed()) return NULL;
    if (host_draws_sharp()) return NULL;
    int cw = 256, ch = 240;
    const uint32_t *pic = cyc_render_present(&cw, &ch);
    if (!pic || cw > CYC_VIDEO_MAX_WIDTH || ch != 240) return NULL;
    if (s_toast_frames > 0) {
        memcpy(s_wide_copy, pic, (size_t)cw * ch * 4);
        cyc_overlay_toast(s_wide_copy, cw, ch, s_toast_text, "");
        pic = s_wide_copy;
    }
    int dw = 0, dh = 0;
    SDL_GetRendererOutputSize((SDL_Renderer *)renderer, &dw, &dh);
    if (dw < cw || dh < ch) { dw = cw * 3; dh = ch * 3; }
    if (dw > 4096) { dh = dh * 4096 / dw; dw = 4096; }
    if (dh > 4096) { dw = dw * 4096 / dh; dh = 4096; }
    void *t = crt_gpu_render(renderer, pic, cw, ch, dw, dh, false);
    if (!t) return NULL;
    s_pres_kind = PRES_CRT;
    s_pres_cw = cw;
    s_pres_w = dw;
    s_pres_h = dh;
    *w = dw;
    *h = dh;
    return t;
}

static const char *display_row_value(int item, char *buf, size_t n) {
    int fs = 0;
    switch (item) {
    case DI_MUSIC:      snprintf(buf, n, "%d%%", s_music_vol); return buf;
    case DI_SFX:        snprintf(buf, n, "%d%%", s_sfx_vol); return buf;
    case DI_FULLSCREEN: cyc_host_display_get(&fs, NULL, NULL); return fs ? "On" : "Off";
    case DI_FILTER:     return k_filter_names[s_filter];
    case DI_SCALE: {
        int max = cyc_host_display_max_scale();
        if (s_scale <= 0 && s_filter != FILTER_SHARP) return "Auto (fill)";
        if (s_scale <= 0) snprintf(buf, n, "Auto (%dx)", max);
        else if (s_scale > max) snprintf(buf, n, "%dx (fits %dx)", s_scale, max);
        else snprintf(buf, n, "%dx", s_scale);
        return buf;
    }
    case DI_SHAPE:      return s_aspect ? "TV (4:3)" : "Square";
    }
    (void)buf; (void)n;
    return "";
}

static const uint32_t *menu_present(void *ctx, int *w, int *h) {
    (void)ctx;
    if (!s_menu_open) return display_present(w, h);
    s_pres_kind = PRES_MENU;
    if (cyc_video_width() != s_frozen_w) grab_frozen();   /* widescreen changed in the menu */
    int pw, ph;
    menu_target_size(&pw, &ph);
    if (pw != s_hi_w || ph != s_hi_h || !s_hi_pic) {
        uint32_t *np = (uint32_t *)realloc(s_hi_pic, (size_t)pw * ph * 4);
        if (!np) return NULL;
        s_hi_pic = np;
        s_hi_w = pw;
        s_hi_h = ph;
    }
    uint32_t *d = s_hi_pic;
    MenuLayout L = menu_layout(pw, ph);
    float u = L.u;
    bool slots = s_menu_mode == MODE_SAVE || s_menu_mode == MODE_LOAD;

    /* game, lightly dimmed (darker behind the slot preview) */
    if (s_filter != FILTER_SHARP) {
        /* keep the CRT look behind the menu: filter the frozen picture once
         * per menu opening / size, then dim a copy each frame */
        static uint32_t *crt_bg = NULL;
        static size_t crt_cap = 0;
        static int crt_w = 0, crt_h = 0, crt_filter = -1, crt_aspect = -1;
        static uint64_t crt_gen = 0;
        if ((size_t)pw * ph > crt_cap) {
            uint32_t *np = (uint32_t *)realloc(crt_bg, (size_t)pw * ph * 4);
            if (np) { crt_bg = np; crt_cap = (size_t)pw * ph; crt_w = 0; }
        }
        if (crt_bg) {
            if (crt_w != pw || crt_h != ph || crt_filter != s_filter || crt_aspect != s_aspect || crt_gen != s_frozen_gen) {
                int keep = crt_get_scale();
                crt_set_scale(0);                 /* the menu always fills the screen */
                crt_render(crt_bg, pw, ph, s_menu_frozen, s_frozen_w, MENU_H, false);
                crt_set_scale(keep);
                crt_w = pw; crt_h = ph; crt_filter = s_filter; crt_aspect = s_aspect; crt_gen = s_frozen_gen;
            }
            memcpy(d, crt_bg, (size_t)pw * ph * 4);
            mr_rect(d, pw, ph, 0, 0, pw, ph, 0x000000, slots ? 165 : 85);
        } else {
            mr_scale_nearest(d, pw, ph, s_menu_frozen, s_frozen_w, MENU_H, slots ? 90 : 170);
        }
    } else {
        mr_scale_nearest(d, pw, ph, s_menu_frozen, s_frozen_w, MENU_H, slots ? 90 : 170);
    }
    mr_rect(d, pw, ph, 0, 0, L.sw, ph, 0x15161D, 236);                 /* sidebar */
    mr_rect(d, pw, ph, L.sw, 0, (int)(2 * u) > 1 ? (int)(2 * u) : 1, ph, 0x2C6FE0, 200);  /* edge accent */

    {   /* title, shrunk to fit the sidebar */
        float tpx = 13.0f * u;
        int avail = L.sw - 2 * L.pad;
        int tw = mr_text_width(tpx, "Maniac Mansion");
        if (tw > avail && tw > 0) tpx = tpx * avail / tw;
        mr_text(d, pw, ph, L.pad, L.title_y, tpx, 0xFFFFFF, 255, "Maniac Mansion");
    }
    const char *sub = s_menu_mode == MODE_SAVE ? "Save to which slot?" :
                      s_menu_mode == MODE_LOAD ? "Load which save?" :
                      s_menu_mode == MODE_DISPLAY ? "Options" : "Paused";
    mr_text(d, pw, ph, L.pad, L.sub_y, 7.5f * u, 0x9AA0B4, 255, sub);
    mr_rect(d, pw, ph, L.pad, L.sep_y, L.sw - 2 * L.pad, (int)u > 1 ? (int)u : 1, 0x3A3D4C, 255);

    int n = menu_row_count();
    int vis = menu_visible_rows();
    menu_scroll_clamp();
    if (vis < n) {   /* scrollbar beside the slot list */
        int track_y = L.item_y0, track_h = vis * (L.item_h + L.item_gap) - L.item_gap;
        int bw = (int)(1.5f * u) > 2 ? (int)(1.5f * u) : 2;
        int bx = L.sw - L.pad / 2 + (L.pad / 2 - bw) / 2;
        int th = track_h * vis / n, ty = track_y + (track_h - th) * s_menu_scroll / (n - vis);
        mr_round_rect(d, pw, ph, bx, track_y, bw, track_h, bw / 2, 0xFFFFFF, 30);
        mr_round_rect(d, pw, ph, bx, ty, bw, th, bw / 2, 0x6FA2F5, 230);
    }
    for (int i = s_menu_scroll; i < s_menu_scroll + vis && i < n; i++) {
        int y0 = L.item_y0 + (i - s_menu_scroll) * (L.item_h + L.item_gap);
        bool sel = i == s_menu_sel;
        bool hov = i == s_menu_hover;
        if (sel) {
            mr_round_rect(d, pw, ph, L.pad / 2, y0, L.sw - L.pad, L.item_h, (int)(4 * u), 0x2C6FE0, 215);
        } else if (hov) {
            mr_round_rect(d, pw, ph, L.pad / 2, y0, L.sw - L.pad, L.item_h, (int)(4 * u), 0xFFFFFF, 28);
        }
        uint32_t fg = sel ? 0xFFFFFF : 0xD6D9E4;
        if (s_menu_mode == MODE_MAIN) {
            float tpx = 8.5f * u;
            mr_text(d, pw, ph, L.pad + (int)(6 * u), y0 + (int)((L.item_h - tpx) / 2), tpx, fg, 255, k_menu_rows[i]);
            continue;
        }
        if (s_menu_mode == MODE_DISPLAY) {
            bool vol = i == DI_MUSIC || i == DI_SFX;
            float tpx = vol ? 7.0f * u : 8.0f * u;
            int ty = vol ? y0 + (int)(2 * u) : y0 + (int)((L.item_h - tpx) / 2);
            mr_text(d, pw, ph, L.pad + (int)(6 * u), ty, tpx, fg, 255, k_display_rows[i]);
            char vb[32];
            const char *val = display_row_value(i, vb, sizeof vb);
            int bx, by, bw, bh;
            if (volume_bar(&L, i, &bx, &by, &bw, &bh)) {
                int v = i == DI_MUSIC ? s_music_vol : s_sfx_vol;
                int r = bh / 2;
                mr_round_rect(d, pw, ph, bx, by, bw, bh, r, 0x000000, 120);
                if (v > 0) mr_round_rect(d, pw, ph, bx, by, bw * v / 100 > bh ? bw * v / 100 : bh, bh, r,
                                         sel ? 0xFFFFFF : 0x6FA2F5, 255);
            }
            if (val[0]) {
                float vpx = 7.0f * u;
                int vx = L.sw - L.pad / 2 - (int)(6 * u) - mr_text_width(vpx, val);
                int vy = vol ? y0 + (int)(2.5f * u) : y0 + (int)((L.item_h - vpx) / 2);
                mr_text(d, pw, ph, vx, vy, vpx, sel ? 0xFFFFFF : 0x9AA0B4, 255, val);
            }
            continue;
        }
        float tpx = 7.5f * u;
        int ty = y0 + (int)((L.item_h - tpx) / 2);
        int slot = row_slot(i);
        if (slot < 0) {
            mr_text(d, pw, ph, L.pad + (int)(6 * u), ty, tpx, fg, 255, "Back");
            continue;
        }
        char name[32], when[48];
        slot_name(slot, name, sizeof name);
        slot_time_text(slot, when, sizeof when);
        mr_text(d, pw, ph, L.pad + (int)(6 * u), ty, tpx, fg, 255, name);
        float dpx = 6.0f * u;
        uint32_t dc = s_slots[slot].used ? (sel ? 0xE6E9F4 : 0x9AA0B4) : 0x5E6274;
        int dx = L.sw - L.pad / 2 - (int)(6 * u) - mr_text_width(dpx, when);
        mr_text(d, pw, ph, dx, y0 + (int)((L.item_h - dpx) / 2), dpx, dc, 255, when);
    }

    if (slots) {
        draw_slot_preview(d, pw, ph, &L);
        if (s_menu_status[0])
            mr_text(d, pw, ph, L.pad, ph - (int)(55 * u), fit_px(6.5f * u, L.sw - 2 * L.pad, s_menu_status), s_menu_status_rgb, 255, s_menu_status);
    } else {
        char info[96];
        if (s_menu_mode == MODE_DISPLAY) snprintf(info, sizeof info, "Left / Right adjust   F9 filter   F11 fullscreen");
        else last_saved_text(info, sizeof info);
        int foot_y = ph - (int)(58 * u);
        int avail = L.sw - 2 * L.pad;
        if (s_menu_status[0]) mr_text(d, pw, ph, L.pad, foot_y - (int)(12 * u), fit_px(7.0f * u, avail, s_menu_status), s_menu_status_rgb, 255, s_menu_status);
        mr_text(d, pw, ph, L.pad, foot_y, fit_px(6.5f * u, avail, info), 0x9AA0B4, 255, info);
    }
    mr_rect(d, pw, ph, L.pad, ph - (int)(44 * u), L.sw - 2 * L.pad, (int)u > 1 ? (int)u : 1, 0x3A3D4C, 255);
    static const char *const hints_main[3] = { "Esc   Resume", "Up / Down   Select", "Enter   Confirm" };
    static const char *const hints_slot[3] = { "Esc / Right-click   Back", "Up / Down / Wheel   Select", "Enter   Confirm" };
    const char *const *hints = s_menu_mode != MODE_MAIN ? hints_slot : hints_main;
    for (int i = 0; i < 3; i++)
        mr_text(d, pw, ph, L.pad, ph - (int)((38 - i * 10) * u), 6.0f * u, 0x7D8296, 255, hints[i]);

    *w = pw;
    *h = ph;
    return d;
}

static void menu_event(void *ctx, const void *event, int player) {
    (void)ctx; (void)player;
    const SDL_Event *ev = (const SDL_Event *)event;
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat) {
        SDL_Scancode sc = ev->key.keysym.scancode;
        if (sc == SDL_SCANCODE_F9)  { s_req_filter_cycle = true; return; }
        if (sc == SDL_SCANCODE_ESCAPE) {
            if (s_menu_open && s_menu_mode != MODE_MAIN) s_menu_req_back = true;
            else s_menu_req_toggle = true;
            return;
        }
        if (!s_menu_open) return;
        int n = menu_row_count();
        if (sc == SDL_SCANCODE_UP)   s_menu_sel = (s_menu_sel + n - 1) % n;
        if (sc == SDL_SCANCODE_DOWN) s_menu_sel = (s_menu_sel + 1) % n;
        if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN) { s_confirm_slot = -1; menu_keep_visible(); }
        if ((sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT) && s_menu_mode == MODE_DISPLAY &&
            (s_menu_sel == DI_MUSIC || s_menu_sel == DI_SFX)) {
            s_vol_req_row = s_menu_sel;
            s_vol_req_value = (s_menu_sel == DI_MUSIC ? s_music_vol : s_sfx_vol) + (sc == SDL_SCANCODE_LEFT ? -5 : 5);
            return;
        }
        if ((sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT) && s_menu_mode == MODE_DISPLAY &&
            s_menu_sel == DI_SCALE) {
            s_scale_req_dir = sc == SDL_SCANCODE_LEFT ? -1 : 1;
            return;
        }
        if ((sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT) && s_menu_mode == MODE_DISPLAY &&
            s_menu_sel == DI_SHAPE) {
            s_shape_req = true;
            return;
        }
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_KP_ENTER)
            s_menu_req_activate = s_menu_sel;
        return;
    }
    if (!s_menu_open) return;
    if (ev->type == SDL_MOUSEWHEEL) {
        /* scroll the slot list; the row under the pointer changes with it */
        int dy = ev->wheel.y;
        if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
        if (dy) {
            s_menu_scroll -= dy;
            menu_scroll_clamp();
            s_menu_hover = -1;
        }
        return;
    }
    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_RIGHT) {
        if (s_menu_mode != MODE_MAIN) s_menu_req_back = true;
        return;
    }
    if (ev->type == SDL_MOUSEBUTTONUP && ev->button.button == SDL_BUTTON_LEFT) s_vol_drag = -1;
    if (s_menu_mode == MODE_DISPLAY && s_hi_w > 0 &&
        ((ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) ||
         (ev->type == SDL_MOUSEMOTION && s_vol_drag >= 0))) {
        /* volume sliders: click or drag on the bar sets the level */
        int x = ev->type == SDL_MOUSEMOTION ? ev->motion.x : ev->button.x;
        int y = ev->type == SDL_MOUSEMOTION ? ev->motion.y : ev->button.y;
        MenuLayout L = menu_layout(s_hi_w, s_hi_h);
        int row = s_vol_drag >= 0 ? s_vol_drag : menu_row_at(x, y);
        int bx, by, bw, bh;
        if (row >= 0 && volume_bar(&L, row, &bx, &by, &bw, &bh) &&
            (s_vol_drag >= 0 || y >= by - (int)(5 * L.u))) {
            int v = (int)((float)(x - bx) * 100.0f / bw + 2.5f) / 5 * 5;
            s_vol_drag = row;
            s_menu_sel = row;
            s_vol_req_row = row;
            s_vol_req_value = v;
            return;
        }
    }
    if (ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEBUTTONDOWN) {
        /* Event coordinates arrive in the renderer's logical space, which is
         * the size we present while the menu is open (SDL scales mouse
         * events to the logical size; don't map them again). */
        int x = ev->type == SDL_MOUSEMOTION ? ev->motion.x : ev->button.x;
        int y = ev->type == SDL_MOUSEMOTION ? ev->motion.y : ev->button.y;
        int row = menu_row_at(x, y);
        s_menu_hover = row;
        if (row >= 0) {
            if (row != s_menu_sel) s_confirm_slot = -1;
            s_menu_sel = row;
        }
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT && row >= 0)
            s_menu_req_activate = row;
    }
}

/* While the menu is open the player uses the normal OS (Windows) cursor:
 * release the window grab and show the system pointer (the hook normally
 * hides it and confines it to the window for the game's own cursor; leaving
 * that on boxed the pointer in, so the lower rows / right side of the menu
 * were unreachable on the Windows test PC). Restored when the menu closes. */
static void menu_os_cursor(bool on) {
    SDL_Window *win = find_game_window();
    if (on) {
        confine_clear(win);
        if (win) SDL_SetWindowGrab(win, SDL_FALSE);
        s_grabbed = false;
        SDL_ShowCursor(SDL_ENABLE);
        s_cursor_hidden = false;      /* apply_absolute_cursor re-hides it later */
    } else {
        SDL_ShowCursor(SDL_DISABLE);
        s_cursor_hidden = true;       /* sync_grab re-grabs on the next poll */
    }
}

/* ctx_busy / lmb_busy are defined further down; opening waits for them. */
static bool menu_input_busy(void);

/* The game's own save (Start menu -> Save game) also writes the most
 * recently saved slot (Slot 1 if there are none yet).
 * Its save routine runs inside the game's menu (room $32, verbs off,
 * override armed), so wait until normal play has held for 30 frames (verbs
 * on $6B0A bit 7, no override, cursor sprite visible) and snapshot then:
 * loading it lands in play, not in that menu. */
static void follow_game_save(void) {
    static int good = 0;
    if (s_toast_frames > 0) s_toast_frames--;
    if (s_game_thumb_frames >= 0 && !s_menu_open && --s_game_thumb_frames < 0)
        write_thumb(s_game_thumb_slot, cyc_frame_argb());
    if (s_game_saved_frames < 0 || s_menu_open) return;
    if (++s_game_saved_frames > 60 * 20) { s_game_saved_frames = -1; good = 0; menu_log("game save: gave up waiting for play", NULL); return; }
    uint16_t ya, xa;
    bool play = (wram(0x6B0A) & 0x80) && !(wram(0x6B06) | wram(0x6B07)) &&
                find_cursor_oam(&ya, &xa) && *ram_byte(ya) < 0xEF;
    good = play ? good + 1 : 0;
    if (good < 30) return;
    good = 0;
    s_game_saved_frames = -1;
    char path[1100], err[160];
    refresh_slots();
    int slot = 1;
    for (int s = 1; s <= NUM_SLOTS; s++)
        if (s_slots[s].used && (!s_slots[slot].used || s_slots[s].mtime > s_slots[slot].mtime)) slot = s;
    slot_file(slot, "state", path, sizeof path);
    if (cyc_state_save_file(path, err, sizeof err)) {
        /* the room is still fading in at this point (thumbnail came out
         * black): take the screenshot ~1.5 s later */
        s_game_thumb_slot = slot;
        s_game_thumb_frames = 90;
        snprintf(s_toast_text, sizeof s_toast_text, "SAVED TO SLOT %d", slot);
        menu_log("game save -> slot updated", path);
        s_toast_frames = 120;
    } else menu_log("game save -> slot write failed", err);
}

/* ---- display settings ---- */
static void display_toast(const char *what, const char *value) {
    snprintf(s_toast_text, sizeof s_toast_text, "%s: %s", what, value);
    for (char *c = s_toast_text; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    s_toast_frames = 90;
}

/* Square-pixel Sharp is the only mode the host draws itself; everything
 * else is a window-resolution picture from the filter module. */
static bool host_draws_sharp(void) { return s_filter == FILTER_SHARP && !s_aspect; }

static void apply_filter(void) {
    /* Sharp: nearest + integer scaling, so every NES pixel is the same size
     * (6x on 1440p; 4x on 1080p with thin bars, user's choice over uneven
     * 4.5x). CRT presents at window resolution (1:1), so no integer step.
     * Sharp at TV shape: the module's sharp-bilinear style, whole vertical
     * scale, 8:7 wide. */
    bool host = host_draws_sharp();
    cyc_host_display_set(-1, host ? 0 : 1, host ? 1 : 0);
    cyc_host_display_set_scale(host ? s_scale : 0);
    crt_set_look(s_filter == FILTER_CRT_STRONG ? CRT_LOOK_STRONG : CRT_LOOK_NORMAL);
    crt_set_style(s_filter == FILTER_SHARP ? CRT_STYLE_SHARP : CRT_STYLE_CRT);
    crt_set_aspect(s_aspect ? CRT_PAR_TV : 1.0f);
    crt_set_integer(s_filter == FILTER_SHARP);
    crt_set_scale(s_scale);     /* fixed size inside the window-res picture */
}

static void set_shape(int aspect) {
    s_aspect = aspect ? 1 : 0;
    apply_filter();
    cyc_host_save_settings();
    display_toast("Shape", s_aspect ? "TV (4:3)" : "Square");
}

/* Next Sharp scale in the cycle Auto, 2x .. largest that fits (dir +1/-1). */
static void step_scale(int dir) {
    int max = cyc_host_display_max_scale();
    int opts[8], n = 0;
    opts[n++] = 0;
    for (int k = 2; k <= max && k <= 6; k++) opts[n++] = k;
    int i = 0;
    for (int j = 0; j < n; j++) if (opts[j] == s_scale) i = j;
    if (s_scale > max && s_scale > 0) i = dir > 0 ? n - 1 : 0;   /* saved scale doesn't fit here: + -> Auto, - -> largest */
    s_scale = opts[((i + dir) % n + n) % n];
    apply_filter();
    cyc_host_save_settings();
    char v[24];
    if (s_scale) snprintf(v, sizeof v, "%dx", s_scale);
    else if (s_filter != FILTER_SHARP) snprintf(v, sizeof v, "Auto (fill)");
    else snprintf(v, sizeof v, "Auto (%dx)", max);
    display_toast("Scale", v);
}

static void set_filter(int f) {
    s_filter = (f % FILTER_COUNT + FILTER_COUNT) % FILTER_COUNT;
    apply_filter();             /* also saves the settings file */
    display_toast("Filter", k_filter_names[s_filter]);
}

/* ---- music / sound effect volume ----
 * The game's sound driver plays sound effects through its slot player
 * (b00:$8C49-$8CC3, register blocks per channel; $728B != 0 while one is
 * playing) and music through the music channel code ($8DB8-$8F95; the
 * title jingle uses the mini player at $A56A). A channel the effect player
 * writes is an effect channel until $728B clears; the mixer gain of each
 * APU channel follows the Music or Sound FX volume accordingly. */
static uint8_t s_sfx_channels = 0;     /* bit per APU channel */

static void apply_audio_gains(void) {
    FILE *log = dbg_log();
    if (log) fprintf(log, "[audio] effect channels=%X music=%d%% sfx=%d%%\n", s_sfx_channels, s_music_vol, s_sfx_vol);
    for (int ch = 0; ch < 5; ch++) {
        int v = (s_sfx_channels >> ch) & 1 ? s_sfx_vol : s_music_vol;
        cyc_apu_set_channel_gain(ch, (float)v / 100.0f);
    }
}

static void apu_write_observer(uint16_t addr, uint8_t value, uint16_t pc) {
    (void)value;
    if (addr > 0x400F || pc < 0x8C49 || pc >= 0x8CC4) return;
    uint8_t bit = (uint8_t)(1u << ((addr - 0x4000) >> 2));
    if (s_sfx_channels & bit) return;
    s_sfx_channels |= bit;
    apply_audio_gains();
}

static void audio_frame_end(void) {
    if (s_sfx_channels && !wram(0x728B)) {
        s_sfx_channels = 0;
        apply_audio_gains();
    }
}

static void set_volume(int row, int v) {
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    int *p = row == DI_MUSIC ? &s_music_vol : &s_sfx_vol;
    if (*p == v) return;
    *p = v;
    apply_audio_gains();
    cyc_host_save_settings();
}

static void display_activate(int item) {
    int fs = 0;
    switch (item) {
    case DI_MUSIC:      set_volume(DI_MUSIC, s_music_vol >= 100 ? 0 : s_music_vol + 5); break;
    case DI_SFX:        set_volume(DI_SFX, s_sfx_vol >= 100 ? 0 : s_sfx_vol + 5); break;
    case DI_FILTER:     set_filter(s_filter + 1); break;
    case DI_SCALE:      step_scale(+1); break;
    case DI_SHAPE:      set_shape(!s_aspect); break;
    case DI_FULLSCREEN:
        cyc_host_display_get(&fs, NULL, NULL);
        cyc_host_display_set(fs ? 0 : 1, -1, -1);
        break;
    }
}

static void display_frame_end(void) {
    if (!s_display_inited) {
        s_display_inited = true;
        apply_filter();
        apply_audio_gains();
        cyc_mod_set_apu_write_hook(apu_write_observer);
    }
    if (s_req_filter_cycle) { s_req_filter_cycle = false; set_filter(s_filter + 1); }
    if (s_scale_req_dir) { step_scale(s_scale_req_dir); s_scale_req_dir = 0; }
    if (s_shape_req) { s_shape_req = false; set_shape(!s_aspect); }
    if (s_vol_req_row >= 0) {
        set_volume(s_vol_req_row, s_vol_req_value);
        s_vol_req_row = -1;
    }
    audio_frame_end();
}

static void display_load_setting(void *ctx, const char *key, const char *value) {
    (void)ctx;
    if (!strcmp(key, "Filter")) { int v = atoi(value); s_filter = v == 4 ? FILTER_CRT_STRONG : (v == 2 || v == 3) ? FILTER_CRT : FILTER_SHARP; }
    else if (!strcmp(key, "MusicVolume")) s_music_vol = atoi(value);
    else if (!strcmp(key, "SfxVolume")) s_sfx_vol = atoi(value);
    else if (!strcmp(key, "Scale")) { int v = atoi(value); s_scale = v >= 2 && v <= 6 ? v : 0; }
    else if (!strcmp(key, "Aspect")) s_aspect = atoi(value) == 1 ? 1 : 0;
    s_music_vol = s_music_vol < 0 ? 0 : s_music_vol > 100 ? 100 : s_music_vol;
    s_sfx_vol = s_sfx_vol < 0 ? 0 : s_sfx_vol > 100 ? 100 : s_sfx_vol;
    if (s_filter < 0 || s_filter >= FILTER_COUNT) s_filter = FILTER_SHARP;
}

static void display_save_settings(void *ctx, FILE *f) {
    (void)ctx;
    fprintf(f, "Filter = %d\nMusicVolume = %d\nSfxVolume = %d\nScale = %d\nAspect = %d\n",
            s_filter == FILTER_CRT_STRONG ? 4 : s_filter == FILTER_CRT ? 2 : 0, s_music_vol, s_sfx_vol, s_scale, s_aspect);
}

static void menu_frame_end(void *ctx) {
    (void)ctx;
    if (!s_menu_open) {
        SDL_Window *win = find_game_window();
        sync_grab(win);
        confine_mouse(win);
        confine_warp_back(win);
    }
    display_frame_end();
    follow_game_save();
    if (s_menu_req_back) {
        s_menu_req_back = false;
        if (s_menu_open && s_menu_mode != MODE_MAIN) menu_back();
    }
    if (s_menu_req_toggle) {
        s_menu_req_toggle = false;
        if (s_menu_open) menu_activate(MI_RESUME);
        else if (!menu_input_busy()) menu_open_now();
    }
    if (s_menu_req_activate >= 0) {
        int item = s_menu_req_activate;
        s_menu_req_activate = -1;
        if (s_menu_open) menu_activate(item);
    }
}

/* Input gate for real_mouse_buttons: true = send the game nothing. Holds
 * after the menu closes until every key/button that could leak through
 * (the activating Enter/Space/click, Escape) is released. */
static bool menu_blocks_input(bool lmb, bool rmb, bool mmb) {
    if (s_menu_open) return true;
    if (s_menu_guard) {
        const uint8_t *k = SDL_GetKeyboardState(NULL);
        if (lmb || rmb || mmb || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE] ||
            k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_KP_ENTER]) return true;
        s_menu_guard = false;
    }
    return false;
}

/* After a click, diff the whole zero-page+OAM state against a baseline
 * taken at the click frame for CLICK_WATCH_FRAMES frames, logging any byte
 * that changes. Answers "does the game react to this click AT ALL" without
 * needing the player to describe what they saw -- if NOTHING in RAM moves
 * after a click lands at a given cursor position, the click isn't reaching
 * game logic (wrong RAM byte, wrong click semantics, etc.); if things do
 * change, the position is at least being read by *something*, narrowing
 * the bug to "reads the click but picks the wrong verb/object" instead. */
#define CLICK_WATCH_FRAMES 60
static uint8_t s_click_baseline[0x800];
static int     s_click_watch_left = 0;
static int     s_click_watch_logged_pos = 0;

/* 2026-11-06: the Windows test PC reports Start/A+B still doesn't skip cutscenes on
 * real hardware, even though his own debug log shows the hook correctly
 * emitting Start/combo bytes. The one thing that log can't show is HOLD
 * DURATION -- CLICK watch only logs the trigger frame's button byte, not
 * how many consecutive frames the press stayed up. If a real keypress is
 * landing as a single-frame blip (vs. the multi-frame holds this was
 * verified against under Wine), that'd explain a skip-check that requires
 * the press sustained across >1 frame. Log every frame's raw button byte
 * for a short window after any edge so the next real log answers this
 * directly instead of needing another guess-and-ship round trip. */
#define BUTTON_HOLD_LOG_FRAMES 12
static int s_button_hold_log_left = 0;

static void click_watch_tick(FILE *log, uint8_t cur_buttons, uint8_t prev_buttons) {
    bool pressed_now = (cur_buttons & (BTN_A | BTN_B | BTN_START)) && !(prev_buttons & (BTN_A | BTN_B | BTN_START));
    if (pressed_now) {
        for (int a = 0; a < 0x800; a++) s_click_baseline[a] = *ram_byte((uint16_t)a);
        s_click_watch_left = CLICK_WATCH_FRAMES;
        s_click_watch_logged_pos = 0;
        s_button_hold_log_left = BUTTON_HOLD_LOG_FRAMES;
    }
    if (log && s_button_hold_log_left > 0) {
        fprintf(log, "HOLD frame+%d buttons=0x%02x\n", BUTTON_HOLD_LOG_FRAMES - s_button_hold_log_left, cur_buttons);
        s_button_hold_log_left--;
        if (s_button_hold_log_left == 0) fprintf(log, "HOLD watch done\n");
        fflush(log);
    }
    if (s_click_watch_left <= 0 || !log) return;
    if (!s_click_watch_logged_pos) {
        uint16_t ya, xa;
        if (find_cursor_oam(&ya, &xa))
            fprintf(log, "CLICK buttons=0x%02x cursorXY=$%02X,$%02X (oam $%04X)\n",
                cur_buttons, *ram_byte(xa), *ram_byte(ya), ya);
        else
            fprintf(log, "CLICK buttons=0x%02x cursorXY=<no $FA tile found in OAM>\n", cur_buttons);
        s_click_watch_logged_pos = 1;
    }
    for (int a = 0; a < 0x800; a++) {
        uint8_t v = *ram_byte((uint16_t)a);
        if (v != s_click_baseline[a]) {
            fprintf(log, "  +%2d $%04X: $%02X -> $%02X\n",
                CLICK_WATCH_FRAMES - s_click_watch_left, a, s_click_baseline[a], v);
            s_click_baseline[a] = v;
        }
    }
    s_click_watch_left--;
    if (s_click_watch_left == 0) fprintf(log, "CLICK watch done\n");
    fflush(log);
}

/* ------------------------------------------------------------------------
 * 2026-11-09: context-sensitive RMB (DOTT-style default verb).
 *
 * Reverse-engineered with nes-explorer (names from its labels.json) and
 * verified live; addresses are for the prototype ROM only:
 *   - SCUMM v2 vars live at $6100+n (lo) / $6000+n (hi). $611A = current
 *     sentence verb, $611E/$611F = VIRT_MOUSE_X/Y.
 *   - $711A = UI box under the cursor (ui_hit_test b00:944B); box 20 is the
 *     room area.
 *   - find_object (b0F:E1E6) walks the room's object slots 1..$63D9:
 *     $63DA x, $63FD y, $6420 w, $6443 h (8px tiles; y compared as
 *     VIRT_MOUSE_Y>>2), $65E7 flags (bit $20 untouchable, bit $80 state),
 *     $6515 parent slot, $6538 required parent state.
 *   - Room directory b0F:$DC95 + 4*room ($63C5): [bank, ?, base lo, base hi].
 *     Each object's code is at base + word($65A1 + 2*slot); its verb table
 *     starts at +15 as (verb, offset) pairs ending in 0.
 *   - Verb ids (from the verb box values $6ACE): OPEN 01, CLOSE 02, GIVE 03,
 *     TURN ON 04, TURN OFF 05, NEW KID 07, PUSH 09, PULL 0A, USE 0B,
 *     READ 0C, GO TO 0D, GET 0E.
 * RMB over the room: pick a verb the object actually handles, write it to
 * $611A, and send A, so the game builds and runs "<verb> <object>" itself.
 * ---------------------------------------------------------------------- */
enum { V_OPEN = 0x01, V_CLOSE = 0x02, V_TURN_ON = 0x04, V_TURN_OFF = 0x05,
       V_PUSH = 0x09, V_PULL = 0x0A, V_USE = 0x0B, V_READ = 0x0C,
       V_GOTO = 0x0D, V_GET = 0x0E };
#define ROOM_BOX 20
#ifndef CTX_SETTLE_POLLS
#define CTX_SETTLE_POLLS 4
#endif
#define PRG_BANKS 16

static const uint8_t *prg_image(void) {
    static uint8_t *img = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        size_t need = cyc_mod_copy_cart_payload(NULL, 0);
        if (need >= (size_t)PRG_BANKS * 0x4000) {
            img = (uint8_t *)malloc(need);
            if (img && cyc_mod_copy_cart_payload(img, need) != need) { free(img); img = NULL; }
        }
    }
    return img;
}

/* Byte at CPU addr with `bank` mapped at $8000 (fixed last bank at $C000). */
static bool prg_read(unsigned bank, uint16_t addr, uint8_t *v) {
    const uint8_t *img = prg_image();
    if (!img || addr < 0x8000) return false;
    size_t off = addr >= 0xC000 ? (size_t)(PRG_BANKS - 1) * 0x4000 + (addr - 0xC000)
                                : (size_t)bank * 0x4000 + (addr - 0x8000);
    if (bank >= PRG_BANKS) return false;
    *v = img[off];
    return true;
}

static uint8_t wram(uint16_t addr) {
    uint8_t *p = cart_ram_byte(addr);
    return p ? *p : 0;
}

/* Same algorithm as the ROM's find_object (b0F:E1E6). Returns slot or 0. */
static int ctx_find_object_slot(uint8_t x, uint8_t vy) {
    uint8_t y = (uint8_t)(vy >> 2);
    int n = wram(0x63D9);
    if (n > 34) n = 34;
    for (int i = 1; i <= n; i++) {
        if (wram(0x65E7 + i) & 0x20) continue;            /* untouchable */
        int b = i, guard = 0;
        bool visible = false;
        for (;;) {                                         /* parent chain */
            uint8_t need = wram(0x6538 + b);
            int p = wram(0x6515 + b);
            if (p == 0) { visible = true; break; }
            if ((wram(0x65E7 + p) & 0x80) != need || ++guard > 34) break;
            b = p;
        }
        if (!visible) continue;
        uint8_t ox = wram(0x63DA + i), oy = wram(0x63FD + i);
        uint8_t ow = wram(0x6420 + i), oh = wram(0x6443 + i);
        if (ox <= x && (uint8_t)(ox + ow) > x && oy <= y && (uint8_t)(oy + oh) > y)
            return i;
    }
    return 0;
}

/* Verbs in the object's verb table. Returns count (0 if unreadable). */
/* Locate the object's code (OBCD) in PRG: bank and CPU address. */
static bool ctx_object_code(int slot, uint8_t *bank_out, uint16_t *addr_out) {
    uint8_t room = wram(0x63C5), bank, lo, hi;
    if (!prg_read(PRG_BANKS - 1, (uint16_t)(0xDC95 + 4 * room), &bank) ||
        !prg_read(PRG_BANKS - 1, (uint16_t)(0xDC97 + 4 * room), &lo) ||
        !prg_read(PRG_BANKS - 1, (uint16_t)(0xDC98 + 4 * room), &hi)) return false;
    uint8_t olo = wram((uint16_t)(0x65A1 + 2 * slot));
    uint8_t ohi = wram((uint16_t)(0x65A2 + 2 * slot));
    *bank_out = bank;
    *addr_out = (uint16_t)(((hi << 8) | lo) + ((ohi << 8) | olo));
    return *addr_out >= 0x8000 && *addr_out < 0xC000;
}

/* Does the object's GET script actually take it? Verb tables include joke
 * handlers (GET door mat says "...leave it there."), so having a GET entry
 * is not enough. Scans only the GET handler's own bytes (up to the next
 * verb's entry or the end of the object) for SCUMM v2 ways of giving the
 * player an item. Opcode families from the interpreter table at b0F:$FE97:
 *   $D0 $09            pickupObject(VAR_ACTIVE_OBJECT1)   ($50/$D0 -> CB4B)
 *   $50 lo hi          pickupObject(this object)
 *   $69 lo hi $00      setOwnerOf(object <= $2FF, VAR_EGO) ($29/$69/$A9/$E9 -> CD41)
 *   $E9 $09 $00        setOwnerOf(active object, VAR_EGO)
 *   $42 $74            startScript($74), the shared take-item script (dimes)
 *   $87/$C7 $09 $D7 $09 the state changes that hide a taken object
 * Whole-ROM check (all rooms): 65 objects classify as takeable (keys, tapes,
 * quarters, dimes, flashlight, food, radiation suit...), 34 as not (door
 * mat, bushes, candles, nailed calendar, glued knives, coin boxes...). */
static bool ctx_get_picks_up(int slot) {
    uint8_t bank, b0, b1, v, o, off = 0;
    uint16_t p;
    if (!ctx_object_code(slot, &bank, &p)) return false;
    if (!prg_read(bank, p, &b0) || !prg_read(bank, (uint16_t)(p + 1), &b1)) return false;
    uint16_t size = (uint16_t)(b0 | (b1 << 8));
    if (size > 1024) return false;
    uint16_t end = size;
    for (uint16_t q = (uint16_t)(p + 15); q < 0xC000 && q < p + 15 + 64; q = (uint16_t)(q + 2))
        if (!prg_read(bank, q, &v) || v == 0) break;
        else if (v == V_GET) prg_read(bank, (uint16_t)(q + 1), &off);
    if (!off || off >= size) return false;
    for (uint16_t q = (uint16_t)(p + 15); q < 0xC000 && q < p + 15 + 64; q = (uint16_t)(q + 2)) {
        if (!prg_read(bank, q, &v) || v == 0) break;
        if (prg_read(bank, (uint16_t)(q + 1), &o) && o > off && o < end) end = o;
    }
    uint8_t olo = wram((uint16_t)(0x64F2 + slot)), ohi = wram((uint16_t)(0x64CF + slot));
    for (uint16_t k = off; k + 1 < end; k++) {
        uint8_t b[4] = { 0, 0, 0xFF, 0xFF };
        uint16_t q = (uint16_t)(p + k);
        if (q + 1 >= 0xC000) break;
        for (int i = 0; i < 4 && k + i < end && q + i < 0xC000; i++) prg_read(bank, (uint16_t)(q + i), &b[i]);
        if (b[0] == 0xD0 && b[1] == 0x09) return true;
        if (b[0] == 0x50 && b[1] == olo && b[2] == ohi) return true;
        if (b[0] == 0x69 && b[3] == 0x00 && b[2] <= 0x02) return true;
        if (b[0] == 0xE9 && b[1] == 0x09 && b[2] == 0x00) return true;
        if (b[0] == 0x42 && b[1] == 0x74) return true;
        if ((b[0] == 0x87 || b[0] == 0xC7) && b[1] == 0x09 && b[2] == 0xD7 && b[3] == 0x09) return true;
    }
    return false;
}

static int ctx_object_verbs(int slot, uint8_t *verbs, int max) {
    uint8_t bank;
    uint16_t p;
    if (!ctx_object_code(slot, &bank, &p)) return 0;
    p = (uint16_t)(p + 15);
    int n = 0;
    for (int k = 0; k < 32 && n < max; k++) {
        uint8_t v;
        if (p >= 0xC000 || !prg_read(bank, p, &v) || v == 0) break;
        verbs[n++] = v;
        p = (uint16_t)(p + 2);
    }
    return n;
}

static bool has_verb(const uint8_t *v, int n, uint8_t want) {
    for (int i = 0; i < n; i++) if (v[i] == want) return true;
    return false;
}

/* Pick the default verb. State bit $80 of the slot flags is the object's
 * open/on state (same bit find_object's parent check uses); with both
 * halves of a pair available, pick the one that changes the state. */
/* Verb tables list every verb an object has ANY script for, joke replies
 * included (the door mat has TURN ON and READ), so a pure "first verb"
 * rule picks silly actions. Generic priority below, plus a small override
 * table (object number -> verb) for objects where it still picks wrong.
 * Overrides only apply if the object actually has that verb. */
static const struct { uint16_t obj; uint8_t verb; } k_ctx_overrides[] = {
    { 0x0036, V_GET },    /* door mat: user choice (its GET is a joke reply; PULL finds the key) */
    { 0x0232, V_PULL },   /* bushes (front yard, hide the grating) */
    { 0x0115, V_PULL },   /* bushes (other side) */
};

static uint8_t ctx_choose_verb(int slot, const uint8_t *v, int n) {
    /* User rule: anything that can actually be picked up defaults to GET. */
    if (has_verb(v, n, V_GET) && ctx_get_picks_up(slot)) return V_GET;
    uint16_t obj = (uint16_t)((wram((uint16_t)(0x64CF + slot)) << 8) | wram((uint16_t)(0x64F2 + slot)));
    /* mailbox flag (front yard, left edge): user rule, PULL it up when it is
     * down, PUSH it down when it is up (state bit $80 = up) */
    if (obj == 0x0231 && has_verb(v, n, V_PULL) && has_verb(v, n, V_PUSH))
        return (wram((uint16_t)(0x65E7 + slot)) & 0x80) ? V_PUSH : V_PULL;
    for (size_t i = 0; i < sizeof k_ctx_overrides / sizeof k_ctx_overrides[0]; i++)
        if (k_ctx_overrides[i].obj == obj && has_verb(v, n, k_ctx_overrides[i].verb))
            return k_ctx_overrides[i].verb;
    bool on = (wram((uint16_t)(0x65E7 + slot)) & 0x80) != 0;
    if (has_verb(v, n, V_OPEN) && has_verb(v, n, V_CLOSE)) return on ? V_CLOSE : V_OPEN;
    if (has_verb(v, n, V_OPEN)) return V_OPEN;
    if (has_verb(v, n, V_PUSH)) return V_PUSH;
    if (has_verb(v, n, V_PULL)) return V_PULL;
    if (has_verb(v, n, V_TURN_ON) && has_verb(v, n, V_TURN_OFF)) return on ? V_TURN_OFF : V_TURN_ON;
    if (has_verb(v, n, V_TURN_ON)) return V_TURN_ON;
    if (has_verb(v, n, V_READ)) return V_READ;
    if (has_verb(v, n, V_USE)) return V_USE;
    if (has_verb(v, n, V_GET)) return V_GET;   /* joke GET: still better than nothing */
    return V_GOTO;
}

/* Called on a fresh RMB press in gameplay. Returns the verb to select for
 * the room object under the cursor, or 0 for "just a plain click" (not over
 * the room, or nothing better than GO TO).
 *
 * Writing $611A alone is NOT enough (tested: the game still ran GO TO; $611A
 * is only the displayed sentence verb). So the caller replays a real verb
 * click instead: CLICK_VERB/CLICK_AREA + the run-input-script flag, exactly
 * what click_verb (b00:91D2) + the frame loop do, lets script 4 select the
 * verb itself; the A click on the object follows once $611A shows it. */
static uint8_t ctx_rmb_pick(FILE *log, int *slot_out) {
    uint8_t hover = wram(0x711A);
    if (hover != ROOM_BOX) {
        if (log) fprintf(log, "[ctx] hover box %u, not room: plain click\n", hover);
        return 0;
    }
    uint8_t x = wram(0x611E), vy = wram(0x611F);
    int slot = ctx_find_object_slot(x, vy);
    if (slot_out) *slot_out = slot;
    uint8_t verb = V_GOTO;
    uint8_t verbs[32];
    int n = 0;
    if (slot) {
        n = ctx_object_verbs(slot, verbs, 32);
        verb = ctx_choose_verb(slot, verbs, n);
    }
    if (log) {
        fprintf(log, "[ctx] vmouse=%u,%u slot=%d obj=$%02X%02X flags=$%02X verbs=",
                x, vy, slot, slot ? wram(0x64CF + slot) : 0, slot ? wram(0x64F2 + slot) : 0,
                slot ? wram(0x65E7 + slot) : 0);
        for (int i = 0; i < n; i++) fprintf(log, "%02X ", verbs[i]);
        fprintf(log, "-> verb $%02X\n", verb);
    }
    return verb == V_GOTO ? 0 : verb;
}

/* Is SCUMM script `num` running in any of the 20 slots? ($63A1 = script
 * number, $633D = status, 0 = free; see start_script b0F:C582.) */
static bool ctx_script_running(uint8_t num) {
    for (int s = 0; s < 20; s++)
        if (wram((uint16_t)(0x633D + s)) != 0 && wram((uint16_t)(0x63A1 + s)) == num) return true;
    return false;
}

/* True while a context click is in flight (verb requested, object click
 * pending/held); the sentence preview stays out of the way meanwhile. */
static bool ctx_busy = false;
/* Verb the sentence-line preview last wrote (V_GOTO = no preview showing),
 * and whether an LMB click is being converted back to GO TO. */
static uint8_t preview_verb = 0x0D;
static bool lmb_busy = false;

static bool menu_input_busy(void) { return ctx_busy || lmb_busy; }

static void after_state_load(void) {
    /* A preview verb sitting in $611A must be one the preview logic knows
     * it owns, else it would stick (and LMB would adopt it). */
    uint8_t ca = wram(0x6120), sv = wram(0x611A);
    preview_verb = ca != 1 ? sv : 0x0D;
}

/* Replay a verb-box click: what click_verb + the frame loop would set. */
static void ctx_select_verb(uint8_t verb) {
    uint8_t *cv = cart_ram_byte(0x6121), *cvh = cart_ram_byte(0x6021);
    uint8_t *ca = cart_ram_byte(0x6120), *cah = cart_ram_byte(0x6020);
    uint8_t *run = cart_ram_byte(0x6CAC);
    if (cv) *cv = verb;
    if (cvh) *cvh = 0;
    if (ca) *ca = 1;          /* CLICK_AREA = verb */
    if (cah) *cah = 0;
    if (run) *run = 1;        /* main loop: start script 4 (b0F:F803) */
}

/* 2026-11-10: LMB on an object ran the previewed verb anyway (user report;
 * reproduced: LMB on the door mat while "GET DOOR MAT" was previewed walked
 * there and took it). The click itself goes out with GO TO, but the game
 * runs the sentence when the kid ARRIVES, with the verb current then - and
 * the preview had already put GET back. So after an LMB click the preview
 * stays off until every actor has stopped moving (positions $6CBE-$6CC5
 * unchanged for 20 polls), capped at 10 s. */
static int  s_walk_pause = 0;      /* polls since the click, 0 = off */
static int  s_walk_still = 0;
static uint8_t s_walk_pos[8];

static void walk_pause_start(void) {
    s_walk_pause = 1;
    s_walk_still = 0;
    for (int i = 0; i < 8; i++) s_walk_pos[i] = wram((uint16_t)(0x6CBE + i));
}

static bool walk_pause_active(void) {
    if (!s_walk_pause) return false;
    bool moved = false;
    for (int i = 0; i < 8; i++) {
        uint8_t v = wram((uint16_t)(0x6CBE + i));
        if (v != s_walk_pos[i]) { moved = true; s_walk_pos[i] = v; }
    }
    s_walk_still = moved ? 0 : s_walk_still + 1;
    if (++s_walk_pause > 600 || (s_walk_pause > 10 && s_walk_still >= 20)) {
        s_walk_pause = 0;
        FILE *log = dbg_log();
        if (log) fprintf(log, "[lmb] walk over, preview back on\n");
        return false;
    }
    return true;
}

static uint8_t real_mouse_buttons(void) {
    /* 2026-11-07: SDL_GetGlobalMouseState() was reported not to see real LMB
     * presses at all on at least one dev machine, while the focus-gated
     * SDL_GetMouseState() a few lines below (used only for cursor position)
     * tracked fine. SDL_GetGlobalMouseState() queries the OS cursor
     * directly (GetCursorPos/XQueryPointer) and should NOT need the SDL
     * event queue pumped to see button state -- but SDL_PumpEvents() is
     * cheap and rules out any event-queue-starvation explanation outright.
     * Also now logging SDL_GetMouseState()'s OWN button mask alongside the
     * global one below so a future log can show directly whether the two
     * calls simply disagree (that would nail the diagnosis) or whether
     * neither sees the press (points somewhere else entirely, e.g. the
     * window not actually owning the click at the OS level). */
    SDL_PumpEvents();
    /* Escape menu open: no grab, no cursor writes, no input to the game. */
    if (s_menu_open) return 0;
    int gx = 0, gy = 0;
    Uint32 buttons = SDL_GetGlobalMouseState(&gx, &gy);
    SDL_Window *win = find_game_window();
    raise_once(win);
    sync_grab(win);
    confine_mouse(win);
    apply_absolute_cursor(win);

    int fmx = 0, fmy = 0;
    Uint32 focused_buttons = SDL_GetMouseState(&fmx, &fmy);

    FILE *log = dbg_log();
    static uint32_t diag_n = 0;
    if (log && (diag_n < 20 || (diag_n % 120) == 0)) {
        int ww = -1, wh = -1, wx = -1, wy = -1;
        if (win) { SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowPosition(win, &wx, &wy); }
        uint16_t ya, xa;
        if (find_cursor_oam(&ya, &xa))
            fprintf(log, "[diag] win=%p size=%dx%d pos=%d,%d global=%d,%d buttons=0x%x focused_buttons=0x%x cursorXY=$%02X,$%02X (oam $%04X)\n",
                (void*)win, ww, wh, wx, wy, gx, gy, buttons, focused_buttons, *ram_byte(xa), *ram_byte(ya), ya);
        else
            fprintf(log, "[diag] win=%p size=%dx%d pos=%d,%d global=%d,%d buttons=0x%x focused_buttons=0x%x cursorXY=<no $FA tile found>\n",
                (void*)win, ww, wh, wx, wy, gx, gy, buttons, focused_buttons);
        fflush(log);
    }
    diag_n++;

    /* 2026-11-06 (reverted the brief A+B-combo experiment from commit
     * 0caf0d5 -- confirmed broken, user wants plain RMB-alone skip back):
     * RMB -> BTN_A | BTN_START, so a right-click both performs the game's
     * normal context action (A) and skips any active cutscene (Start) in
     * the same press -- no combo required. Confirmed working for the
     * title-card screen and in-game dialogue, but NOT for the opening
     * Lucasfilm logo -- user confirmed Enter (plain BTN_START) skips that
     * screen but RMB (BTN_A|BTN_START together) does not. That logo's
     * skip-check is likely a one-off exact-equality test against Start
     * alone (common for primitive pre-engine splash code), not a bitwise
     * "Start is set" test like the in-game skip uses. Fix: on the first
     * frame of a new RMB press, send a clean Start-only pulse (satisfies
     * an exact-equality check); on subsequent held frames, fall back to
     * A|Start as before (preserves the already-confirmed-working
     * title-card/dialogue skip and ordinary RMB context-click). */
    /* 2026-11-07: OR in SDL_GetMouseState()'s button bits too (only
     * meaningful while this window actually has focus, same gate
     * apply_absolute_cursor() already uses for position) as a real fallback,
     * not just a diagnostic -- if SDL_GetGlobalMouseState() is the one
     * failing to see the press on some setups, the focused call picks up
     * the slack instead of leaving LMB/RMB dead there. */
    bool have_focus = (SDL_GetMouseFocus() == win);
    uint8_t out = 0;
    bool lmb = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) ||
               (have_focus && (focused_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)));
    bool rmb = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) ||
               (have_focus && (focused_buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)));
    bool mmb = (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) ||
               (have_focus && (focused_buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)));
    /* 2026-11-09: Escape menu open (or its activating press still held):
     * the game gets no input at all. */
    if (menu_blocks_input(lmb, rmb, mmb)) return 0;
    /* 2026-11-07: the Windows test PC reports RMB "acts as LMB" -- i.e. the context
     * action (A) is visibly happening but no cutscene/dialogue skip is.
     * Root cause: the held-frame A|Start combo makes A's visible effect
     * (interact/advance-dialogue-one-line) dominate, while Start's actual
     * skip apparently never lands -- the mouse_debug.log HOLD trace from
     * that machine shows the button byte going 0x10 (Start alone, first
     * frame) then usually 0x90 (A|Start) exactly as designed, so this isn't
     * an input-delivery bug, it's that A|Start doesn't actually trigger
     * this prototype ROM's skip the way the old (differently-built, never
     * actually re-verified end to end) A|Start claim assumed. Decoupled:
     * RMB is now a pure Start-only skip button on every frame, first press
     * or held, no A at all -- isolates skip from the context-click entirely
     * so it can be verified on its own, and LMB (already the context-click
     * button) is unaffected. If skip still doesn't visibly happen after
     * this, the problem isn't the button combo at all and needs tracing
     * further (e.g. this skip-check genuinely needs the OLD held level each
     * frame tested differently, or it isn't a simple "Start is set" test in
     * every screen the user is testing against). */
    if (lmb) out |= BTN_A;
    /* 2026-11-09: RMB = Start (skip) only during the pre-game screens
     * (logo/credits/title) before the cursor sprite has EVER been visible.
     * Once the cursor has appeared (new-game menu onward) the latch
     * `game_started` stays set for the process lifetime and RMB is always A:
     * Start in gameplay opens the save menu, and A advances/skips Dave's
     * dialogue. Keyboard Enter/Space still send Start explicitly. */
    static bool game_started = false;
    {
        uint16_t cya, cxa;
        if (find_cursor_oam(&cya, &cxa) && *ram_byte(cya) < 0xEF) game_started = true;
    }
    if (rmb) out |= game_started ? BTN_A : BTN_START;
    /* 2026-11-09: middle button = Start (user request): in gameplay that
     * opens the game's own menu (save/load/new kids), like the NES pad. */
    if (mmb) out |= BTN_START;

    /* Space also sends Start (cutscene skip) -- for fast iteration while
     * testing, so skipping doesn't require chasing the grabbed/confined
     * cursor back to a precise right-click. */
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_SPACE]) out |= BTN_START;
    if (keys[SDL_SCANCODE_RETURN]) out |= BTN_START; /* Enter == Start, for now */

    /* 2026-11-09: cutscene skip, root-caused with nes-explorer + live RAM.
     * The ROM's key handler (b0F:F7B1) skips a cutscene only when the latched
     * key byte ($2F) EQUALS the SCUMM var CUTSCENE_EXIT_KEY ($6128, which the
     * game sets to $40 = B) AND the cutscene's override pointer ($6B06/$6B07)
     * is non-zero. We used to send Start ($10), which the game just treats as
     * a keypress, so the tree scene and Dave's yard speech never skipped even
     * though both set an override. While an override is armed, RMB / Enter /
     * Space / Esc send exactly the exit key (alone, so the equality holds).
     * Outside cutscenes nothing changes. */
    {
        uint8_t *ov_lo = cart_ram_byte(0x6B06);
        uint8_t *ov_hi = cart_ram_byte(0x6B07);
        uint8_t *exit_key = cart_ram_byte(0x6128);
        bool skippable = ov_lo && ov_hi && exit_key && (*ov_lo | *ov_hi) && *exit_key;
        /* (Escape removed 2026-11-09: it opens the save/load menu now) */
        bool skip_req = rmb || keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RETURN];
        /* Once a skip press has been sent, the same physical press must not
         * turn into Start/A when the cutscene ends under it (the edge from
         * $40 to $10 would latch a fresh Start on the next screen, e.g.
         * pressing START on character select). Swallow it until release. */
        static bool skip_held = false;
        if (!skip_req) skip_held = false;
        if (skip_req && (skippable || skip_held)) {
            out &= (uint8_t)~(BTN_A | BTN_B | BTN_SELECT | BTN_START);
            if (skippable) {
                out |= *exit_key;
                skip_held = true;
                if (log) fprintf(log, "[skip] override=$%02X%02X exit_key=$%02X -> out=$%02X\n",
                                 *ov_hi, *ov_lo, *exit_key, out);
            }
        }
    }

    /* 2026-11-09: context RMB. On a fresh RMB press in gameplay that is still
     * going out as A (i.e. not consumed by the cutscene skip above), preset
     * the sentence verb for the object under the cursor. See ctx_rmb_apply. */
    {
        /* 0 idle, 1 verb requested (A held back), 2 A sent, swallow until release */
        static int ctx_state = 0, ctx_wait = 0;
        static uint8_t ctx_verb = 0;
        static int ctx_slot = 0;
        static bool prev_rmb = false;
        if (ctx_state == 0 && rmb && !prev_rmb && game_started && (out & BTN_A)) {
            uint8_t v = ctx_rmb_pick(log, &ctx_slot);
            if (v) {
                ctx_select_verb(v);
                ctx_verb = v;
                ctx_state = 1;
                ctx_wait = 0;
            }
        }
        if (ctx_state == 1) {
            uint8_t *run = cart_ram_byte(0x6CAC);
            /* Wait for script 4 to FINISH the verb selection: clicking the
             * object while it still runs restarts it and the selection is
             * lost (tested: $611A showed the verb after 2 polls, but the
             * object click still ran GO TO). */
            static int settle = 0;
            bool ready = wram(0x611A) == ctx_verb && run && *run != 1 &&
                         !ctx_script_running(4);
            if (!ready) settle = 0;
            else if (settle < CTX_SETTLE_POLLS) { settle++; ready = false; }
            if (ready || ++ctx_wait > 120) {
                settle = 0;
                /* Selecting a verb clears the sentence object ($611B); the
                 * game's hover logic only refills it when the hovered object
                 * CHANGES, and the cursor hasn't moved, so the click would
                 * land with no object and do nothing (tested). Put the
                 * object back the way hovering would. */
                uint8_t *olo = cart_ram_byte(0x611B), *ohi = cart_ram_byte(0x601B);
                if (olo) *olo = wram((uint16_t)(0x64F2 + ctx_slot));
                if (ohi) *ohi = wram((uint16_t)(0x64CF + ctx_slot));
                out |= BTN_A;                 /* the object click */
                ctx_state = 2;
                if (log) fprintf(log, "[ctx] verb $%02X %s after %d polls, clicking object\n",
                                 ctx_verb, ready ? "selected" : "TIMEOUT", ctx_wait);
            } else {
                out &= (uint8_t)~BTN_A;
            }
        } else if (ctx_state == 2) {
            if (rmb) out &= (uint8_t)~BTN_A;  /* no second edge while held */
            else ctx_state = 0;
        }
        prev_rmb = rmb;
        ctx_busy = ctx_state != 0;
    }

    /* 2026-11-09: LMB on an object stays GO TO (user choice, DOTT-style).
     * The preview below makes the game adopt the shown verb (tested: LMB on
     * the sign ran READ while "READ SIGN" was showing), so on a fresh LMB
     * while a preview is up: put GO TO back, force the hover redraw, hold
     * the click until the hover script has redrawn ($61D8/$61D9 no longer
     * our $FF), then send it. Preview stays paused until LMB is released. */
    {
        static int lmb_state = 0, lmb_wait = 0;
        static bool prev_lmb = false;
        uint8_t *sv = cart_ram_byte(0x611A);
        uint8_t *h8 = cart_ram_byte(0x61D8), *h9 = cart_ram_byte(0x61D9);
        if (lmb_state == 0 && lmb && !prev_lmb && game_started && !ctx_busy && (out & BTN_A) &&
            sv && h8 && h9 && preview_verb != V_GOTO && *sv == preview_verb) {
            *sv = V_GOTO;
            *h8 = 0xFF;
            *h9 = 0xFF;
            preview_verb = V_GOTO;
            lmb_state = 1;
            lmb_wait = 0;
            if (log) fprintf(log, "[lmb] preview cleared, GO TO before click\n");
        }
        if (lmb_state == 1) {
            bool redrawn = h8 && h9 && *h8 != 0xFF && *h9 != 0xFF;
            if (redrawn || ++lmb_wait > 30) {
                out |= BTN_A;
                lmb_state = 2;
                if (log) fprintf(log, "[lmb] click after %d polls%s\n", lmb_wait, redrawn ? "" : " (TIMEOUT)");
            } else {
                out &= (uint8_t)~BTN_A;
            }
        } else if (lmb_state == 2) {
            if (lmb) out &= (uint8_t)~BTN_A;   /* one click per press */
            else { lmb_state = 0; walk_pause_start(); }
        }
        prev_lmb = lmb;
        lmb_busy = lmb_state != 0;
    }

    /* 2026-11-09: sentence-line preview of the RMB verb. While hovering a
     * room object with no verb picked (sentence verb is GO TO, or our own
     * previous preview), show "<context verb> <object>" so the player sees
     * what RMB will do. $611A is display-only (writing it does not change
     * what LMB executes - tested), but the line is only redrawn by the
     * hover script's drawSentence (opcode $AC) when the hovered object
     * changes, so we also clear its last-hovered var $61D9 to force that.
     * Skipped while a verb was explicitly clicked (CLICK_AREA $6120 == 1),
     * during a context click in progress, and outside gameplay/cutscenes. */
    {
        uint8_t *sv = cart_ram_byte(0x611A);
        uint8_t *ov_lo = cart_ram_byte(0x6B06), *ov_hi = cart_ram_byte(0x6B07);
        bool cutscene = ov_lo && ov_hi && (*ov_lo | *ov_hi);
        if (sv && game_started && !cutscene && !ctx_busy && !lmb_busy && !walk_pause_active() &&
            wram(0x6120) != 1 &&
            (*sv == V_GOTO || *sv == preview_verb)) {
            uint8_t want = V_GOTO;
            if (wram(0x711A) == ROOM_BOX) {
                int slot = ctx_find_object_slot(wram(0x611E), wram(0x611F));
                if (slot) {
                    uint8_t verbs[32];
                    int n = ctx_object_verbs(slot, verbs, 32);
                    want = ctx_choose_verb(slot, verbs, n);
                }
            }
            if (want != *sv) {
                *sv = want;
                /* force the hover redraw: the hover script keeps the last
                 * hovered object in vars $D8/$D9 (both seen tracking it) */
                uint8_t *h8 = cart_ram_byte(0x61D8), *h9 = cart_ram_byte(0x61D9);
                /* $FF never matches (0 would match "nothing hovered") */
                if (h8) *h8 = 0xFF;
                if (h9) *h9 = 0xFF;
                if (log) fprintf(log, "[preview] sentence verb -> $%02X\n", want);
            }
            preview_verb = want;
        }
    }

    /* 2026-11-07: live report -- in-game cursor sprite tracks the real
     * mouse fine (confirms SDL_GetMouseState/window focus are all good),
     * but LMB clicks never register at all (confirms SDL_GetGlobalMouseState
     * -- the separate, focus-independent query used for buttons -- isn't
     * seeing the press on this machine/setup, even though position tracking
     * via the focus-gated SDL_GetMouseState a few lines up works every
     * frame). Root cause not chased further; added Z as a keyboard-driven
     * stand-in for BTN_A (matches common NES-emulator-on-keyboard
     * convention) so the character-select screen -- and anything else
     * gated on A -- is reachable regardless of that bug. */
    if (keys[SDL_SCANCODE_Z]) out |= BTN_A;
    if (keys[SDL_SCANCODE_TAB] || keys[SDL_SCANCODE_S]) out |= BTN_SELECT; /* RE aid: Select */

    /* 2026-11-06: D-pad was defined (BTN_UP/DOWN/LEFT/RIGHT) but never
     * wired to any input source -- this mod only ever drove A/B/Start.
     * That's fine for verb-grid gameplay (absolute cursor positioning
     * replaces needing a d-pad there), but screens like character select
     * aren't driven by cursor position at all: the ROM's own portrait
     * highlight is a focus INDEX that only moves on a real d-pad
     * Left/Right press, with A confirming whichever portrait currently
     * holds focus. No amount of clicking a portrait can satisfy that --
     * confirmed by testing (clicking two portraits + Start never left
     * the "PLEASE CHOOSE TWO OTHER KIDS" screen). Arrow keys here let
     * that screen (and any other index-driven menu) work the original
     * way, independent of the mouse-click path above. */
    /* d-pad takes the cursor back: stop restoring the mouse position */
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_RIGHT])
        s_unclamp_on = false;
    if (keys[SDL_SCANCODE_UP])    out |= BTN_UP;
    if (keys[SDL_SCANCODE_DOWN])  out |= BTN_DOWN;
    if (keys[SDL_SCANCODE_LEFT])  out |= BTN_LEFT;
    if (keys[SDL_SCANCODE_RIGHT]) out |= BTN_RIGHT;

    /* 2026-11-07: TEMP diagnostic -- isolate whether SDL is even seeing
     * synthetic arrow-key presses at all (vs. the game logic not reacting
     * to a real $02 d-pad bit). Logs any frame where any arrow/Z scancode
     * reads true. Remove once the live-arrow-key regression is understood. */
    if (log && (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_DOWN] ||
                keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_RIGHT] ||
                keys[SDL_SCANCODE_Z])) {
        fprintf(log, "[keydiag] U=%d D=%d L=%d R=%d Z=%d out=0x%02x focus=%d\n",
            keys[SDL_SCANCODE_UP], keys[SDL_SCANCODE_DOWN],
            keys[SDL_SCANCODE_LEFT], keys[SDL_SCANCODE_RIGHT],
            keys[SDL_SCANCODE_Z], out, have_focus);
        fflush(log);
    }

    static uint8_t prev_out = 0;
    click_watch_tick(log, out, prev_out);
    prev_out = out;

    return out;
}
#else
/* No SDL in this build (headless) -- fixed test pattern so the hook/RTS
 * plumbing stays exercised by the headless regression runs. */
static uint8_t real_mouse_buttons(void) {
    static uint32_t n = 0;
    return (n++ / 120) % 2 ? BTN_A : 0;
}
#endif

/* RE probe mode (temporary): NESRECOMP_RE_PROBE=<path> dumps a per-frame
 * full-RAM diff (JSONL, same shape as the main_runner WRAM trace, which the
 * cyc host never wires up) without taking over button state, so --input
 * scripts still drive the real $4016 poll underneath. Used to locate the
 * cursor-position bytes by correlating RAM deltas with real d-pad holds. */
static FILE *s_re_probe_f = NULL;
static int   s_re_probe_state = -1;
static uint8_t s_re_probe_prev[0x800];

static void re_probe_frame(void) {
    if (s_re_probe_state < 0) {
        const char *e = getenv("NESRECOMP_RE_PROBE");
        s_re_probe_f = (e && *e) ? fopen(e, "w") : NULL;
        s_re_probe_state = s_re_probe_f ? 1 : 0;
    }
    if (s_re_probe_state != 1) return;
    static uint32_t frame_no = 0;
    for (int a = 0; a < 0x800; a++) {
        uint8_t v = *ram_byte((uint16_t)a);
        if (v != s_re_probe_prev[a]) {
            fprintf(s_re_probe_f, "{\"f\":%u,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                    frame_no, a, s_re_probe_prev[a], v);
            s_re_probe_prev[a] = v;
        }
    }
    frame_no++;
    fflush(s_re_probe_f);
}

static int joypad_poll_hook(uint16_t addr) {
    static uint32_t fire_count = 0;
    if (s_re_probe_state == 1 || getenv("NESRECOMP_RE_PROBE")) {
        re_probe_frame();
        return 0; /* let the real $4016 poll run underneath */
    }

    uint8_t pad1_old = *ram_byte(0x02);
    uint8_t pad2_old = *ram_byte(0x03);
    *ram_byte(0x04) = pad1_old; /* edge-detect stash, same as the original */
    *ram_byte(0x05) = pad2_old;

    uint8_t cur = real_mouse_buttons();
    *ram_byte(0x02) = cur;                  /* player 1: mouse-driven */
    *ram_byte(0x03) = 0x00;                 /* player 2: unused, no input */

    /* 2026-11-06: $02/$03 is only the raw joypad-strobe snapshot. A known
     * working SNES-mouse hack for this same ROM family (HonkeyKong's
     * maniac_mousedriver.asm) confirms the game's menu/click hit-test code
     * (character select, verb grid) actually reads two SEPARATE latched
     * registers it calls LowerButtons ($002F, holds B/Select/Start) and
     * UpperButtons ($710E, holds A) -- not the raw $02 snapshot. That hack
     * ORs its injected mouse bits directly into those two addresses every
     * frame rather than relying on $02 to propagate into them. We replaced
     * the entire $FB68 raw-read routine, so if something downstream was
     * relying on that routine (or its caller) to also populate $2F/$710E
     * from $02, it no longer happens -- explaining why Start-driven cutscene
     * skip works (reads $02/$04 directly) but A-button portrait/verb clicks
     * never did anything. Mirror the hack: write the same bits directly.
     *
     * 2026-11-06 (follow-up): the reference hack only stores into these
     * registers on a RISING EDGE of the real controller read (it checks
     * InputBuffer, the previous frame's snapshot, was clear before storing
     * the new one) -- this is a one-frame "fresh click" pulse, not the
     * held/level state. Latching the raw held bit every frame (as below,
     * originally) re-fires the click event on every single frame the mouse
     * button stays down, which is enough to toggle a verb selection back
     * off (or re-trigger) before a second click can land -- symptom:
     * single-click menu buttons appeared to work (one frame of travel is
     * enough), but two-step verb+object sequences (open door, get
     * flashlight) never completed. Edge-detect our own synthetic state the
     * same way: only pulse the bit on press, write 0 every other frame
     * (including while still held), so the game sees exactly one click. */
    static uint8_t prev_cur = 0;
    uint8_t pressed_edge = (uint8_t)(cur & ~prev_cur);
    prev_cur = cur;

    uint8_t *lower = ram_byte(0x002F);
    uint8_t *upper = cart_ram_byte(0x710E);
    /* 2026-11-09: match the ROM exactly (b0F:FB90-FBAD): on a rising edge it
     * stores the held B/Select/Start bits into $2F (and A into $710E) and
     * otherwise LEAVES THE LATCH ALONE; the consumers read-and-clear it
     * (read_clear_key_latch b0F:FBB7, read_clear_A_latch b0F:FBAE). We used
     * to write the edge value every poll, i.e. 0 on every frame after the
     * press, which erased presses before slow consumers (the main loop
     * during cutscenes) ever saw them: cutscene skip never landed, and
     * clicks were flaky. */
    if (pressed_edge & (BTN_B | BTN_SELECT | BTN_START))
        *lower = (uint8_t)(cur & (BTN_B | BTN_SELECT | BTN_START));
    if (upper && (pressed_edge & BTN_A))
        *upper = (uint8_t)(cur & BTN_A);

    /* 2026-11-07: this used to only go to stderr, which a Windows GUI build
     * has nowhere to send -- made it invisible in mouse_debug.log, so there
     * was no way to tell whether the $9EAB bank-0 site was ever actually
     * firing (vs. silently CRC-mismatching in cyc_hooks_fire()'s holds()
     * check, e.g. because a different bank is swapped into $8000-$BFFF at
     * that PC on the real call path) or just never being reached. Mirror
     * into dbg_log() too, with a separate fire count per hook address so the
     * two sites don't get confused in one counter. */
    static uint32_t fire_count_fb68 = 0, fire_count_9eab = 0;
    uint32_t *site_count = (addr == 0x9EAB) ? &fire_count_9eab : &fire_count_fb68;
    if (fire_count < 10 || *ram_byte(0x02) != 0) {
        fprintf(stderr, "[mouse-cursor] poll #%u at $%04X -> pad1=$%02X pad2=$%02X\n",
            fire_count, addr, *ram_byte(0x02), *ram_byte(0x03));
    }
    FILE *log = dbg_log();
    if (log && (*site_count < 10 || (*site_count % 300) == 0 || *ram_byte(0x02) != 0)) {
        fprintf(log, "[mouse-cursor] poll #%u at $%04X -> pad1=$%02X pad2=$%02X\n",
            *site_count, addr, *ram_byte(0x02), *ram_byte(0x03));
        fflush(log);
    }
    (*site_count)++;
    fire_count++;
    return 1; /* handled: RTS-style return to $8322, the real call site's next instruction */
}

/* The game's own save routine ran (b00:$9659, SCUMM saveLoadGame's save
 * branch; $96C3 is restore). Observe only: return 0 so the original body runs. The Escape
 * menu code writes ManiacMansion.state once the game is back in play. */
static int game_save_observer(uint16_t addr) {
    (void)addr;
    s_game_saved_frames = 0;
    FILE *log = dbg_log();
    if (log) { fprintf(log, "[menu] game's own save routine ran\n"); fflush(log); }
    return 0;
}

/* cursor_to_oam ($9539), reached right after cursor_move_dpad clamped the
 * cursor: put the mouse's unclamped position back (observer, returns 0). */
static int cursor_unclamp_hook(uint16_t addr) {
    (void)addr;
#ifdef CYC_WITH_SDL
    if (s_unclamp_on && !s_menu_open) {
        uint8_t *x = cart_ram_byte(CURSOR_SRC_X), *y = cart_ram_byte(CURSOR_SRC_Y);
        if (x && y) { *x = s_unclamp_x; *y = s_unclamp_y; }
    }
#endif
    return 0;
}

NES_MOD_CONSTRUCTOR(register_mouse_cursor_hook) {
#ifdef CYC_WITH_SDL
    /* the GPU CRT filter needs SDL's OpenGL renderer (Windows would pick
     * Direct3D); an explicit SDL_RENDER_DRIVER still wins */
    if (!getenv("SDL_RENDER_DRIVER")) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
    nes_mod_register_function_entry_plugin(HOOK_ID, 0xFB68, joypad_poll_hook);
    nes_mod_register_function_entry_plugin(HOOK_ID_BANK0, 0x9EAB, joypad_poll_hook);
    nes_mod_register_function_entry_plugin(HOOK_ID_GAME_SAVE, 0x9659, game_save_observer);
    nes_mod_register_function_entry_plugin(HOOK_ID_UNCLAMP, 0x9539, cursor_unclamp_hook);
    /* RE experiments need stock $4016 polling so --input scripts drive the
     * real controller path; NESRECOMP_MOUSE_HOOK_DISABLE=1 leaves the hook
     * registered but inactive. Unset (the normal case) behaves as before. */
    int enabled = getenv("NESRECOMP_MOUSE_HOOK_DISABLE") ? 0 : 1;
    nes_mod_set_function_hook_enabled(HOOK_ID, enabled);
    nes_mod_set_function_hook_enabled(HOOK_ID_BANK0, enabled);
    nes_mod_set_function_hook_enabled(HOOK_ID_GAME_SAVE, 1); /* observer only, always on */
    nes_mod_set_function_hook_enabled(HOOK_ID_UNCLAMP, enabled);
}

/* 2026-11-09: the host-extras surface, for the Escape menu (SDL builds). */
#ifdef CYC_WITH_SDL
static const CycHostExtras s_extras = {
    .present = menu_present,
    .frame_end = menu_frame_end,
    .event = menu_event,
    .present_texture = display_present_texture,
    .load_setting = display_load_setting,
    .save_settings = display_save_settings,
};
const CycHostExtras *cyc_host_extras(void) { return &s_extras; }
#else
const CycHostExtras *cyc_host_extras(void) { return NULL; }
#endif
