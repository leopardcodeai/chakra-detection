// SPDX-License-Identifier: Apache-2.0
// Host test of chakra_light.c, driven the way chakra_core.c drives it: one call per 40 ms tick, a
// lamp command at each effect's start and at each due down. Run with tests/run.sh.
// Covers: a drum at 4 beats a second kicks at most once every 2 s; a full kick stays under the
// WCAG flash line (relative luminance change under 0.1) for every chakra colour and brightness, on
// the lamps and on the screen; a gong swells (two commands, no white, the colour held) instead of
// kicking; a stale "other" before a gong; the gate's colour rule; and the pace of network lamps.
// Each block prints the old rule's number beside the new one (before -> after).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "chakra_detect.h"
#include "chakra_light.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

// ------------------------------------------------------------------ helpers --

static double lin(int v)
{
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* WCAG relative luminance of a colour at a brightness in percent (the lamp's or the screen's). */
static double lum(uint32_t rgb, int brightness)
{
    const double l = 0.2126 * lin((int)(rgb >> 16) & 0xFF) + 0.7152 * lin((int)(rgb >> 8) & 0xFF) +
                     0.0722 * lin((int)rgb & 0xFF);
    return l * brightness / 100.0;
}

static uint32_t towards_white(uint32_t rgb, double w)
{
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const double v = (double)((rgb >> sh) & 0xFF);
        out = (out << 8) | (uint32_t)(v + (255.0 - v) * w + 0.5);
    }
    return out;
}

/* The largest luminance step of a full kick of `points` and `white` over all colours and brightness. */
static double worst_kick(int points, double white, int screen)
{
    double worst = 0.0;
    for (int c = 0; c < CHAKRA_COUNT; ++c) {
        const uint32_t rgb = chakra_info(c)->rgb;
        for (int b = screen ? 100 : 10; b <= 100; ++b) {
            const int up = screen ? 100 : (b + points > 100 ? 100 : b + points);
            const double d = lum(towards_white(rgb, white), up) - lum(rgb, b);
            worst = d > worst ? d : worst;
        }
    }
    return worst;
}

/* One lamp command as chakra_core.c would send it. */
typedef struct {
    uint32_t ms;
    int brightness, transition_ms;
    float white;
} cmd_t;

#define MAX_CMDS 512
static cmd_t cmds[MAX_CMDS];
static int ncmds;

static void send(const chakra_light_t *l, int base, int transition_ms)
{
    if (ncmds < MAX_CMDS) {
        cmd_t *c = &cmds[ncmds++];
        c->ms = l->ms;
        c->transition_ms = transition_ms;
        chakra_light_level(l, base, &c->brightness, &c->white);
    }
}

/* A gate line as the core gets it: the effect it starts goes out as a command at once. */
static int sound(chakra_light_t *l, int base, int kind, int strike)
{
    chakra_light_heard(l, kind);
    const int e = chakra_light_sound(l, kind, strike != 0);
    if (e == CHAKRA_EFFECT_KICK) {
        send(l, base, CHAKRA_KICK_UP_MS);
    } else if (e == CHAKRA_EFFECT_SWELL) {
        send(l, base, CHAKRA_SWELL_UP_MS);
    }
    return e;
}

static void tick(chakra_light_t *l, int base)
{
    const int down = chakra_light_tick(l);
    if (down) {
        send(l, base, down);
    }
}

/* Most commands in any window of `window_ms`. */
static int most_in(const uint32_t *t, int n, uint32_t window_ms)
{
    int most = 0;
    for (int i = 0; i < n; ++i) {
        int k = 0;
        for (int j = i; j < n && t[j] - t[i] < window_ms; ++j) {
            ++k;
        }
        most = k > most ? k : most;
    }
    return most;
}

/* The old rule (before 26.10.2026): every strike kick = 1, the kick decays by 0.9 a tick, and the
 * lamps get a command at the strike and every third tick while it lasts. Commands in `seconds`. */
static int old_commands(double strike_hz, double seconds, int *most_per_s)
{
    static uint32_t t[4096];
    int n = 0;
    float kick = 0.0f;
    double next = 0.0;
    for (uint32_t tk = 1; tk * 40 <= seconds * 1000; ++tk) {
        const uint32_t ms = tk * 40;
        if (ms >= next * 1000) {
            next += 1.0 / strike_hz;
            kick = 1.0f;
            t[n++] = ms;
        }
        if (kick > 0.0f) {
            kick = kick < 0.02f ? 0.0f : kick * 0.90f;
            if (tk % 3 == 0 || kick == 0.0f) {
                t[n++] = ms;
            }
        }
    }
    *most_per_s = most_in(t, n, 1000);
    return n;
}

// ------------------------------------------------------------------ tests --

static void test_drum(void)
{
    /* 20 s of a frame drum at 4 beats a second: the gate's strike line on every beat, its verdict
     * "other" every 0.5 s. */
    chakra_light_t l;
    chakra_light_init(&l);
    ncmds = 0;
    uint32_t kicks[64];
    int nk = 0, beats = 0;
    for (int tk = 1; tk <= 500; ++tk) {
        tick(&l, 30);
        beats += tk % 6 == 0;
        if (tk % 6 == 0 || tk % 25 == 0) {  // 240 ms beats (about 4 Hz) and the 0.5 s verdicts
            if (sound(&l, 30, CHAKRA_SOUND_OTHER, tk % 6 == 0) == CHAKRA_EFFECT_KICK && nk < 64) {
                kicks[nk++] = l.ms;
            }
        }
    }
    int gaps_ok = 1;
    for (int i = 1; i < nk; ++i) {
        gaps_ok &= kicks[i] - kicks[i - 1] >= CHAKRA_KICK_GAP_MS;
    }
    char what[160];
    snprintf(what, sizeof(what), "drum 4 Hz: kicks at least 2 s apart (%d kicks in 20 s)", nk);
    check(gaps_ok && nk >= 9 && nk <= 10, what);
    uint32_t t[MAX_CMDS];
    for (int i = 0; i < ncmds; ++i) {
        t[i] = cmds[i].ms;
    }
    const int most = most_in(t, ncmds, 1000);
    snprintf(what, sizeof(what), "drum 4 Hz: 2 lamp commands a kick (%d for %d kicks), at most 2 in any second (%d)",
             ncmds, nk, most);
    check(ncmds == 2 * nk && most <= 2, what);
    int old_most;
    const int old = old_commands(1000.0 / 240.0, 20.0, &old_most);
    printf("drum 4 Hz, 20 s: kicks %d -> %d, lamp commands per lamp %d -> %d, most in a second %d -> %d\n", beats, nk,
           old, ncmds, old_most, most);
}

static void test_flash(void)
{
    const double lamp = worst_kick(CHAKRA_KICK_BRIGHT, CHAKRA_KICK_WHITE, 0);
    const double screen = worst_kick(0, CHAKRA_KICK_WHITE, 1);
    const double old_lamp = worst_kick(40, 0.35, 0), old_screen = worst_kick(0, 0.35, 1);
    char what[160];
    snprintf(what, sizeof(what), "a full kick stays under the flash line 0.1 (lamps %.3f, screen %.3f)", lamp, screen);
    check(lamp < 0.1 && screen < 0.1, what);
    printf("full kick, largest luminance step (flash line 0.1): lamps %.3f -> %.3f, screen %.3f -> %.3f\n", old_lamp,
           lamp, old_screen, screen);
}

static void test_gong(void)
{
    /* Silence ("none"), a gong struck at 1 s (its strike line still carries "none"), the verdict
     * "gong" from 1.9 s on every 0.5 s while it rings, a second strike at 2 s inside the swell, and
     * a new strike at 8 s. */
    chakra_light_t l;
    chakra_light_init(&l);
    ncmds = 0;
    int swells = 0, kicks = 0, held = 1;
    float white = 0.0f;
    for (int tk = 1; tk <= 300; ++tk) {
        tick(&l, 30);
        int e = CHAKRA_EFFECT_NONE;
        if (tk == 25) {
            e = sound(&l, 30, CHAKRA_SOUND_NONE, 1);
        } else if (tk == 50 || tk == 200) {
            e = sound(&l, 30, CHAKRA_SOUND_GONG, 1);
        } else if (tk % 12 == 0) {
            e = sound(&l, 30, tk < 37 ? CHAKRA_SOUND_NONE : CHAKRA_SOUND_GONG, 0);
        }
        swells += e == CHAKRA_EFFECT_SWELL;
        kicks += e == CHAKRA_EFFECT_KICK;
        if (tk > 37) {
            held &= chakra_light_gate_holds(&l);  // a gong never lets the detection change the colour
        }
        white = chakra_light_screen_white(&l) > white ? chakra_light_screen_white(&l) : white;
    }
    char what[200];
    snprintf(what, sizeof(what), "gong: two swells (%d), no kick (%d), colour held (%d), no white (%.2f)", swells, kicks,
             held, (double)white);
    check(swells == 2 && kicks == 0 && held && white == 0.0f, what);
    int ok = ncmds == 4;
    for (int i = 0; ok && i < ncmds; ++i) {
        const int up = i % 2 == 0;
        ok &= cmds[i].white == 0.0f;
        ok &= up ? cmds[i].brightness == 30 + CHAKRA_SWELL_BRIGHT && cmds[i].transition_ms == CHAKRA_SWELL_UP_MS
                 : cmds[i].brightness == 30 && cmds[i].transition_ms == CHAKRA_SWELL_DOWN_MS &&
                       cmds[i].ms - cmds[i - 1].ms >= CHAKRA_SWELL_UP_MS;
    }
    snprintf(what, sizeof(what), "gong: per swell one command up (+%d over %.1f s) and one down (over %.0f s); %d sent",
             CHAKRA_SWELL_BRIGHT, CHAKRA_SWELL_UP_MS / 1000.0, CHAKRA_SWELL_DOWN_MS / 1000.0, ncmds);
    check(ok, what);
    const double step = (double)CHAKRA_SWELL_BRIGHT / (CHAKRA_SWELL_UP_MS / 1000.0);
    snprintf(what, sizeof(what), "gong: brightness step %.0f points a second, at most 15", step);
    check(step <= 15.0, what);
    printf("gong strike: kick +40 points, 35 %% white, ~8 commands a second -> swell +%d, no white, %d commands\n",
           CHAKRA_SWELL_BRIGHT, ncmds / swells);
}

static void test_stale_kick(void)
{
    /* A drum, then a gong whose first strike still carries "other": the kick is taken back by the
     * swell (its down command is not sent; the swell's up removes the white). */
    chakra_light_t l;
    chakra_light_init(&l);
    ncmds = 0;
    for (int tk = 1; tk <= 100; ++tk) {
        tick(&l, 30);
        if (tk == 10) {
            sound(&l, 30, CHAKRA_SOUND_OTHER, 1);
        } else if (tk == 14) {
            sound(&l, 30, CHAKRA_SOUND_GONG, 0);
        }
    }
    char what[160];
    snprintf(what, sizeof(what), "stale other then gong: kick up, swell up, swell down (%d commands, white %.2f %.2f %.2f)",
             ncmds, (double)cmds[0].white, (double)cmds[1].white, (double)cmds[2].white);
    check(ncmds == 3 && cmds[0].white > 0.0f && cmds[1].white == 0.0f && cmds[2].transition_ms == CHAKRA_SWELL_DOWN_MS,
          what);
}

static void test_gate(void)
{
    chakra_light_t l;
    chakra_light_init(&l);
    check(!chakra_light_gate_holds(&l), "gate: no verdict yet, the detection leads");
    chakra_light_heard(&l, CHAKRA_SOUND_BOWL);
    check(!chakra_light_gate_holds(&l), "gate: a bowl lets the colour change");
    for (int tk = 0; tk < 51; ++tk) {
        chakra_light_tick(&l);
    }
    chakra_light_heard(&l, CHAKRA_SOUND_GONG);
    check(chakra_light_gate_holds(&l), "gate: 2 s after the bowl a gong holds the colour");
    for (int tk = 0; tk < 126; ++tk) {
        chakra_light_tick(&l);
    }
    check(!chakra_light_gate_holds(&l), "gate: silent for 5 s, the detection leads again");
}

static void test_pace(void)
{
    /* 16 Hue lamps on one bridge, a kick's up and down 320 ms apart: every lamp command waits for
     * the bridge's pace. */
    chakra_pace_t p;
    chakra_pace_init(&p, CHAKRA_LAMP_GAP_MS);
    static uint32_t t[64];
    int n = 0;
    uint32_t now = 0;
    for (int cmd = 0; cmd < 2; ++cmd) {
        now = now > (uint32_t)cmd * CHAKRA_KICK_HOLD_MS ? now : (uint32_t)cmd * CHAKRA_KICK_HOLD_MS;
        for (int lamp = 0; lamp < 16; ++lamp) {
            now += chakra_pace_wait(&p, now);
            chakra_pace_sent(&p, now);
            t[n++] = now;
            now += 30;  // the HTTP request itself
        }
    }
    const int most = most_in(t, n, 1000);
    int old_most;
    old_commands(0.01, 2.0, &old_most);  // one kick
    char what[160];
    snprintf(what, sizeof(what), "pace: 16 Hue lamps, %d commands, at most %d a second to the bridge (under 10)", n, most);
    check(most < 10, what);
    printf("Hue bridge, 16 lamps over a kick: commands asked in the busiest second %d -> %d (limit 10)\n",
           16 * old_most, most);
}

int main(void)
{
    test_drum();
    test_flash();
    test_gong();
    test_stale_kick();
    test_gate();
    test_pace();
    if (failures) {
        printf("%d light check(s) failed\n", failures);
        return 1;
    }
    printf("all light checks passed\n");
    return 0;
}
