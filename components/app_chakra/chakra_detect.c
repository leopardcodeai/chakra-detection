// SPDX-License-Identifier: Apache-2.0
// Chakra detection, see include/chakra_detect.h. Lessons from the Box-3 generations
// (archived 24.10.2026): their 512-point FFT gave 31 Hz bins, two semitones at 250 Hz; their hold
// counted frames of a length the screen got wrong; nothing said how sure a reading was.
#include "chakra_detect.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BIN_HZ ((float)CHAKRA_RATE / CHAKRA_FFT)
#define F_MIN 90.0f  // below: room hum and fans (56 Hz read as a chakra on 24.10.2026), no bowl or voice
#define F_MAX 1100.0f
#define HARMONICS 4
#define LOCK_MS 256u    // from silence: this long the same reading locks a chakra
#define SWITCH_MS 640u  // from another chakra: a steadier tone (owner: "nicht so viel rumspringen"; 24.10.2026 recordings)
#define KEEP_SEMITONES 0.6f   // a tone this close to a note of the locked chakra keeps it (0.75 kept D -32 ct on C#)
#define KEEP_CENTS 30.0f       // Solfeggio and bowls: how much nearer another centre must be to switch
#define STEADY_CENTS 35.0f     // a tone must hold its pitch this closely while it locks (drums, gongs do not)
#define BOWL_CENTS 150.0f      // bowls: farther than this from every learned bowl is no bowl
#define HOP_MS ((uint32_t)(1000u * CHAKRA_HOP / CHAKRA_RATE))

static const chakra_info_t INFO[CHAKRA_COUNT] = {
    {"Root", "Wurzel", "Muladhara", 0xE0262B, 396},
    {"Sacral", "Sakral", "Svadhisthana", 0xF47A1F, 417},
    {"Solar plexus", "Solarplexus", "Manipura", 0xF2B705, 528},
    {"Heart", "Herz", "Anahata", 0x2FA84F, 639},
    {"Throat", "Hals", "Vishuddha", 0x1E88E5, 741},
    {"Third eye", "Stirn", "Ajna", 0x4B3FB5, 852},
    {"Crown", "Krone", "Sahasrara", 0x9B3FC8, 963},
};

/* C C# D D# E F F# G G# A A# B, as in all Box-3 generations. */
static const int8_t NOTE_TO_CHAKRA[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 6, 6};
static const char *const NOTES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

struct chakra_detect {
    chakra_detect_config_t cfg;
    float ring[CHAKRA_FFT];   // the last FFT samples, oldest first after `pos`
    size_t pos, fill, pending;
    float window[CHAKRA_FFT];
    float re[CHAKRA_FFT], im[CHAKRA_FFT];
    float cosv[CHAKRA_FFT / 2], sinv[CHAKRA_FFT / 2];
    float mag[CHAKRA_FFT / 2];
    uint16_t bands[CHAKRA_BARS + 1];
    float bar[CHAKRA_BARS];
    int locked, cand;
    uint32_t cand_ms, since_seen_ms;
    float cand_hz;            // pitch at the start of the current candidate run
    chakra_reading_t last;
};

void chakra_detect_defaults(chakra_detect_config_t *cfg)
{
    cfg->threshold_db = -55.0f;
    cfg->min_confidence = 0.45f;
    cfg->hold_ms = 1500;
    cfg->map = CHAKRA_MAP_NOTES;
    cfg->a4_hz = 440.0f;
    cfg->lock_ms = LOCK_MS;
    cfg->switch_ms = SWITCH_MS;
}

const chakra_info_t *chakra_info(int chakra)
{
    return chakra >= 0 && chakra < CHAKRA_COUNT ? &INFO[chakra] : NULL;
}

const char *chakra_note_name(int note)
{
    return note >= 0 && note < 12 ? NOTES[note] : "";
}

chakra_detect_t *chakra_detect_create(const chakra_detect_config_t *cfg)
{
    chakra_detect_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    if (cfg) {
        d->cfg = *cfg;
    } else {
        chakra_detect_defaults(&d->cfg);
    }
    for (int i = 0; i < CHAKRA_FFT; ++i) {
        d->window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (CHAKRA_FFT - 1));
    }
    for (int i = 0; i < CHAKRA_FFT / 2; ++i) {
        d->cosv[i] = cosf(2.0f * (float)M_PI * (float)i / CHAKRA_FFT);
        d->sinv[i] = -sinf(2.0f * (float)M_PI * (float)i / CHAKRA_FFT);
    }
    for (int b = 0; b <= CHAKRA_BARS; ++b) {  // 60 Hz .. 4 kHz, equal steps on a log scale
        const float f = 60.0f * powf(4000.0f / 60.0f, (float)b / CHAKRA_BARS);
        d->bands[b] = (uint16_t)(f / BIN_HZ + 0.5f);
    }
    d->locked = d->cand = -1;
    d->last.chakra = d->last.candidate = -1;
    d->last.level_db = -120.0f;
    return d;
}

void chakra_detect_set_config(chakra_detect_t *d, const chakra_detect_config_t *cfg)
{
    if (d && cfg) {
        d->cfg = *cfg;
    }
}

void chakra_detect_destroy(chakra_detect_t *d)
{
    free(d);
}

/* In-place iterative radix-2 FFT over re/im. */
static void fft(chakra_detect_t *d)
{
    const int n = CHAKRA_FFT;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = d->re[i];
            d->re[i] = d->re[j];
            d->re[j] = t;
            t = d->im[i];
            d->im[i] = d->im[j];
            d->im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; ++k) {
                const float wr = d->cosv[k * step], wi = d->sinv[k * step];
                const int a = i + k, b = i + k + len / 2;
                const float xr = d->re[b] * wr - d->im[b] * wi;
                const float xi = d->re[b] * wi + d->im[b] * wr;
                d->re[b] = d->re[a] - xr;
                d->im[b] = d->im[a] - xi;
                d->re[a] += xr;
                d->im[a] += xi;
            }
        }
    }
}

static float band_energy(const chakra_detect_t *d, int centre, int half)
{
    float e = 0.0f;
    for (int k = centre - half; k <= centre + half; ++k) {
        if (k > 0 && k < CHAKRA_FFT / 2) {
            e += d->mag[k] * d->mag[k];
        }
    }
    return e;
}

/* Cents from f to the nearest octave of c, in -600..600. */
static float octave_cents(float f, float c)
{
    float x = 1200.0f * log2f(f / c);
    x = fmodf(x, 1200.0f);
    if (x < -600.0f) {
        x += 1200.0f;
    } else if (x >= 600.0f) {
        x -= 1200.0f;
    }
    return x;
}

static int map_freq(const chakra_detect_t *d, float f, chakra_reading_t *r)
{
    const float midi = 69.0f + 12.0f * log2f(f / d->cfg.a4_hz);
    const int nearest = (int)lrintf(midi);
    r->note = ((nearest % 12) + 12) % 12;
    r->octave = nearest / 12 - 1;
    r->cents = (int)lrintf((midi - (float)nearest) * 100.0f);
    if (d->cfg.map == CHAKRA_MAP_NOTES) {
        return NOTE_TO_CHAKRA[r->note];
    }
    /* Solfeggio spans more than an octave, so compare against every octave of every centre.
     * Bowls compare straight, no octaves: a bowl is its own sound, partials and all. */
    int best = -1;
    float best_abs = 1e9f;
    for (int c = 0; c < CHAKRA_COUNT; ++c) {
        float a;
        if (d->cfg.map == CHAKRA_MAP_BOWLS) {
            if (d->cfg.bowl_hz[c] <= 0.0f) {
                continue;
            }
            a = fabsf(1200.0f * log2f(f / d->cfg.bowl_hz[c]));
        } else {
            a = fabsf(octave_cents(f, (float)INFO[c].solfeggio_hz));
        }
        if (a < best_abs) {
            best_abs = a;
            best = c;
        }
    }
    /* A sound far from every learned bowl is not one of them. */
    return d->cfg.map == CHAKRA_MAP_BOWLS && best_abs > BOWL_CENTS ? -1 : best;
}

/* Hysteresis: true when f still belongs to the locked chakra, so a bowl that sits near a note
 * boundary (210 Hz is A3 -47 ct at A = 432) does not flicker between two chakras. */
static bool keeps_locked(const chakra_detect_t *d, float f, int best)
{
    const int locked = d->locked;
    if (locked < 0 || best == locked) {
        return best == locked;
    }
    if (d->cfg.map == CHAKRA_MAP_NOTES) {
        const float midi = 69.0f + 12.0f * log2f(f / d->cfg.a4_hz);
        for (int n = (int)floorf(midi) - 1; n <= (int)ceilf(midi) + 1; ++n) {
            if (NOTE_TO_CHAKRA[((n % 12) + 12) % 12] == locked && fabsf(midi - (float)n) <= KEEP_SEMITONES) {
                return true;
            }
        }
        return false;
    }
    if (d->cfg.map == CHAKRA_MAP_BOWLS) {
        if (d->cfg.bowl_hz[locked] <= 0.0f) {
            return false;
        }
        const float to_locked = fabsf(1200.0f * log2f(f / d->cfg.bowl_hz[locked]));
        const float to_best = best >= 0 ? fabsf(1200.0f * log2f(f / d->cfg.bowl_hz[best])) : BOWL_CENTS;
        return to_locked <= BOWL_CENTS && to_locked <= to_best + KEEP_CENTS;
    }
    if (best < 0) {
        return false;
    }
    const float to_locked = fabsf(octave_cents(f, (float)INFO[locked].solfeggio_hz));
    const float to_best = fabsf(octave_cents(f, (float)INFO[best].solfeggio_hz));
    return to_locked <= to_best + KEEP_CENTS;
}

static void analyse(chakra_detect_t *d)
{
    chakra_reading_t *r = &d->last;
    /* Level of the newest hop, then the windowed frame, oldest sample first. */
    double sq = 0.0;
    for (int i = 0; i < CHAKRA_HOP; ++i) {
        const float s = d->ring[(d->pos + CHAKRA_FFT - CHAKRA_HOP + (size_t)i) % CHAKRA_FFT];
        sq += (double)s * s;
    }
    const float rms = sqrtf((float)(sq / CHAKRA_HOP));
    r->level_db = rms > 1e-6f ? 20.0f * log10f(rms) : -120.0f;
    for (int i = 0; i < CHAKRA_FFT; ++i) {
        d->re[i] = d->ring[(d->pos + (size_t)i) % CHAKRA_FFT] * d->window[i];
        d->im[i] = 0.0f;
    }
    fft(d);
    for (int k = 0; k < CHAKRA_FFT / 2; ++k) {
        d->mag[k] = sqrtf(d->re[k] * d->re[k] + d->im[k] * d->im[k]);
    }

    /* Bars: dB of each band, -90..-25 dBFS onto 0..100, rising at once and falling 5 a hop. */
    const float norm = 2.0f / (CHAKRA_FFT * 0.5f);  // Hann gain 0.5, one-sided spectrum
    for (int b = 0; b < CHAKRA_BARS; ++b) {
        float e = 0.0f;
        for (int k = d->bands[b]; k < d->bands[b + 1] || k == d->bands[b]; ++k) {
            e += d->mag[k] * d->mag[k];
        }
        const float db = 10.0f * log10f(e * norm * norm + 1e-12f);
        float v = (db + 90.0f) * (100.0f / 65.0f);
        v = v < 0.0f ? 0.0f : v > 100.0f ? 100.0f : v;
        d->bar[b] = v > d->bar[b] ? v : (d->bar[b] > 5.0f ? d->bar[b] - 5.0f : 0.0f);
        r->bars[b] = (uint8_t)d->bar[b];
    }

    /* Fundamental: the bin whose harmonics weigh most. */
    const int kmin = (int)(F_MIN / BIN_HZ), kmax = (int)(F_MAX / BIN_HZ);
    static const float W[HARMONICS] = {1.0f, 0.5f, 0.33f, 0.25f};
    int best = -1;
    float best_score = 0.0f;
    for (int k = kmin; k <= kmax; ++k) {
        float s = 0.0f;
        for (int h = 0; h < HARMONICS && (h + 1) * k < CHAKRA_FFT / 2; ++h) {
            s += W[h] * d->mag[(h + 1) * k];
        }
        if (s > best_score) {
            best_score = s;
            best = k;
        }
    }
    int candidate = -1;
    if (best > 1) {
        /* Parabolic interpolation on log magnitudes. */
        const float a = logf(d->mag[best - 1] + 1e-9f), b = logf(d->mag[best] + 1e-9f);
        const float c = logf(d->mag[best + 1] + 1e-9f);
        const float den = a - 2.0f * b + c;
        const float delta = fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
        const float f = ((float)best + (delta > 0.5f ? 0.5f : delta < -0.5f ? -0.5f : delta)) * BIN_HZ;
        /* Confidence: share of the 50 Hz .. 4 kHz energy that sits on the tone's harmonics. */
        float harm = 0.0f;
        for (int h = 1; h <= HARMONICS && h * best + 2 < CHAKRA_FFT / 2; ++h) {
            harm += band_energy(d, h * best, 2);
        }
        float total = 0.0f;
        for (int k = (int)(50.0f / BIN_HZ); k < (int)(4000.0f / BIN_HZ); ++k) {
            total += d->mag[k] * d->mag[k];
        }
        r->confidence = total > 0.0f ? harm / total : 0.0f;
        r->tone = r->level_db >= d->cfg.threshold_db && r->confidence >= d->cfg.min_confidence;
        if (r->tone) {
            r->freq_hz = f;
            candidate = map_freq(d, f, r);
            if (keeps_locked(d, f, candidate)) {
                candidate = d->locked;
            }
        }
    } else {
        r->confidence = 0.0f;
        r->tone = false;
    }
    r->candidate = candidate;

    /* Lock after LOCK_MS of the same reading, release after hold_ms without the locked one. */
    /* A run counts only while the pitch stays put: percussion and noise wander, bowls hold. */
    const bool steady = candidate < 0 || d->cand_hz <= 0.0f ||
                        fabsf(1200.0f * log2f(r->freq_hz / d->cand_hz)) <= STEADY_CENTS;
    if (candidate == d->cand && steady) {
        d->cand_ms += HOP_MS;
    } else {
        d->cand = candidate;
        d->cand_ms = HOP_MS;
        d->cand_hz = candidate >= 0 ? r->freq_hz : 0.0f;
    }
    if (candidate >= 0 && candidate != d->locked && d->cand_ms >= (d->locked < 0 ? d->cfg.lock_ms : d->cfg.switch_ms)) {
        d->locked = candidate;
        d->since_seen_ms = 0;
    } else if (d->locked >= 0 && candidate == d->locked) {
        d->since_seen_ms = 0;
    } else if (d->locked >= 0) {
        d->since_seen_ms += HOP_MS;
        if (d->since_seen_ms >= d->cfg.hold_ms) {
            d->locked = -1;
        }
    }
    r->chakra = d->locked;
}

int chakra_detect_feed(chakra_detect_t *d, const float *samples, size_t n, chakra_reading_t *out)
{
    int hops = 0;
    for (size_t i = 0; i < n; ++i) {
        d->ring[d->pos] = samples[i];
        d->pos = (d->pos + 1) % CHAKRA_FFT;
        if (d->fill < CHAKRA_FFT) {
            d->fill++;
        }
        if (++d->pending >= CHAKRA_HOP) {
            d->pending = 0;
            if (d->fill >= CHAKRA_FFT) {
                analyse(d);
                hops++;
            }
        }
    }
    if (out) {
        *out = d->last;
    }
    return hops;
}
