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
| `romless_native.c.in` | Stand-in for the generated game code in the release build (ROM hashes and hook sites only; finds the ROM next to the exe) |
| `game.toml` | Hook sites and their CRC32 checks |
| `runner-patches/` | The small NESRecomp patch (display controls, GPU present hook, APU write hook, per-channel volume) |

## Hook sites

Each site in `game.toml` is matched by address and checked by the CRC32 of the bytes there, so a different ROM refuses to hook.

| Site | What the mod does there |
|---|---|
| Joypad read (`$FB68`, bank-0 copy `$9EAB`) | Replaces the controller bits with the mouse, keyboard or pad state. Presses are written the way the ROM latches them, on the rising edge only. |
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

`achievements_table.h` is generated: each achievement is a list of conditions on the game's RAM (equality, comparison with a mask, a script being active, or "was true earlier this session"). The engine checks the table every frame while a game is in play and holds a condition for a few frames before it counts, so loading or starting the game cannot unlock anything by accident. Unlocks are stored in a signed block inside the exe, so there is no file to edit, and a new build starts with none.

## Update check

At launch a background thread asks GitHub once whether a newer release exists, and the game shows a short notice if so. Nothing about the user is sent. `UpdateCheck = 0` in `config.ini` turns it off.
