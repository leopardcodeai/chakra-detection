// SPDX-License-Identifier: Apache-2.0
// Chakra detection: mono 16 kHz samples in, the chakra of the tone that sounds out.
//
// Pure C, no GSP and no ESP-IDF, so the board, the simulator and the host tests run the same
// code. A 4096-point FFT every 1024 samples (64 ms, 3.9 Hz per bin), a weighted harmonic sum
// picks the fundamental between 90 and 1100 Hz, parabolic interpolation refines it. The tone
// maps to a chakra by its note (A = 440 Hz by default, 432 selectable, octave-independent, as
// the Box-3 generations did), by the nearest Solfeggio frequency, or by the user's own learned bowls. A
// chakra locks after 256 ms of one reading (640 ms to switch from another, with hysteresis at
// note boundaries) and is released after `hold_ms` without it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHAKRA_RATE 16000
#define CHAKRA_FFT 4096
#define CHAKRA_HOP 1024
#define CHAKRA_COUNT 7
#define CHAKRA_BARS 16

typedef enum {
    CHAKRA_MAP_NOTES = 0,      // note class of the tone, A = a4_hz
    CHAKRA_MAP_SOLFEGGIO = 1,  // nearest of 396, 417, 528, 639, 741, 852, 963 Hz (by octaves)
    CHAKRA_MAP_BOWLS = 2,      // nearest of the user's own bowls, learned once (bowl_hz)
} chakra_map_t;

typedef struct {
    const char *english;   // "Heart", the screen's default
    const char *german;    // "Herz"
    const char *sanskrit;  // "Anahata"
    uint32_t rgb;          // 0xRRGGBB, screen and lamps
    uint16_t solfeggio_hz;
} chakra_info_t;

typedef struct {
    float threshold_db;  // quieter frames count as silence; -55 dBFS by default
    float min_confidence;  // 0..1, share of the energy in the tone's harmonics; 0.45 by default
    uint32_t hold_ms;    // how long a chakra stays after its tone stopped; 1500 by default
    chakra_map_t map;
    float a4_hz;         // 440 by default (most bowls are sold by their note at A = 440)
    float bowl_hz[CHAKRA_COUNT];  // CHAKRA_MAP_BOWLS: each chakra's bowl as the app heard it; 0 = none
    uint32_t lock_ms;    // from silence: this long one steady reading locks a chakra (256)
    uint32_t switch_ms;  // from another chakra (640); Flow uses longer, Strike shorter
} chakra_detect_config_t;

typedef struct {
    int chakra;        // the locked chakra 0..6, or -1 in silence
    int candidate;     // this hop's reading, -1 when gated
    bool tone;         // the hop passed level and confidence, whatever the mapping (for learning)
    float freq_hz;     // fundamental of the last gated-in hop
    int note;          // 0 = C .. 11 = B, of freq_hz
    int octave;        // scientific pitch octave
    int cents;         // deviation from the note, -50..50
    float level_db;    // dBFS of the hop
    float confidence;  // 0..1
    uint8_t bars[CHAKRA_BARS];  // 0..100, log bands 60 Hz .. 4 kHz, falling slowly
} chakra_reading_t;

typedef struct chakra_detect chakra_detect_t;

/** Defaults for the fields above. */
void chakra_detect_defaults(chakra_detect_config_t *cfg);

/** NULL on allocation failure (about 120 KB). */
chakra_detect_t *chakra_detect_create(const chakra_detect_config_t *cfg);
void chakra_detect_set_config(chakra_detect_t *d, const chakra_detect_config_t *cfg);
void chakra_detect_destroy(chakra_detect_t *d);

/** Feeds samples in [-1, 1]; returns how many hops were analysed, `out` holds the last one. */
int chakra_detect_feed(chakra_detect_t *d, const float *samples, size_t n, chakra_reading_t *out);

const chakra_info_t *chakra_info(int chakra);
const char *chakra_note_name(int note);

#ifdef __cplusplus
}
#endif
