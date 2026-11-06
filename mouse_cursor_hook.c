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
 * Mansion's interface); relative mouse motion -> BTN_LEFT/RIGHT/UP/DOWN,
 * using SDL_GetRelativeMouseState() (accumulates since the last poll,
 * works without needing the window handle -- cyc_sdl.c doesn't expose one
 * to HOST_EXTRAS code). A small pixel deadzone avoids jitter spamming the
 * d-pad on tiny untended mouse movement. This maps motion to discrete
 * taps, approximating a mouse cursor using the game's existing digital
 * cursor-movement speed -- not true 1:1 cursor tracking, which would need
 * the verb-cursor's RAM position and isn't implemented yet. In a headless
 * (no-SDL) build this falls back to a fixed test pattern so the project
 * still builds/runs there for regression-testing the hook plumbing.
 *
 * RIGHT-CLICK / DOTT-style default-action: SCOPED BACK, not implemented
 * (see mmresume.md's "Right-click" section -- this NES port's classic
 * 15-verb grid interface, not DOTT's 2-button one, makes a true
 * "default verb for the object under the cursor" feature a real
 * reverse-engineering project on its own, not attempted here).
 *
 * RIGHT-CLICK -> cutscene skip (2026-11-06, narrower ask): RMB -> BTN_START.
 * Mirrors the original PC SCUMM version's own convention (right mouse
 * button / Esc skips a cutscene there -- confirmed via web search of the
 * PC manual). The NES port's actual skip button is NOT independently
 * confirmed from this ROM's disassembly or direct testing -- one itch.io
 * page (describing what may be a fan project, not necessarily this
 * cartridge) says Start skips the opening cutscene, and Start is also the
 * conventional "skip/pause" button across NES games generally, so it's
 * the reasonable first guess. Verify empirically next time the build is
 * tested against an actual cutscene; remap here if wrong (BTN_B is the
 * next candidate to try).
 */
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_host_extras.h"
#include <stdio.h>
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

/* No deadzone (2026-11-06, user request): any nonzero accumulated
 * relative motion taps a d-pad direction. Was 2px to damp jitter from a
 * barely-moving mouse; removed because it made small/careful movements
 * feel unresponsive on top of the cursor's existing fixed-step-per-frame
 * speed cap (see the file header's accuracy note). */
#define MOTION_DEADZONE_PX 1

static uint8_t *ram_byte(uint16_t addr) {
    return (uint8_t *)&cyc_cpu_ram()[addr & 0x7FF];
}

/* Carries motion below MOTION_DEADZONE_PX forward instead of discarding it
 * each poll, so a slow/careful mouse movement still eventually registers
 * instead of never crossing the threshold in any single frame. */
static int s_carry_dx = 0, s_carry_dy = 0;
static bool s_relative_mode_set = false;

#ifdef CYC_WITH_SDL
static uint8_t real_mouse_buttons(void) {
    if (!s_relative_mode_set) {
        /* Capture the OS cursor so deltas aren't clamped/distorted by the
         * window edge or OS pointer-acceleration curves -- without this,
         * SDL_GetRelativeMouseState() still "works" but its input is the
         * real, ballistics-adjusted, edge-clamped system cursor, not a
         * clean raw delta. The game draws its own in-game cursor sprite,
         * so hiding the OS one is correct here, not a regression. */
        SDL_SetRelativeMouseMode(SDL_TRUE);
        s_relative_mode_set = true;
    }
    int dx = 0, dy = 0;
    Uint32 buttons = SDL_GetRelativeMouseState(&dx, &dy);
    dx += s_carry_dx;
    dy += s_carry_dy;
    uint8_t out = 0;
    if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) out |= BTN_A;
    if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) out |= BTN_START; /* cutscene skip -- see below */
    if (dx <= -MOTION_DEADZONE_PX) { out |= BTN_LEFT; dx = 0; }
    else if (dx >= MOTION_DEADZONE_PX) { out |= BTN_RIGHT; dx = 0; }
    if (dy <= -MOTION_DEADZONE_PX) { out |= BTN_UP; dy = 0; }
    else if (dy >= MOTION_DEADZONE_PX) { out |= BTN_DOWN; dy = 0; }
    s_carry_dx = dx;
    s_carry_dy = dy;
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

static int joypad_poll_hook(uint16_t addr) {
    static uint32_t fire_count = 0;

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
    nes_mod_set_function_hook_enabled(HOOK_ID, 1);
}
