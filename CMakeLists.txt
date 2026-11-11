cmake_minimum_required(VERSION 3.20)
project(ManiacMansionMouseCursorTest C CXX)

set(NESRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")

# Your own dump of the Maniac Mansion NES prototype ROM (not included).
set(MM_ROM "" CACHE FILEPATH "Path to Maniac Mansion (Prototype).nes")
if(NOT MM_ROM OR NOT EXISTS "${MM_ROM}")
    message(FATAL_ERROR "Set -DMM_ROM=<path to Maniac Mansion (Prototype).nes>")
endif()

nesrecomp_add_cycle_game(nes_game
    ROM "${MM_ROM}"
    GAME_CONFIG "${CMAKE_CURRENT_LIST_DIR}/game.toml"
    HOST_EXTRAS "${CMAKE_CURRENT_LIST_DIR}/mouse_cursor_hook.c" "${CMAKE_CURRENT_LIST_DIR}/menu_render.c" "${CMAKE_CURRENT_LIST_DIR}/crt_filter.c" "${CMAKE_CURRENT_LIST_DIR}/crt_gpu.c"
    MODS GAME_ID "maniac-mansion-proto"
    RECOMPILER "${NESRECOMP_ROOT}/build/recompiler/NESRecomp"
)

# Release build without any of the game: drop the generated translation units
# (recompiled code + the embedded PRG ROM copy) and run the player's ROM on the
# interpreter. The ROM is still needed at configure time, only to read the
# ROM hashes and the hook-site table out of the generated dispatch file.
option(MM_ROMLESS "Build without the game's code; the player's ROM runs on the interpreter" OFF)
if(MM_ROMLESS)
    get_target_property(_mm_srcs nes_game SOURCES)
    set(_mm_keep "")
    set(_mm_main "")
    foreach(_s IN LISTS _mm_srcs)
        if(_s MATCHES "/generated/[^/]+_cyc(_[br][0-9A-Fa-f]+)?\\.c$")
            if(_s MATCHES "/generated/[^/]+_cyc\\.c$")
                set(_mm_main "${_s}")
            endif()
        else()
            list(APPEND _mm_keep "${_s}")
        endif()
    endforeach()
    if(NOT _mm_main)
        message(FATAL_ERROR "MM_ROMLESS: generated dispatch file not found in nes_game's sources")
    endif()
    file(READ "${_mm_main}" _mm_gen)
    set(MM_ID_LINES "")
    foreach(_v cart_hash prg_hash program_name display_name console)
        string(REGEX MATCH "\nconst [^\n]*cyc_native_${_v} = [^;]*;" _line "${_mm_gen}")
        if(NOT _line)
            message(FATAL_ERROR "MM_ROMLESS: cyc_native_${_v} not found in ${_mm_main}")
        endif()
        string(STRIP "${_line}" _line)
        string(APPEND MM_ID_LINES "${_line}\n")
    endforeach()
    string(FIND "${_mm_gen}" "const CycHookSite cyc_native_hook_sites" _a)
    string(FIND "${_mm_gen}" "cyc_native_hook_site_count = " _b)
    if(_a LESS 0 OR _b LESS 0)
        message(FATAL_ERROR "MM_ROMLESS: hook-site table not found in ${_mm_main}")
    endif()
    string(SUBSTRING "${_mm_gen}" ${_b} -1 _tail)
    string(FIND "${_tail}" ";" _semi)
    math(EXPR _len "${_b} - ${_a} + ${_semi} + 1")
    string(SUBSTRING "${_mm_gen}" ${_a} ${_len} MM_HOOK_SITES)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/romless_native.c.in" "${CMAKE_CURRENT_BINARY_DIR}/romless_native.c" @ONLY)
    list(APPEND _mm_keep "${CMAKE_CURRENT_BINARY_DIR}/romless_native.c")
    set_property(TARGET nes_game PROPERTY SOURCES ${_mm_keep})
    message(STATUS "MM_ROMLESS: no game code in the exe (interpreter + ${_mm_main} hook sites)")
endif()

# Release builds (MM_ROMLESS) are named ManiacMansion.exe and open no console
# window; test builds keep nes_game.exe and the console with the log output.
# -DMM_CONSOLE=ON brings the console back on a release build for testing.
if(MM_ROMLESS)
    set_target_properties(nes_game PROPERTIES OUTPUT_NAME ManiacMansion)
    set(_mm_console_default OFF)
else()
    set(_mm_console_default ON)
endif()
option(MM_CONSOLE "Windows: open a console window with the log output" ${_mm_console_default})
if(MINGW AND NOT MM_CONSOLE)
    set_target_properties(nes_game PROPERTIES WIN32_EXECUTABLE TRUE)   # -mwindows: GUI subsystem
endif()

if(MINGW)
    # Ship a self-contained .exe: statically link MinGW's own runtime DLLs
    # (libgcc_s_seh-1.dll, libstdc++-6.dll -- pulled in by mod_runtime.cpp's
    # C++ linkage) instead of distributing them alongside SDL2.dll.
    target_link_options(nes_game PRIVATE -static-libgcc -static-libstdc++)
endif()

# Player guide (README.txt) next to the executable (CRLF so Notepad on Windows shows it right).
add_custom_command(TARGET nes_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} "-DIN=${CMAKE_CURRENT_LIST_DIR}/README.txt" "-DOUT=$<TARGET_FILE_DIR:nes_game>/README.txt"
            -P "${CMAKE_CURRENT_LIST_DIR}/copy_guide.cmake"
    VERBATIM)
