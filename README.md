# Maniac Mansion NES PC

Mouse-driven play for the **Maniac Mansion NES prototype** on PC, plus a pause menu, save slots, volume controls and picture filters. It runs on a static recompilation of the game (no emulator at play time), built with [NESRecomp](https://github.com/mstan/nesrecomp).

**No game data is included.** You need your own dump of `Maniac Mansion (Prototype).nes`.

## Features

- **Point and click.** The mouse drives the game cursor over the whole screen.
  - **Left click** walks, picks verbs and items (an object with no verb picked = GO TO).
  - **Right click** does the obvious thing with the object under the cursor: GET what can be taken, OPEN/CLOSE doors, READ signs, PUSH/PULL, TURN ON/OFF, USE. The sentence line previews it while you hover ("READ SIGN"). Right click also skips cutscenes.
  - **Middle click** opens the **in-game save menu**: the game's own Save game / Restore saved game / New game screen (Start on the NES pad).
  - Two-step commands still work the classic way: pick a verb, then the object(s).
- **Pause menu (Esc):** Resume, Save State and Load State (10 slots with screenshots, scrolling list), Options, Exit.
- **Options:** separate Music and Sound FX volume, fullscreen, filter, scale (Auto or a fixed 2x to 6x, for both filters), shape (square pixels or TV 4:3).
- **Filters:**
  - *Sharp*: pixel-perfect integer scaling. *Auto* uses the largest whole scale that fits (6x on 1440p, 4x on 1080p), or pick a fixed 2x to 6x.
  - *CRT* and *CRT Strong*: scanlines that widen on bright colors, an RGB stripe mask and a soft glow (this project's own filter); Strong has thinner beams and darker gaps. Run on the GPU (OpenGL) with a CPU fallback. *Auto* fills the screen, or pick a fixed 2x to 6x.
- The game's own battery save also updates your most recent save-state slot.

The full player guide (controls, saving, tips) is in [README.txt](README.txt), which ships next to the exe.

## Mouse

| Button | Action |
|---|---|
| Left click | Walk, pick a verb or item (GO TO on objects) |
| Right click | Context verb on the object under the cursor; skips cutscenes |
| Middle click | In-game save menu (Save game / Restore / New game) |

## Keyboard

| Key | Action |
|---|---|
| Esc | Pause menu |
| Enter / Space | Skip a cutscene |
| F9 | Filter: Sharp / CRT / CRT Strong |
| F11 | Fullscreen |
| F12 | Screenshot |
| Tab (hold) | Fast forward |
| Z | A button (same as left click) |
| Arrow keys | NES d-pad |

## How it works

The port hooks the recompiled game at a few ROM sites (listed in `game.toml`, each checked by CRC32):

- **Joypad reads**: mouse buttons become A / B / Start, written the way the ROM latches them (on the rising edge only).
- **Cursor clamp** (`$9539`): restores the mouse position after the game clamps its cursor, so it reaches every edge of the screen.
- **Game save** (`$9659`): mirrors the battery save into a save-state slot.

The context verb comes from reading the room's object tables and each object's SCUMM v2 verb scripts (for example, whether its GET script really picks it up). The click is then replayed through the game's own verb/sentence variables, so the game runs the command itself.

Main files:

| File | What |
|---|---|
| `mouse_cursor_hook.c` | Input, context verbs, pause menu, options, save slots, audio routing |
| `menu_render.c` | Menu drawing (stb_truetype + embedded DejaVu Sans Bold) |
| `crt_gpu.c`, `crt_filter.c` | CRT filter, GPU and CPU versions |
| `game.toml` | Hook sites and their CRCs |

## Building

You need a NESRecomp checkout with this folder inside it, CMake 3.20+, SDL2, and your ROM:

```sh
cmake -S . -B build -DMM_ROM="/path/to/Maniac Mansion (Prototype).nes"
cmake --build build
```

Windows builds are cross-compiled with MinGW (`-DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake`).

Add `-DMM_ROMLESS=ON` for the release build: it leaves the recompiled game code out of the exe, and the player's own ROM (found next to the exe) runs on the runner's interpreter at full speed. The exe then contains none of the game.

**Note:** this project also needs a few additions to the NESRecomp runner that aren't published yet: host display controls (fullscreen, filter, fixed scale), a GPU texture present hook, an APU write hook and per-channel volume. Upstream NESRecomp alone won't build it.

## License

This project's code is licensed under the **MIT License**; see [LICENSE](LICENSE).

Bundled third-party files keep their own licenses: `third_party/stb_truetype.h` (public domain / MIT) and the DejaVu Sans Bold font embedded in `menu_font.h` (DejaVu fonts license). The Windows release also contains the NESRecomp runner (PolyForm Noncommercial 1.0.0) and SDL2 (zlib); their license texts ship in the release's `licenses` folder.

## Credits

- Maniac Mansion is © Lucasfilm Games / LucasArts. This project contains no game code or data and is not affiliated with or endorsed by them.
- [NESRecomp](https://github.com/mstan/nesrecomp) by mstan.
- [stb_truetype](https://github.com/nothings/stb) (public domain / MIT).
- [DejaVu fonts](https://dejavu-fonts.github.io/) (free license).
