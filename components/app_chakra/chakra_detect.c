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

/* Strikes (spectral flux). A loud bowl that rings on wins the harmonic sum over every quieter bowl
 * struck after it (the root bowl, three times louder, held Root for 50 s over six later strikes,
 * 25.10.2026). So a strike is found on its own: the positive log-spectrum change summed over
 * 100 .. 1500 Hz jumps over FLUX_RATIO x its running median. The held spectrum from before the
 * strike (ONSET_BACK hops back, a whole window, so the strike is not in it) is taken off the spectra
 * after it, twice over (6 dB, for a bowl that still swells), and the harmonic sum of what is left
 * names the new tone. It counts where it rose ONSET_RISE_DB over what sounded there before, passes
 * the confidence and level gates, holds its pitch for the lock time, and the tone shown still rings
 * (over a ringing tone only; from silence and for voices the plain reading stays in charge). It
 * switches only where the plain reading cannot see it, and then holds the chakra while it rings. */
#define FLUX_K0 (100 * CHAKRA_FFT / CHAKRA_RATE)   // the flux sums 100 .. 1500 Hz
#define FLUX_K1 (1500 * CHAKRA_FFT / CHAKRA_RATE)
#define FLUX_FLOOR 1e-4f       // -80 dBFS: quieter bins count as this, so room noise makes no flux
#define FLUX_RATIO 3.0f
#define FLUX_MIN 60.0f         // and at least this (natural-log units, summed): a strike moves many bins
                               // at once (bowls 95 and up), a gong mode blooming only a few (42)
#define FLUX_HIST 64           // the median's span, 4 s
#define ONSET_BACK 4           // hops: the spectrum before the strike, one window earlier
#define ONSET_GAP_HOPS 8       // strikes at least 0.5 s apart
#define ONSET_WINDOW_HOPS 14   // the new tone must settle within 0.9 s of the strike
#define ONSET_RISE_GAIN 2.0f   // the spectrum before, doubled, comes off the one after
#define ONSET_RISE_DB 12.0f    // the new tone's fundamental this far over what sounded there before
#define RINGING_DROP_DB 10.0f  // the tone shown still rings: at most this far below where it was
#define ENV_DECAY_DB 0.5f      // the held spectrum falls 8 dB a second: it bridges a bowl's beats
#define ALIVE_DB 20.0f         // a struck tone holds its chakra until it fell this far from its peak
#define PARTIAL_LO 2.5f        // a bowl's second mode over its fundamental, 2.5 .. 3.1
#define PARTIAL_HI 3.1f
#define PARTIAL_DB 12.0f       // a fundamental this far under its partial still counts as the tone
#define ONSET_BINS 1152        // the bins the onset path keeps: up to 4.5 kHz, all harmonics searched

/* Bowl modes (bowl_modes). A bowl has no 2x and 4x partial: its modes grow roughly with n squared,
 * one measured bowl 1 : 2.96 : 5.41 (Wilkerson, Serafin, Ng, ICMC 2002), so the harmonic sum misses a
 * bowl whose second mode sits away from 3x and the harmonic share calls most of its energy "not the
 * tone". A candidate fundamental is also scored by the strongest bin in its second-mode window
 * (PARTIAL_LO .. PARTIAL_HI) and third-mode window (MODE3_LO .. MODE3_HI); the better of the two
 * models wins, so voices and forks stay harmonic. The confidence likewise takes the better share.
 * Only spectral lines count as modes (SHARP_UNDER): without that rule shakers' broad resonances read
 * as bowls (Commons shakers 0 to 19 % colour in Flow). Evaluation 26.10.2026: strikes right Flow
 * 58 to 61 of 70 with SuperFlux, bowls and bells colour more, gongs and voices 1 to 2 points more. */
#define MODE3_LO 5.0f
#define MODE3_HI 5.7f
#define BOWL_W2 0.5f           // the bowl template's weights, as the harmonic sum's on 2x and 3x
#define BOWL_W3 0.33f
#define SHARP_BINS 3           // a bowl's mode is a line: SHARP_BINS either side lie 15 dB under it
#define SHARP_UNDER 0.178f     // (10^(-15/20); a Hann line falls 30 dB there, shaker resonances and noise peaks do not)

/* SuperFlux strikes (superflux; Boeck, Widmer, DAFx 2013): each bin's log level is compared with the
 * largest of itself and its two neighbours in the hop before, so a partial that wobbles or beats into
 * the next bin (a bowl's doublets lie a bin or so apart) makes no flux, while a new tone and a mallet's
 * broadband attack do. The filter lowers the running median more than it lowers a strike, so the
 * ratio over the median rises to SF_FLUX_RATIO. Evaluation 26.10.2026 (tools/bench/pipeline.py, 70
 * strikes): the owner's hand-marked rounds 13 to 14 of 14 in Flow and Strike, nothing else worse;
 * ratio 7 lost three late strikes of a fast round. Log bands (24 an octave, as madmom does it) found
 * only 54 to 66 of the 70 onsets, at as many or more onsets elsewhere: they squeeze the attack's
 * broadband rise, so the bins stay linear. */
#define SF_FLUX_RATIO 5.0f

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
    uint16_t bands[CHAKRA_BARS + 1];  // the bands' bin edges, from a4_hz (band_edges)
    float bar[CHAKRA_BARS];
    int locked, cand;
    uint32_t cand_ms, since_seen_ms;
    float cand_hz;            // pitch at the start of the current candidate run
    int locked_bin;           // the locked tone's fundamental bin, -1 when unknown
    /* The onset path: the last spectra, the flux and its median, the strike being read. */
    float hist[ONSET_BACK][ONSET_BINS];  // the previous ONSET_BACK spectra, hist_pos the oldest
    int hist_pos, hist_n;
    float env[ONSET_BINS];    // the spectrum held at its peaks, falling ENV_DECAY_DB a hop: bowls beat
    float before[ONSET_BINS];
    float fresh[ONSET_BINS];  // after minus before: what the strike brought
    float prev_log[FLUX_K1 + 1];
    bool onset;               // a strike was found in this hop
    float flux_hist[FLUX_HIST], prev_flux;
    int flux_n, flux_pos;
    int since_onset, onset_left;  // hops since the last strike, hops left to read its new tone
    int run_chakra, run_hops, run_bin;
    float run_hz, switched_hz;  // the run's last pitch; the pitch this strike switched to, 0 = none yet
    bool holding;             // a struck tone holds the chakra while it rings (hold_bin, hold_peak)
    int hold_bin, hold_bin0;
    float hold_peak;
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
    cfg->bowl_modes = true;
    cfg->superflux = true;
}

const chakra_info_t *chakra_info(int chakra)
{
    return chakra >= 0 && chakra < CHAKRA_COUNT ? &INFO[chakra] : NULL;
}

const char *chakra_note_name(int note)
{
    return note >= 0 && note < 12 ? NOTES[note] : "";
}

/* The bands' bin edges: band b runs from 50 cents below its note to 50 cents above, and holds at
 * least one bin. Again when a4_hz changes. */
static void band_edges(chakra_detect_t *d)
{
    const float a4 = d->cfg.a4_hz > 0.0f ? d->cfg.a4_hz : 440.0f;
    for (int b = 0; b <= CHAKRA_BARS; ++b) {
        const float f = a4 * powf(2.0f, ((float)(CHAKRA_BAR_MIDI0 + b) - 69.5f) / 12.0f);
        d->bands[b] = (uint16_t)ceilf(f / BIN_HZ);
    }
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
    band_edges(d);
    d->locked = d->cand = d->locked_bin = d->run_chakra = -1;
    d->since_onset = ONSET_GAP_HOPS;
    d->last.chakra = d->last.candidate = -1;
    d->last.level_db = -120.0f;
    return d;
}

void chakra_detect_set_config(chakra_detect_t *d, const chakra_detect_config_t *cfg)
{
    if (d && cfg) {
        const bool retune = cfg->a4_hz != d->cfg.a4_hz;
        d->cfg = *cfg;
        if (retune) {
            band_edges(d);
        }
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

static float band_energy(const float *m, int limit, int centre, int half)
{
    float e = 0.0f;
    for (int k = centre - half; k <= centre + half; ++k) {
        if (k > 0 && k < limit) {
            e += m[k] * m[k];
        }
    }
    return e;
}

/* The strongest bin of m (bins 0 .. limit-1) from lo x k to hi x k, -1 when the window lies past
 * limit. Two bins short of limit, so the energy around it is inside. */
static int window_max(const float *m, int limit, int k, float lo, float hi)
{
    const int a = (int)ceilf(lo * (float)k);
    int b = (int)(hi * (float)k);
    b = b < limit - 3 ? b : limit - 3;
    int best = -1;
    for (int i = a; i <= b; ++i) {
        best = best < 0 || m[i] > m[best] ? i : best;
    }
    return best;
}

/* True when bin x of m is a spectral line: SHARP_BINS to either side lie 15 dB under it. */
static bool sharp(const float *m, int limit, int x)
{
    const float under = m[x] * SHARP_UNDER;
    return x > SHARP_BINS && x + SHARP_BINS < limit && m[x - SHARP_BINS] <= under && m[x + SHARP_BINS] <= under;
}

/* A bowl's modes over candidate fundamental k: the strongest bin of each mode window, -1 where that
 * bin is no line (or the window lies past limit). False when k itself is no line. */
static bool find_modes(const float *m, int limit, int k, int *p2, int *p3)
{
    if (!sharp(m, limit, k)) {
        return false;
    }
    *p2 = window_max(m, limit, k, PARTIAL_LO, PARTIAL_HI);
    *p3 = window_max(m, limit, k, MODE3_LO, MODE3_HI);
    *p2 = *p2 > 0 && sharp(m, limit, *p2) ? *p2 : -1;
    *p3 = *p3 > 0 && sharp(m, limit, *p3) ? *p3 : -1;
    return true;
}

/* The bowl template's score of candidate k: its fundamental and its strongest second and third mode. */
static float bowl_score(const float *m, int limit, int k)
{
    int p2, p3;
    if (!find_modes(m, limit, k, &p2, &p3)) {
        return 0.0f;
    }
    return m[k] + (p2 > 0 ? BOWL_W2 * m[p2] : 0.0f) + (p3 > 0 ? BOWL_W3 * m[p3] : 0.0f);
}

/* The fundamental of spectrum m (bins 0 .. limit-1): the bin whose harmonics (or, with bowl_modes,
 * whose bowl modes) weigh most, -1 if none. The bowl template scans two windows a candidate, so it
 * runs second and only where it could still win: m[k] plus the template's weights times the
 * spectrum's largest bin must beat the best so far (on a clear tone a few candidates are left). */
static int harmonic_peak(const chakra_detect_t *d, const float *m, int limit)
{
    const int kmin = (int)(F_MIN / BIN_HZ), kmax = (int)(F_MAX / BIN_HZ);
    static const float W[HARMONICS] = {1.0f, 0.5f, 0.33f, 0.25f};
    int best = -1;
    float best_score = 0.0f;
    for (int k = kmin; k <= kmax; ++k) {
        float s = 0.0f;
        for (int h = 0; h < HARMONICS && (h + 1) * k < limit; ++h) {
            s += W[h] * m[(h + 1) * k];
        }
        if (s > best_score) {
            best_score = s;
            best = k;
        }
    }
    if (d->cfg.bowl_modes) {
        float top = 0.0f;
        for (int i = kmin; i < limit; ++i) {
            top = m[i] > top ? m[i] : top;
        }
        for (int k = kmin; k <= kmax; ++k) {
            if (m[k] + (BOWL_W2 + BOWL_W3) * top > best_score && m[k] >= m[k - 1] && m[k] >= m[k + 1]) {
                const float s = bowl_score(m, limit, k);
                if (s > best_score) {
                    best_score = s;
                    best = k;
                }
            }
        }
    }
    return best > 1 ? best : -1;
}

/* Parabolic interpolation on log magnitudes around bin k, in Hz. */
static float peak_hz(const float *m, int k)
{
    const float a = logf(m[k - 1] + 1e-9f), b = logf(m[k] + 1e-9f), c = logf(m[k + 1] + 1e-9f);
    const float den = a - 2.0f * b + c;
    const float delta = fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
    return ((float)k + (delta > 0.5f ? 0.5f : delta < -0.5f ? -0.5f : delta)) * BIN_HZ;
}

/* Share of the 50 Hz .. 4 kHz energy of m that sits on the harmonics of bin k; with bowl_modes the
 * better of that and the share on k and its strongest second and third mode. */
static float tone_confidence(const chakra_detect_t *d, const float *m, int limit, int k)
{
    float harm = 0.0f;
    for (int h = 1; h <= HARMONICS && h * k + 2 < limit; ++h) {
        harm += band_energy(m, limit, h * k, 2);
    }
    int p2, p3;
    if (d->cfg.bowl_modes && find_modes(m, limit, k, &p2, &p3)) {
        const float modes = band_energy(m, limit, k, 2) + (p2 > 0 ? band_energy(m, limit, p2, 2) : 0.0f) +
                            (p3 > 0 ? band_energy(m, limit, p3, 2) : 0.0f);
        harm = modes > harm ? modes : harm;
    }
    float total = 0.0f;
    for (int i = (int)(50.0f / BIN_HZ); i < (int)(4000.0f / BIN_HZ) && i < limit; ++i) {
        total += m[i] * m[i];
    }
    return total > 0.0f ? harm / total : 0.0f;
}

/* The strongest of bins k-1 .. k+1: a tone's level, whichever bin its pitch sits in. */
static float near_max(const float *m, int k)
{
    float v = m[k];
    v = m[k - 1] > v ? m[k - 1] : v;
    return m[k + 1] > v ? m[k + 1] : v;
}

/* True when f is 2.4 .. 3.2 times ref: a bowl's second mode (or third harmonic) of the tone at ref. */
static bool is_partial(float f, float ref)
{
    return ref > 0.0f && f >= ref * 2.4f && f <= ref * 3.2f;
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
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

#define MAG_NORM (2.0f / (CHAKRA_FFT * 0.5f))  // a bin's magnitude to the amplitude of its tone (Hann 0.5)

/* The magnitude of a bin whose tone sits at db dBFS. */
static float level_mag(float db)
{
    return powf(10.0f, db / 20.0f) / MAG_NORM;
}

/* True when a strike may hand the chakra on: a chakra is shown and its tone still rings, no more
 * than RINGING_DROP_DB under where it was before the strike. From silence, and from a shown tone
 * that stopped (a voice moving to its next note), the plain reading and its calm times stay in
 * charge: that keeps voices, drums and gong washes as they were. */
static bool ringing(const chakra_detect_t *d)
{
    if (d->locked < 0 || d->locked_bin < 1) {
        return false;
    }
    const float now = near_max(d->mag, d->locked_bin), was = near_max(d->before, d->locked_bin);
    return now >= level_mag(d->cfg.threshold_db) && now >= was * powf(10.0f, -RINGING_DROP_DB / 20.0f);
}

/* The onset path, once a hop after the FFT: finds strikes by the spectral flux and reads the tone
 * each one brought. Returns the chakra to switch to (its bin in *bin), or -1. Keeps the spectrum. */
static int strike(chakra_detect_t *d, int plain, int *bin)
{
    /* The held spectrum: what sounded, a bowl's beat troughs bridged (the spectrum before a strike
     * is taken from it, so a trough there does not make the old bowl look newly struck). */
    const float decay = powf(10.0f, -ENV_DECAY_DB / 20.0f);
    for (int k = 0; k < ONSET_BINS; ++k) {
        const float held = d->env[k] * decay;
        d->env[k] = d->mag[k] > held ? d->mag[k] : held;
    }
    /* Positive flux of the log spectrum, 100 .. 1500 Hz, against the hop before: bin by bin, or
     * (SuperFlux) against the largest of the bin and its two neighbours before. */
    float flux = 0.0f, left = 0.0f;  // left: bin k-1 of the hop before, already overwritten
    for (int k = FLUX_K0; k <= FLUX_K1; ++k) {
        const float l = logf(d->mag[k] * MAG_NORM + FLUX_FLOOR);
        float ref = d->prev_log[k];
        if (d->cfg.superflux) {
            ref = k > FLUX_K0 && left > ref ? left : ref;
            ref = k < FLUX_K1 && d->prev_log[k + 1] > ref ? d->prev_log[k + 1] : ref;
        }
        if (d->hist_n > 0 && l > ref) {
            flux += l - ref;
        }
        left = d->prev_log[k];
        d->prev_log[k] = l;
    }
    float median = 0.0f;
    if (d->flux_n > 0) {
        float sorted[FLUX_HIST];
        memcpy(sorted, d->flux_hist, sizeof(float) * (size_t)d->flux_n);
        qsort(sorted, (size_t)d->flux_n, sizeof(float), cmp_float);
        median = sorted[d->flux_n / 2];
    }
    if (d->hist_n > 0) {
        d->flux_hist[d->flux_pos] = flux;
        d->flux_pos = (d->flux_pos + 1) % FLUX_HIST;
        d->flux_n += d->flux_n < FLUX_HIST;
    }
    d->since_onset += d->since_onset < ONSET_GAP_HOPS;
    d->onset = false;
    if (d->hist_n >= ONSET_BACK && d->flux_n >= 8 && flux > (d->cfg.superflux ? SF_FLUX_RATIO : FLUX_RATIO) * median &&
        flux > FLUX_MIN && flux > d->prev_flux && d->since_onset >= ONSET_GAP_HOPS) {
        /* A strike: the spectrum one window back did not hear it yet. */
        memcpy(d->before, d->hist[d->hist_pos], sizeof(d->before));
        d->onset_left = ONSET_WINDOW_HOPS;
        d->since_onset = 0;
        d->onset = true;
        d->switched_hz = 0.0f;
        d->run_chakra = -1;
        d->run_hops = 0;
    }
    d->prev_flux = flux;

    int out = -1;
    if (d->onset_left > 0) {
        d->onset_left--;
        for (int k = 0; k < ONSET_BINS; ++k) {
            const float v = d->mag[k] - ONSET_RISE_GAIN * d->before[k];
            d->fresh[k] = v > 0.0f ? v : 0.0f;
        }
        int chakra = -1;
        float f = 0.0f;
        int k = harmonic_peak(d, d->fresh, ONSET_BINS);
        const float rise = powf(10.0f, ONSET_RISE_DB / 20.0f);
        if (k > 0) {
            /* A bowl's second mode (2.5 to 3.1 times the fundamental, not a harmonic) often sounds
             * first and loudest; when its fundamental rose with it, the fundamental is the tone. */
            const int kmin = (int)(F_MIN / BIN_HZ);
            int j = (int)((float)k / PARTIAL_HI);
            j = j < kmin ? kmin : j;
            int low = -1;
            for (; (float)j <= (float)k / PARTIAL_LO; ++j) {
                low = low < 0 || d->fresh[j] > d->fresh[low] ? j : low;
            }
            if (low > 1 && d->fresh[low] >= d->fresh[k] * powf(10.0f, -PARTIAL_DB / 20.0f) &&
                near_max(d->mag, low) >= near_max(d->before, low) * rise) {
                k = low;
            }
        }
        if (k > 0) {
            const float now = near_max(d->mag, k), was = near_max(d->before, k);
            if (now >= level_mag(d->cfg.threshold_db) && now >= was * rise &&
                tone_confidence(d, d->fresh, ONSET_BINS, k) >= d->cfg.min_confidence) {
                chakra_reading_t scratch;
                f = peak_hz(d->mag, k);
                chakra = map_freq(d, f, &scratch);
                /* The second mode of a tone already in play is no new tone: of the one this strike
                 * brought, or of the bowl shown when that one was struck again (it rose too). */
                const float shown_hz = d->locked_bin > 0 ? (float)d->locked_bin * BIN_HZ : 0.0f;
                const bool shown_rose = d->locked_bin > 0 &&
                                        near_max(d->mag, d->locked_bin) >= near_max(d->before, d->locked_bin) * 1.41f;
                if (is_partial(f, d->run_chakra >= 0 ? d->run_hz : 0.0f) || is_partial(f, d->switched_hz) ||
                    (shown_rose && is_partial(f, shown_hz))) {
                    chakra = -1;
                }
                if (chakra >= 0 && keeps_locked(d, f, chakra)) {
                    chakra = d->locked;
                }
                chakra = chakra == d->locked ? -1 : chakra;  // the same chakra struck again: nothing to do
                /* After this strike switched once, only a lower tone corrects it (a fundamental that
                 * came after its partial), never the struck bowl's own upper partial. */
                chakra = d->switched_hz > 0.0f && f > d->switched_hz * 0.75f ? -1 : chakra;
            }
        }
        /* The new tone must hold its pitch for the lock time (Strike 128 ms, Flow 384): a bowl's upper
         * partial often leads its fundamental by a few hops, and a later steady run corrects it. */
        if (chakra >= 0 && chakra == d->run_chakra && fabsf(1200.0f * log2f(f / d->run_hz)) <= STEADY_CENTS) {
            d->run_hops++;
            d->run_bin = k;
            d->run_hz = f;  // against the hop before: a swelling bowl's pitch settles over a few hops
        } else if (chakra >= 0 || d->run_hops < 2) {
            /* Another tone restarts the run; a hop without a clear new tone pauses a run of two
             * hops or more (a swelling bowl dips) and ends a single one (a passing partial). */
            d->run_chakra = chakra;
            d->run_hops = chakra >= 0;
            d->run_hz = f;
            d->run_bin = k;
        }
        uint32_t confirm = d->cfg.lock_ms / HOP_MS;
        confirm = confirm < 2u ? 2u : confirm;
        /* Only where the plain reading cannot follow: a new tone it has read steadily for as long
         * switches at its own calm pace (a gong's next note, a melody); one it cannot see, or sees
         * only now and then under the ringing one, switches here. */
        const bool plain_follows = plain == d->run_chakra && d->cand == plain && 2u * (d->cand_ms + HOP_MS) >= confirm * HOP_MS;
        if (d->run_hops >= (int)confirm && !plain_follows && ringing(d)) {
            out = d->run_chakra;
            *bin = d->run_bin;
            d->switched_hz = d->run_hz;
            d->run_chakra = -1;
            d->run_hops = 0;
        }
    }

    memcpy(d->hist[d->hist_pos], d->env, sizeof(d->hist[0]));
    d->hist_pos = (d->hist_pos + 1) % ONSET_BACK;
    d->hist_n += d->hist_n < ONSET_BACK;
    return out;
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

    /* Bars: the strongest bin of each band in dB, -90..-25 dBFS onto 0..100, rising at once and
     * falling 5 a hop. The strongest bin, not the band's sum: near C3 a band holds two bins and the
     * Hann window spreads a tone over three, so a sum lit the neighbouring semitone as high. */
    const float norm = MAG_NORM;  // Hann gain 0.5, one-sided spectrum
    for (int b = 0; b < CHAKRA_BARS; ++b) {
        float m = 0.0f;
        for (int k = d->bands[b]; k < d->bands[b + 1] || k == d->bands[b]; ++k) {
            m = d->mag[k] > m ? d->mag[k] : m;
        }
        const float db = 20.0f * log10f(m * norm + 1e-6f);
        float v = (db + 90.0f) * (100.0f / 65.0f);
        v = v < 0.0f ? 0.0f : v > 100.0f ? 100.0f : v;
        d->bar[b] = v > d->bar[b] ? v : (d->bar[b] > 5.0f ? d->bar[b] - 5.0f : 0.0f);
        r->bars[b] = (uint8_t)d->bar[b];
    }

    /* Fundamental: the bin whose harmonics weigh most. */
    const int best = harmonic_peak(d, d->mag, CHAKRA_FFT / 2);
    int candidate = -1, tone_bin = best;  // tone_bin: the fundamental this hop's reading stands for
    if (best > 0) {
        const float f = peak_hz(d->mag, best);
        /* Confidence: share of the 50 Hz .. 4 kHz energy that sits on the tone's harmonics. */
        r->confidence = tone_confidence(d, d->mag, CHAKRA_FFT / 2, best);
        r->tone = r->level_db >= d->cfg.threshold_db && r->confidence >= d->cfg.min_confidence;
        if (r->tone) {
            r->freq_hz = f;
            candidate = map_freq(d, f, r);
            if (keeps_locked(d, f, candidate)) {
                candidate = d->locked;
            } else if (candidate >= 0 && d->locked_bin > 0 && is_partial(f, (float)d->locked_bin * BIN_HZ) &&
                       near_max(d->mag, d->locked_bin) >= near_max(d->mag, best) * powf(10.0f, -PARTIAL_DB / 20.0f)) {
                /* The shown bowl's second mode, louder for a moment as the bowl swells or beats, while
                 * the bowl itself still sounds: still that bowl. */
                candidate = d->locked;
                tone_bin = d->locked_bin;
                r->freq_hz = peak_hz(d->mag, d->locked_bin);
                map_freq(d, r->freq_hz, r);
            }
        }
    } else {
        r->confidence = 0.0f;
        r->tone = false;
    }
    r->candidate = candidate;
    int strike_bin = -1;
    const int struck = strike(d, candidate, &strike_bin);
    r->onset = d->onset;

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
    /* A strike's new tone takes the chakra at once and holds it while it rings, whatever louder
     * bowl still covers it; only the next strike, or its fading, hands the chakra on. */
    if (struck >= 0) {
        d->locked = struck;
        d->since_seen_ms = 0;
        d->holding = true;
        d->hold_bin = d->hold_bin0 = d->locked_bin = strike_bin;
        d->hold_peak = near_max(d->mag, strike_bin);
        d->cand = -1;
        d->cand_ms = 0;
        d->cand_hz = 0.0f;
    } else if (d->holding) {
        int k = d->hold_bin0;  // the struck tone's bin, or a neighbour as its pitch drifts, never further
        k = d->mag[k - 1] > d->mag[k] && d->mag[k - 1] >= d->mag[k + 1] ? k - 1 : d->mag[k + 1] > d->mag[k] ? k + 1 : k;
        d->hold_bin = d->locked_bin = k;
        const float now = near_max(d->env, k);  // held: a beat trough does not end it
        d->hold_peak = now > d->hold_peak ? now : d->hold_peak;
        d->holding = now >= level_mag(d->cfg.threshold_db) && now >= d->hold_peak * powf(10.0f, -ALIVE_DB / 20.0f);
    }
    if (d->holding) {
        if (candidate != d->locked) {  // report the struck tone, not the one that covers it
            r->freq_hz = peak_hz(d->mag, d->hold_bin);
            map_freq(d, r->freq_hz, r);
            r->candidate = d->locked;
        }
        /* The struck tone itself, or a steady one that covers it (the louder bowl): it stays. No
         * clear or no steady tone (a gong wash, a drum roll): the hold time runs as without a strike. */
        if (candidate == d->locked || (candidate >= 0 && d->cand == candidate && d->cand_ms >= d->cfg.lock_ms)) {
            d->since_seen_ms = 0;
        } else {
            d->since_seen_ms += HOP_MS;
            if (d->since_seen_ms >= d->cfg.hold_ms) {
                d->locked = d->locked_bin = r->candidate = -1;
                d->holding = false;
            }
        }
    } else if (candidate >= 0 && candidate != d->locked && d->cand_ms >= (d->locked < 0 ? d->cfg.lock_ms : d->cfg.switch_ms)) {
        d->locked = candidate;
        d->locked_bin = tone_bin;
        d->since_seen_ms = 0;
    } else if (d->locked >= 0 && candidate == d->locked) {
        d->locked_bin = tone_bin;
        d->since_seen_ms = 0;
    } else if (d->locked >= 0) {
        d->since_seen_ms += HOP_MS;
        if (d->since_seen_ms >= d->cfg.hold_ms) {
            d->locked = d->locked_bin = -1;
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
