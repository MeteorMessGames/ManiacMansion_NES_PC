# Building from source

This mod is built on [NESRecomp](https://github.com/mstan/nesrecomp) (PolyForm Noncommercial). It needs a handful of host features stock NESRecomp does not have: display controls (fullscreen, filter, fixed scale), a GPU texture present hook, an APU write hook, per-channel volume, a window-close hook and no default fast-forward key. They are small (about 150 lines in 5 files under `runner/cyc/`) and ship here as a patch: [`runner-patches/0001-maniac-mansion-host-extras.patch`](runner-patches/0001-maniac-mansion-host-extras.patch).

The patch applies to NESRecomp commit **`e8091c8dd149f64ee3bd3489d00c011c50100f09`**. Newer upstream commits have moved the same files, so use that commit.

You also need CMake 3.20+, a C/C++ toolchain, SDL2 development files, and **your own dump** of `Maniac Mansion (Prototype).nes` (no game data is included).

## Linux

```sh
git clone https://github.com/mstan/nesrecomp.git
cd nesrecomp
git checkout e8091c8dd149f64ee3bd3489d00c011c50100f09
git apply path/to/ManiacMansion_NES_PC/runner-patches/0001-maniac-mansion-host-extras.patch

# this repo must sit inside the NESRecomp checkout, named mm-proto-port
git clone https://github.com/MeteorMessGames/ManiacMansion_NES_PC.git mm-proto-port

# 1. the recompiler (a plain C tool)
cmake -S recompiler -B build/recompiler -DCMAKE_BUILD_TYPE=Release
cmake --build build/recompiler -j

# 2. the game
cd mm-proto-port
cmake -S . -B build -DMM_ROM="/path/to/Maniac Mansion (Prototype).nes" -DMM_NO_SIM=ON
cmake --build build -j
./build/nes_game "/path/to/Maniac Mansion (Prototype).nes"
```

## Windows exe (cross-compiled with MinGW)

Same checkout and patch as above, then:

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
      -DMM_ROM="/path/to/Maniac Mansion (Prototype).nes" -DMM_ROMLESS=ON -DMM_NO_SIM=ON
cmake --build build-win -j
```

`-DMM_ROMLESS=ON` is the release build: the exe contains none of the game; the player's own ROM, next to the exe, runs on the runner's interpreter. It is named `ManiacMansion.exe` and opens no console (`-DMM_CONSOLE=ON` brings the console back). Without it you get `nes_game`, built on the recompiled game code, with the console.

## Notes

- `-DMM_NO_SIM=ON` keeps the developer-only simulator out of the build; the repo does not contain it, so a normal clone builds the same either way.
- Unlocked achievements are stored inside the built exe itself; building a new exe starts with none.
- The runner patch is a derivative of NESRecomp and carries its license (PolyForm Noncommercial, see `licenses/`). This repo's own code is MIT.
