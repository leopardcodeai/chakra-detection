// SPDX-License-Identifier: Apache-2.0
// Simulator side of the chakra app: audio arrives over loopback TCP from tools/chakra_feed.py
// (the Mac's microphone through ffmpeg, a test tone, a demo round or a WAV file), the same
// chakra_detect.c as on the board runs on it, and every light command goes back as a line, so
// the feeder can colour the screen lamp page (or, with --light, a real lamp in Home Assistant).
//
// Protocol: in, raw signed 16-bit little-endian mono samples at 16 kHz; out, one line per
// change: light\t<chakra -1..6>\t<RRGGBB>\t<brightness %>\t<transition s>\n, close\n for the hold gesture,
// and while a chakra is locked, at most twice a second when it changes, the reading for a lamp page:
// reading\t<chakra>\t<english>\t<german>\t<sanskrit>\t<note+octave>\t<Hz>\t<cents>\t<solfeggio Hz>\t<en|de>\n
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
    int lang;          // CHAKRA_LANG_EN or CHAKRA_LANG_DE, for the reading line
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
    if (!(host.lamps & 0x03)) {
        return;  // neither the screen lamp (row 0) nor the feeder's Home Assistant lamp (row 1) is ticked
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
    host.lang = s->lang;
    fprintf(stderr, "chakra: settings lamps=0x%02x bright=%d gate=%.0f dB hold=%u ms map=%d lang=%d\n", s->lamps,
            s->brightness, (double)cfg.threshold_db, (unsigned)cfg.hold_ms, (int)cfg.map, s->lang);
    if (s->map == CHAKRA_MAP_BOWLS) {
        fprintf(stderr, "chakra: bowls %.1f %.1f %.1f %.1f %.1f %.1f %.1f Hz\n", (double)s->bowl_hz[0],
                (double)s->bowl_hz[1], (double)s->bowl_hz[2], (double)s->bowl_hz[3], (double)s->bowl_hz[4],
                (double)s->bowl_hz[5], (double)s->bowl_hz[6]);
    }
}

/* The core's clock (hold-to-close, drawing budget): the bridge's monotonic milliseconds. */
static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)gsp_sim_bridge_time_ms();
}

static void on_close(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "chakra: top bar held 3 s, the board would go back to Vibe Mode now\n");
    send_text("close\n");
}

/* The locked chakra's reading for a lamp page (names, note, frequency, cents), only on a change
 * and at most every 500 ms; nothing while silent or when the feeder's lamp row is not ticked. */
static void send_reading(const chakra_reading_t *r)
{
    static int last_chakra = -2, last_cents = 999;
    static char last_note[8];
    static uint32_t last_ms;
    if (!(host.lamps & 0x01) || r->chakra < 0 || r->chakra >= CHAKRA_COUNT) {  // row 0: the screen lamp
        last_chakra = -2;
        return;
    }
    char note[8];
    snprintf(note, sizeof(note), "%s%d", chakra_note_name(r->note), r->octave);
    const uint32_t t = (uint32_t)gsp_sim_bridge_time_ms();
    const bool changed = r->chakra != last_chakra || r->cents != last_cents || strcmp(note, last_note) != 0;
    if (!changed || (r->chakra == last_chakra && t - last_ms < 500)) {
        return;
    }
    last_chakra = r->chakra;
    last_cents = r->cents;
    snprintf(last_note, sizeof(last_note), "%s", note);
    last_ms = t;
    const chakra_info_t *i = chakra_info(r->chakra);
    char line[192];
    snprintf(line, sizeof(line), "reading\t%d\t%s\t%s\t%s\t%s\t%.1f\t%+d\t%u\t%s\n", r->chakra, i->english, i->german,
             i->sanskrit, note, (double)r->freq_hz, r->cents, (unsigned)i->solfeggio_hz,
             host.lang == CHAKRA_LANG_DE ? "de" : "en");
    send_text(line);
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
    if (hops) {
        send_reading(out);
    }
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
                                .on_close = on_close, .now_ms = now_ms};
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
        /* Row 0 is the screen lamp the feeder forwards to (--lamp; ticked by default), its address
         * from CHAKRA_LAMP_URL (the launcher sets it, the LAN address with --lamp-lan). Row 1 is the
         * lamp the feeder sets in Home Assistant, paired only when sim.sh got CHAKRA_LIGHT. The
         * others show what the board can drive once paired outside the app. */
        const char *name = getenv("CHAKRA_LAMP_NAME"), *url = getenv("CHAKRA_LAMP_URL"), *ha = getenv("CHAKRA_LIGHT");
        chakra_lamp_t lamps[CHAKRA_LAMPS] = {
            {.kind = CHAKRA_LAMP_SCREEN, .paired = true},
            {.kind = CHAKRA_LAMP_HA, .paired = ha && *ha},
            {.kind = CHAKRA_LAMP_HUE},
            {.kind = CHAKRA_LAMP_WIZ},
        };
        if (url && strncmp(url, "http://", 7) == 0) {
            url += 7;  // typed without it, browsers add it
        }
        snprintf(lamps[0].url, sizeof(lamps[0].url), "%s", url && *url ? url : "127.0.0.1:3310");
        size_t n = strlen(lamps[0].url);
        if (n && lamps[0].url[n - 1] == '/') {
            lamps[0].url[n - 1] = '\0';
        }
        snprintf(lamps[1].name, sizeof(lamps[1].name), "%s", name && *name ? name : "Living room LED strip");
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
