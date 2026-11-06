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
 * CUTSCENE SKIP (2026-11-06, updated again): LMB -> BTN_A, RMB -> BTN_B,
 * each independent; holding both together ORs in BTN_START, matching the
 * standard NES "hold A+B" skip convention instead of overloading RMB alone
 * as a permanent Start-sender (which fired Start on every ordinary RMB
 * context-click too, not just intentional skip attempts).
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

    float lx = 0.0f, ly = 0.0f;
    SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly);
    if (lx < 0) lx = 0; else if (lx > 255) lx = 255;
    if (ly < 0) ly = 0; else if (ly > 239) ly = 239;
    *src_x = (uint8_t)lx;
    *src_y = (uint8_t)ly;
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

static void click_watch_tick(FILE *log, uint8_t cur_buttons, uint8_t prev_buttons) {
    bool pressed_now = (cur_buttons & (BTN_A | BTN_B | BTN_START)) && !(prev_buttons & (BTN_A | BTN_B | BTN_START));
    if (pressed_now) {
        for (int a = 0; a < 0x800; a++) s_click_baseline[a] = *ram_byte((uint16_t)a);
        s_click_watch_left = CLICK_WATCH_FRAMES;
        s_click_watch_logged_pos = 0;
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

static uint8_t real_mouse_buttons(void) {
    int gx = 0, gy = 0;
    Uint32 buttons = SDL_GetGlobalMouseState(&gx, &gy);
    SDL_Window *win = find_game_window();
    raise_once(win);
    sync_grab(win);
    apply_absolute_cursor(win);

    FILE *log = dbg_log();
    static uint32_t diag_n = 0;
    if (log && (diag_n < 20 || (diag_n % 120) == 0)) {
        int ww = -1, wh = -1, wx = -1, wy = -1;
        if (win) { SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowPosition(win, &wx, &wy); }
        uint16_t ya, xa;
        if (find_cursor_oam(&ya, &xa))
            fprintf(log, "[diag] win=%p size=%dx%d pos=%d,%d global=%d,%d buttons=0x%x cursorXY=$%02X,$%02X (oam $%04X)\n",
                (void*)win, ww, wh, wx, wy, gx, gy, buttons, *ram_byte(xa), *ram_byte(ya), ya);
        else
            fprintf(log, "[diag] win=%p size=%dx%d pos=%d,%d global=%d,%d buttons=0x%x cursorXY=<no $FA tile found>\n",
                (void*)win, ww, wh, wx, wy, gx, gy, buttons);
        fflush(log);
    }
    diag_n++;

    /* 2026-11-06: switched to the standard NES A+B-held-together skip
     * convention instead of RMB alone sending Start. LMB -> A, RMB -> B
     * (each its own independent face button now, not bundled with Start),
     * and holding both at once ORs in Start so a cutscene-skip is just
     * "press both mouse buttons" -- no longer overloads RMB as a permanent
     * Start-sender, which would have fired Start on every ordinary RMB
     * context-click too. */
    uint8_t out = 0;
    bool lmb = buttons & SDL_BUTTON(SDL_BUTTON_LEFT);
    bool rmb = buttons & SDL_BUTTON(SDL_BUTTON_RIGHT);
    if (lmb) out |= BTN_A;
    if (rmb) out |= BTN_B;
    if (lmb && rmb) out |= BTN_START;

    /* Space also sends Start (cutscene skip) -- for fast iteration while
     * testing, so skipping doesn't require chasing the grabbed/confined
     * cursor back to a precise right-click. */
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_SPACE]) out |= BTN_START;
    if (keys[SDL_SCANCODE_RETURN]) out |= BTN_START; /* Enter == Start, for now */

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
