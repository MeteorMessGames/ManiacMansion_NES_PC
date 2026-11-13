/* arcade_sfx.h - a short chiptune jingle for the Meteor Mess screen (room 39), synthesized
 * at first use (48 kHz mono s16), no sample data in the source. */
#pragma once
#include <stdint.h>

#define ARCADE_SFX_RATE 48000
/* Returns the jingle (building it on first call) and its length in samples. */
const int16_t *arcade_sfx(int *len);
