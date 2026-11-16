/* The ROMs this mod knows, identified by the FNV-1a hash of their PRG data (the same hash the runner computes), never by file name.
 * Shared by the release build's ROM picker (romless_native.c.in) and the game (mouse_cursor_hook.c). */
#ifndef MM_ROM_DB_H
#define MM_ROM_DB_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t prg_hash; const char *id; const char *title; } MmRomInfo;

static const MmRomInfo k_mm_roms[] = {
    { 0x49ADE912u, "prototype", "Maniac Mansion (Prototype)" },
    { 0x94B7EEDFu, "retail",    "Maniac Mansion (USA)" },
    /* the community "Uncensored" hack (v2.7 Default patch + lflu_rah, muff_diver, playmummy, remove_mouse) on the retail ROM: 512 KB */
    { 0xDF916787u, "uncensored", "Maniac Mansion (USA, Uncensored)" },
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

/* How a ROM names its own files and records, from the hash of its PRG data (every patch permutation has its own hash, so each gets its
 * own save states, cartridge save and achievement list): "" for the prototype (its files keep the old names), "retail" for the retail US
 * ROM (as released), "x<HASH>" for every other ROM. */
static inline void mm_rom_tag(uint32_t h, char *out, size_t n) {
    const MmRomInfo *r = mm_rom_by_hash(h);
    if (r && r->id[0] == 'p') { out[0] = 0; return; }
    if (r && r->id[0] == 'r') { snprintf(out, n, "retail"); return; }
    snprintf(out, n, "x%08X", (unsigned)h);
}
/* A readable title: the known one, else "Maniac Mansion (custom ROM XXXXXXXX)". */
static inline void mm_rom_title(uint32_t h, char *out, size_t n) {
    const MmRomInfo *r = mm_rom_by_hash(h);
    if (r) snprintf(out, n, "%s", r->title); else snprintf(out, n, "Maniac Mansion (custom ROM %08X)", (unsigned)h);
}

/* Is this iNES image the Maniac Mansion NES program family (the prototype, the retail ROM and patched versions of them)? True when it is
 * MMC1 with 256 or 512 KB of PRG and the four places the mod hooks hold the expected code: the joypad read in the last bank, and the
 * second joypad read, the game save and the cursor clamp in bank 0. */
static inline int mm_is_family(const uint8_t *img, size_t size) {
    static const uint8_t k_fb68[16] = { 0xA2,0x01,0xB5,0x02,0x95,0x04,0xA9,0x08,0x8D,0x21,0x72,0xA9,0x01,0x8D,0x16,0x40 };
    static const uint8_t k_9eab[16] = { 0xA2,0x01,0xB5,0x02,0x95,0x04,0xA9,0x08,0x8D,0x4E,0x03,0xA9,0x01,0x8D,0x16,0x40 };
    static const uint8_t k_9659[16] = { 0xA9,0xA8,0x85,0x19,0xA9,0x00,0x85,0x1A,0xA9,0xFA,0x85,0x1B,0xA9,0x6B,0x85,0x1C };
    static const uint8_t k_9539[16] = { 0xAD,0x16,0x71,0x8D,0x07,0x02,0xAD,0x18,0x71,0x8D,0x04,0x02,0xA9,0xFA,0x8D,0x05 };
    if (size < 16 || img[0] != 'N' || img[1] != 'E' || img[2] != 'S' || img[3] != 0x1A) return 0;
    int mapper = (img[6] >> 4) | (img[7] & 0xF0), banks = img[4];
    size_t off = 16 + ((img[6] & 4) ? 512 : 0);
    if (mapper != 1 || (banks != 16 && banks != 32) || off + (size_t)banks * 16384 > size) return 0;
    const uint8_t *prg = img + off;
    return !memcmp(prg + (size_t)(banks - 1) * 16384 + 0x3B68, k_fb68, 16) && !memcmp(prg + 0x1EAB, k_9eab, 16) &&
           !memcmp(prg + 0x1659, k_9659, 16) && !memcmp(prg + 0x1539, k_9539, 16);
}
#endif
