// SPDX-License-Identifier: Apache-2.0
// Chakra on the ESP-Mosaico, board side: display, microphone, Wi-Fi, lamps, settings, haptics.
//
// What: starts the GSP UI with chakra_core (components/app_chakra), listens to the ES8311
// microphone (48 kHz stereo, mixed down to 16 kHz mono) and runs chakra_detect.c in its own task,
// joins the strongest known Wi-Fi (the one set in Vibe Mode plus the extra networks written by
// tools/provision.py), and colours the ticked lamps: Home Assistant lights over its websocket
// (Bluetooth lamps paired in Home Assistant included), a Philips Hue bridge directly on the LAN,
// and WiZ bulbs over UDP. Settings are kept in NVS.
//
// Why: owner 24.10.2026, the device stands in the middle of a sound healing room and the light
// follows the bowls; the screen itself is a lamp when there is none.
//
// Pitfalls:
//   * GSP callbacks must not block: lamps go through a queue to the light task, settings to the
//     microphone task, which applies them between two reads (the detection is not thread-safe).
//   * Pairing happens outside the app (Home Assistant, the Hue link button via tools/hue_pair.py);
//     a kind without credentials shows as "not paired".
//   * Hue uses the bridge's v1 REST API over plain HTTP with the key in the path (LAN only); the
//     v2 API over HTTPS with the hue-application-key header is the next step.
//   * Untested on hardware until the board arrives (ordered 24.10.2026).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_display.h"
#include "bsp/audio.h"
#include "bsp/motor.h"
#include "chakra_core.h"
#include "cJSON.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "chakra_mosaico";

#define NVS_NS "chakra"
#define SETTINGS_VERSION 2
#define MAX_HA 4
#define MAX_HUE 16
#define MAX_WIZ 8
#define MAX_NETS 4
#define MIC_FRAMES 1536  // 32 ms at 48 kHz

typedef struct {
    int chakra;
    uint32_t rgb;
    int brightness;
    int transition_ds;
} light_cmd_t;

static struct {
    esp_gsp_handle_t ui;
    chakra_core_handle_t core;
    chakra_detect_t *detect;
    QueueHandle_t readings;  // chakra_reading_t, length 1, the newest wins
    QueueHandle_t lights;    // light_cmd_t
    volatile bool cfg_dirty;
    chakra_detect_config_t cfg;
    chakra_settings_t settings;
    volatile bool save_due;
    /* lamps, in the order of the settings' rows */
    chakra_lamp_t rows[CHAKRA_LAMPS];
    int row_count;
    int row_ha[CHAKRA_LAMPS];        // index into ha_entity, -1 when the row is no HA light
    char ha_url[128], ha_token[256];
    char ha_entity[MAX_HA][64];
    int ha_count;
    char hue_host[64], hue_key[64];
    int hue_ids[MAX_HUE], hue_count;
    char wiz_ip[MAX_WIZ][16];
    int wiz_count;
    esp_websocket_client_handle_t ws;
    bool authed;
    int next_id;
    volatile bool online;
} app = {.next_id = 1};

// ------------------------------------------------------------------ settings --

static void nvs_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static void add_row(chakra_lamp_kind_t kind, const char *name, bool paired, int ha)
{
    if (app.row_count >= CHAKRA_LAMPS) {
        return;
    }
    chakra_lamp_t *r = &app.rows[app.row_count];
    r->kind = kind;
    snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
    r->paired = paired;
    app.row_ha[app.row_count++] = ha;
}

/* NVS "chakra" (tools/provision.py): ha_url, ha_token, ha_lights "light.a=Name;light.b=Name",
 * hue_host, hue_key, hue_lights "5,6,8", wiz "192.168.1.20,192.168.1.21". */
static void load_lamps(void)
{
    nvs_handle_t h;
    char ha_lights[512] = "", hue_lights[128] = "", wiz[256] = "";
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_str(h, "ha_url", app.ha_url, sizeof(app.ha_url));
        nvs_str(h, "ha_token", app.ha_token, sizeof(app.ha_token));
        nvs_str(h, "ha_lights", ha_lights, sizeof(ha_lights));
        nvs_str(h, "hue_host", app.hue_host, sizeof(app.hue_host));
        nvs_str(h, "hue_key", app.hue_key, sizeof(app.hue_key));
        nvs_str(h, "hue_lights", hue_lights, sizeof(hue_lights));
        nvs_str(h, "wiz", wiz, sizeof(wiz));
        nvs_close(h);
    }
    const bool ha = app.ha_url[0] && app.ha_token[0];
    for (char *save = NULL, *tok = strtok_r(ha_lights, ";", &save); tok && app.ha_count < MAX_HA;
         tok = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
        }
        snprintf(app.ha_entity[app.ha_count], sizeof(app.ha_entity[0]), "%s", tok);
        add_row(CHAKRA_LAMP_HA, eq ? eq + 1 : tok, ha, app.ha_count);
        app.ha_count++;
    }
    for (char *save = NULL, *tok = strtok_r(hue_lights, ",", &save); tok && app.hue_count < MAX_HUE;
         tok = strtok_r(NULL, ",", &save)) {
        app.hue_ids[app.hue_count++] = atoi(tok);
    }
    for (char *save = NULL, *tok = strtok_r(wiz, ",", &save); tok && app.wiz_count < MAX_WIZ;
         tok = strtok_r(NULL, ",", &save)) {
        snprintf(app.wiz_ip[app.wiz_count++], sizeof(app.wiz_ip[0]), "%s", tok);
    }
    char name[48];
    snprintf(name, sizeof(name), "Hue (%d)", app.hue_count);
    add_row(CHAKRA_LAMP_HUE, app.hue_count ? name : NULL, app.hue_host[0] && app.hue_key[0] && app.hue_count, -1);
    snprintf(name, sizeof(name), "WiZ (%d)", app.wiz_count);
    add_row(CHAKRA_LAMP_WIZ, app.wiz_count ? name : NULL, app.wiz_count > 0, -1);
    if (app.ha_count == 0) {
        add_row(CHAKRA_LAMP_HA, NULL, false, -1);
    }
    add_row(CHAKRA_LAMP_BLE, "Bluetooth (via Home Assistant)", false, -1);
}

static void load_settings(void)
{
    chakra_settings_defaults(&app.settings);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[sizeof(chakra_settings_t) + 1];
        size_t len = sizeof(blob);
        if (nvs_get_blob(h, "settings", blob, &len) == ESP_OK && len == sizeof(blob) && blob[0] == SETTINGS_VERSION) {
            memcpy(&app.settings, blob + 1, sizeof(chakra_settings_t));
        }
        nvs_close(h);
    }
    chakra_settings_detect(&app.settings, &app.cfg);
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t blob[sizeof(chakra_settings_t) + 1];
    blob[0] = SETTINGS_VERSION;
    memcpy(blob + 1, &app.settings, sizeof(chakra_settings_t));
    if (nvs_set_blob(h, "settings", blob, sizeof(blob)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// ------------------------------------------------------------------ microphone --

static void mic_task(void *arg)
{
    (void)arg;
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BSP_AUDIO_DEFAULT_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 2,
    };
    if (!mic || esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone unavailable; the app shows Ready only");
        vTaskDelete(NULL);
        return;
    }
    esp_codec_dev_set_in_gain(mic, 30.0f);
    static int16_t pcm[MIC_FRAMES * 2];
    static float mono[MIC_FRAMES / 3];
    chakra_reading_t r;
    for (;;) {
        if (esp_codec_dev_read(mic, pcm, sizeof(pcm)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (app.cfg_dirty) {
            app.cfg_dirty = false;
            chakra_detect_set_config(app.detect, &app.cfg);
        }
        /* Both microphones mixed, then every three frames averaged: 48 kHz to 16 kHz. Bowls and
         * voices sit well below the 8 kHz this keeps. */
        size_t n = 0;
        for (int i = 0; i + 2 < MIC_FRAMES; i += 3) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) {
                s += (float)pcm[2 * (i + k)] + (float)pcm[2 * (i + k) + 1];
            }
            mono[n++] = s / (6.0f * 32768.0f);
        }
        if (chakra_detect_feed(app.detect, mono, n, &r) > 0) {
            xQueueOverwrite(app.readings, &r);
        }
    }
}

// ------------------------------------------------------------------ core callbacks (UI context) --

static int poll_reading(chakra_reading_t *out, void *ctx)
{
    (void)ctx;
    static int last_minute = -2, last_conn = -1;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    const int minute = tm.tm_year > 120 ? tm.tm_hour * 60 + tm.tm_min : -1;
    if (minute != last_minute) {
        last_minute = minute;
        char hhmm[16];
        if (minute < 0) {
            snprintf(hhmm, sizeof(hhmm), "--:--");
        } else {
            snprintf(hhmm, sizeof(hhmm), "%02u:%02u", (unsigned)tm.tm_hour % 24u, (unsigned)tm.tm_min % 60u);
        }
        chakra_core_set_clock(app.core, hhmm);
    }
    const int conn = app.online ? 2 : 1;
    if (conn != last_conn) {
        last_conn = conn;
        chakra_core_set_connection(app.core, conn);
    }
    return xQueueReceive(app.readings, out, 0) == pdTRUE;
}

static void on_light(int chakra, uint32_t rgb, int brightness, void *ctx)
{
    (void)ctx;
    if (chakra < 0) {
        return;  // silence: the lamps keep their colour
    }
    const light_cmd_t cmd = {chakra, rgb, brightness, app.settings.reaction == CHAKRA_STRIKE ? 1 : 15};
    xQueueSend(app.lights, &cmd, 0);
}

static void on_settings(const chakra_settings_t *s, void *ctx)
{
    (void)ctx;
    app.settings = *s;
    chakra_settings_detect(s, &app.cfg);
    app.cfg_dirty = true;
    app.save_due = true;
}

static void on_haptic(int pattern, void *ctx)
{
    (void)ctx;
    bsp_motor_set_strength(pattern == 2 ? 100 : 60);
    bsp_motor_set(true);
    vTaskDelay(pdMS_TO_TICKS(pattern == 2 ? 120 : 30));
    bsp_motor_set(false);
}

// A one-shot task: the Vibe Mode boot intent goes to NVS, which the UI task must not wait for.
static void close_task(void *arg)
{
    (void)arg;
    const esp_err_t e = iris_ota_support_enter_recovery();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "back to Vibe Mode failed: %s", esp_err_to_name(e));
    }
    vTaskDelete(NULL);
}

/* The core's clock (hold-to-close, drawing budget): microseconds since boot, as milliseconds. */
static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_close(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "top bar held: back to Vibe Mode");
    xTaskCreate(close_task, "chakra_close", 4096, NULL, 5, NULL);
}

// ------------------------------------------------------------------ lamps --

static void ws_send_json(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    if (text && app.ws && app.authed) {
        esp_websocket_client_send_text(app.ws, text, (int)strlen(text), pdMS_TO_TICKS(1000));
    }
    free(text);
}

static void ha_light(const char *entity, const light_cmd_t *c)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", app.next_id++);
    cJSON_AddStringToObject(m, "type", "call_service");
    cJSON_AddStringToObject(m, "domain", "light");
    cJSON_AddStringToObject(m, "service", "turn_on");
    cJSON *d = cJSON_AddObjectToObject(m, "service_data");
    const int rgb[3] = {(int)(c->rgb >> 16) & 0xFF, (int)(c->rgb >> 8) & 0xFF, (int)c->rgb & 0xFF};
    cJSON_AddItemToObject(d, "rgb_color", cJSON_CreateIntArray(rgb, 3));
    cJSON_AddNumberToObject(d, "brightness_pct", c->brightness);
    cJSON_AddNumberToObject(d, "transition", c->transition_ds / 10.0);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(m, "target"), "entity_id", entity);
    ws_send_json(m);
    cJSON_Delete(m);
}

/* sRGB to CIE xy, as Philips documents it for its bulbs (wide gamut). */
static void rgb_to_xy(uint32_t rgb, float *x, float *y)
{
    float c[3] = {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f};
    for (int i = 0; i < 3; ++i) {
        c[i] = c[i] > 0.04045f ? powf((c[i] + 0.055f) / 1.055f, 2.4f) : c[i] / 12.92f;
    }
    const float X = c[0] * 0.664511f + c[1] * 0.154324f + c[2] * 0.162028f;
    const float Y = c[0] * 0.283881f + c[1] * 0.668433f + c[2] * 0.047685f;
    const float Z = c[0] * 0.000088f + c[1] * 0.072310f + c[2] * 0.986039f;
    const float sum = X + Y + Z;
    *x = sum > 0.0f ? X / sum : 0.3127f;
    *y = sum > 0.0f ? Y / sum : 0.3290f;
}

static void hue_light(int id, const light_cmd_t *c)
{
    float x, y;
    rgb_to_xy(c->rgb, &x, &y);
    char url[192], body[128];
    snprintf(url, sizeof(url), "http://%s/api/%s/lights/%d/state", app.hue_host, app.hue_key, id);
    snprintf(body, sizeof(body), "{\"on\":true,\"xy\":[%.4f,%.4f],\"bri\":%d,\"transitiontime\":%d}", (double)x,
             (double)y, c->brightness * 254 / 100, c->transition_ds);
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_PUT, .timeout_ms = 1500};
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, body, (int)strlen(body));
    if (esp_http_client_perform(h) != ESP_OK) {
        ESP_LOGW(TAG, "Hue light %d did not answer", id);
    }
    esp_http_client_cleanup(h);
}

static void wiz_light(int sock, const char *ip, const light_cmd_t *c)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"method\":\"setPilot\",\"params\":{\"r\":%d,\"g\":%d,\"b\":%d,\"dimming\":%d}}",
             (int)(c->rgb >> 16) & 0xFF, (int)(c->rgb >> 8) & 0xFF, (int)c->rgb & 0xFF, c->brightness);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(38899)};
    if (inet_pton(AF_INET, ip, &to.sin_addr) == 1) {
        sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void light_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    light_cmd_t c;
    for (;;) {
        if (xQueueReceive(app.lights, &c, pdMS_TO_TICKS(2000)) != pdTRUE) {
            if (app.save_due) {  // settings: at most one flash write every two seconds
                app.save_due = false;
                save_settings();
            }
            continue;
        }
        light_cmd_t newer;
        while (xQueueReceive(app.lights, &newer, 0) == pdTRUE) {
            c = newer;  // only the latest colour matters
        }
        for (int row = 0; row < app.row_count; ++row) {
            if (!app.rows[row].paired || !(app.settings.lamps & (1u << row))) {
                continue;
            }
            switch (app.rows[row].kind) {
            case CHAKRA_LAMP_HA:
                if (app.row_ha[row] >= 0) {
                    ha_light(app.ha_entity[app.row_ha[row]], &c);
                }
                break;
            case CHAKRA_LAMP_HUE:
                for (int i = 0; i < app.hue_count; ++i) {
                    hue_light(app.hue_ids[i], &c);
                }
                break;
            case CHAKRA_LAMP_WIZ:
                for (int i = 0; i < app.wiz_count && sock >= 0; ++i) {
                    wiz_light(sock, app.wiz_ip[i], &c);
                }
                break;
            default:
                break;
            }
        }
    }
}

// ------------------------------------------------------------------ Home Assistant --

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *e = event_data;
    if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        app.authed = false;
    } else if (id == WEBSOCKET_EVENT_DATA && e->op_code == 0x1 && e->data_len > 0 && e->payload_offset == 0 &&
               e->data_len == e->payload_len) {
        /* Only short control frames matter here: auth_required, auth_ok, auth_invalid. */
        cJSON *msg = cJSON_ParseWithLength(e->data_ptr, (size_t)e->data_len);
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
        if (type && strcmp(type, "auth_required") == 0) {
            cJSON *auth = cJSON_CreateObject();
            cJSON_AddStringToObject(auth, "type", "auth");
            cJSON_AddStringToObject(auth, "access_token", app.ha_token);
            char *text = cJSON_PrintUnformatted(auth);
            esp_websocket_client_send_text(app.ws, text, (int)strlen(text), pdMS_TO_TICKS(1000));
            free(text);
            cJSON_Delete(auth);
        } else if (type && strcmp(type, "auth_ok") == 0) {
            app.authed = true;
            ESP_LOGI(TAG, "Home Assistant: ready for %d light(s)", app.ha_count);
        } else if (type && strcmp(type, "auth_invalid") == 0) {
            ESP_LOGE(TAG, "Home Assistant rejected the token; no retries until reboot");
            esp_websocket_client_stop(app.ws);
        }
        cJSON_Delete(msg);
    }
}

// ------------------------------------------------------------------ Wi-Fi --

typedef struct {
    char ssid[33], pass[65];
} net_t;

/* Vibe Mode's network first, then the extras from NVS "chakra" (net0..net3 as "ssid\tpassword"). */
static int known_networks(net_t *nets)
{
    int n = 0;
    nvs_handle_t h;
    if (nvs_open_from_partition("sysmeta", "shared", NVS_READONLY, &h) == ESP_OK) {
        size_t a = sizeof(nets[0].ssid), b = sizeof(nets[0].pass);
        if (nvs_get_str(h, "wifi_ssid", nets[0].ssid, &a) == ESP_OK && nets[0].ssid[0]) {
            if (nvs_get_str(h, "wifi_password", nets[0].pass, &b) != ESP_OK) {
                nets[0].pass[0] = '\0';
            }
            n = 1;
        }
        nvs_close(h);
    }
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < MAX_NETS && n < MAX_NETS + 1; ++i) {
            char key[8], v[100];
            snprintf(key, sizeof(key), "net%d", i);
            nvs_str(h, key, v, sizeof(v));
            char *tab = strchr(v, '\t');
            if (v[0] && tab) {
                *tab = '\0';
                snprintf(nets[n].ssid, sizeof(nets[n].ssid), "%.32s", v);
                snprintf(nets[n].pass, sizeof(nets[n].pass), "%.64s", tab + 1);
                n++;
            }
        }
        nvs_close(h);
    }
    return n;
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        app.online = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        app.online = false;
        esp_wifi_connect();  // the same network again; the scan happens at boot
    }
}

static void wifi_and_lamps_start(void)
{
    static net_t nets[MAX_NETS + 1];
    const int count = known_networks(nets);
    if (count == 0) {
        ESP_LOGW(TAG, "No Wi-Fi known; the screen is the lamp");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop);
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, ip_event, NULL);
    /* The strongest network the app knows: home, the sound healing room, wherever it stands. */
    int best = 0, best_rssi = -1000;
    if (count > 1 && esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t found = 20;
        static wifi_ap_record_t aps[20];
        esp_wifi_scan_get_ap_records(&found, aps);
        for (int i = 0; i < found; ++i) {
            for (int k = 0; k < count; ++k) {
                if (strcmp((const char *)aps[i].ssid, nets[k].ssid) == 0 && aps[i].rssi > best_rssi) {
                    best_rssi = aps[i].rssi;
                    best = k;
                }
            }
        }
    }
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%.31s", nets[best].ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%.63s", nets[best].pass);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Wi-Fi: joining \"%s\" (%d known)", nets[best].ssid, count);
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    if (app.ha_url[0] && app.ha_token[0] && app.ha_count > 0) {
        /* wss:// is checked against the certificate bundle; ws:// sends the token in the clear,
         * acceptable only on a trusted LAN with a Home Assistant user that may switch lights only. */
        const bool tls = strncmp(app.ha_url, "wss://", 6) == 0;
        if (!tls) {
            ESP_LOGW(TAG, "Home Assistant over ws:// sends the token unencrypted; prefer wss://");
        }
        esp_websocket_client_config_t ws = {
            .uri = app.ha_url,
            .reconnect_timeout_ms = 10000,
            .network_timeout_ms = 10000,
            .buffer_size = 2048,
            .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
        };
        app.ws = esp_websocket_client_init(&ws);
        esp_websocket_register_events(app.ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        esp_websocket_client_start(app.ws);  // retries by itself until Wi-Fi is up
    }
}

// ------------------------------------------------------------------ main --

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &app.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(app.ui));

    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_motor_init());
    load_settings();
    load_lamps();
    app.readings = xQueueCreate(1, sizeof(chakra_reading_t));
    app.lights = xQueueCreate(8, sizeof(light_cmd_t));
    app.detect = chakra_detect_create(&app.cfg);
    if (!app.detect) {
        ESP_LOGE(TAG, "no memory for the detection");
        return;
    }
    const chakra_core_config_t core_cfg = {.poll = poll_reading, .on_light = on_light, .on_settings = on_settings,
                                           .on_close = on_close, .on_haptic = on_haptic, .now_ms = now_ms,
                                           .settings = app.settings};
    ESP_ERROR_CHECK(chakra_core_create(app.ui, &core_cfg, &app.core));
    chakra_core_set_lamps(app.core, app.rows, app.row_count);
    xTaskCreate(mic_task, "chakra_mic", 8192, NULL, 6, NULL);
    xTaskCreate(light_task, "chakra_light", 6144, NULL, 4, NULL);
    wifi_and_lamps_start();
    ESP_LOGI(TAG, "Chakra ready");
}
