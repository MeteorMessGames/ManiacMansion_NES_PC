#include "splash_sfx.h"
#include <math.h>
#include <string.h>

#define SECONDS 4.5
#define LEN 216000   /* SECONDS * SPLASH_SFX_RATE: the startup splash is 270 frames = 4.5 s */

static int16_t s_buf[LEN];
static int s_built = 0;

static double note_hz(int midi) { return 440.0 * pow(2.0, (midi - 69) / 12.0); }

/* Pulse wave with the given duty (0..1) at phase p in [0,1). */
static double pulse(double p, double duty) { return p < duty ? 1.0 : -1.0; }
/* Triangle wave at phase p. */
static double tri(double p) { return p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p; }

/* Mix one note into the buffer starting at t0 seconds. kind 0 = lead pulse, 1 = bass triangle, 2 = ping, 3 = twinkle (a high, bell-like sine that fades quickly). */
static void add_note(double t0, double dur, int midi, double amp, int kind)
{
    double hz = note_hz(midi);
    int i0 = (int)(t0 * SPLASH_SFX_RATE), n = (int)(dur * SPLASH_SFX_RATE);
    for (int i = 0; i < n && i0 + i < LEN; i++) {
        double t = (double)i / SPLASH_SFX_RATE;
        double p = fmod(t * hz, 1.0);
        double env = (t < 0.004 ? t / 0.004 : 1.0) * exp(-t / (kind == 3 ? 0.22 : kind == 2 ? 0.16 : dur * 0.55));
        double v = kind == 0 ? pulse(p, 0.25) : kind == 1 ? tri(p) : kind == 3
            ? sin(6.283185307 * t * hz) + 0.35 * sin(6.283185307 * t * hz * 2.76) * exp(-t / 0.03)   /* inharmonic partial: the glassy attack */
            : pulse(p, 0.5) * 0.6 + tri(fmod(t * hz * 2.0, 1.0)) * 0.4;
        if (kind == 0) v += 0.35 * pulse(fmod(t * hz * 1.003, 1.0), 0.125);     /* slight chorus */
        int s = s_buf[i0 + i] + (int)(v * env * amp * 32767.0);
        s_buf[i0 + i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

static void build(void)
{
    memset(s_buf, 0, sizeof s_buf);
    /* Twinkles only, no tune: bell-like sparkles that ring out and overlap, scattered over the whole screen (fixed
     * pseudo-random times and pitches, so it always sounds the same), now and then in pairs or a quick falling run. */
    static const int penta[] = { 88, 91, 93, 96, 98, 100, 103, 105, 108 };   /* high A minor pentatonic bell notes */
    unsigned seed = 0x2F6E2B1u;
    double t = 0.05;
    while (t < 4.1) {
        seed = seed * 1664525u + 1013904223u;
        int idx = (int)((seed >> 16) % 9);
        double amp = 0.22 + 0.12 * ((seed >> 8 & 15) / 15.0);
        double fade = t > 3.5 ? (4.1 - t) / 0.6 : 1.0;                 /* let the last ones die away with the splash fade */
        add_note(t, 1.1, penta[idx], amp * fade, 3);
        switch ((seed >> 4) & 7) {
        case 0: add_note(t + 0.07, 1.0, penta[(idx + 3) % 9], amp * 0.7 * fade, 3); break;               /* a quick second sparkle */
        case 1: for (int k = 1; k <= 3; k++) add_note(t + 0.09 * k, 0.9, penta[idx > k ? idx - k : 0], amp * (0.8 - 0.15 * k) * fade, 3); break;   /* falling run */
        default: break;
        }
        t += 0.18 + 0.42 * ((seed >> 12 & 255) / 255.0);
    }
    /* normalize to about 51% of full scale (85% less 40%: it was too loud); the sound follows the Sound FX volume */
    int peak = 1;
    for (int i = 0; i < LEN; i++) { int v = s_buf[i] < 0 ? -s_buf[i] : s_buf[i]; if (v > peak) peak = v; }
    for (int i = 0; i < LEN; i++) s_buf[i] = (int16_t)((int)s_buf[i] * 16680 / peak);
}

const int16_t *splash_sfx(int *len)
{
    if (!s_built) { build(); s_built = 1; }
    if (len) *len = LEN;
    return s_buf;
}
