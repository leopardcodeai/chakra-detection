// SPDX-License-Identifier: Apache-2.0
// Chakra app controller, see include/chakra_core.h. The scene is ui/main.json (make_scene.py);
// the gsp_chakra_* functions are generated from its names.
#include "chakra_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bundle_gsp.h"

#define TICK_MS 40
#define FADE_FLOW 0.06f        // share of the way to the target colour per tick: about 1.6 s
#define FADE_STRIKE 0.35f      // about 0.3 s
#define TUNE_STEP 0.25f        // the tuning ring follows a little slower than the reading
#define SUB_EVERY_TICKS 3      // the note line at most every 120 ms
#define HOLD_MS 3000
#define HOLD_STEP_MS 30
#define IDLE_RGB 0x17141Fu
#define ON_RGB 0x6E5ACFu       // a chosen option or a ticked lamp
#define OFF_RGB 0x3A3744u      // an option not chosen
#define ROW_RGB 0x2C2936u      // a lamp not ticked
#define LEARN_MS 1500          // a bowl must ring steadily this long to be learned
#define LEARN_CENTS 40.0f      // steady: within this of the running mean
#define HOP_MS (1000 * CHAKRA_HOP / CHAKRA_RATE)

/* Material Design Icons, UTF-8, as in make_scene.py: home-assistant, lightbulb, wifi, bluetooth. */
static const char *const KIND_ICON[] = {"\xf3\xb0\x9f\x90", "\xf3\xb0\x8c\xb5", "\xf3\xb0\x96\xa9", "\xf3\xb0\x82\xaf"};

/* Every word on the screen, English first (the default), German second. */
enum {
    S_READY, S_STRIKE, S_LIGHTS, S_BRIGHT, S_SENS, S_HOLD, S_NOTES, S_NOT_PAIRED,
    S_KIND_HA, S_KIND_HUE, S_KIND_WIZ, S_KIND_BLE, S_HOLD_CLOSE, S_CLOSING, S_BOWLS, S_STRIKE_BOWL,
    S_HOLD_TONE, S_FLOW, S_STRIKE_MODE, S_COUNT
};
static const char *const TEXT[2][S_COUNT] = {
    {"Ready", "Strike a bowl or hum", "LIGHTS", "Brightness", "Sensitivity", "Afterglow", "Notes", "not paired",
     "Home Assistant", "Hue", "Wi-Fi", "Bluetooth", "Hold: close Chakra", "Closing Chakra", "Bowls",
     "Strike your %s bowl", "hold the tone", "Flow", "Strike"},
    {"Bereit", "Klangschale anschlagen oder summen", "LICHT", "Helligkeit", "Empfindlichkeit", "Nachklang", "Noten",
     "nicht gekoppelt", "Home Assistant", "Hue", "WLAN", "Bluetooth", "Halten: Chakra schließen",
     "Chakra wird geschlossen", "Schalen", "%s-Schale anschlagen", "Ton halten", "Fließen", "Anschlag"},
};

struct chakra_core {
    esp_gsp_handle_t ui;
    chakra_core_config_t cfg;
    chakra_settings_t s;
    chakra_lamp_t lamps[CHAKRA_LAMPS];
    int lamp_count;
    void *timer;
    void *hold_timer;
    uint32_t hold_ms;
    bool hold_shown;
    bool settings_open;
    int shown;            // chakra on screen, -1 idle
    bool sounding;        // a gated-in tone this hop
    float cur[3];         // background now, 0..255
    uint32_t target_rgb;
    uint32_t drawn_rgb;
    float tune;           // tuning ring now, 0..100
    int tune_drawn;
    uint8_t bars[CHAKRA_BARS];
    uint32_t ticks, sub_tick;
    chakra_reading_t last;
    int learn;            // bowl being learned 0..6, -1 when not learning
    uint32_t learn_ms;
    float learn_sum, learn_mean, learn_prev;
    int learn_n;
    bool learn_gap;       // after a capture: wait for silence or another bowl
    esp_gsp_err_t last_error;
};

static void err(struct chakra_core *c, esp_gsp_err_t e)
{
    if (e != ESP_GSP_OK) {
        c->last_error = e;
    }
}

static const char *T(const struct chakra_core *c, int id)
{
    return TEXT[c->s.lang == CHAKRA_LANG_DE ? 1 : 0][id];
}

static void haptic(struct chakra_core *c, int pattern)
{
    if (c->cfg.on_haptic) {
        c->cfg.on_haptic(pattern, c->cfg.ctx);
    }
}

void chakra_settings_defaults(chakra_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->lamps = 0x01;
    s->brightness = 60;
    s->sensitivity = 8;
    s->hold_ds = 15;
    s->map = CHAKRA_MAP_NOTES;
    s->lang = CHAKRA_LANG_EN;
    s->a4 = 440;
    s->reaction = CHAKRA_FLOW;
}

void chakra_settings_detect(const chakra_settings_t *s, chakra_detect_config_t *out)
{
    chakra_detect_defaults(out);
    out->threshold_db = -30.0f - 5.0f * (float)s->sensitivity;
    out->hold_ms = (uint32_t)s->hold_ds * 100u;
    out->map = s->map == CHAKRA_MAP_SOLFEGGIO || s->map == CHAKRA_MAP_BOWLS ? (chakra_map_t)s->map : CHAKRA_MAP_NOTES;
    memcpy(out->bowl_hz, s->bowl_hz, sizeof(out->bowl_hz));
    out->a4_hz = s->a4 == 432 ? 432.0f : 440.0f;
    out->lock_ms = s->reaction == CHAKRA_STRIKE ? 128u : 384u;
    out->switch_ms = s->reaction == CHAKRA_STRIKE ? 256u : 1500u;
}

// ------------------------------------------------------------------ drawing --

static void draw_chakra(struct chakra_core *c, int chakra)
{
    static esp_gsp_err_t (*const sym[CHAKRA_COUNT])(esp_gsp_handle_t, bool) = {
        gsp_chakra_grp_sym_0_set_visible, gsp_chakra_grp_sym_1_set_visible, gsp_chakra_grp_sym_2_set_visible,
        gsp_chakra_grp_sym_3_set_visible, gsp_chakra_grp_sym_4_set_visible, gsp_chakra_grp_sym_5_set_visible,
        gsp_chakra_grp_sym_6_set_visible};
    static esp_gsp_err_t (*const ring[CHAKRA_COUNT])(esp_gsp_handle_t, bool) = {
        gsp_chakra_grp_ring_0_set_visible, gsp_chakra_grp_ring_1_set_visible, gsp_chakra_grp_ring_2_set_visible,
        gsp_chakra_grp_ring_3_set_visible, gsp_chakra_grp_ring_4_set_visible, gsp_chakra_grp_ring_5_set_visible,
        gsp_chakra_grp_ring_6_set_visible};
    for (int i = 0; i < CHAKRA_COUNT; ++i) {
        err(c, sym[i](c->ui, i == chakra));
        err(c, ring[i](c->ui, i == chakra));
    }
    err(c, gsp_chakra_grp_sym_idle_set_visible(c->ui, chakra < 0));
    err(c, gsp_chakra_grp_tune_set_visible(c->ui, chakra >= 0));
    const chakra_info_t *info = chakra_info(chakra);
    const char *name = !info ? T(c, S_READY) : c->s.lang == CHAKRA_LANG_DE ? info->german : info->english;
    err(c, gsp_chakra_name_text_set_text(c->ui, name));
    err(c, gsp_chakra_sanskrit_text_set_text(c->ui, info ? info->sanskrit : ""));
    if (!info) {
        err(c, gsp_chakra_sub_text_set_text(c->ui, T(c, S_STRIKE)));
    }
    c->target_rgb = info ? info->rgb : IDLE_RGB;
}

/* The note line, as a tuner shows it: "F#3 · 171.4 Hz · +3 ct" while the tone sounds. */
static void draw_sub(struct chakra_core *c)
{
    if (!chakra_info(c->shown)) {
        return;
    }
    const chakra_reading_t *r = &c->last;
    const int tenths = (int)(r->freq_hz * 10.0f + 0.5f);
    char line[96];
    snprintf(line, sizeof(line), c->s.lang == CHAKRA_LANG_DE ? "%s%d · %d,%d Hz · %+d ct" : "%s%d · %d.%d Hz · %+d ct",
             chakra_note_name(r->note), r->octave, tenths / 10, tenths % 10, r->cents);
    err(c, gsp_chakra_sub_text_set_text(c->ui, line));
}

static void draw_bars(struct chakra_core *c, const uint8_t *bars)
{
    static esp_gsp_err_t (*const set[CHAKRA_BARS])(esp_gsp_handle_t, int32_t) = {
        gsp_chakra_bar_0_set_value, gsp_chakra_bar_1_set_value, gsp_chakra_bar_2_set_value,
        gsp_chakra_bar_3_set_value, gsp_chakra_bar_4_set_value, gsp_chakra_bar_5_set_value,
        gsp_chakra_bar_6_set_value, gsp_chakra_bar_7_set_value, gsp_chakra_bar_8_set_value,
        gsp_chakra_bar_9_set_value, gsp_chakra_bar_10_set_value, gsp_chakra_bar_11_set_value,
        gsp_chakra_bar_12_set_value, gsp_chakra_bar_13_set_value, gsp_chakra_bar_14_set_value,
        gsp_chakra_bar_15_set_value};
    for (int b = 0; b < CHAKRA_BARS; ++b) {
        const int d = (int)bars[b] - (int)c->bars[b];
        if (d >= 2 || d <= -2 || (bars[b] == 0 && c->bars[b] != 0)) {
            c->bars[b] = bars[b];
            err(c, set[b](c->ui, bars[b]));
        }
    }
}

/* Background towards the chakra's colour, tuning ring towards how well the tone sits. */
static void animate(struct chakra_core *c)
{
    const float t[3] = {(float)((c->target_rgb >> 16) & 0xFF), (float)((c->target_rgb >> 8) & 0xFF),
                        (float)(c->target_rgb & 0xFF)};
    uint32_t rgb = 0;
    for (int i = 0; i < 3; ++i) {
        const float d = t[i] - c->cur[i];
        c->cur[i] = (d > -1.0f && d < 1.0f) ? t[i] : c->cur[i] + d * (c->s.reaction == CHAKRA_STRIKE ? FADE_STRIKE : FADE_FLOW);
        rgb = (rgb << 8) | (uint32_t)(c->cur[i] + 0.5f);
    }
    if (rgb != c->drawn_rgb) {
        c->drawn_rgb = rgb;
        err(c, gsp_chakra_bg_set_color_rgb888(c->ui, rgb));
    }
    if (c->learn < 0) {
        const int cents = c->last.cents < 0 ? -c->last.cents : c->last.cents;
        const float goal = c->sounding ? 100.0f - 2.0f * (float)cents : 0.0f;
        c->tune += (goal - c->tune) * TUNE_STEP;
    }
    const int v = (int)(c->tune + 0.5f);
    if (v != c->tune_drawn) {
        c->tune_drawn = v;
        err(c, gsp_chakra_tune_arc_set_value(c->ui, v));
    }
}

/* Words that only change with the language. */
static void draw_labels(struct chakra_core *c)
{
    err(c, gsp_chakra_lbl_lights_set_text(c->ui, T(c, S_LIGHTS)));
    err(c, gsp_chakra_lbl_bright_set_text(c->ui, T(c, S_BRIGHT)));
    err(c, gsp_chakra_lbl_sens_set_text(c->ui, T(c, S_SENS)));
    err(c, gsp_chakra_lbl_hold_set_text(c->ui, T(c, S_HOLD)));
    char notes[24];
    snprintf(notes, sizeof(notes), "%s %d", T(c, S_NOTES), c->s.a4 == 432 ? 432 : 440);
    err(c, gsp_chakra_lbl_map_notes_set_text(c->ui, notes));
    err(c, gsp_chakra_lbl_map_bowls_set_text(c->ui, T(c, S_BOWLS)));
    err(c, gsp_chakra_lbl_react_flow_set_text(c->ui, T(c, S_FLOW)));
    err(c, gsp_chakra_lbl_react_strike_set_text(c->ui, T(c, S_STRIKE_MODE)));
}

static void draw_lamps(struct chakra_core *c)
{
    static esp_gsp_err_t (*const row_vis[CHAKRA_LAMPS])(esp_gsp_handle_t, bool) = {
        gsp_chakra_grp_lamp_0_set_visible, gsp_chakra_grp_lamp_1_set_visible, gsp_chakra_grp_lamp_2_set_visible,
        gsp_chakra_grp_lamp_3_set_visible};
    static esp_gsp_err_t (*const row_rgb[CHAKRA_LAMPS])(esp_gsp_handle_t, uint32_t) = {
        gsp_chakra_lamp_0_set_color_rgb888, gsp_chakra_lamp_1_set_color_rgb888, gsp_chakra_lamp_2_set_color_rgb888,
        gsp_chakra_lamp_3_set_color_rgb888};
    static esp_gsp_err_t (*const icon[CHAKRA_LAMPS])(esp_gsp_handle_t, const char *) = {
        gsp_chakra_lamp_icon_0_set_text, gsp_chakra_lamp_icon_1_set_text, gsp_chakra_lamp_icon_2_set_text,
        gsp_chakra_lamp_icon_3_set_text};
    static esp_gsp_err_t (*const name[CHAKRA_LAMPS])(esp_gsp_handle_t, const char *) = {
        gsp_chakra_lamp_name_0_set_text, gsp_chakra_lamp_name_1_set_text, gsp_chakra_lamp_name_2_set_text,
        gsp_chakra_lamp_name_3_set_text};
    static esp_gsp_err_t (*const info[CHAKRA_LAMPS])(esp_gsp_handle_t, const char *) = {
        gsp_chakra_lamp_info_0_set_text, gsp_chakra_lamp_info_1_set_text, gsp_chakra_lamp_info_2_set_text,
        gsp_chakra_lamp_info_3_set_text};
    static esp_gsp_err_t (*const check[CHAKRA_LAMPS])(esp_gsp_handle_t, bool) = {
        gsp_chakra_grp_check_0_set_visible, gsp_chakra_grp_check_1_set_visible, gsp_chakra_grp_check_2_set_visible,
        gsp_chakra_grp_check_3_set_visible};
    static esp_gsp_err_t (*const veil[CHAKRA_LAMPS])(esp_gsp_handle_t, bool) = {
        gsp_chakra_grp_veil_0_set_visible, gsp_chakra_grp_veil_1_set_visible, gsp_chakra_grp_veil_2_set_visible,
        gsp_chakra_grp_veil_3_set_visible};
    for (int i = 0; i < CHAKRA_LAMPS; ++i) {
        const bool used = i < c->lamp_count;
        err(c, row_vis[i](c->ui, used));
        if (!used) {
            continue;
        }
        const chakra_lamp_t *l = &c->lamps[i];
        const bool on = l->paired && (c->s.lamps & (1u << i));
        err(c, row_rgb[i](c->ui, on ? ON_RGB : ROW_RGB));
        err(c, icon[i](c->ui, KIND_ICON[l->kind]));
        err(c, name[i](c->ui, l->name[0] ? l->name : T(c, S_KIND_HA + (int)l->kind)));
        err(c, info[i](c->ui, l->paired ? T(c, S_KIND_HA + (int)l->kind) : T(c, S_NOT_PAIRED)));
        err(c, check[i](c->ui, on));
        err(c, veil[i](c->ui, !l->paired));
    }
}

static void draw_settings(struct chakra_core *c)
{
    draw_lamps(c);
    char v[24];
    snprintf(v, sizeof(v), "%d %%", c->s.brightness);
    err(c, gsp_chakra_val_bright_set_text(c->ui, v));
    snprintf(v, sizeof(v), "%d dB", -30 - 5 * c->s.sensitivity);
    err(c, gsp_chakra_val_sens_set_text(c->ui, v));
    snprintf(v, sizeof(v), c->s.lang == CHAKRA_LANG_DE ? "%d,%d s" : "%d.%d s", c->s.hold_ds / 10, c->s.hold_ds % 10);
    err(c, gsp_chakra_val_hold_set_text(c->ui, v));
    err(c, gsp_chakra_sl_bright_set_value(c->ui, c->s.brightness));
    err(c, gsp_chakra_sl_sens_set_value(c->ui, c->s.sensitivity));
    err(c, gsp_chakra_sl_hold_set_value(c->ui, c->s.hold_ds));
    err(c, gsp_chakra_map_notes_set_color_rgb888(c->ui, c->s.map == CHAKRA_MAP_NOTES ? ON_RGB : OFF_RGB));
    err(c, gsp_chakra_map_solf_set_color_rgb888(c->ui, c->s.map == CHAKRA_MAP_SOLFEGGIO ? ON_RGB : OFF_RGB));
    err(c, gsp_chakra_map_bowls_set_color_rgb888(c->ui, c->s.map == CHAKRA_MAP_BOWLS ? ON_RGB : OFF_RGB));
    err(c, gsp_chakra_react_flow_set_color_rgb888(c->ui, c->s.reaction == CHAKRA_FLOW ? ON_RGB : OFF_RGB));
    err(c, gsp_chakra_react_strike_set_color_rgb888(c->ui, c->s.reaction == CHAKRA_STRIKE ? ON_RGB : OFF_RGB));
    err(c, gsp_chakra_lang_en_set_color_rgb888(c->ui, c->s.lang == CHAKRA_LANG_EN ? ON_RGB : OFF_RGB));
    err(c, gsp_chakra_lang_de_set_color_rgb888(c->ui, c->s.lang == CHAKRA_LANG_DE ? ON_RGB : OFF_RGB));
}

static void settings_changed(struct chakra_core *c)
{
    draw_settings(c);
    if (c->cfg.on_settings) {
        c->cfg.on_settings(&c->s, c->cfg.ctx);
    }
}

static void light(struct chakra_core *c)
{
    const chakra_info_t *info = chakra_info(c->shown);
    if (c->cfg.on_light) {
        c->cfg.on_light(c->shown, info ? info->rgb : 0, c->s.brightness, c->cfg.ctx);
    }
}

// ------------------------------------------------------------------ hold to close --

static void hold_stop(struct chakra_core *c)
{
    if (c->hold_timer) {
        esp_gsp_timer_delete(c->ui, c->hold_timer);
        c->hold_timer = NULL;
    }
    if (c->hold_shown) {
        c->hold_shown = false;
        err(c, gsp_chakra_grp_hold_set_visible(c->ui, false));
        err(c, esp_gsp_set_press_feedback_enabled(c->ui, true));
    }
}

static void hold_tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    struct chakra_core *c = ctx;
    if (!c->hold_timer) {
        return;  // released while this call was already scheduled
    }
    c->hold_ms += HOLD_STEP_MS;
    if (c->hold_ms < HOLD_MS) {
        err(c, gsp_chakra_hold_bar_set_value(c->ui, (int32_t)(c->hold_ms * 1000 / HOLD_MS)));
        return;
    }
    esp_gsp_timer_delete(c->ui, c->hold_timer);
    c->hold_timer = NULL;
    err(c, gsp_chakra_hold_bar_set_value(c->ui, 1000));
    err(c, gsp_chakra_hold_text_set_text(c->ui, T(c, S_CLOSING)));
    haptic(c, 2);
    if (c->cfg.on_close) {
        c->cfg.on_close(c->cfg.ctx);
    }
}

static void hold_start(struct chakra_core *c)
{
    if (c->hold_timer) {
        return;
    }
    c->hold_ms = 0;
    err(c, gsp_chakra_hold_bar_set_value(c->ui, 0));
    err(c, gsp_chakra_hold_text_set_text(c->ui, T(c, S_HOLD_CLOSE)));
    err(c, gsp_chakra_grp_hold_set_visible(c->ui, true));
    // The press shade would darken the orange fill; off while held, back on release.
    err(c, esp_gsp_set_press_feedback_enabled(c->ui, false));
    c->hold_shown = true;
    c->hold_timer = esp_gsp_timer_create(c->ui, HOLD_STEP_MS, hold_tick, c);
    if (!c->hold_timer) {
        hold_stop(c);
    }
}

/* A cancelled press sends no release, so the lifted finger is also read here. */
static void on_pointer(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui;
    (void)x;
    (void)y;
    if (!pressed) {
        hold_stop(ctx);
    }
}

// ------------------------------------------------------------------ learning the user's bowls --

static void learn_draw(struct chakra_core *c, const chakra_reading_t *r)
{
    const chakra_info_t *info = chakra_info(c->learn);
    char line[96];
    snprintf(line, sizeof(line), T(c, S_STRIKE_BOWL), c->s.lang == CHAKRA_LANG_DE ? info->german : info->english);
    err(c, gsp_chakra_name_text_set_text(c->ui, line));
    if (r && r->tone && c->learn_n > 0) {
        const int tenths = (int)(c->learn_mean * 10.0f + 0.5f);
        snprintf(line, sizeof(line), c->s.lang == CHAKRA_LANG_DE ? "%d/7 · %d,%d Hz" : "%d/7 · %d.%d Hz", c->learn + 1,
                 tenths / 10, tenths % 10);
    } else {
        snprintf(line, sizeof(line), "%d/7 · %s", c->learn + 1, T(c, S_HOLD_TONE));
    }
    err(c, gsp_chakra_sub_text_set_text(c->ui, line));
    c->tune = c->learn_n > 0 ? 100.0f * (float)c->learn_ms / LEARN_MS : 0.0f;
}

static void learn_step(struct chakra_core *c, int step)
{
    static esp_gsp_err_t (*const sym[CHAKRA_COUNT])(esp_gsp_handle_t, bool) = {
        gsp_chakra_grp_sym_0_set_visible, gsp_chakra_grp_sym_1_set_visible, gsp_chakra_grp_sym_2_set_visible,
        gsp_chakra_grp_sym_3_set_visible, gsp_chakra_grp_sym_4_set_visible, gsp_chakra_grp_sym_5_set_visible,
        gsp_chakra_grp_sym_6_set_visible};
    c->learn = step;
    c->learn_ms = 0;
    c->learn_n = 0;
    c->learn_sum = 0.0f;
    if (step < 0) {
        return;
    }
    draw_chakra(c, step);  // the step's colour, symbol, dot and Sanskrit name
    for (int i = 0; i < CHAKRA_COUNT; ++i) {
        err(c, sym[i](c->ui, i == step));
    }
    learn_draw(c, NULL);
}

static void learn_start(struct chakra_core *c)
{
    c->settings_open = false;
    err(c, gsp_chakra_grp_settings_set_visible(c->ui, false));
    err(c, gsp_chakra_grp_pulse_set_visible(c->ui, false));
    c->learn_gap = false;
    c->shown = -1;
    learn_step(c, 0);
}

static void learn_finish(struct chakra_core *c)
{
    learn_step(c, -1);
    c->s.map = CHAKRA_MAP_BOWLS;
    settings_changed(c);
    c->shown = -1;
    draw_chakra(c, -1);
    haptic(c, 2);
}

/* One reading while learning: a steady tone for LEARN_MS becomes this step's bowl. */
static void learn_tick(struct chakra_core *c, const chakra_reading_t *r)
{
    const float f = r->freq_hz;
    if (c->learn_gap) {  // the bowl just learned still rings: wait for silence or a new sound
        if (!r->tone || fabsf(1200.0f * log2f(f / c->learn_prev)) > 150.0f) {
            c->learn_gap = false;
        }
        learn_draw(c, NULL);
        return;
    }
    if (r->tone && (c->learn_n == 0 || fabsf(1200.0f * log2f(f / c->learn_mean)) < LEARN_CENTS)) {
        c->learn_sum += f;
        c->learn_n++;
        c->learn_mean = c->learn_sum / (float)c->learn_n;
        c->learn_ms += HOP_MS;
    } else {
        c->learn_ms = 0;
        c->learn_n = 0;
        c->learn_sum = 0.0f;
    }
    if (c->learn_ms >= LEARN_MS) {
        c->s.bowl_hz[c->learn] = c->learn_mean;
        c->learn_prev = c->learn_mean;
        c->learn_gap = true;
        haptic(c, 1);
        if (c->learn + 1 >= CHAKRA_COUNT) {
            learn_finish(c);
            return;
        }
        learn_step(c, c->learn + 1);
        return;
    }
    learn_draw(c, r);
}

static bool bowls_learned(const struct chakra_core *c)
{
    for (int i = 0; i < CHAKRA_COUNT; ++i) {
        if (c->s.bowl_hz[i] <= 0.0f) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ events and tick --

static int lamp_row(const esp_gsp_event_t *event)
{
    return gsp_chakra_event_is_lamp_0(event)   ? 0
           : gsp_chakra_event_is_lamp_1(event) ? 1
           : gsp_chakra_event_is_lamp_2(event) ? 2
           : gsp_chakra_event_is_lamp_3(event) ? 3
                                                : -1;
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    (void)ui;
    struct chakra_core *c = ctx;
    const int row = lamp_row(event);
    if (gsp_chakra_event_is_hold_start(event)) {
        hold_start(c);
    } else if (gsp_chakra_event_is_hold_end(event)) {
        hold_stop(c);
    } else if (gsp_chakra_event_is_gear(event) && c->learn >= 0) {
        learn_step(c, -1);  // the gear cancels learning; bowls learned so far stay
        draw_chakra(c, -1);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_gear(event)) {
        c->settings_open = !c->settings_open;
        if (c->settings_open) {
            draw_settings(c);
        }
        err(c, gsp_chakra_grp_settings_set_visible(c->ui, c->settings_open));
        haptic(c, 1);
    } else if (row >= 0) {
        if (row >= c->lamp_count || !c->lamps[row].paired) {
            return;  // greyed: pair it outside the app first
        }
        c->s.lamps ^= (uint8_t)(1u << row);
        settings_changed(c);
        haptic(c, 1);
        if (c->shown >= 0) {
            light(c);  // a lamp ticked now joins at once
        }
    } else if (gsp_chakra_event_is_sl_bright(event)) {
        c->s.brightness = (int)(int32_t)event->arg;
        settings_changed(c);
        if (c->shown >= 0) {
            light(c);
        }
    } else if (gsp_chakra_event_is_sl_sens(event)) {
        c->s.sensitivity = (int)(int32_t)event->arg;
        settings_changed(c);
    } else if (gsp_chakra_event_is_sl_hold(event)) {
        c->s.hold_ds = (int)(int32_t)event->arg;
        settings_changed(c);
    } else if (gsp_chakra_event_is_bowls_learn(event) ||
               (gsp_chakra_event_is_map_bowls(event) && !bowls_learned(c))) {
        haptic(c, 1);
        learn_start(c);
    } else if (gsp_chakra_event_is_map_bowls(event)) {
        c->s.map = CHAKRA_MAP_BOWLS;
        settings_changed(c);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_map_notes(event) && c->s.map == CHAKRA_MAP_NOTES) {
        c->s.a4 = c->s.a4 == 432 ? 440 : 432;  // a second tap on Notes switches the reference pitch
        draw_labels(c);
        settings_changed(c);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_map_notes(event) || gsp_chakra_event_is_map_solf(event)) {
        c->s.map = gsp_chakra_event_is_map_solf(event) ? CHAKRA_MAP_SOLFEGGIO : CHAKRA_MAP_NOTES;
        settings_changed(c);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_react_flow(event) || gsp_chakra_event_is_react_strike(event)) {
        c->s.reaction = gsp_chakra_event_is_react_strike(event) ? CHAKRA_STRIKE : CHAKRA_FLOW;
        settings_changed(c);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_lang_en(event) || gsp_chakra_event_is_lang_de(event)) {
        c->s.lang = gsp_chakra_event_is_lang_de(event) ? CHAKRA_LANG_DE : CHAKRA_LANG_EN;
        draw_labels(c);
        draw_chakra(c, c->shown);
        settings_changed(c);
        haptic(c, 1);
    }
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    struct chakra_core *c = ctx;
    c->ticks++;
    chakra_reading_t r;
    bool fresh = false;
    for (int budget = 0; budget < 8 && c->cfg.poll && c->cfg.poll(&r, c->cfg.ctx) > 0; ++budget) {
        fresh = true;
        if (r.chakra != c->shown) {  // never skip a change inside a burst
            break;
        }
    }
    if (c->learn >= 0) {
        if (fresh) {
            learn_tick(c, &r);
        }
        animate(c);
        return;
    }
    if (fresh) {
        const bool sounding = r.candidate >= 0 && r.candidate == r.chakra;
        if (sounding || r.chakra != c->shown) {
            c->last = r;  // the note line keeps the last real tone through the after-glow
        }
        c->last.chakra = r.chakra;
        if (sounding != c->sounding) {
            c->sounding = sounding;
            err(c, gsp_chakra_grp_pulse_set_visible(c->ui, sounding));
        }
        if (r.chakra != c->shown) {
            const bool from_silence = c->shown < 0;
            c->shown = r.chakra;
            draw_chakra(c, r.chakra);
            c->sub_tick = c->ticks;
            draw_sub(c);  // at once: the new name never stands over the old note
            if (r.chakra >= 0 && from_silence) {
                haptic(c, 1);
            }
            light(c);
        }
        draw_bars(c, r.bars);
    }
    if (c->shown >= 0 && c->sounding && c->ticks - c->sub_tick >= SUB_EVERY_TICKS) {
        c->sub_tick = c->ticks;
        draw_sub(c);
    }
    animate(c);
}

// ------------------------------------------------------------------ public --

void chakra_core_set_clock(chakra_core_handle_t c, const char *hhmm)
{
    if (c) {
        err(c, gsp_chakra_clock_set_text(c->ui, hhmm));
    }
}

void chakra_core_set_connection(chakra_core_handle_t c, int state)
{
    static const uint32_t COLOURS[] = {0xFF453A, 0xFF9500, 0x34C759, 0x30B0C7};
    if (c && state >= 0 && state < 4) {
        err(c, gsp_chakra_conn_dot_set_color_rgb888(c->ui, COLOURS[state]));
    }
}

void chakra_core_set_lamps(chakra_core_handle_t c, const chakra_lamp_t *lamps, int count)
{
    if (!c) {
        return;
    }
    c->lamp_count = count < 0 ? 0 : count > CHAKRA_LAMPS ? CHAKRA_LAMPS : count;
    for (int i = 0; i < c->lamp_count; ++i) {
        c->lamps[i] = lamps[i];
        c->lamps[i].name[sizeof(c->lamps[i].name) - 1] = '\0';
        if (!lamps[i].paired) {
            c->s.lamps &= (uint8_t)~(1u << i);
        }
    }
    draw_lamps(c);
}

esp_gsp_err_t chakra_core_create(esp_gsp_handle_t ui, const chakra_core_config_t *config, chakra_core_handle_t *out)
{
    if (!out || !ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out = NULL;
    struct chakra_core *c = calloc(1, sizeof(*c));
    if (!c) {
        return ESP_GSP_ERR_NO_MEM;
    }
    c->ui = ui;
    c->shown = -1;
    c->learn = -1;
    c->tune_drawn = -1;
    if (config) {
        c->cfg = *config;
        c->s = config->settings;
    }
    if (c->s.brightness <= 0) {
        chakra_settings_defaults(&c->s);
    }
    c->target_rgb = c->drawn_rgb = IDLE_RGB;
    c->cur[0] = (IDLE_RGB >> 16) & 0xFF;
    c->cur[1] = (IDLE_RGB >> 8) & 0xFF;
    c->cur[2] = IDLE_RGB & 0xFF;
    draw_labels(c);
    draw_chakra(c, -1);
    draw_settings(c);
    esp_gsp_err_t e = esp_gsp_on_event(ui, on_event, c);
    if (e == ESP_GSP_OK) {
        e = esp_gsp_set_pointer_observer(ui, on_pointer, c);
    }
    if (e == ESP_GSP_OK) {
        c->timer = esp_gsp_timer_create(ui, TICK_MS, tick, c);
        if (!c->timer) {
            esp_gsp_on_event(ui, NULL, NULL);
            esp_gsp_set_pointer_observer(ui, NULL, NULL);
            e = ESP_GSP_ERR_NO_MEM;
        }
    }
    if (e != ESP_GSP_OK) {
        free(c);
        return e;
    }
    *out = c;
    return ESP_GSP_OK;
}

esp_gsp_err_t chakra_core_destroy(chakra_core_handle_t *handle)
{
    if (!handle || !*handle) {
        return ESP_GSP_OK;
    }
    struct chakra_core *c = *handle;
    esp_gsp_err_t e = esp_gsp_on_event(c->ui, NULL, NULL);
    if (e == ESP_GSP_OK) {
        e = esp_gsp_set_pointer_observer(c->ui, NULL, NULL);
    }
    if (e == ESP_GSP_OK && c->hold_timer) {
        e = esp_gsp_timer_delete(c->ui, c->hold_timer);
        c->hold_timer = NULL;
    }
    if (e == ESP_GSP_OK && c->timer) {
        e = esp_gsp_timer_delete(c->ui, c->timer);
    }
    if (e != ESP_GSP_OK) {
        return e;
    }
    free(c);
    *handle = NULL;
    return ESP_GSP_OK;
}
