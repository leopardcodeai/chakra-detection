// SPDX-License-Identifier: Apache-2.0
// Chakra app controller: readings in, screen and light commands out.
//
// The platform (main/main.c on the board, host/platform_pc.c in the simulator) runs the
// detection (chakra_detect.h) wherever its audio arrives and hands each reading over through
// `poll`; the core fades the background to the chakra's colour, swaps the symbol, draws the
// spectrum, runs the settings page and the hold-to-close top bar, and asks the platform to set
// the lamps through `on_light`. All callbacks run on the UI task and must not block.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "chakra_detect.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHAKRA_LAMPS 4  // rows in the settings' lamp list (ui/make_scene.py LAMPS)

typedef enum { CHAKRA_LAMP_HA, CHAKRA_LAMP_HUE, CHAKRA_LAMP_WIZ, CHAKRA_LAMP_BLE } chakra_lamp_kind_t;

enum { CHAKRA_LANG_EN, CHAKRA_LANG_DE };

/* Measured on 24.10.2026 (tests/analyse.sh): Flow locks 0.38 s after a strike and changed 3 times on
 * a handpan melody, Strike locks after 0.13 s and changed 116 times; both kept all seven bowls. */
enum { CHAKRA_FLOW, CHAKRA_STRIKE };

/** A lamp the platform knows. Pairing happens outside the app (Home Assistant, the Hue app,
 *  tools/provision.py); an unpaired kind is listed greyed so the user sees what is possible. */
typedef struct {
    chakra_lamp_kind_t kind;
    char name[48];   // "Living room LED strip"
    bool paired;
} chakra_lamp_t;

typedef struct {
    uint8_t lamps;     // bit i: lamp row i follows the chakra
    int brightness;    // lamps, 10..100 %
    int sensitivity;   // 1..10; the gate is -30 - 5 x sensitivity dBFS
    int hold_ds;       // after-glow in tenths of a second, 5..50
    int map;           // chakra_map_t
    int lang;          // CHAKRA_LANG_EN (default) or CHAKRA_LANG_DE
    int a4;            // reference pitch for Notes: 440 (default) or 432; a second tap on Notes switches
    int reaction;      // CHAKRA_FLOW (default: steady tones, soft fades) or CHAKRA_STRIKE (each strike, fast)
    float bowl_hz[CHAKRA_COUNT];  // the user's bowls as learned (Bowls in the settings); 0 = not yet
} chakra_settings_t;

typedef struct chakra_core *chakra_core_handle_t;

/** A new reading, 1; nothing new, 0. Called on every tick. */
typedef int (*chakra_poll_cb_t)(chakra_reading_t *out, void *ctx);
/** The locked chakra changed; -1 is silence after the after-glow (lamps keep their colour then).
 *  The platform sets the lamps ticked in the settings it got last through on_settings. */
typedef void (*chakra_light_cb_t)(int chakra, uint32_t rgb, int brightness, void *ctx);
/** The user changed a setting; the platform stores it and applies it to the detection. */
typedef void (*chakra_settings_cb_t)(const chakra_settings_t *settings, void *ctx);
/** The top bar was held 3 s: back to Vibe Mode. */
typedef void (*chakra_close_cb_t)(void *ctx);
/** 1 a tap or a new chakra, 2 closing. */
typedef void (*chakra_haptic_cb_t)(int pattern, void *ctx);

typedef struct {
    chakra_poll_cb_t poll;
    chakra_light_cb_t on_light;         // optional
    chakra_settings_cb_t on_settings;   // optional
    chakra_close_cb_t on_close;         // optional
    chakra_haptic_cb_t on_haptic;       // optional
    chakra_settings_t settings;         // shown at start
    void *ctx;
} chakra_core_config_t;

void chakra_settings_defaults(chakra_settings_t *s);
/** The detection's config for these settings. */
void chakra_settings_detect(const chakra_settings_t *s, chakra_detect_config_t *out);

esp_gsp_err_t chakra_core_create(esp_gsp_handle_t ui, const chakra_core_config_t *config, chakra_core_handle_t *out);
esp_gsp_err_t chakra_core_destroy(chakra_core_handle_t *handle);

/** Top bar: "H:MM" or "--:--"; dot 0 off, 1 connecting, 2 connected, 3 simulator. UI task only. */
void chakra_core_set_clock(chakra_core_handle_t c, const char *hhmm);
void chakra_core_set_connection(chakra_core_handle_t c, int state);
/** The lamp list, at most CHAKRA_LAMPS; ticks on unpaired lamps are cleared. UI task only. */
void chakra_core_set_lamps(chakra_core_handle_t c, const chakra_lamp_t *lamps, int count);

#ifdef __cplusplus
}
#endif
