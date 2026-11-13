# Maniac Mansion NES PC

Mouse-driven play for the **Maniac Mansion NES prototype** on PC, plus a pause menu, save slots, volume controls and picture filters. Built with [NESRecomp](https://github.com/mstan/nesrecomp).

**No game data is included.** You need your own dump of `Maniac Mansion (Prototype).nes`.

## Download

Get `ManiacMansion_NES_PC_win64.zip` from the [latest release](https://github.com/MeteorMessGames/ManiacMansion_NES_PC/releases/latest) (Windows 64-bit). Unzip it, put your ROM in the folder named exactly `Maniac Mansion (Prototype).nes`, and double-click `ManiacMansion.exe`. Only that exact ROM works.

## Features

- **Point and click.** The mouse drives the game cursor over the whole screen (in play it stops at the top of the room, out of the text lines).
  - **Left click** walks, picks verbs and items (an object with no verb picked = GO TO).
  - **Right click** does the obvious thing with the object under the cursor (GET, OPEN/CLOSE, READ, PUSH/PULL, TURN ON/OFF, USE). The sentence line previews it while you hover. Right click also skips cutscenes.
    - Switchable things flip between TURN ON and TURN OFF by their state.
    - Some things stay plain GO TO, and decorations never default to OPEN/CLOSE.
  - **Middle click** opens the **in-game save menu**: the game's own Save / Restore / New game screen (Start on the NES pad).
  - Two-step commands still work the classic way: pick a verb, then the object(s).
- **Mouse wheel** scrolls the inventory, one notch at a time and only while the verb bar is up.
- **Click to skip text** above the room and during cutscenes.
- **Pause menu (Esc):** save and load states, options, and exit (it asks first if you have unsaved progress).
- **Achievements** (on the [`achievements` branch](https://github.com/MeteorMessGames/ManiacMansion_NES_PC/tree/achievements)): rated Common, Rare, Epic and Legendary, with a banner when you earn one.
- **Options:** volume, fullscreen, filter, scale, shape, mouse and key speed.
- **Update check** at launch (turn off with `UpdateCheck = 0` in `config.ini`).
- **Filters:** Sharp, CRT and CRT Strong.
- **Shape:** square pixels or TV (4:3).

The full player guide (controls, saving, tips) is in [README.txt](README.txt), which ships next to the exe.

## Mouse

| Button | Action |
|---|---|
| Left click | Walk, pick a verb or item (GO TO on objects); skips the current message when you click above the room or during a cutscene |
| Right click | Context verb on the object under the cursor; skips cutscenes |
| Middle click | In-game save menu (Save game / Restore / New game); click again inside it to go back to the game |
| Wheel | Scroll the inventory |

## Controller

| Input | Action |
|---|---|
| D-pad / left stick | Move the cursor (speed follows Options > Key Speed) |
| A | Left click |
| B | Right click (context verb, skips cutscenes) |
| X | Cycle the verb (Select) |
| Y | Skip the current message |
| Start, Back / Select, Guide | Pause menu; D-pad moves in it, A confirms, B goes back |
| Left shoulder | The game's own menu (save / restore) |

## Keyboard

| Key | Action |
|---|---|
| Esc | Pause menu |
| Enter / Space | Skip a cutscene |
| F9 | Filter: Sharp / CRT / CRT Strong |
| F11 | Fullscreen |
| F12 | Screenshot |
| Tab | Cycle the verb (Select) |
| Z | A button (same as left click) |
| Arrow keys | NES d-pad |

## How it works

The port hooks the game at a few ROM sites (listed in `game.toml`, each checked by CRC32):

- **Joypad reads**: mouse buttons become A / B / Start, written the way the ROM latches them (on the rising edge only).
- **Cursor clamp** (`$9539`): restores the mouse position after the game clamps its cursor, so it reaches the screen edges (and, in play, stops at the room's first line).
- **Game save** (`$9659`): mirrors the battery save into this session's save-state slot (first save: an empty slot).
- **Audio mix**: the Esc menu silences the game's sound.
- **Controller**: read through SDL's game controller API (the runner's own pad mapping is bypassed by the joypad hook).

The context verb comes from reading the room's object tables and each object's SCUMM v2 verb scripts (for example, whether its GET script really picks it up). The click is then replayed through the game's own verb/sentence variables, so the game runs the command itself.

Main files:

| File | What |
|---|---|
| `mouse_cursor_hook.c` | Input, context verbs, pause menu, options, save slots, audio routing |
| `menu_render.c` | Menu drawing (stb_truetype + embedded DejaVu Sans Bold) |
| `update_check.c` | The launch-time update check (background thread) |
| `runner-patches/`, `BUILDING.md` | The NESRecomp patch and the build-from-source guide |
| `crt_gpu.c`, `crt_filter.c` | CRT filters and the TV-shape sharp scaler, GPU and CPU versions |
| `romless_native.c.in` | Stand-in for the generated code in the release build (ROM hashes and hook sites only; finds the ROM next to the exe) |
| `game.toml` | Hook sites and their CRCs |

## Building

Full steps are in **[BUILDING.md](BUILDING.md)**. In short: you need NESRecomp at the pinned commit with the small runner patch from [`runner-patches/`](runner-patches/) applied, this folder inside that checkout, CMake 3.20+, SDL2, and your own ROM:

```sh
cmake -S . -B build -DMM_ROM="/path/to/Maniac Mansion (Prototype).nes"
cmake --build build
```

Windows builds are cross-compiled with MinGW (`-DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake`).

Add `-DMM_ROMLESS=ON` for the release build: it leaves the recompiled game code out of the exe, and the player's own ROM (found next to the exe) runs on the runner's interpreter at full speed. The exe then contains none of the game. It is named `ManiacMansion.exe` and opens no console window (`-DMM_CONSOLE=ON` brings the console back for testing). Without it you get `nes_game`, built on the recompiled code, with the console.

The runner patch (display controls, a GPU texture present hook, an APU write hook, per-channel volume) is a small derivative of [NESRecomp](https://github.com/mstan/nesrecomp) and keeps its PolyForm Noncommercial license; see [BUILDING.md](BUILDING.md).

## License

This project's code is licensed under the **MIT License**; see [LICENSE](LICENSE).

Bundled third-party files keep their own licenses: `third_party/stb_truetype.h` (public domain / MIT) and the DejaVu Sans Bold font embedded in `menu_font.h` (DejaVu fonts license). The Windows release also contains the NESRecomp runner (PolyForm Noncommercial 1.0.0) and SDL2 (zlib); their license texts ship in the release's `licenses` folder.

## Credits

- Maniac Mansion is © Lucasfilm Games / LucasArts. This project contains no game code or data and is not affiliated with or endorsed by them.
- [NESRecomp](https://github.com/mstan/nesrecomp) by mstan.
- [stb_truetype](https://github.com/nothings/stb) (public domain / MIT).
- [DejaVu fonts](https://dejavu-fonts.github.io/) (free license).
