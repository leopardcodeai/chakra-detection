// SPDX-License-Identifier: Apache-2.0
// Simulator side of the chakra app: audio arrives over loopback TCP from tools/chakra_feed.py
// (the Mac's microphone through ffmpeg, a test tone, a demo round or a WAV file), the same
// chakra_detect.c as on the board runs on it, and every light command goes back as a line, so
// the feeder can set a real lamp in Home Assistant.
//
// Protocol: in, raw signed 16-bit little-endian mono samples at 16 kHz; out, one line per
// change: light\t<chakra -1..6>\t<RRGGBB>\t<brightness %>\t<transition s>\n, close\n for the hold gesture.
// Port: CHAKRA_FEED_PORT or 8698, 127.0.0.1 only, one client at a time. All socket work is
// non-blocking and runs in the core's 40 ms tick on the bridge's single thread.
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "chakra_core.h"
#include "gsp_sim_bridge.h"

#define CHUNK 4096

static struct {
    chakra_core_handle_t core;
    chakra_detect_t *detect;
    int listener, client;
    uint8_t carry;     // an odd byte left from the last read
    bool has_carry;
    int16_t pcm[CHUNK];
    float samples[CHUNK];
    unsigned long hops;
    uint8_t lamps;     // ticked rows, from the last on_settings
    int reaction;
} host = {.listener = -1, .client = -1};

static void nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static int listen_loopback(void)
{
    const char *env = getenv("CHAKRA_FEED_PORT");
    const int port = env ? atoi(env) : 8698;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        fprintf(stderr, "chakra: cannot listen on 127.0.0.1:%d (%s)\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    nonblock(fd);
    fprintf(stderr, "chakra: audio on 127.0.0.1:%d (s16le mono 16 kHz)\n", port);
    return fd;
}

static void send_text(const char *line)
{
    if (host.client >= 0) {
        (void)!write(host.client, line, strlen(line));
    }
}

static void on_light(int chakra, uint32_t rgb, int brightness, void *ctx)
{
    (void)ctx;
    if (!(host.lamps & 0x01)) {
        return;  // row 0, the feeder's Home Assistant lamp, is not ticked
    }
    char line[64];
    /* Lamps fade like the screen: Flow softly, Strike at once. */
    snprintf(line, sizeof(line), "light\t%d\t%06X\t%d\t%.1f\n", chakra, (unsigned)rgb, brightness,
             host.reaction == CHAKRA_STRIKE ? 0.1 : 1.5);
    fprintf(stderr, "chakra: %s", line);
    send_text(line);
}

static void on_settings(const chakra_settings_t *s, void *ctx)
{
    (void)ctx;
    chakra_detect_config_t cfg;
    chakra_settings_detect(s, &cfg);
    chakra_detect_set_config(host.detect, &cfg);
    host.lamps = s->lamps;
    host.reaction = s->reaction;
    fprintf(stderr, "chakra: settings lamps=0x%02x bright=%d gate=%.0f dB hold=%u ms map=%d lang=%d\n", s->lamps,
            s->brightness, (double)cfg.threshold_db, (unsigned)cfg.hold_ms, (int)cfg.map, s->lang);
    if (s->map == CHAKRA_MAP_BOWLS) {
        fprintf(stderr, "chakra: bowls %.1f %.1f %.1f %.1f %.1f %.1f %.1f Hz\n", (double)s->bowl_hz[0],
                (double)s->bowl_hz[1], (double)s->bowl_hz[2], (double)s->bowl_hz[3], (double)s->bowl_hz[4],
                (double)s->bowl_hz[5], (double)s->bowl_hz[6]);
    }
}

static void on_close(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "chakra: top bar held 3 s, the board would go back to Vibe Mode now\n");
    send_text("close\n");
}

static int poll_audio(chakra_reading_t *out, void *ctx)
{
    (void)ctx;
    static int last_minute = -1, last_conn = -1;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_min != last_minute) {
        last_minute = tm.tm_min;
        char hhmm[16];
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
        chakra_core_set_clock(host.core, hhmm);
    }
    const int conn = host.client >= 0 ? 3 : 1;
    if (conn != last_conn) {
        last_conn = conn;
        chakra_core_set_connection(host.core, conn);
    }
    if (host.listener >= 0) {
        int fd = accept(host.listener, NULL, NULL);
        if (fd >= 0) {
            if (host.client >= 0) {
                close(host.client);
            }
            nonblock(fd);
            host.client = fd;
            host.has_carry = false;
        }
    }
    if (host.client < 0) {
        return 0;
    }
    uint8_t *bytes = (uint8_t *)host.pcm;
    size_t have = 0;
    if (host.has_carry) {
        bytes[have++] = host.carry;
        host.has_carry = false;
    }
    ssize_t r = read(host.client, bytes + have, sizeof(host.pcm) - have);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(host.client);
        host.client = -1;
        return 0;
    }
    if (r > 0) {
        have += (size_t)r;
    }
    if (have & 1) {
        host.carry = bytes[--have];
        host.has_carry = true;
    }
    const size_t n = have / 2;
    for (size_t i = 0; i < n; ++i) {
        host.samples[i] = (float)host.pcm[i] / 32768.0f;
    }
    const int hops = n ? chakra_detect_feed(host.detect, host.samples, n, out) : 0;
    host.hops += (unsigned long)hops;
    /* CHAKRA_LOG=1: one reading a second on stderr, for tests against recordings. */
    static const char *log;
    static unsigned long logged;
    if (!log) {
        log = getenv("CHAKRA_LOG") ? "1" : "";
    }
    if (*log && hops && host.hops / 16 != logged) {
        logged = host.hops / 16;
        fprintf(stderr, "reading chakra=%d cand=%d f=%.1f note=%s%d cents=%+d conf=%.2f level=%.0f\n", out->chakra,
                out->candidate, (double)out->freq_hz, chakra_note_name(out->note), out->octave, out->cents,
                (double)out->confidence, (double)out->level_db);
    }
    return hops > 0;
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    host.listener = listen_loopback();
    chakra_core_config_t cfg = {.poll = poll_audio, .on_light = on_light, .on_settings = on_settings,
                                .on_close = on_close};
    chakra_settings_defaults(&cfg.settings);
    host.lamps = cfg.settings.lamps;
    chakra_detect_config_t dc;
    chakra_settings_detect(&cfg.settings, &dc);
    host.detect = chakra_detect_create(&dc);
    if (!host.detect) {
        return ESP_GSP_ERR_NO_MEM;
    }
    const esp_gsp_err_t e = chakra_core_create(ui, &cfg, &host.core);
    if (e == ESP_GSP_OK) {
        /* Row 0 is the lamp the feeder sets in Home Assistant (--light); the others show what the
         * board can drive once paired outside the app. */
        const char *name = getenv("CHAKRA_LAMP_NAME");
        chakra_lamp_t lamps[CHAKRA_LAMPS] = {
            {.kind = CHAKRA_LAMP_HA, .paired = true},
            {.kind = CHAKRA_LAMP_HUE},
            {.kind = CHAKRA_LAMP_WIZ},
            {.kind = CHAKRA_LAMP_BLE},
        };
        snprintf(lamps[0].name, sizeof(lamps[0].name), "%s", name && *name ? name : "Living room LED strip");
        chakra_core_set_lamps(host.core, lamps, CHAKRA_LAMPS);
    }
    return e;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    fprintf(stderr, "chakra: %lu hops analysed\n", host.hops);
    chakra_core_destroy(&host.core);
    chakra_detect_destroy(host.detect);
    if (host.client >= 0) {
        close(host.client);
    }
    if (host.listener >= 0) {
        close(host.listener);
    }
}
