// SPDX-License-Identifier: Apache-2.0
// Kick, gong swell and lamp pace, see include/chakra_light.h.
#include "chakra_light.h"

#include <string.h>

void chakra_light_init(chakra_light_t *l)
{
    memset(l, 0, sizeof(*l));
}

static bool passed(uint32_t now, uint32_t since, uint32_t gap)
{
    return now - since >= gap;  // unsigned: right across a wrap of the clock
}

void chakra_light_heard(chakra_light_t *l, int kind)
{
    l->gate_ms = l->ms;
    l->gate_heard = true;
    if (kind == CHAKRA_SOUND_BOWL) {
        l->bowl_ms = l->ms;
        l->bowl_heard = true;
    }
}

bool chakra_light_gate_holds(const chakra_light_t *l)
{
    const bool alive = l->gate_heard && !passed(l->ms, l->gate_ms, CHAKRA_GATE_ALIVE_MS + 1);
    return alive && (!l->bowl_heard || passed(l->ms, l->bowl_ms, CHAKRA_BOWL_HOLD_MS + 1));
}

int chakra_light_sound(chakra_light_t *l, int kind, bool strike)
{
    int effect = CHAKRA_EFFECT_NONE;
    if (strike) {
        l->strike_ms = l->ms;
        l->struck = true;
        l->strike_swelled = false;
    }
    const bool swell_free = !l->swelled || passed(l->ms, l->swell_ms, CHAKRA_SWELL_GAP_MS);
    if (kind == CHAKRA_SOUND_GONG) {
        /* A gong's onset: a strike heard with it, or a strike just before its verdict (the strike
         * line carries the verdict before it, often "none" from the silence before a gong). A gong
         * ringing on, or a gong verdict without a strike (a voice the head mistook), swells nothing. */
        const bool after_strike = l->struck && !l->strike_swelled && !passed(l->ms, l->strike_ms, CHAKRA_GONG_AFTER_MS + 1);
        if ((strike || after_strike) && swell_free) {
            l->swelled = true;
            l->swell_ms = l->ms;
            l->strike_swelled = true;
            l->kick = 0.0f;  // a gong never goes towards white, not even after a kick a stale verdict gave
            l->up = CHAKRA_EFFECT_SWELL;
            l->up_ms = l->ms;
            effect = CHAKRA_EFFECT_SWELL;
        }
    } else if (kind == CHAKRA_SOUND_OTHER && strike) {
        const bool kick_free = !l->kicked || passed(l->ms, l->kick_ms, CHAKRA_KICK_GAP_MS);
        if (kick_free && swell_free && l->up != CHAKRA_EFFECT_SWELL) {
            l->kicked = true;
            l->kick_ms = l->ms;
            l->kick = 1.0f;
            l->up = CHAKRA_EFFECT_KICK;
            l->up_ms = l->ms;
            effect = CHAKRA_EFFECT_KICK;
        }
    }
    return effect;
}

int chakra_light_tick(chakra_light_t *l)
{
    l->ms += CHAKRA_LIGHT_TICK_MS;
    if (l->kick > 0.0f) {
        l->kick = l->kick < 0.02f ? 0.0f : l->kick * CHAKRA_KICK_DECAY;
    }
    if (l->up == CHAKRA_EFFECT_KICK && passed(l->ms, l->up_ms, CHAKRA_KICK_HOLD_MS)) {
        l->up = CHAKRA_EFFECT_NONE;
        return CHAKRA_KICK_DOWN_MS;
    }
    if (l->up == CHAKRA_EFFECT_SWELL && passed(l->ms, l->up_ms, CHAKRA_SWELL_UP_MS)) {
        l->up = CHAKRA_EFFECT_NONE;
        return CHAKRA_SWELL_DOWN_MS;
    }
    return 0;
}

void chakra_light_level(const chakra_light_t *l, int base, int *brightness, float *white)
{
    int b = base;
    float w = 0.0f;
    if (l->up == CHAKRA_EFFECT_KICK) {
        b += CHAKRA_KICK_BRIGHT;
        w = CHAKRA_KICK_WHITE;
    } else if (l->up == CHAKRA_EFFECT_SWELL) {
        b += CHAKRA_SWELL_BRIGHT;
    }
    *brightness = b > 100 ? 100 : b;
    *white = w;
}

float chakra_light_screen_white(const chakra_light_t *l)
{
    return CHAKRA_KICK_WHITE * l->kick;
}

bool chakra_light_active(const chakra_light_t *l)
{
    return l->up != CHAKRA_EFFECT_NONE || l->kick > 0.0f;
}

void chakra_light_clear(chakra_light_t *l)
{
    l->kick = 0.0f;
    l->up = CHAKRA_EFFECT_NONE;
}

void chakra_pace_init(chakra_pace_t *p, uint32_t gap_ms)
{
    memset(p, 0, sizeof(*p));
    p->gap_ms = gap_ms;
}

uint32_t chakra_pace_wait(const chakra_pace_t *p, uint32_t now_ms)
{
    if (!p->used || passed(now_ms, p->last_ms, p->gap_ms)) {
        return 0;
    }
    return p->gap_ms - (now_ms - p->last_ms);
}

void chakra_pace_sent(chakra_pace_t *p, uint32_t now_ms)
{
    p->last_ms = now_ms;
    p->used = true;
}
