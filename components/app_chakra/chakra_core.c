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
#define FADE_AI 0.03f          // with the AI gate: about 3 s, the softest of the three
#define AI_HOLD_TICKS 50       // a bowl verdict lets the colour change for 2 s
#define KICK_DECAY 0.90f       // the strike kick fades to a tenth in about 0.9 s
#define KICK_WHITE 0.35f       // how far a full kick pushes the colour towards white
#define KICK_BRIGHT 40         // and the lamps' brightness up, in percent points
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
#define DRAW_BUDGET_MS (TICK_MS / 2)  // optional drawing per tick, see spare()
#define SHADOW_SLOTS 256       // a power of two, about twice the scene's setters

/* Material Design Icons, UTF-8, as in make_scene.py: home-assistant, lightbulb, wifi, bluetooth, monitor. */
static const char *const KIND_ICON[] = {"\xf3\xb0\x9f\x90", "\xf3\xb0\x8c\xb5", "\xf3\xb0\x96\xa9", "\xf3\xb0\x82\xaf",
                                        "\xf3\xb0\x8d\xb9"};

/* Every word on the screen, English first (the default), German second. */
enum {
    S_READY, S_STRIKE, S_BRIGHT, S_SENS, S_HOLD, S_NOTES, S_NOT_PAIRED,
    S_KIND_HA, S_KIND_HUE, S_KIND_WIZ, S_KIND_BLE, S_KIND_SCREEN, S_HOLD_CLOSE, S_CLOSING, S_BOWLS, S_STRIKE_BOWL,
    S_HOLD_TONE, S_FLOW, S_STRIKE_MODE, S_SETTINGS, S_CONNECTIONS, S_AUTO, S_COLOUR, S_AI, S_OFF, S_ON, S_COUNT
};
static const char *const TEXT[2][S_COUNT] = {
    {"Ready", "Strike a bowl or hum", "Brightness", "Sensitivity", "Afterglow", "Notes", "not paired",
     "Home Assistant", "Hue", "Wi-Fi", "Bluetooth", "Screen lamp", "Hold: close Chakra", "Closing Chakra", "Bowls",
     "Strike your %s bowl", "hold the tone", "Flow", "Strike", "Settings", "Connections", "Auto", "Colour",
     "AI sound gate", "Off", "On"},
    {"Bereit", "Klangschale anschlagen oder summen", "Helligkeit", "Empfindlichkeit", "Nachklang", "Noten",
     "nicht gekoppelt", "Home Assistant", "Hue", "WLAN", "Bluetooth", "Bildschirm-Lampe", "Halten: Chakra schließen",
     "Chakra wird geschlossen", "Schalen", "%s-Schale anschlagen", "Ton halten", "Fließen", "Anschlag",
     "Einstellungen", "Anbindungen", "Auto", "Farbe", "KI-Klangfilter", "Aus", "An"},
};

/* The value a setter last sent; see put() below. */
typedef struct {
    uintptr_t setter;  // 0: a free slot
    uint64_t value;    // the value, or a hash of the text
} shadow_t;

struct chakra_core {
    esp_gsp_handle_t ui;
    chakra_core_config_t cfg;
    chakra_settings_t s;
    chakra_lamp_t lamps[CHAKRA_LAMPS];
    int lamp_count;
    void *timer;
    void *hold_timer;
    uint32_t hold_ms;
    uint32_t hold_since;  // now_ms() when the hold began
    uint32_t tick_start;  // now_ms() when this tick began
    bool hold_shown;
    bool settings_open;
    int page;             // settings page: 0 Settings, 1 Connections
    int shown;            // chakra on screen, -1 idle
    bool sounding;        // a gated-in tone this hop
    float cur[3];         // background now, 0..255
    uint32_t target_rgb;
    uint32_t drawn_rgb;
    float tune;           // tuning ring now, 0..100
    int tune_drawn;
    uint8_t bars[CHAKRA_BARS];       // the spectrum on screen
    uint8_t bars_want[CHAKRA_BARS];  // the last reading's spectrum
    int bar_next;                    // the bar the next spectrum pass starts with
    uint32_t ticks, sub_tick;
    uint32_t bowl_tick;   // the tick of the AI gate's last bowl verdict (settings.ai)
    bool bowl_heard;      // whether there was one at all
    float kick;           // the strike kick now, 1 at a strike, decaying to 0
    chakra_reading_t last;
    int learn;            // bowl being learned 0..6, -1 when not learning
    uint32_t learn_ms;
    float learn_sum, learn_mean, learn_prev;
    int learn_n;
    bool learn_gap;       // after a capture: wait for silence or another bowl
    esp_gsp_err_t last_error;
    shadow_t shadow[SHADOW_SLOTS];
};

static void err(struct chakra_core *c, esp_gsp_err_t e)
{
    if (e != ESP_GSP_OK) {
        c->last_error = e;
    }
}

// ------------------------------------------------------------------ setters --

/* Every setter is one round trip to the screen. In the simulator that is one frame of its host,
 * about 20 ms (measured 25.10.2026), so a tick that drew the fade, the tuning ring, the note line
 * and 16 bars took 100 to 500 ms instead of 40, and the hold-to-close gesture fell behind. The
 * value each setter sent last is kept, and an unchanged value is not sent again; put() picks the
 * right one of these by the setter's type. */
typedef esp_gsp_err_t (*set_bool_t)(esp_gsp_handle_t, bool);
typedef esp_gsp_err_t (*set_i32_t)(esp_gsp_handle_t, int32_t);
typedef esp_gsp_err_t (*set_rgb_t)(esp_gsp_handle_t, uint32_t);
typedef esp_gsp_err_t (*set_text_t)(esp_gsp_handle_t, const char *);

/* The setter's slot, or NULL when the table is full (then it always sends). */
static shadow_t *shadow_of(struct chakra_core *c, uintptr_t setter)
{
    size_t i = (size_t)(((uint64_t)setter * 0x9E3779B97F4A7C15ull) >> 40) & (SHADOW_SLOTS - 1);
    for (int n = 0; n < SHADOW_SLOTS; ++n, i = (i + 1) & (SHADOW_SLOTS - 1)) {
        if (c->shadow[i].setter == setter || c->shadow[i].setter == 0) {
            return &c->shadow[i];
        }
    }
    return NULL;
}

static bool unchanged(const shadow_t *s, uint64_t value)
{
    return s && s->setter && s->value == value;
}

/* Only a value the screen took is remembered; after an error the next call sends again. */
static void remember(struct chakra_core *c, shadow_t *s, uintptr_t setter, uint64_t value, esp_gsp_err_t e)
{
    err(c, e);
    if (s && e == ESP_GSP_OK) {
        s->setter = setter;
        s->value = value;
    }
}

static void put_bool(struct chakra_core *c, set_bool_t set, bool v)
{
    shadow_t *s = shadow_of(c, (uintptr_t)set);
    if (!unchanged(s, v)) {
        remember(c, s, (uintptr_t)set, v, set(c->ui, v));
    }
}

static void put_i32(struct chakra_core *c, set_i32_t set, int32_t v)
{
    shadow_t *s = shadow_of(c, (uintptr_t)set);
    if (!unchanged(s, (uint32_t)v)) {
        remember(c, s, (uintptr_t)set, (uint32_t)v, set(c->ui, v));
    }
}

static void put_rgb(struct chakra_core *c, set_rgb_t set, uint32_t v)
{
    shadow_t *s = shadow_of(c, (uintptr_t)set);
    if (!unchanged(s, v)) {
        remember(c, s, (uintptr_t)set, v, set(c->ui, v));
    }
}

static void put_text(struct chakra_core *c, set_text_t set, const char *text)
{
    uint64_t h = 0xCBF29CE484222325ull;  // FNV-1a
    for (const char *p = text; p && *p; ++p) {
        h = (h ^ (uint8_t)*p) * 0x100000001B3ull;
    }
    shadow_t *s = shadow_of(c, (uintptr_t)set);
    if (!unchanged(s, h)) {
        remember(c, s, (uintptr_t)set, h, set(c->ui, text));
    }
}

#define put(c, setter, v) _Generic((setter), set_bool_t: put_bool, set_i32_t: put_i32, set_rgb_t: put_rgb, \
                                   set_text_t: put_text)(c, setter, v)

static uint32_t now(const struct chakra_core *c)
{
    return c->cfg.now_ms ? c->cfg.now_ms(c->cfg.ctx) : 0;
}

/* Time left in this tick for drawing that can wait (the fade, the tuning ring, the note line,
 * the spectrum): what is skipped now is drawn with a later tick's values. On the board every
 * setter is quick and nothing waits; in the simulator about one such setter fits a tick. */
static bool spare(const struct chakra_core *c)
{
    return !c->cfg.now_ms || now(c) - c->tick_start < DRAW_BUDGET_MS;
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

// ------------------------------------------------------------------ the light's colour --

/* A hue 0..359 at full saturation and value, as 0xRRGGBB. */
static uint32_t hue_rgb(int hue)
{
    hue = ((hue % 360) + 360) % 360;
    const int x = (hue % 60) * 255 / 60, seg = hue / 60;
    const int r[6] = {255, 255 - x, 0, 0, x, 255}, g[6] = {x, 255, 255, 255 - x, 0, 0}, b[6] = {0, 0, x, 255, 255, 255 - x};
    return ((uint32_t)r[seg] << 16) | ((uint32_t)g[seg] << 8) | (uint32_t)b[seg];
}

/* The fixed colour, or 0 when the colour follows the chakra (Auto). */
static uint32_t fixed_rgb(const struct chakra_core *c)
{
    if (c->s.color_mode == CHAKRA_COLOR_CHAKRA && chakra_info(c->s.color_value)) {
        return chakra_info(c->s.color_value)->rgb;
    }
    return c->s.color_mode == CHAKRA_COLOR_HUE ? hue_rgb(c->s.color_value) : 0;
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
        put(c, sym[i], i == chakra);
        put(c, ring[i], i == chakra);
    }
    put(c, gsp_chakra_grp_sym_idle_set_visible, chakra < 0);
    put(c, gsp_chakra_grp_tune_set_visible, chakra >= 0);
    const chakra_info_t *info = chakra_info(chakra);
    const char *name = !info ? T(c, S_READY) : c->s.lang == CHAKRA_LANG_DE ? info->german : info->english;
    put(c, gsp_chakra_name_text_set_text, name);
    put(c, gsp_chakra_sanskrit_text_set_text, info ? info->sanskrit : "");
    if (!info) {
        put(c, gsp_chakra_sub_text_set_text, T(c, S_STRIKE));
    }
    const uint32_t fixed = fixed_rgb(c);
    c->target_rgb = fixed ? fixed : info ? info->rgb : IDLE_RGB;
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
    put(c, gsp_chakra_sub_text_set_text, line);
}

static void draw_bars(struct chakra_core *c)
{
    static esp_gsp_err_t (*const set[CHAKRA_BARS])(esp_gsp_handle_t, int32_t) = {
        gsp_chakra_bar_0_set_value, gsp_chakra_bar_1_set_value, gsp_chakra_bar_2_set_value,
        gsp_chakra_bar_3_set_value, gsp_chakra_bar_4_set_value, gsp_chakra_bar_5_set_value,
        gsp_chakra_bar_6_set_value, gsp_chakra_bar_7_set_value, gsp_chakra_bar_8_set_value,
        gsp_chakra_bar_9_set_value, gsp_chakra_bar_10_set_value, gsp_chakra_bar_11_set_value,
        gsp_chakra_bar_12_set_value, gsp_chakra_bar_13_set_value, gsp_chakra_bar_14_set_value,
        gsp_chakra_bar_15_set_value};
    const uint8_t *bars = c->bars_want;
    for (int k = 0; k < CHAKRA_BARS && spare(c); ++k) {
        const int b = (c->bar_next + k) % CHAKRA_BARS;
        const int d = (int)bars[b] - (int)c->bars[b];
        if (d >= 2 || d <= -2 || (bars[b] == 0 && c->bars[b] != 0)) {
            c->bars[b] = bars[b];
            put(c, set[b], bars[b]);
            c->bar_next = (b + 1) % CHAKRA_BARS;
        }
    }
}

static void light(struct chakra_core *c);

/* Background towards the chakra's colour, tuning ring towards how well the tone sits. */
static void animate(struct chakra_core *c)
{
    const float t[3] = {(float)((c->target_rgb >> 16) & 0xFF), (float)((c->target_rgb >> 8) & 0xFF),
                        (float)(c->target_rgb & 0xFF)};
    uint32_t rgb = 0;
    for (int i = 0; i < 3; ++i) {
        const float d = t[i] - c->cur[i];
        const float fade = c->s.ai ? FADE_AI : c->s.reaction == CHAKRA_STRIKE ? FADE_STRIKE : FADE_FLOW;
        c->cur[i] = (d > -1.0f && d < 1.0f) ? t[i] : c->cur[i] + d * fade;
        const float shown = c->cur[i] + (255.0f - c->cur[i]) * KICK_WHITE * c->kick;  // a strike: towards white
        rgb = (rgb << 8) | (uint32_t)(shown + 0.5f);
    }
    if (c->kick > 0.0f) {
        c->kick = c->kick < 0.02f ? 0.0f : c->kick * KICK_DECAY;
        if (c->ticks % 3 == 0 || c->kick == 0.0f) {
            light(c);  // the lamps take the kick too, at most every third tick
        }
    }
    if (rgb != c->drawn_rgb && spare(c)) {
        c->drawn_rgb = rgb;
        put(c, gsp_chakra_bg_set_color_rgb888, rgb);
    }
    if (c->learn < 0) {
        const int cents = c->last.cents < 0 ? -c->last.cents : c->last.cents;
        const float goal = c->sounding ? 100.0f - 2.0f * (float)cents : 0.0f;
        c->tune += (goal - c->tune) * TUNE_STEP;
    }
    const int v = (int)(c->tune + 0.5f);
    if (v != c->tune_drawn && spare(c)) {
        c->tune_drawn = v;
        put(c, gsp_chakra_tune_arc_set_value, v);
    }
}

/* Words that only change with the language. */
static void draw_labels(struct chakra_core *c)
{
    put(c, gsp_chakra_lbl_tab_set_set_text, T(c, S_SETTINGS));
    put(c, gsp_chakra_lbl_tab_conn_set_text, T(c, S_CONNECTIONS));
    put(c, gsp_chakra_lbl_col_auto_set_text, T(c, S_AUTO));
    put(c, gsp_chakra_lbl_hue_set_text, T(c, S_COLOUR));
    put(c, gsp_chakra_lbl_url_set_text, T(c, S_KIND_SCREEN));
    put(c, gsp_chakra_lbl_bright_set_text, T(c, S_BRIGHT));
    put(c, gsp_chakra_lbl_sens_set_text, T(c, S_SENS));
    put(c, gsp_chakra_lbl_hold_set_text, T(c, S_HOLD));
    char notes[24];
    snprintf(notes, sizeof(notes), "%s %d", T(c, S_NOTES), c->s.a4 == 432 ? 432 : 440);
    put(c, gsp_chakra_lbl_map_notes_set_text, notes);
    put(c, gsp_chakra_lbl_map_bowls_set_text, T(c, S_BOWLS));
    put(c, gsp_chakra_lbl_react_flow_set_text, T(c, S_FLOW));
    put(c, gsp_chakra_lbl_react_strike_set_text, T(c, S_STRIKE_MODE));
    put(c, gsp_chakra_lbl_ai_set_text, T(c, S_AI));
    put(c, gsp_chakra_lbl_ai_off_set_text, T(c, S_OFF));
    put(c, gsp_chakra_lbl_ai_on_set_text, T(c, S_ON));
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
    const char *url = NULL;
    for (int i = 0; i < CHAKRA_LAMPS; ++i) {
        const bool used = i < c->lamp_count;
        put(c, row_vis[i], used);
        if (!used) {
            continue;
        }
        const chakra_lamp_t *l = &c->lamps[i];
        const bool on = l->paired && (c->s.lamps & (1u << i));
        put(c, row_rgb[i], on ? ON_RGB : ROW_RGB);
        put(c, icon[i], KIND_ICON[l->kind]);
        put(c, name[i], l->name[0] ? l->name : T(c, S_KIND_HA + (int)l->kind));
        /* The screen lamp's row names itself; its address stands large in the card below. */
        put(c, info[i], !l->paired ? T(c, S_NOT_PAIRED) : l->kind == CHAKRA_LAMP_SCREEN ? "" : T(c, S_KIND_HA + (int)l->kind));
        put(c, check[i], on);
        put(c, veil[i], !l->paired);
        if (l->kind == CHAKRA_LAMP_SCREEN && l->paired && l->url[0] && !url) {
            url = l->url;
        }
    }
    put(c, gsp_chakra_grp_url_set_visible, url != NULL);
    put(c, gsp_chakra_lamp_url_set_text, url ? url : "");
}

/* The tabs and the page they show. */
static void draw_page(struct chakra_core *c)
{
    put(c, gsp_chakra_tab_set_set_color_rgb888, c->page == 0 ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_tab_conn_set_color_rgb888, c->page == 1 ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_grp_page_set_set_visible, c->page == 0);
    put(c, gsp_chakra_grp_page_conn_set_visible, c->page == 1);
}

static void draw_colour(struct chakra_core *c)
{
    static esp_gsp_err_t (*const ring[CHAKRA_COUNT])(esp_gsp_handle_t, bool) = {
        gsp_chakra_grp_col_ring_0_set_visible, gsp_chakra_grp_col_ring_1_set_visible,
        gsp_chakra_grp_col_ring_2_set_visible, gsp_chakra_grp_col_ring_3_set_visible,
        gsp_chakra_grp_col_ring_4_set_visible, gsp_chakra_grp_col_ring_5_set_visible,
        gsp_chakra_grp_col_ring_6_set_visible};
    const int mode = c->s.color_mode;
    put(c, gsp_chakra_col_auto_set_color_rgb888, mode == CHAKRA_COLOR_AUTO ? ON_RGB : OFF_RGB);
    for (int i = 0; i < CHAKRA_COUNT; ++i) {
        put(c, ring[i], mode == CHAKRA_COLOR_CHAKRA && c->s.color_value == i);
    }
    const uint32_t fixed = fixed_rgb(c);
    put(c, gsp_chakra_hue_swatch_set_color_rgb888, fixed ? fixed : OFF_RGB);  // grey while Auto
    char v[16];
    if (mode == CHAKRA_COLOR_HUE) {
        snprintf(v, sizeof(v), "%d°", c->s.color_value);
    } else {
        snprintf(v, sizeof(v), "%s", mode == CHAKRA_COLOR_AUTO ? T(c, S_AUTO) : "");
    }
    put(c, gsp_chakra_val_hue_set_text, v);
}

static void draw_settings(struct chakra_core *c)
{
    draw_page(c);
    draw_lamps(c);
    draw_colour(c);
    char v[24];
    snprintf(v, sizeof(v), "%d %%", c->s.brightness);
    put(c, gsp_chakra_val_bright_set_text, v);
    snprintf(v, sizeof(v), "%d dB", -30 - 5 * c->s.sensitivity);
    put(c, gsp_chakra_val_sens_set_text, v);
    snprintf(v, sizeof(v), c->s.lang == CHAKRA_LANG_DE ? "%d,%d s" : "%d.%d s", c->s.hold_ds / 10, c->s.hold_ds % 10);
    put(c, gsp_chakra_val_hold_set_text, v);
    /* A finger moves the sliders too, so they are never skipped as unchanged. */
    err(c, gsp_chakra_sl_bright_set_value(c->ui, c->s.brightness));
    if (c->s.color_mode == CHAKRA_COLOR_HUE) {
        err(c, gsp_chakra_sl_hue_set_value(c->ui, c->s.color_value));
    }
    err(c, gsp_chakra_sl_sens_set_value(c->ui, c->s.sensitivity));
    err(c, gsp_chakra_sl_hold_set_value(c->ui, c->s.hold_ds));
    put(c, gsp_chakra_map_notes_set_color_rgb888, c->s.map == CHAKRA_MAP_NOTES ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_map_solf_set_color_rgb888, c->s.map == CHAKRA_MAP_SOLFEGGIO ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_map_bowls_set_color_rgb888, c->s.map == CHAKRA_MAP_BOWLS ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_react_flow_set_color_rgb888, c->s.reaction == CHAKRA_FLOW ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_react_strike_set_color_rgb888, c->s.reaction == CHAKRA_STRIKE ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_ai_off_set_color_rgb888, c->s.ai ? OFF_RGB : ON_RGB);
    put(c, gsp_chakra_ai_on_set_color_rgb888, c->s.ai ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_lang_en_set_color_rgb888, c->s.lang == CHAKRA_LANG_EN ? ON_RGB : OFF_RGB);
    put(c, gsp_chakra_lang_de_set_color_rgb888, c->s.lang == CHAKRA_LANG_DE ? ON_RGB : OFF_RGB);
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
    if (!c->cfg.on_light) {
        return;
    }
    const uint32_t fixed = fixed_rgb(c);
    int bright = c->s.brightness + (int)((float)KICK_BRIGHT * c->kick + 0.5f);  // a strike kicks it up
    bright = bright > 100 ? 100 : bright;
    if (fixed) {  // a steady light: its own colour, whatever the detection hears
        c->cfg.on_light(c->s.color_mode == CHAKRA_COLOR_CHAKRA ? c->s.color_value : CHAKRA_FIXED_HUE, fixed, bright,
                        c->cfg.ctx);
        return;
    }
    const chakra_info_t *info = chakra_info(c->shown);
    c->cfg.on_light(c->shown, info ? info->rgb : 0, bright, c->cfg.ctx);
}

/* A new colour choice: the background, the lamps and the settings follow at once. */
static void colour_changed(struct chakra_core *c)
{
    draw_chakra(c, c->shown);
    draw_colour(c);
    if (c->cfg.on_settings) {
        c->cfg.on_settings(&c->s, c->cfg.ctx);
    }
    if (fixed_rgb(c) || c->shown >= 0) {
        light(c);
    }
}

static int colour_dot(const esp_gsp_event_t *event)
{
    return gsp_chakra_event_is_col_0(event)   ? 0
           : gsp_chakra_event_is_col_1(event) ? 1
           : gsp_chakra_event_is_col_2(event) ? 2
           : gsp_chakra_event_is_col_3(event) ? 3
           : gsp_chakra_event_is_col_4(event) ? 4
           : gsp_chakra_event_is_col_5(event) ? 5
           : gsp_chakra_event_is_col_6(event) ? 6
                                              : -1;
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
        put(c, gsp_chakra_grp_hold_set_visible, false);
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
    /* Real time since the press, not the sum of the timer's periods: a late tick must not
     * stretch the 3 s. Without a clock the ticks are counted, as before. */
    c->hold_ms = c->cfg.now_ms ? now(c) - c->hold_since : c->hold_ms + HOLD_STEP_MS;
    if (c->hold_ms < HOLD_MS) {
        put(c, gsp_chakra_hold_bar_set_value, (int32_t)(c->hold_ms * 1000 / HOLD_MS));
        return;
    }
    esp_gsp_timer_delete(c->ui, c->hold_timer);
    c->hold_timer = NULL;
    put(c, gsp_chakra_hold_bar_set_value, 1000);
    put(c, gsp_chakra_hold_text_set_text, T(c, S_CLOSING));
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
    c->hold_since = now(c);
    put(c, gsp_chakra_hold_bar_set_value, 0);
    put(c, gsp_chakra_hold_text_set_text, T(c, S_HOLD_CLOSE));
    put(c, gsp_chakra_grp_hold_set_visible, true);
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
    put(c, gsp_chakra_name_text_set_text, line);
    if (r && r->tone && c->learn_n > 0) {
        const int tenths = (int)(c->learn_mean * 10.0f + 0.5f);
        snprintf(line, sizeof(line), c->s.lang == CHAKRA_LANG_DE ? "%d/7 · %d,%d Hz" : "%d/7 · %d.%d Hz", c->learn + 1,
                 tenths / 10, tenths % 10);
    } else {
        snprintf(line, sizeof(line), "%d/7 · %s", c->learn + 1, T(c, S_HOLD_TONE));
    }
    put(c, gsp_chakra_sub_text_set_text, line);
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
        put(c, sym[i], i == step);
    }
    learn_draw(c, NULL);
}

static void learn_start(struct chakra_core *c)
{
    c->settings_open = false;
    put(c, gsp_chakra_grp_settings_set_visible, false);
    put(c, gsp_chakra_grp_pulse_set_visible, false);
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
    const int dot = colour_dot(event);
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
        put(c, gsp_chakra_grp_settings_set_visible, c->settings_open);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_tab_set(event) || gsp_chakra_event_is_tab_conn(event)) {
        c->page = gsp_chakra_event_is_tab_conn(event) ? 1 : 0;
        draw_page(c);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_col_auto(event)) {
        c->s.color_mode = CHAKRA_COLOR_AUTO;
        colour_changed(c);
        haptic(c, 1);
    } else if (dot >= 0) {
        c->s.color_mode = CHAKRA_COLOR_CHAKRA;
        c->s.color_value = dot;
        colour_changed(c);
        haptic(c, 1);
    } else if (gsp_chakra_event_is_sl_hue(event)) {
        c->s.color_mode = CHAKRA_COLOR_HUE;
        c->s.color_value = (int)(int32_t)event->arg;
        colour_changed(c);
    } else if (row >= 0) {
        if (row >= c->lamp_count || !c->lamps[row].paired) {
            return;  // greyed: pair it outside the app first
        }
        c->s.lamps ^= (uint8_t)(1u << row);
        settings_changed(c);
        haptic(c, 1);
        if (c->shown >= 0 || fixed_rgb(c)) {
            light(c);  // a lamp ticked now joins at once
        }
    } else if (gsp_chakra_event_is_sl_bright(event)) {
        c->s.brightness = (int)(int32_t)event->arg;
        settings_changed(c);
        if (c->shown >= 0 || fixed_rgb(c)) {
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
    } else if (gsp_chakra_event_is_ai_off(event) || gsp_chakra_event_is_ai_on(event)) {
        c->s.ai = gsp_chakra_event_is_ai_on(event) ? 1 : 0;
        c->kick = 0.0f;
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
    c->tick_start = now(c);
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
            put(c, gsp_chakra_grp_pulse_set_visible, sounding);
        }
        /* With the AI gate, only a bowl-like sound may change the colour: a gong or a drum that
         * the detection reads as a tone keeps the colour and kicks the light instead. */
        const bool ai_blocks = c->s.ai && r.chakra >= 0 &&
                               (!c->bowl_heard || c->ticks - c->bowl_tick > AI_HOLD_TICKS);
        if (r.chakra != c->shown && !ai_blocks) {
            const bool from_silence = c->shown < 0;
            c->shown = r.chakra;
            draw_chakra(c, r.chakra);
            c->sub_tick = c->ticks;
            draw_sub(c);  // at once: the new name never stands over the old note
            if (r.chakra >= 0 && from_silence) {
                haptic(c, 1);
            }
            if (!fixed_rgb(c)) {
                light(c);  // a fixed colour stays, whatever is heard
            }
        }
        memcpy(c->bars_want, r.bars, sizeof(c->bars_want));
    }
    animate(c);
    if (c->shown >= 0 && c->sounding && c->ticks - c->sub_tick >= SUB_EVERY_TICKS && spare(c)) {
        c->sub_tick = c->ticks;
        draw_sub(c);
    }
    draw_bars(c);
}

// ------------------------------------------------------------------ public --

void chakra_core_set_sound(chakra_core_handle_t c, int kind, int strike)
{
    if (!c || !c->s.ai) {
        return;
    }
    if (kind == CHAKRA_SOUND_BOWL) {
        c->bowl_tick = c->ticks;
        c->bowl_heard = true;
    } else if (kind == CHAKRA_SOUND_OTHER && strike) {
        c->kick = 1.0f;
        light(c);
    }
}

void chakra_core_set_clock(chakra_core_handle_t c, const char *hhmm)
{
    if (c) {
        put(c, gsp_chakra_clock_set_text, hhmm);
    }
}

void chakra_core_set_connection(chakra_core_handle_t c, int state)
{
    static const uint32_t COLOURS[] = {0xFF453A, 0xFF9500, 0x34C759, 0x30B0C7};
    if (c && state >= 0 && state < 4) {
        put(c, gsp_chakra_conn_dot_set_color_rgb888, COLOURS[state]);
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
    if (fixed_rgb(c)) {
        light(c);  // a stored steady colour lights the lamps as soon as they are known
    }
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
    if (c->s.color_mode < CHAKRA_COLOR_AUTO || c->s.color_mode > CHAKRA_COLOR_HUE) {
        c->s.color_mode = CHAKRA_COLOR_AUTO;  // a blob from before the colour setting, or damaged
    }
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
