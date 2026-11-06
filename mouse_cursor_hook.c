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
 * RIGHT-CLICK -> cutscene skip (2026-11-06, updated): RMB -> BTN_B, i.e. the
 * other NES face button from LMB's BTN_A (user request: "the other input
 * than lmb either a or b"). Previously mapped to BTN_START as a guess --
 * not independently confirmed from this ROM's disassembly or direct
 * testing either way. Verify empirically next time the build is tested
 * against an actual cutscene; BTN_START is the fallback candidate if B
 * doesn't skip it.
 */
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_host_extras.h"
#include <stdio.h>
#include <stdlib.h>
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
const CycHostExtras *cyc_host_extras(void) { return NULL; }

#define HOOK_ID "maniac-mansion.mouse-cursor.joypad-read"

/* Standard NES button bits, matching the assembled byte's layout. */
#define BTN_A      0x80
#define BTN_B      0x40
#define BTN_SELECT 0x20
#define BTN_START  0x10
#define BTN_UP     0x08
#define BTN_DOWN   0x04
#define BTN_LEFT   0x02
#define BTN_RIGHT  0x01

/* Cursor sprite OAM entry (RE'd 2026-11-06 -- see the file header). */
#define CURSOR_OAM_Y    0x0204
#define CURSOR_OAM_X    0x0207

static uint8_t *ram_byte(uint16_t addr) {
    return (uint8_t *)&cyc_cpu_ram()[addr & 0x7FF];
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

static void apply_absolute_cursor(SDL_Window *win, int gx, int gy) {
    if (!s_cursor_hidden) {
        SDL_ShowCursor(SDL_DISABLE); /* the game draws its own cursor sprite */
        s_cursor_hidden = true;
    }
    if (!win) return; /* no window found at all yet: leave cursor where it was */
    int ww = 0, wh = 0, wx = 0, wy = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetWindowPosition(win, &wx, &wy);
    if (ww <= 0 || wh <= 0) return;
    int mx = gx - wx, my = gy - wy; /* desktop coords -> window-relative */
    double sx = (double)ww / 256.0, sy = (double)wh / 240.0;
    double scale = sx < sy ? sx : sy;
    double off_x = (ww - 256.0 * scale) / 2.0;
    double off_y = (wh - 240.0 * scale) / 2.0;
    double nx = (mx - off_x) / scale;
    double ny = (my - off_y) / scale;
    if (nx < 0) nx = 0; else if (nx > 255) nx = 255;
    if (ny < 0) ny = 0; else if (ny > 239) ny = 239;
    *ram_byte(CURSOR_OAM_X) = (uint8_t)nx;
    *ram_byte(CURSOR_OAM_Y) = (uint8_t)ny;
}

static uint8_t real_mouse_buttons(void) {
    int gx = 0, gy = 0;
    Uint32 buttons = SDL_GetGlobalMouseState(&gx, &gy);
    SDL_Window *win = find_game_window();
    apply_absolute_cursor(win, gx, gy);

    static uint32_t diag_n = 0;
    if (diag_n < 20 || (diag_n % 120) == 0) {
        int ww = -1, wh = -1, wx = -1, wy = -1;
        if (win) { SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowPosition(win, &wx, &wy); }
        fprintf(stderr, "[mouse-cursor][diag] win=%p size=%dx%d pos=%d,%d global=%d,%d buttons=0x%x cursorXY=$%02X,$%02X\n",
            (void*)win, ww, wh, wx, wy, gx, gy, buttons,
            *ram_byte(CURSOR_OAM_X), *ram_byte(CURSOR_OAM_Y));
    }
    diag_n++;

    uint8_t out = 0;
    if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) out |= BTN_A;
    if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) out |= BTN_B; /* cutscene skip -- see below */
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

    *ram_byte(0x02) = real_mouse_buttons(); /* player 1: mouse-driven */
    *ram_byte(0x03) = 0x00;                 /* player 2: unused, no input */

    if (fire_count < 10 || *ram_byte(0x02) != 0) {
        fprintf(stderr, "[mouse-cursor] poll #%u at $%04X -> pad1=$%02X pad2=$%02X\n",
            fire_count, addr, *ram_byte(0x02), *ram_byte(0x03));
    }
    fire_count++;
    return 1; /* handled: RTS-style return to $8322, the real call site's next instruction */
}

NES_MOD_CONSTRUCTOR(register_mouse_cursor_hook) {
    nes_mod_register_function_entry_plugin(HOOK_ID, 0xFB68, joypad_poll_hook);
    /* RE experiments need stock $4016 polling so --input scripts drive the
     * real controller path; NESRECOMP_MOUSE_HOOK_DISABLE=1 leaves the hook
     * registered but inactive. Unset (the normal case) behaves as before. */
    nes_mod_set_function_hook_enabled(HOOK_ID, getenv("NESRECOMP_MOUSE_HOOK_DISABLE") ? 0 : 1);
}
