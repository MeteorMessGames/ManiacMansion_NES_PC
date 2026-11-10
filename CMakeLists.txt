cmake_minimum_required(VERSION 3.20)
project(ManiacMansionMouseCursorTest C CXX)

set(NESRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")

nesrecomp_add_cycle_game(nes_game
    ROM "${MM_ROM}"
    GAME_CONFIG "${CMAKE_CURRENT_LIST_DIR}/game.toml"
    HOST_EXTRAS "${CMAKE_CURRENT_LIST_DIR}/mouse_cursor_hook.c" "${CMAKE_CURRENT_LIST_DIR}/menu_render.c"
    MODS GAME_ID "maniac-mansion-proto"
    RECOMPILER "${NESRECOMP_ROOT}/build/recompiler/NESRecomp"
)

if(MINGW)
    # Ship a self-contained .exe: statically link MinGW's own runtime DLLs
    # (libgcc_s_seh-1.dll, libstdc++-6.dll -- pulled in by mod_runtime.cpp's
    # C++ linkage) instead of distributing them alongside SDL2.dll.
    target_link_options(nes_game PRIVATE -static-libgcc -static-libstdc++)
endif()
