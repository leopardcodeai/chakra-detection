// SPDX-License-Identifier: Apache-2.0
// How the light answers a strike or a gong, and how fast network lamps may be told about it.
//
// Plain C without the UI, so the host tests run it as it is (tests/test_light.c); chakra_core.c
// drives it once per 40 ms tick. The rules (owner 26.10.2026, from the research in ChakraV's
// docs/SOUND_LIGHT_EXPERIENCE.md, recommendations 1, 2 and 8):
//   * A strike kicks the light at most once every 2 s, so a drum at 4 beats a second (inside the
//     3 to 30 Hz photosensitivity band) never flashes per beat. A full kick adds 8 points and 8 %
//     white: the change in relative luminance (colour times brightness) stays under 0.1, the WCAG
//     "flash" line, for every colour and brightness.
//   * A gong neither kicks nor changes the colour: at its strike the lamps swell 12 points over
//     1.5 s and fall back over 7 s, with no white; a gong ringing on swells nothing more.
//   * Each kick or swell is two lamp commands: one up with a short transition, one down with a
//     long one; the bulb does the fade. The screen fades locally, tick by tick.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the local AI sound gate hears (chakra_core_set_sound). GONG was added 26.10.2026 at the
 * end; a gate that never says it works as before. */
enum { CHAKRA_SOUND_NONE, CHAKRA_SOUND_BOWL, CHAKRA_SOUND_OTHER, CHAKRA_SOUND_GONG };

#define CHAKRA_LIGHT_TICK_MS 40
#define CHAKRA_KICK_GAP_MS 2000    // at most one kick in this time
#define CHAKRA_KICK_BRIGHT 8       // points a kick adds to the lamps' brightness
#define CHAKRA_KICK_WHITE 0.08f    // and how far it moves the colour towards white (screen and lamps)
#define CHAKRA_KICK_DECAY 0.90f    // the screen's kick per tick: to a tenth in about 0.9 s
#define CHAKRA_KICK_UP_MS 200      // lamps: up over this
#define CHAKRA_KICK_HOLD_MS 320    // the down command this long after the up
#define CHAKRA_KICK_DOWN_MS 900    // and back over this
#define CHAKRA_SWELL_BRIGHT 12     // points a gong swell adds
#define CHAKRA_SWELL_UP_MS 1500    // up over this; the down command follows when it is reached
#define CHAKRA_SWELL_DOWN_MS 7000  // and back over this
#define CHAKRA_SWELL_GAP_MS 2000   // at most one new swell in this time; no kick in it either
#define CHAKRA_GONG_AFTER_MS 1000  // a gong verdict this soon after a strike makes that strike a gong
#define CHAKRA_BOWL_HOLD_MS 2000   // a bowl verdict lets the detection change the colour this long
#define CHAKRA_GATE_ALIVE_MS 5000  // no verdict this long: the gate is gone, the detection leads alone

/* Network lamps (Hue, Home Assistant): at most one command per gap. 125 ms, 8 a second, keeps a
 * Hue bridge under its 10 commands a second to lights (groups take 1 a second; the app sends none). */
#define CHAKRA_LAMP_GAP_MS 125

enum { CHAKRA_EFFECT_NONE, CHAKRA_EFFECT_KICK, CHAKRA_EFFECT_SWELL };

typedef struct {
    uint32_t ms;          // the effect clock, CHAKRA_LIGHT_TICK_MS per tick
    uint32_t kick_ms, swell_ms, strike_ms;
    bool kicked, swelled, struck;
    uint32_t gate_ms, bowl_ms;  // the gate's last verdict of any kind, and its last bowl
    bool gate_heard, bowl_heard;
    bool strike_swelled;  // the last strike has had its swell
    float kick;           // the screen's kick, 1 at a kick, decaying per tick
    int up;               // the lamps are up for this effect, CHAKRA_EFFECT_*; NONE: at the base
    uint32_t up_ms;       // when they went up
} chakra_light_t;

void chakra_light_init(chakra_light_t *l);
/** Any verdict of the gate, whatever the AI setting: it shows the gate is alive. */
void chakra_light_heard(chakra_light_t *l, int kind);
/** With the AI gate leading: true while it holds the colour (alive, and no bowl in the last 2 s).
 *  A gong, a drum, a voice or silence never let the detection change the colour. */
bool chakra_light_gate_holds(const chakra_light_t *l);
/** A verdict of the gate while it leads (AI on). Returns the effect that starts now (the lamps get their up command),
 *  or CHAKRA_EFFECT_NONE. Bowls start nothing; the colour rules stay in chakra_core.c. */
int chakra_light_sound(chakra_light_t *l, int kind, bool strike);
/** One tick. Returns the transition in ms when the lamps' down command is due now, else 0. */
int chakra_light_tick(chakra_light_t *l);
/** What a lamp command sent now carries on top of the base brightness: points and white share. */
void chakra_light_level(const chakra_light_t *l, int base, int *brightness, float *white);
/** The screen's white share now (the kick, fading locally). */
float chakra_light_screen_white(const chakra_light_t *l);
/** Any effect visible now (in silence a kick or swell lights the last colour). */
bool chakra_light_active(const chakra_light_t *l);
/** Ends every effect at once, without a command (a change of reaction). */
void chakra_light_clear(chakra_light_t *l);

/* The pace of one network target (a Hue bridge, Home Assistant). */
typedef struct {
    uint32_t gap_ms;
    uint32_t last_ms;
    bool used;
} chakra_pace_t;

void chakra_pace_init(chakra_pace_t *p, uint32_t gap_ms);
/** Milliseconds to wait before the next command may go; 0: now. */
uint32_t chakra_pace_wait(const chakra_pace_t *p, uint32_t now_ms);
/** A command went out now. */
void chakra_pace_sent(chakra_pace_t *p, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
