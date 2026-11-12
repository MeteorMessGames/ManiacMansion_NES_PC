/* achievements.h - achievement tracking for the Maniac Mansion mod.
 * Definitions come from the generated achievements_table.h (tools/ach/ach_tool.py gen):
 * each achievement is a set of memory conditions that must all hold at once. Unlocks
 * are stored in <exe dir>/ManiacMansion.achievements, separate from save states, so
 * loading a state never takes one away. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t (*AchRead)(uint16_t addr);

void        ach_init(const char *dir);          /* load the unlock file (dir may be "") */
int         ach_count(void);
const char *ach_name(int i);
const char *ach_desc(int i);
int         ach_points(int i);
bool        ach_unlocked(int i);
int         ach_unlocked_count(void);
int         ach_points_unlocked(void);
int         ach_points_total(void);
/* Evaluate every locked achievement against RAM; newly unlocked ones are saved and queued. */
void        ach_poll(AchRead rd);
/* Pop the next newly unlocked achievement for display, or -1. */
int         ach_next_toast(void);
