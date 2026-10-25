// SPDX-License-Identifier: Apache-2.0
// Host test of chakra_detect.c: synthetic tones in, chakras out. Run with tests/run.sh.
// Covers each note class at A = 432 Hz across three octaves, a bowl-like tone with
// inharmonic partials, Solfeggio mode, noise and silence (no chakra), the hold time, and the
// spectrum bars (one a semitone, A2 to D6: each note leads its own band, silence lets them fall to zero),
// and a quieter bowl struck over a louder one that still rings (the onset path).
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

/* A struck bowl: struck at `start` seconds, its fundamental and second mode (2.71) ringing down
 * with time constant `tau`, the second mode fading three times faster. */
typedef struct {
    float f, amp, start, tau;
} strike_t;

/* Plays the strikes from t = 0 for `seconds` (plus a little noise, and a 30 ms knock of loud noise
 * at `knock` seconds unless negative) and returns the chakra shown at each of `at` (seconds,
 * ascending, n of them) in `shown`. */
static void play_strikes(chakra_detect_t *d, const strike_t *s, int ns, float knock, float seconds, const float *at,
                         int *shown, int n)
{
    static float buf[CHAKRA_HOP];
    unsigned seed = 7;
    chakra_reading_t r = {0};
    int next = 0;
    const int hops = (int)(seconds * CHAKRA_RATE / CHAKRA_HOP);
    for (int h = 0; h < hops; ++h) {
        for (int i = 0; i < CHAKRA_HOP; ++i) {
            const double t = (double)(h * CHAKRA_HOP + i) / CHAKRA_RATE;
            float v = 0.0f;
            for (int k = 0; k < ns; ++k) {
                const double u = t - s[k].start;
                if (u >= 0.0) {
                    const double attack = u < 0.01 ? u / 0.01 : 1.0;  // a felt mallet: 10 ms to full
                    v += (float)(attack * s[k].amp * (exp(-u / s[k].tau) * sin(2.0 * M_PI * s[k].f * u) +
                                                      0.3 * exp(-3.0 * u / s[k].tau) * sin(2.0 * M_PI * 2.71 * s[k].f * u)));
                }
            }
            seed = seed * 1103515245u + 12345u;
            const float white = (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
            const bool knocking = knock >= 0.0f && t >= knock && t < knock + 0.03;
            buf[i] = v + (knocking ? 0.3f : 0.002f) * white;
        }
        chakra_detect_feed(d, buf, CHAKRA_HOP, &r);
        const float t_end = (float)((h + 1) * CHAKRA_HOP) / CHAKRA_RATE;
        while (next < n && t_end >= at[next]) {
            shown[next++] = r.chakra;
        }
    }
}

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
        { char w9[64]; snprintf(w9, sizeof(w9), "still third eye 1.0 s after the tone (hold 1.5 s) (got %d)", r.chakra); check(r.chakra == 5, w9); }
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

    /* A loud C3 bowl rings on and a quieter E3 bowl is struck on top of it (a third of its level):
     * the strike switches to E (solar plexus) in Flow and in Strike and E stays while it rings,
     * although C3 still wins the harmonic sum (it held Root for 50 s before the onset path). A knock
     * (a burst of noise) over the ringing C3 changes nothing. A = 440 Hz, as the owner's bowls. */
    {
        static const strike_t BOWLS[] = {{130.81f, 0.3f, 0.5f, 8.0f}, {164.81f, 0.1f, 3.0f, 8.0f}};
        static const float AT[] = {2.9f, 4.0f, 5.9f};
        static const char *const MODE[] = {"Flow", "Strike"};
        for (int m = 0; m < 2; ++m) {
            chakra_detect_config_t sc;
            chakra_detect_defaults(&sc);
            sc.threshold_db = -70.0f;
            sc.lock_ms = m ? 128u : 384u;  // the app's Flow and Strike (chakra_core.c)
            sc.switch_ms = m ? 256u : 1500u;
            chakra_detect_t *sd = chakra_detect_create(&sc);
            int shown[3];
            play_strikes(sd, BOWLS, 2, -1.0f, 6.0f, AT, shown, 3);
            char what[128];
            snprintf(what, sizeof(what), "%s: loud C3 ringing -> root before the E3 strike (got %d)", MODE[m], shown[0]);
            check(shown[0] == 0, what);
            snprintf(what, sizeof(what), "%s: quieter E3 struck over it -> solar plexus within 1 s (got %d)", MODE[m], shown[1]);
            check(shown[1] == 2, what);
            snprintf(what, sizeof(what), "%s: E3 still shown 3 s after its strike (got %d)", MODE[m], shown[2]);
            check(shown[2] == 2, what);
            chakra_detect_destroy(sd);

            /* The same C3, a 30 ms knock of noise at 3 s instead of the E3 bowl. */
            sd = chakra_detect_create(&sc);
            static const float AT2[] = {3.5f, 4.9f};
            play_strikes(sd, BOWLS, 1, 3.0f, 5.0f, AT2, shown, 2);
            snprintf(what, sizeof(what), "%s: a knock over the ringing C3 -> still root (got %d, %d)", MODE[m], shown[0], shown[1]);
            check(shown[0] == 0 && shown[1] == 0, what);
            chakra_detect_destroy(sd);
        }
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

    /* The bars: one band a semitone, A2 .. D6. Each note's own band leads them (also at A = 432 and
     * for a bowl with inharmonic partials), and silence lets them all fall to zero. */
    {
        chakra_detect_config_t bc;
        chakra_detect_defaults(&bc);  // A = 440
        chakra_detect_t *bd = chakra_detect_create(&bc);
        static const float zero[] = {0.0f};
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                bc.a4_hz = 432.0f;
                chakra_detect_set_config(bd, &bc);
            }
            for (int b = 0; b < CHAKRA_BARS; ++b) {
                const int midi = CHAKRA_BAR_MIDI0 + b;
                const float f = bc.a4_hz * powf(2.0f, (midi - 69) / 12.0f);
                play(bd, 1.0f, ONE, zero, 1, 0.0f, 1.5f);
                static const float quiet[] = {0.01f};  // -43 dBFS: below the bars' top, as a bowl in a room
                chakra_reading_t r = play(bd, f, ONE, quiet, 1, 0.0005f, 0.6f);
                int lead = 0;
                for (int k = 1; k < CHAKRA_BARS; ++k) {
                    lead = r.bars[k] > r.bars[lead] ? k : lead;
                }
                char what[128];
                snprintf(what, sizeof(what), "bars at A = %.0f: %s%d %.1f Hz leads band %d at %u (got band %d at %u)",
                         bc.a4_hz, chakra_note_name(midi % 12), midi / 12 - 1, f, b, r.bars[b], lead, r.bars[lead]);
                check(lead == b && r.bars[b] >= 50, what);
            }
        }
        static const float ratios[] = {1.0f, 2.71f, 5.15f}, amps[] = {0.01f, 0.005f, 0.002f};
        play(bd, 1.0f, ONE, zero, 1, 0.0f, 1.5f);
        const float f3 = 432.0f * powf(2.0f, (53 - 69) / 12.0f);
        chakra_reading_t r = play(bd, f3, ratios, amps, 3, 0.0005f, 0.6f);  // an F3 bowl
        int lead = 0;
        for (int k = 1; k < CHAKRA_BARS; ++k) {
            lead = r.bars[k] > r.bars[lead] ? k : lead;
        }
        char what[96];
        snprintf(what, sizeof(what), "bars: F3 bowl leads band %d (got band %d)", 53 - CHAKRA_BAR_MIDI0, lead);
        check(lead == 53 - CHAKRA_BAR_MIDI0, what);
        r = play(bd, 1.0f, ONE, zero, 1, 0.0f, 2.0f);
        int sum = 0;
        for (int k = 0; k < CHAKRA_BARS; ++k) {
            sum += r.bars[k];
        }
        snprintf(what, sizeof(what), "bars: silence falls to zero (got sum %d)", sum);
        check(sum == 0, what);
        chakra_detect_destroy(bd);
    }

    chakra_detect_destroy(d);
    printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
