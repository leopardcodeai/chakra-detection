// SPDX-License-Identifier: Apache-2.0
// Runs chakra_detect.c over a recording and prints when which chakra was locked.
//
// Usage: analyse_wav FILE.wav [notes|solfeggio|bowls F0,F1,...,F6] [--a4 440]
// The file must be 16 kHz mono signed 16-bit PCM (tests/analyse.sh converts anything with
// ffmpeg). Output: one line per locked stretch, "start-end  chakra  median Hz  note", then a
// summary of the share of time each chakra was shown.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chakra_detect.h"

static int cmp(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void report(int run, double start, double end, float *freqs, int nfreq, float a4)
{
    if (run < 0 || nfreq == 0) {
        return;
    }
    qsort(freqs, (size_t)nfreq, sizeof(float), cmp);
    const float med = freqs[nfreq / 2];
    const int note = ((int)lrintf(69.0f + 12.0f * log2f(med / a4)) % 12 + 12) % 12;
    printf("%6.1f-%6.1f s  %-12s %7.1f Hz  %s\n", start, end, chakra_info(run)->english, (double)med,
           chakra_note_name(note));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.wav [notes|solfeggio|bowls F0,...,F6] [--a4 HZ]\n", argv[0]);
        return 2;
    }
    chakra_detect_config_t cfg;
    chakra_detect_defaults(&cfg);
    cfg.threshold_db = -70.0f;  // sensitivity 8, the app's default
    cfg.lock_ms = 384;          // Flow, the app's default reaction (chakra_core.c)
    cfg.switch_ms = 1500;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "solfeggio") == 0) {
            cfg.map = CHAKRA_MAP_SOLFEGGIO;
        } else if (strcmp(argv[i], "bowls") == 0 && i + 1 < argc) {
            cfg.map = CHAKRA_MAP_BOWLS;
            char *s = argv[++i];
            for (int c = 0; c < CHAKRA_COUNT && s; ++c) {
                cfg.bowl_hz[c] = strtof(s, &s);
                s = *s == ',' ? s + 1 : NULL;
            }
        } else if (strcmp(argv[i], "--lock") == 0 && i + 2 < argc) {  // lock and switch in ms
            cfg.lock_ms = (uint32_t)atoi(argv[++i]);
            cfg.switch_ms = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--a4") == 0 && i + 1 < argc) {
            cfg.a4_hz = strtof(argv[++i], NULL);
        }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 44, SEEK_SET);  // canonical WAV header, as ffmpeg writes it
    chakra_detect_t *d = chakra_detect_create(&cfg);
    static int16_t pcm[CHAKRA_HOP];
    static float x[CHAKRA_HOP];
    static float freqs[100000];
    unsigned long hop = 0, shown[CHAKRA_COUNT + 1] = {0};
    int run = -2, nfreq = 0;
    bool was_tone = false;
    double onset = -1.0, lat_sum = 0.0;
    int lat_n = 0;
    double run_start = 0.0;
    size_t n;
    chakra_reading_t r;
    while ((n = fread(pcm, 2, CHAKRA_HOP, f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            x[i] = (float)pcm[i] / 32768.0f;
        }
        if (chakra_detect_feed(d, x, n, &r) == 0) {
            continue;
        }
        const double t = (double)(++hop) * CHAKRA_HOP / CHAKRA_RATE;
        shown[r.chakra + 1]++;
        /* Latency: from the hop where a tone rose out of silence to the hop that locked it. */
        if (r.tone && !was_tone) {
            onset = t - (double)CHAKRA_HOP / CHAKRA_RATE;
        }
        was_tone = r.tone;
        if (r.chakra != run && r.chakra >= 0 && run < 0 && onset >= 0.0) {
            printf("  lock %.0f ms after onset at %.2f s\n", 1000.0 * (t - onset), onset);
            lat_sum += t - onset;
            lat_n++;
        }
        if (r.chakra != run) {
            report(run, run_start, t, freqs, nfreq, cfg.a4_hz);
            run = r.chakra;
            run_start = t;
            nfreq = 0;
        }
        if (r.chakra >= 0 && r.candidate == r.chakra && nfreq < 100000) {
            freqs[nfreq++] = r.freq_hz;
        }
    }
    fclose(f);
    report(run, run_start, (double)hop * CHAKRA_HOP / CHAKRA_RATE, freqs, nfreq, cfg.a4_hz);
    printf("shown:");
    for (int c = -1; c < CHAKRA_COUNT; ++c) {
        if (shown[c + 1]) {
            printf("  %s %.0f%%", c < 0 ? "none" : chakra_info(c)->english, 100.0 * shown[c + 1] / (double)hop);
        }
    }
    printf("\n");
    if (lat_n) {
        printf("latency from silence: mean %.0f ms over %d locks\n", 1000.0 * lat_sum / lat_n, lat_n);
    }
    chakra_detect_destroy(d);
    return 0;
}
