/* The ROMs this mod knows, identified by the FNV-1a hash of their PRG data (the same hash the runner computes), never by file name.
 * Shared by the release build's ROM picker (romless_native.c.in) and the game (mouse_cursor_hook.c). */
#ifndef MM_ROM_DB_H
#define MM_ROM_DB_H
#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t prg_hash; const char *id; const char *title; } MmRomInfo;

static const MmRomInfo k_mm_roms[] = {
    { 0x49ADE912u, "prototype", "Maniac Mansion (Prototype)" },
    { 0x94B7EEDFu, "retail",    "Maniac Mansion (USA)" },
};
#define MM_ROM_COUNT ((int)(sizeof k_mm_roms / sizeof k_mm_roms[0]))

static inline const MmRomInfo *mm_rom_by_hash(uint32_t h) {
    for (int i = 0; i < MM_ROM_COUNT; i++) if (k_mm_roms[i].prg_hash == h) return &k_mm_roms[i];
    return NULL;
}

/* FNV-1a 32 of the PRG ROM in an iNES image (what cyc_prg_hash() returns for the loaded cartridge), 0 if it is not a usable image. */
static inline uint32_t mm_ines_prg_hash(const uint8_t *img, size_t size) {
    if (size < 16 || img[0] != 'N' || img[1] != 'E' || img[2] != 'S' || img[3] != 0x1A) return 0;
    size_t off = 16 + ((img[6] & 4) ? 512 : 0), len = (size_t)img[4] * 16384u;
    if (!len || off + len > size) return 0;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ img[off + i]) * 16777619u;
    return h;
}
#endif
