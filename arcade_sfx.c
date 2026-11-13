#include "arcade_sfx.h"
#include <math.h>
#include <string.h>

#define SECONDS 5.4
#define LEN 259200   /* SECONDS * ARCADE_SFX_RATE */

static int16_t s_buf[LEN];
static int s_built = 0;

static double note_hz(int midi) { return 440.0 * pow(2.0, (midi - 69) / 12.0); }

/* Pulse wave with the given duty (0..1) at phase p in [0,1). */
static double pulse(double p, double duty) { return p < duty ? 1.0 : -1.0; }
/* Triangle wave at phase p. */
static double tri(double p) { return p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p; }

/* Mix one note into the buffer starting at t0 seconds. kind 0 = lead pulse, 1 = bass triangle, 2 = ping. */
static void add_note(double t0, double dur, int midi, double amp, int kind)
{
    double hz = note_hz(midi);
    int i0 = (int)(t0 * ARCADE_SFX_RATE), n = (int)(dur * ARCADE_SFX_RATE);
    for (int i = 0; i < n && i0 + i < LEN; i++) {
        double t = (double)i / ARCADE_SFX_RATE;
        double p = fmod(t * hz, 1.0);
        double env = (t < 0.004 ? t / 0.004 : 1.0) * exp(-t / (kind == 2 ? 0.16 : dur * 0.55));
        double v = kind == 0 ? pulse(p, 0.25) : kind == 1 ? tri(p) : pulse(p, 0.5) * 0.6 + tri(fmod(t * hz * 2.0, 1.0)) * 0.4;
        if (kind == 0) v += 0.35 * pulse(fmod(t * hz * 1.003, 1.0), 0.125);     /* slight chorus */
        int s = s_buf[i0 + i] + (int)(v * env * amp * 32767.0);
        s_buf[i0 + i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

static void build(void)
{
    memset(s_buf, 0, sizeof s_buf);
    /* 0.00 - 1.20: "game start" run, C major arpeggio climbing, with a final ping */
    static const int run[] = { 72, 76, 79, 84, 88, 91, 96 };
    for (int i = 0; i < 7; i++) add_note(0.07 * i, 0.16, run[i], 0.20, 0);
    add_note(0.55, 0.60, 96, 0.22, 2);
    add_note(0.58, 0.55, 84, 0.16, 2);
    add_note(0.0, 0.9, 36, 0.28, 1);
    /* 1.20 - 5.00: attract-mode loop, A minor feel: 16 eighth notes at 150 bpm (0.2 s), twice through */
    static const int lead[16] = { 69, 72, 76, 81, 79, 76, 72, 76, 77, 81, 84, 81, 76, 79, 83, 88 };
    static const int bass[4] = { 33, 36, 29, 28 };
    const double step = 0.2;
    for (int bar = 0; bar < 2; bar++)
        for (int i = 0; i < 16; i++) {
            double t = 1.2 + (bar * 16 + i) * step * 0.5;     /* 0.1 s per note */
            if (t > 4.9) break;
            double fade = t > 4.4 ? (4.9 - t) / 0.5 : 1.0;
            add_note(t, 0.14, lead[i], 0.12 * fade, 0);
            if (i % 4 == 0) add_note(t, 0.38, bass[i / 4], 0.20 * fade, 1);
        }
    /* tail: a soft closing ping */
    add_note(4.95, 0.45, 81, 0.10, 2);
}

const int16_t *arcade_sfx(int *len)
{
    if (!s_built) { build(); s_built = 1; }
    if (len) *len = LEN;
    return s_buf;
}
