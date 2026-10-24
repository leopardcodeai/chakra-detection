// SPDX-License-Identifier: Apache-2.0
// Host test of chakra_detect.c: synthetic tones in, chakras out. Run with tests/run.sh.
// Covers each note class at A = 432 Hz across three octaves, a bowl-like tone with
// inharmonic partials, Solfeggio mode, noise and silence (no chakra), and the hold time.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "chakra_detect.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

/* Feeds `seconds` of a tone (partials as frequency ratios with amplitudes) plus noise. */
static chakra_reading_t play(chakra_detect_t *d, float f, const float *ratios, const float *amps, int partials,
                             float noise, float seconds)
{
    static float buf[CHAKRA_HOP];
    static unsigned seed = 1;
    static double t;
    chakra_reading_t r = {0};
    const int hops = (int)(seconds * CHAKRA_RATE / CHAKRA_HOP);
    for (int h = 0; h < hops; ++h) {
        for (int i = 0; i < CHAKRA_HOP; ++i, t += 1.0 / CHAKRA_RATE) {
            float s = 0.0f;
            for (int p = 0; p < partials; ++p) {
                s += amps[p] * (float)sin(2.0 * M_PI * f * ratios[p] * t);
            }
            seed = seed * 1103515245u + 12345u;
            s += noise * ((float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f);
            buf[i] = s;
        }
        chakra_detect_feed(d, buf, CHAKRA_HOP, &r);
    }
    return r;
}

static const float ONE[] = {1.0f}, AMP1[] = {0.3f};

int main(void)
{
    static const int EXPECT[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 6, 6};
    chakra_detect_config_t cfg;
    chakra_detect_defaults(&cfg);
    cfg.a4_hz = 432.0f;  // the Box-3 generations' tuning; 440 is the default since 24.10.2026
    chakra_detect_t *d = chakra_detect_create(&cfg);

    /* Every note, octaves 3 to 5 (C3 about 128 Hz at A = 432). */
    for (int octave = 3; octave <= 5; ++octave) {
        for (int note = 0; note < 12; ++note) {
            const int midi = (octave + 1) * 12 + note;
            const float f = 432.0f * powf(2.0f, (midi - 69) / 12.0f);
            chakra_reading_t r = play(d, f, ONE, AMP1, 1, 0.002f, 0.9f);
            char what[96];
            snprintf(what, sizeof(what), "note %s%d %.1f Hz -> chakra %d (got %d, %.1f Hz, conf %.2f)",
                     chakra_note_name(note), octave, f, EXPECT[note], r.chakra, r.freq_hz, r.confidence);
            check(r.chakra == EXPECT[note] && r.note == note && fabsf(r.freq_hz - f) < 2.0f, what);
        }
    }

    /* A singing bowl: strong fundamental, inharmonic partials (2.71, 5.15). F3 = heart. */
    {
        static const float ratios[] = {1.0f, 2.71f, 5.15f}, amps[] = {0.3f, 0.15f, 0.06f};
        const float f = 432.0f * powf(2.0f, (53 - 69) / 12.0f);
        chakra_reading_t r = play(d, f, ratios, amps, 3, 0.003f, 1.0f);
        char what[96];
        snprintf(what, sizeof(what), "bowl %.1f Hz -> heart (got %d, %.1f Hz)", f, r.chakra, r.freq_hz);
        check(r.chakra == 3, what);
    }

    /* A hummed voice: fundamental with harmonics 2..5. D3 = sacral. */
    {
        static const float ratios[] = {1, 2, 3, 4, 5}, amps[] = {0.2f, 0.25f, 0.15f, 0.1f, 0.05f};
        const float f = 432.0f * powf(2.0f, (50 - 69) / 12.0f);
        chakra_reading_t r = play(d, f, ratios, amps, 5, 0.003f, 1.0f);
        char what[96];
        snprintf(what, sizeof(what), "voice %.1f Hz -> sacral (got %d, %.1f Hz)", f, r.chakra, r.freq_hz);
        check(r.chakra == 1, what);
    }

    /* Silence releases after the hold time, not before. */
    {
        static const float zero[] = {0.0f};
        play(d, 432.0f, ONE, AMP1, 1, 0.0f, 1.2f);
        chakra_reading_t r = play(d, 1.0f, ONE, zero, 1, 0.0f, 1.0f);
        check(r.chakra == 5, "still third eye 1.0 s after the tone (hold 1.5 s)");
        r = play(d, 1.0f, ONE, zero, 1, 0.0f, 0.7f);
        check(r.chakra == -1, "released 1.7 s after the tone");
    }

    /* A bowl near a note boundary that wobbles across it stays on one chakra (hysteresis). */
    {
        int changes = 0, last = -2;
        for (int i = 0; i < 20; ++i) {
            const float f = 210.4f * powf(2.0f, ((i % 2) ? 12.0f : -12.0f) / 1200.0f);  // A3 -35 / -59 ct
            chakra_reading_t r = play(d, f, ONE, AMP1, 1, 0.002f, 0.3f);
            if (r.chakra != last) {
                changes++;
                last = r.chakra;
            }
        }
        char what[96];
        snprintf(what, sizeof(what), "boundary wobble at 210 Hz -> one chakra (changes %d)", changes);
        check(changes == 1, what);
    }

    /* Noise alone gives no chakra. */
    {
        static const float zero[] = {0.0f};
        chakra_reading_t r = play(d, 1.0f, ONE, zero, 1, 0.3f, 2.0f);
        char what[96];
        snprintf(what, sizeof(what), "white noise -> none (got %d, conf %.2f)", r.chakra, r.confidence);
        check(r.chakra == -1, what);
    }

    /* Solfeggio: each centre maps to its chakra, also an octave below. */
    cfg.map = CHAKRA_MAP_SOLFEGGIO;
    chakra_detect_set_config(d, &cfg);
    for (int c = 0; c < CHAKRA_COUNT; ++c) {
        for (int down = 0; down <= 1; ++down) {
            const float f = (float)chakra_info(c)->solfeggio_hz / (down ? 2.0f : 1.0f);
            chakra_reading_t r = play(d, f, ONE, AMP1, 1, 0.002f, 1.2f);
            char what[96];
            snprintf(what, sizeof(what), "solfeggio %.1f Hz -> %s (got %d)", f, chakra_info(c)->english, r.chakra);
            check(r.chakra == c, what);
        }
    }

    /* Learned bowls (the seven of the templesounds tune-up as the Mac heard them, root bowl's
     * missing ones filled in): each maps to its chakra, a sound between bowls to none. */
    {
        static const float BOWLS[CHAKRA_COUNT] = {210.4f, 139.4f, 326.7f, 350.8f, 881.3f, 505.6f, 760.0f};
        cfg.map = CHAKRA_MAP_BOWLS;
        for (int c = 0; c < CHAKRA_COUNT; ++c) {
            cfg.bowl_hz[c] = BOWLS[c];
        }
        chakra_detect_set_config(d, &cfg);
        static const float zero[] = {0.0f};
        for (int c = 0; c < CHAKRA_COUNT; ++c) {
            play(d, 1.0f, ONE, zero, 1, 0.0f, 2.0f);  // silence between bowls
            chakra_reading_t r = play(d, BOWLS[c] * 1.01f, ONE, AMP1, 1, 0.002f, 0.9f);  // 17 ct off
            char what[96];
            snprintf(what, sizeof(what), "bowl %.1f Hz -> %s (got %d)", BOWLS[c], chakra_info(c)->english, r.chakra);
            check(r.chakra == c, what);
        }
        play(d, 1.0f, ONE, zero, 1, 0.0f, 2.0f);
        chakra_reading_t r = play(d, 640.0f, ONE, AMP1, 1, 0.002f, 0.9f);
        char what[96];
        snprintf(what, sizeof(what), "640 Hz between bowls -> none (got %d)", r.chakra);
        check(r.chakra == -1, what);
    }

    chakra_detect_destroy(d);
    printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
