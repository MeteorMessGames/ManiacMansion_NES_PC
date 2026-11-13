/* splash_sfx.h - twinkling sparkles for the Meteor Mess Games startup splash, synthesized
 * at first use (48 kHz mono s16), no sample data in the source. */
#pragma once
#include <stdint.h>

#define SPLASH_SFX_RATE 48000
/* Returns the jingle (building it on first call) and its length in samples. */
const int16_t *splash_sfx(int *len);
