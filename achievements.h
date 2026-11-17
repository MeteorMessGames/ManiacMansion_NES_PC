/* achievements.h - achievement tracking for the Maniac Mansion mod.
 * Definitions come from the generated achievements_table.h (tools/ach/ach_tool.py gen):
 * each achievement is a set of memory conditions that must all hold at once. Unlocks
 * are stored in an encrypted file next to the exe, ManiacMansion.achievements (test builds write it
 * as plain signed records), separate from save states, so loading a state never takes one away. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t (*AchRead)(uint16_t addr);

void        ach_init(const char *dir);          /* load the unlocks from the unlock file in dir */
void        ach_reset_all(void);                /* forget every unlock and save */
bool        ach_enabled(void);                  /* menu switch: false = nothing is checked, shown or saved */
void        ach_set_enabled(bool on);
void        ach_state_loaded(void);             /* a saved state was just loaded: what is true now is not new (test builds use this) */
int         ach_count(void);
const char *ach_id(int i);
const char *ach_name(int i);
const char *ach_desc(int i);
int         ach_rarity(int i);                  /* 0 Common, 1 Rare, 2 Epic, 3 Legendary (rated by how hard each is) */
const char *ach_rarity_name(int tier);
bool        ach_hidden(int i);                  /* shown as "???" in the menu until unlocked */
int         ach_points(int i);
bool        ach_unlocked(int i);
int         ach_unlocked_count(void);
int         ach_points_unlocked(void);
int         ach_points_total(void);
/* Evaluate every locked achievement against RAM; newly unlocked ones are saved and queued. */
void        ach_poll(AchRead rd);
/* Pop the next newly unlocked achievement for display, or -1. */
int         ach_next_toast(void);
