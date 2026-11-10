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
    if (lx < 0) lx = 0; else if (lx > 255) lx = 255;
    if (ly < 0) ly = 0; else if (ly > 239) ly = 239;
    *src_x = (uint8_t)lx;
    *src_y = (uint8_t)ly;
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
 *   - SAVE GAME writes that snapshot (the moment the menu opened) to
 *     <exe dir>/ManiacMansion.state via cyc_state_save_file (atomic).
 *   - LOAD GAME loads that file and closes the menu.
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
enum { MI_RESUME, MI_SAVE, MI_LOAD, MI_EXIT, MI_COUNT };
static const char *const k_menu_rows[MI_COUNT] = { "Resume Game", "Save State", "Load State", "Exit Game" };

static int      s_game_saved_frames = -1;  /* -1 idle, else frames since the game's own save */
static int      s_toast_frames = 0;        /* "SAVE FILE UPDATED" banner countdown */
static bool     s_menu_open = false;
static bool     s_menu_guard = false;          /* swallow input until all released */
static int      s_menu_sel = 0;
static int      s_menu_hover = -1;             /* row under the mouse, or -1 */
static uint8_t *s_menu_snap = NULL;
static size_t   s_menu_snap_len = 0;
static uint32_t s_menu_frozen[MENU_W * MENU_H];
static uint32_t s_menu_pic[MENU_W * MENU_H];   /* 256x240 toast picture */
static uint32_t *s_hi_pic = NULL;              /* window-resolution menu picture */
static int      s_hi_w = 0, s_hi_h = 0;        /* its size = the presented size */
static char     s_menu_status[96] = "";
static uint32_t s_menu_status_rgb = 0x7CE08A;
/* queued from the event callback, run in frame_end (a frame boundary) */
static bool     s_menu_req_toggle = false;
static int      s_menu_req_activate = -1;

static void save_path(char *out, size_t n) {
    char *base = SDL_GetBasePath();
    snprintf(out, n, "%sManiacMansion.state", base ? base : "");
    if (base) SDL_free(base);
}

/* Sidebar geometry for a presented size (all derived from the height). */
typedef struct { int sw, pad, item_y0, item_h, item_gap, title_y, sub_y, sep_y; float u; } MenuLayout;
static MenuLayout menu_layout(int w, int h) {
    MenuLayout L;
    L.u = (float)h / 240.0f;
    L.sw = (int)(w * 0.40f);
    L.pad = (int)(10 * L.u);
    L.title_y = (int)(16 * L.u);
    L.sub_y = (int)(33 * L.u);
    L.sep_y = (int)(46 * L.u);
    L.item_y0 = (int)(58 * L.u);
    L.item_h = (int)(19 * L.u);
    L.item_gap = (int)(4 * L.u);
    return L;
}

static int menu_row_at(int x, int y) {
    if (s_hi_w <= 0) return -1;
    MenuLayout L = menu_layout(s_hi_w, s_hi_h);
    if (x < L.pad / 2 || x >= L.sw - L.pad / 2) return -1;
    for (int i = 0; i < MI_COUNT; i++) {
        int y0 = L.item_y0 + i * (L.item_h + L.item_gap);
        if (y >= y0 && y < y0 + L.item_h) return i;
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

static void menu_open_now(void) {
    char err[160];
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!cyc_state_save(&buf, &len, err, sizeof err)) { menu_log("open failed", err); return; }
    s_menu_snap = buf;
    s_menu_snap_len = len;
    const uint32_t *f = cyc_frame_argb();
    if (f) memcpy(s_menu_frozen, f, sizeof s_menu_frozen);
    else memset(s_menu_frozen, 0, sizeof s_menu_frozen);
    s_menu_open = true;
    s_menu_sel = MI_RESUME;
    s_menu_hover = -1;
    s_menu_status[0] = 0;
    menu_os_cursor(true);
    menu_log("open", NULL);
}

static void menu_activate(int item) {
    char err[160] = "", path[1100];
    save_path(path, sizeof path);
    if (item == MI_SAVE) {
        /* rewind to the paused moment, then write it */
        if (restore_snapshot(err, sizeof err) && cyc_state_save_file(path, err, sizeof err)) {
            menu_status("Game saved", 0x7CE08A);
            menu_log("saved", path);
        } else {
            menu_status("Save failed", 0xF07070);
            menu_log("save failed", err);
        }
    } else if (item == MI_LOAD) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            menu_status("No save file yet", 0xF0C060);
            menu_log("load: no file", path);
            return;
        }
        fclose(f);
        if (cyc_state_load_file(path, err, sizeof err)) {
            cyc_session_state_loaded();
            after_state_load();
            menu_log("loaded", path);
            menu_close();
        } else {
            menu_status("Save is from a different version", 0xF07070);
            menu_log("load failed", err);
        }
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

/* "Last saved" line from the save file's modification time. */
static void last_saved_text(char *out, size_t n) {
    char path[1100];
    save_path(path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) { snprintf(out, n, "No save file yet"); return; }
    struct tm *tm = localtime(&st.st_mtime);
    if (!tm) { snprintf(out, n, "Save file present"); return; }
    char when[48];
    strftime(when, sizeof when, "%b %d, %H:%M", tm);
    snprintf(out, n, "Last saved %s", when);
}

/* Size to present the menu at: the window's real drawable, letterboxed to
 * the game's 256:240 aspect, so the texture maps ~1:1 and text is crisp. */
static void menu_target_size(int *w, int *h) {
    int ow = 0, oh = 0;
    SDL_Window *win = find_game_window();
    SDL_Renderer *ren = win ? SDL_GetRenderer(win) : NULL;
    if (ren) SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow < MENU_W || oh < MENU_H) { ow = MENU_W * 3; oh = MENU_H * 3; }
    float sx = (float)ow / MENU_W, sy = (float)oh / MENU_H, sc = sx < sy ? sx : sy;
    int tw = (int)(MENU_W * sc + 0.5f), th = (int)(MENU_H * sc + 0.5f);
    if (tw > 4096) { th = th * 4096 / tw; tw = 4096; }
    if (th > 4096) { tw = tw * 4096 / th; th = 4096; }
    *w = tw;
    *h = th;
}

static const uint32_t *menu_present(void *ctx, int *w, int *h) {
    (void)ctx;
    if (!s_menu_open) {
        if (s_toast_frames <= 0) return NULL;
        const uint32_t *f = cyc_frame_argb();
        if (!f) return NULL;
        memcpy(s_menu_pic, f, sizeof s_menu_pic);
        cyc_overlay_toast(s_menu_pic, MENU_W, MENU_H, "SAVE FILE UPDATED", "");
        *w = MENU_W;
        *h = MENU_H;
        return s_menu_pic;
    }
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

    mr_scale_nearest(d, pw, ph, s_menu_frozen, MENU_W, MENU_H, 170);   /* game, lightly dimmed */
    mr_rect(d, pw, ph, 0, 0, L.sw, ph, 0x15161D, 236);                 /* sidebar */
    mr_rect(d, pw, ph, L.sw, 0, (int)(2 * u) > 1 ? (int)(2 * u) : 1, ph, 0x2C6FE0, 200);  /* edge accent */

    {   /* title, shrunk to fit the sidebar */
        float tpx = 13.0f * u;
        int avail = L.sw - 2 * L.pad;
        int tw = mr_text_width(tpx, "Maniac Mansion");
        if (tw > avail && tw > 0) tpx = tpx * avail / tw;
        mr_text(d, pw, ph, L.pad, L.title_y, tpx, 0xFFFFFF, 255, "Maniac Mansion");
    }
    mr_text(d, pw, ph, L.pad, L.sub_y, 7.5f * u, 0x9AA0B4, 255, "Paused");
    mr_rect(d, pw, ph, L.pad, L.sep_y, L.sw - 2 * L.pad, (int)u > 1 ? (int)u : 1, 0x3A3D4C, 255);

    for (int i = 0; i < MI_COUNT; i++) {
        int y0 = L.item_y0 + i * (L.item_h + L.item_gap);
        bool sel = i == s_menu_sel;
        bool hov = i == s_menu_hover;
        if (sel) {
            mr_round_rect(d, pw, ph, L.pad / 2, y0, L.sw - L.pad, L.item_h, (int)(4 * u), 0x2C6FE0, 215);
        } else if (hov) {
            mr_round_rect(d, pw, ph, L.pad / 2, y0, L.sw - L.pad, L.item_h, (int)(4 * u), 0xFFFFFF, 28);
        }
        float tpx = 8.5f * u;
        mr_text(d, pw, ph, L.pad + (int)(6 * u), y0 + (int)((L.item_h - tpx) / 2), tpx,
                sel ? 0xFFFFFF : 0xD6D9E4, 255, k_menu_rows[i]);
    }

    char info[96];
    last_saved_text(info, sizeof info);
    int foot_y = ph - (int)(58 * u);
    if (s_menu_status[0]) mr_text(d, pw, ph, L.pad, foot_y - (int)(12 * u), 7.0f * u, s_menu_status_rgb, 255, s_menu_status);
    mr_text(d, pw, ph, L.pad, foot_y, 6.5f * u, 0x9AA0B4, 255, info);
    mr_rect(d, pw, ph, L.pad, ph - (int)(44 * u), L.sw - 2 * L.pad, (int)u > 1 ? (int)u : 1, 0x3A3D4C, 255);
    static const char *const hints[3] = { "Esc   Resume", "Up / Down   Select", "Enter   Confirm" };
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
        if (sc == SDL_SCANCODE_ESCAPE) { s_menu_req_toggle = true; return; }
        if (!s_menu_open) return;
        if (sc == SDL_SCANCODE_UP)   s_menu_sel = (s_menu_sel + MI_COUNT - 1) % MI_COUNT;
        if (sc == SDL_SCANCODE_DOWN) s_menu_sel = (s_menu_sel + 1) % MI_COUNT;
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_KP_ENTER)
            s_menu_req_activate = s_menu_sel;
        return;
    }
    if (!s_menu_open) return;
    if (ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEBUTTONDOWN) {
        /* Event coordinates arrive in the renderer's logical space, which is
         * the size we present while the menu is open (SDL scales mouse
         * events to the logical size; don't map them again). */
        int x = ev->type == SDL_MOUSEMOTION ? ev->motion.x : ev->button.x;
        int y = ev->type == SDL_MOUSEMOTION ? ev->motion.y : ev->button.y;
        int row = menu_row_at(x, y);
        s_menu_hover = row;
        if (row >= 0) s_menu_sel = row;
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

/* The game's own save (Start menu -> Save game) also refreshes the single
 * save file, newest wins. Its save routine runs inside the game's menu
 * (room $32, verbs off, override armed), so wait until normal play has
 * held for 30 frames (verbs on $6B0A bit 7, no override, cursor sprite
 * visible) and snapshot then: loading it lands in play, not in that menu. */
static void follow_game_save(void) {
    static int good = 0;
    if (s_toast_frames > 0) s_toast_frames--;
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
    save_path(path, sizeof path);
    if (cyc_state_save_file(path, err, sizeof err)) { menu_log("game save -> save file updated", path); s_toast_frames = 120; }
    else menu_log("game save -> save file write failed", err);
}

static void menu_frame_end(void *ctx) {
    (void)ctx;
    follow_game_save();
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
static bool menu_blocks_input(bool lmb, bool rmb) {
    if (s_menu_open) return true;
    if (s_menu_guard) {
        const uint8_t *k = SDL_GetKeyboardState(NULL);
        if (lmb || rmb || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE] ||
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
    /* 2026-11-09: Escape menu open (or its activating press still held):
     * the game gets no input at all. */
    if (menu_blocks_input(lmb, rmb)) return 0;
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
            else lmb_state = 0;
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
        if (sv && game_started && !cutscene && !ctx_busy && !lmb_busy && wram(0x6120) != 1 &&
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

NES_MOD_CONSTRUCTOR(register_mouse_cursor_hook) {
    nes_mod_register_function_entry_plugin(HOOK_ID, 0xFB68, joypad_poll_hook);
    nes_mod_register_function_entry_plugin(HOOK_ID_BANK0, 0x9EAB, joypad_poll_hook);
    nes_mod_register_function_entry_plugin(HOOK_ID_GAME_SAVE, 0x9659, game_save_observer);
    /* RE experiments need stock $4016 polling so --input scripts drive the
     * real controller path; NESRECOMP_MOUSE_HOOK_DISABLE=1 leaves the hook
     * registered but inactive. Unset (the normal case) behaves as before. */
    int enabled = getenv("NESRECOMP_MOUSE_HOOK_DISABLE") ? 0 : 1;
    nes_mod_set_function_hook_enabled(HOOK_ID, enabled);
    nes_mod_set_function_hook_enabled(HOOK_ID_BANK0, enabled);
    nes_mod_set_function_hook_enabled(HOOK_ID_GAME_SAVE, 1); /* observer only, always on */
}

/* 2026-11-09: the host-extras surface, for the Escape menu (SDL builds). */
#ifdef CYC_WITH_SDL
static const CycHostExtras s_extras = {
    .present = menu_present,
    .frame_end = menu_frame_end,
    .event = menu_event,
};
const CycHostExtras *cyc_host_extras(void) { return &s_extras; }
#else
const CycHostExtras *cyc_host_extras(void) { return NULL; }
#endif
