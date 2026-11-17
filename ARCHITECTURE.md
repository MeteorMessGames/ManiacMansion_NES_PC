# Architecture

How the port is put together. For using the game see [README.md](README.md); for building it see [BUILDING.md](BUILDING.md).

The port is a mod on top of [NESRecomp](https://github.com/mstan/nesrecomp). NESRecomp runs the NES game (recompiled to C, or interpreted in the release build) and gives the mod a few hook points. Everything here sits on the host side: input, menus, saves, audio mixing, picture filters and achievements. The adventure logic is always the game's own.

## Source files

| File | Responsibility |
|---|---|
| `mouse_cursor_hook.c` | Input (mouse, keyboard, controller), context verbs, the pause menu, options, save-state slots, audio routing |
| `menu_render.c` | Menu drawing (stb_truetype with an embedded DejaVu Sans Bold) |
| `achievements.c`, `achievements_table.h` | Achievement engine, and the generated table of conditions, rarities and points |
| `achievement_sfx.h` | The unlock chime |
| `update_check.c` | The launch-time update check (background thread) |
| `crt_gpu.c`, `crt_filter.c` | The CRT filters and the TV-shape sharp scaler, GPU and CPU versions |
| `rom_db.h`, `rom_picker.c` | The known ROMs by PRG hash, and the window that asks which to play |
| `romless_native.c.in` | Stand-in for the generated game code in the release build (ROM hashes and hook sites only; finds the ROM next to the exe) |
| `game.toml` | Hook sites and their CRC32 checks |
| `runner-patches/` | The small NESRecomp patch (display controls, GPU present hook, APU write hook, per-channel volume, a second accepted ROM and its cartridge-save name for the release build, no window flash while loading) |

## Hook sites

Each site in `game.toml` is matched by address and checked by the CRC32 of the bytes there, so a different ROM refuses to hook. The prototype and retail US ROM have identical hook sites, opcodes and object numbers, so the release build accepts both (the runner patch adds a second PRG hash).

Any other Maniac Mansion ROM (patched versions such as the community "Uncensored" hack, in any patch combination) is accepted when it is MMC1 with 256 or 512 KB of PRG and the four hook sites are intact (`mm_is_family` in `rom_db.h`). Those hacks ship their own mouse driver, which redirects the joypad call to `$9810`; the release build lists a fifth hook site there (added in `CMakeLists.txt`, since the recompiler needs every `game.toml` site to exist in its build ROM).

Each ROM keeps its own progress: save slots (`ManiacMansion.<tag>.slotN.*`), cartridge save (`saves/ManiacMansion_<tag>.sav`) and achievement records (every other ROM's ids are hashed with `#` and its tag, `#retail` or `#x<HASH>`, stored beside the prototype's).

| Site | What the mod does there |
|---|---|
| Joypad read (`$FB68`, bank-0 copy `$9EAB`; `$9810` in the Uncensored hacks, which redirect the call to their own mouse driver) | Replaces the controller bits with the mouse, keyboard or pad state. Presses are written the way the ROM latches them, on the rising edge only. |
| Cursor clamp (`$9539`) | Undoes the game's clamp of its cursor position, so the mouse reaches the screen edges. In play the cursor is held to the room area. |
| Game save (`$9659`) | Mirrors the cartridge battery save into the save-state slot used this session. |

Audio is mixed on the host side: the pause menu silences the game, and the achievement chime is added to the output. Controllers are read through SDL's game controller API; the runner's own pad mapping is bypassed by the joypad hook.

## Context verbs (right click)

The game itself does not have a "do the obvious thing" action, so the mod works one out from the game's data:

1. **Find the object under the cursor.** The room loader fills arrays of object numbers, positions and states. The mod reads them and finds the object at the mouse position the same way the game does.
2. **Pick a verb.** The object's SCUMM v2 verb scripts are inspected, for example whether its GET script really picks the object up, whether it is a door (OPEN / CLOSE by its state), or a switchable thing (TURN ON / OFF by its state bit). A small override table handles the few objects where the scripts do not say enough.
3. **Replay the click through the game.** The mod writes the verb into the game's own sentence variables, waits for the game's verb script to settle, restores the object, and sends one A press. The game then runs the command itself, so every message and side effect is the original.
4. **Preview.** While hovering with no verb picked, the chosen verb is shown on the sentence line, and the game's hover redraw is forced so it appears.

Two-object commands (an item used on something) are two replayed clicks.

## Save states and slots

Save states are serialised emulator state files, one per slot (`ManiacMansion.slotN.state`), with a thumbnail and a timestamp. The slot list grows with the saves that exist, plus one empty slot when saving. A save state only loads in the exact build that wrote it.

The game's own save is separate: it is the cartridge battery save, and survives updates.

## Rendering

The runner presents a 256x240 frame. The mod chooses the scale (automatic whole-number scale or a fixed one) and the shape (square pixels or 4:3 TV, with NES pixels 8:7 wide). *Sharp* is integer scaling, kept crisp at TV shape with a sharp-bilinear blend. *CRT* and *CRT Strong* draw scanlines that widen on bright colours, an RGB stripe mask and a soft glow; they run on the GPU through OpenGL with a CPU fallback.

## Achievements

`achievements_table.h` is generated: each achievement is a list of conditions on the game's RAM (equality, comparison with a mask, a script being active, or "was true earlier this session"). The engine checks the table every frame while a game is in play and holds a condition for a few frames before it counts, so loading or starting the game cannot unlock anything by accident. Unlocks are stored in `ManiacMansion.dat` next to the exe: every record carries its own signature tag, and the file is encrypted with a key that ships in the program, which keeps casual looking and editing out. The exe itself is never rewritten.

## Update check

At launch a background thread asks GitHub once whether a newer release exists, and the game shows a short notice if so. Nothing about the user is sent. `UpdateCheck = 0` in `config.ini` turns it off.

## Performance notes

- The ROM's identity is its PRG hash, and the runner recomputes it over the whole PRG (256 or 512 KB) on every call, so the menu and the achievement code take it once per run (`rom_hash_cached`, `s_salt_hash`).
- The save-slot scan reads the folder once and keeps the exe folder path; it used to ask the OS for the path and stat a file for each of up to 999 slots.
- Menu steps that take 12 ms or more are written to `mouse_debug.log` as `[perf]` lines (`perf_end` in `mouse_cursor_hook.c`), so a slow machine can be diagnosed from the log.
