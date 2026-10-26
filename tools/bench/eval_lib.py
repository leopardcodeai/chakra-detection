#!/usr/bin/env python3
"""The measuring side of the evaluation pipeline (pipeline.py): onsets, ground truth, scores, intervals.

What: plain functions, no file layout and no stages, so a test or a notebook can call them alone.
        decode / read_wav     any recording to 16 kHz mono, cached by path, size and mtime
        parse_timeline        the C detector's lines "start- end s  Chakra  Hz  note" (analyse_wav)
        flux_onsets           strike onsets from the spectral flux (log magnitude, rising part)
        tone_gain             how much a bowl's fundamental rose across an onset (new energy)
        align_order           the onsets that best fit a known order of bowls (dynamic programming)
        score_strikes         per strike: right chakra yes/no, the first new colour, latency
        light_metrics         changes a minute, short colours, flashes in 1 s and 10 s windows
        bootstrap_ci          percentile bootstrap over recordings (the unit that is independent)
        wilson_ci             binomial interval over strikes, for comparison
Why:  owner 26.10.2026: "bau alles mit Training und so als Pipelines und wiederverwendbar, und
      wissenschaftliche Auswertung für Blog und so". Numbers in the README came from one-off runs;
      these functions make every number come from one definition.
Definitions (the report states them too):
  * Onset: a peak of the half-wave rectified log-magnitude flux, 60 to 4000 Hz, 2048-point frames
    every 256 samples (16 ms), normalised by the recording's median flux; the frame centre plus
    ONSET_SHIFT_S. test_eval_lib.py checks it against synthetic strikes: within 10 ms.
  * A strike is right when the intended chakra is shown at some time in [onset - 0.1 s, end], end
    being the next strike or onset + window_s, whichever comes first. Latency is from the onset to
    the start of that colour; strikes whose chakra was already shown before the onset (the same bowl
    again) count for accuracy, not for latency.
  * Bootstrap: resample recordings with replacement (strikes inside one recording are not
    independent), statistic recomputed on the pooled strikes or the mean of recording values.
Pitfalls:
  * With 2 to 6 recordings a percentile bootstrap is coarse: when every recording scores 100 % the
    interval is [100, 100]. Wilson over strikes is given beside it; neither fixes a small n.
  * Detector times are printed to 0.1 s and a hop is 64 ms: latencies are good to about 0.1 s.
Usage:  imported by pipeline.py and eval_report.py; python3 tools/bench/test_eval_lib.py tests it.
"""
from __future__ import annotations

import hashlib
import json
import math
import re
import subprocess
import wave
from pathlib import Path

import numpy as np

RATE = 16000
CHAKRAS = ["Root", "Sacral", "Solar plexus", "Heart", "Throat", "Third eye", "Crown"]
KEYS = ["root", "sacral", "solar_plexus", "heart", "throat", "third_eye", "crown"]
SEG = re.compile(r"^\s*([\d.]+)-\s*([\d.]+) s\s+(\S.*?)\s{2,}\s*([\d.]+) Hz")
N_FFT, HOP = 2048, 256
# A flux peak sits 40 to 44 ms before the true onset (synthetic strikes, test_eval_lib.py): added back.
ONSET_SHIFT_S = 0.042


def chakra_index(name: str) -> int:
    """'Root', 'root', 'third_eye', 'Third eye' or 0..6 to 0..6."""
    if isinstance(name, int):
        return name
    k = name.strip().lower().replace(" ", "_")
    return KEYS.index(k)


# ------------------------------------------------------------------ files

def file_key(path: Path) -> str:
    st = path.stat()
    return hashlib.sha1(f"{path.resolve()}|{st.st_size}|{int(st.st_mtime)}".encode()).hexdigest()[:16]


def sha256_file(path: Path, cache: dict | None = None) -> str:
    """Full content hash, remembered in `cache` by path, size and mtime (the media are 5 GB)."""
    key = file_key(path)
    if cache is not None and key in cache:
        return cache[key]
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    if cache is not None:
        cache[key] = h.hexdigest()
    return h.hexdigest()


def decode(path: Path, cache_dir: Path) -> Path:
    """A 16 kHz mono 16-bit WAV of the recording, made once and kept in cache_dir."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    out = cache_dir / f"{file_key(path)}.wav"
    if not out.exists():
        tmp = out.with_suffix(".tmp.wav")
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(path), "-vn", "-ac", "1", "-ar",
                        str(RATE), "-c:a", "pcm_s16le", str(tmp)], check=True)
        tmp.replace(out)
    return out


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path)) as w:
        return np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768.0


# ------------------------------------------------------------------ the detector's output

def parse_timeline(text: str) -> list[dict]:
    """[{start, end, chakra (0..6), hz}] from analyse_wav's stdout."""
    segs = []
    for line in text.splitlines():
        m = SEG.match(line)
        if m:
            segs.append({"start": float(m.group(1)), "end": float(m.group(2)),
                         "chakra": CHAKRAS.index(m.group(3).strip()), "hz": float(m.group(4))})
    return segs


def shown_at(segs: list[dict], t: float) -> int:
    for s in segs:
        if s["start"] <= t < s["end"]:
            return s["chakra"]
    return -1


def sounding_mask(x: np.ndarray, step: float = 0.1) -> np.ndarray:
    """True per step where something plays: RMS above 3 % of the loudest step (gate_bench's rule)."""
    hop = int(RATE * step)
    n = max(1, len(x) // hop)
    rms = np.sqrt(np.mean(x[:n * hop].reshape(n, hop) ** 2, axis=1) + 1e-12)
    return rms > max(rms.max() * 0.03, 1e-4)


def lit_mask(segs: list[dict], n: int, step: float = 0.1) -> np.ndarray:
    on = np.zeros(n, dtype=bool)
    for s in segs:
        on[int(s["start"] / step):int(math.ceil(s["end"] / step))] = True
    return on


# ------------------------------------------------------------------ onsets and ground truth

def flux(x: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Times (frame centres), normalised flux, the frequencies and the power spectrogram."""
    n = 1 + max(0, (len(x) - N_FFT) // HOP)
    idx = np.arange(N_FFT)[None, :] + HOP * np.arange(n)[:, None]
    frames = x[idx] * np.hanning(N_FFT)[None, :]
    spec = np.abs(np.fft.rfft(frames, axis=1)).astype(np.float32)
    f = np.fft.rfftfreq(N_FFT, 1 / RATE)
    band = (f >= 60) & (f <= 4000)
    m = np.log1p(100 * spec[:, band])
    d = np.maximum(0, np.diff(m, axis=0)).sum(1)
    d = d / (np.median(d) + 1e-9)
    t = (np.arange(1, n) * HOP + N_FFT / 2) / RATE + ONSET_SHIFT_S
    return t, d, f, spec


def flux_onsets(x: np.ndarray, min_gap_s: float = 0.5, prominence: float = 3.0) -> list[dict]:
    """[{t, strength}] for every flux peak at least min_gap_s from a stronger one."""
    from scipy.signal import find_peaks
    t, d, _, _ = flux(x)
    pk, pr = find_peaks(d, distance=max(1, int(min_gap_s * RATE / HOP)), prominence=prominence)
    return [{"t": round(float(t[i]), 3), "strength": round(float(p), 2)} for i, p in zip(pk, pr["prominences"])]


def tone_gain(x: np.ndarray, t: float, hz: float, cents: float = 35.0) -> float:
    """Power that arrived at hz (+- cents) across t: [t+0.15, t+0.6] against [t-0.5, t-0.05],
    as a share of all power that arrived in 60 to 2000 Hz. 8192-point spectra (2 Hz bins)."""
    def power(a: float, b: float) -> np.ndarray:
        seg = x[max(0, int(a * RATE)):max(0, int(b * RATE))]
        if len(seg) < 256:
            return np.zeros(4097)
        seg = np.pad(seg, (0, max(0, 8192 - len(seg))))[:8192] * np.hanning(8192)
        return np.abs(np.fft.rfft(seg)) ** 2
    f = np.fft.rfftfreq(8192, 1 / RATE)
    new = np.maximum(0, power(t + 0.15, t + 0.6) - power(t - 0.5, t - 0.05))
    band = (f >= 60) & (f <= 2000)
    near = np.abs(1200 * np.log2(np.maximum(f, 1) / hz)) <= cents
    return float(new[near].sum() / (new[band].sum() + 1e-12))


def align_order(x: np.ndarray, onsets: list[dict], bowl_hz: list[float], order: list[int]) -> list[dict]:
    """The increasing choice of one onset per bowl in `order` that maximises the summed tone gain of
    each bowl at its onset, weighted by the onset's strength. Returns [{t, chakra, gain, strength}]."""
    m, k = len(onsets), len(order)
    if m < k:
        return []
    top = max(o["strength"] for o in onsets)
    # Gain times strength: a ringing bowl's later beats bring new energy at its pitch too, but weakly.
    score = np.array([[tone_gain(x, o["t"], bowl_hz[c]) * o["strength"] / top for o in onsets]
                      for c in order])
    best = np.full((k, m), -np.inf)
    back = np.zeros((k, m), dtype=int)
    best[0] = score[0]
    for i in range(1, k):
        run, arg = -np.inf, -1
        for j in range(m):
            if j - 1 >= 0 and best[i - 1, j - 1] > run:
                run, arg = best[i - 1, j - 1], j - 1
            if arg >= 0:
                best[i, j] = run + score[i, j]
                back[i, j] = arg
    j = int(np.argmax(best[k - 1]))
    picks = [j]
    for i in range(k - 1, 0, -1):
        j = back[i, j]
        picks.append(j)
    picks.reverse()
    return [{"t": onsets[j]["t"], "chakra": c, "gain": round(tone_gain(x, onsets[j]["t"], bowl_hz[c]), 3),
             "strength": onsets[j]["strength"]}
            for i, (j, c) in enumerate(zip(picks, order))]


def strongest_in(onsets: list[dict], a: float, b: float, count: int, min_gap_s: float) -> list[dict]:
    """The `count` strongest onsets in [a, b), each min_gap_s from a stronger pick, in time order."""
    cand = sorted((o for o in onsets if a <= o["t"] < b), key=lambda o: -o["strength"])
    picks: list[dict] = []
    for o in cand:
        if all(abs(o["t"] - p["t"]) >= min_gap_s for p in picks):
            picks.append(o)
        if len(picks) == count:
            break
    return sorted(picks, key=lambda o: o["t"])


# ------------------------------------------------------------------ scores

def score_strikes(segs: list[dict], strikes: list[dict], window_s: float, early_s: float = 0.1) -> list[dict]:
    """Per strike {t, chakra, right, first_right, already, latency_s (None if already or wrong)}."""
    out = []
    for i, s in enumerate(strikes):
        t, c = s["t"], s["chakra"]
        end = min(strikes[i + 1]["t"] if i + 1 < len(strikes) else math.inf, t + window_s)
        already = shown_at(segs, t - early_s) == c
        # Colours that start in the window (the first new colour after the strike).
        starts = [g for g in segs if t - early_s <= g["start"] < end]
        hit = next((g for g in starts if g["chakra"] == c), None)
        right = already or hit is not None
        first = starts[0]["chakra"] if starts else (shown_at(segs, t) if shown_at(segs, t) >= 0 else None)
        out.append({"t": t, "chakra": c, "right": bool(right), "already": bool(already),
                    "first_right": bool(already and not starts) or (first == c),
                    "latency_s": None if already or hit is None else round(max(0.0, hit["start"] - t), 3)})
    return out


def segment_share(segs: list[dict], a: float, b: float, chakra: int, step: float = 0.1) -> float:
    """Share of [a, b) during which `chakra` is shown."""
    ts = np.arange(a, b, step)
    return float(np.mean([shown_at(segs, t) == chakra for t in ts])) if len(ts) else float("nan")


def light_metrics(segs: list[dict], length: float, short_s: float = 3.0, flash_limit: float = 3.0) -> dict:
    """light_timeline.measure plus: share of colours under short_s, and the most changes in any 1 s."""
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    import light_timeline  # noqa: E402  (tools/light_timeline.py, imported, not edited)
    tl = [(s["start"], s["end"], CHAKRAS[s["chakra"]]) for s in segs]
    m = light_timeline.measure(tl, length)
    changes, prev_end, prev = [], None, None
    for a, b, name in tl:
        if not (prev == name and prev_end is not None and a - prev_end < 0.05):
            changes.append(a)
        if prev_end is not None and a - prev_end >= 0.05:
            changes.append(prev_end)
        prev_end, prev = b, name
    changes.sort()
    per_1s = max((sum(1 for u in changes if c <= u < c + 1.0) for c in changes), default=0)
    holds = [b - a for a, b, _ in tl]
    m.update({"colours": len(holds),
              "short_share": round(sum(h < short_s for h in holds) / len(holds), 3) if holds else None,
              "max_changes_1s": per_1s, "over_flash_limit": per_1s > flash_limit,
              "lit_share": round(sum(holds) / length, 3) if length else None})
    return m


# ------------------------------------------------------------------ intervals

def bootstrap_ci(groups: list, stat, n: int = 10000, seed: int = 26102026, alpha: float = 0.05):
    """Point value and percentile interval of stat(list of groups) over recordings drawn with
    replacement. Returns (value, low, high); (value, None, None) for fewer than 2 groups."""
    if not groups:
        return None, None, None
    value = stat(groups)
    if len(groups) < 2:
        return value, None, None
    rng = np.random.default_rng(seed)
    vals = []
    for _ in range(n):
        pick = [groups[i] for i in rng.integers(0, len(groups), len(groups))]
        v = stat(pick)
        if v is not None and not (isinstance(v, float) and math.isnan(v)):
            vals.append(v)
    if not vals:
        return value, None, None
    lo, hi = np.percentile(vals, [100 * alpha / 2, 100 * (1 - alpha / 2)])
    return value, float(lo), float(hi)


def wilson_ci(k: int, n: int, z: float = 1.96):
    if n == 0:
        return None, None
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, c - h), min(1.0, c + h)


def pooled_rate(key: str):
    """stat for bootstrap_ci: share of strikes with `key` true, pooled over the drawn recordings."""
    def stat(groups):
        xs = [s[key] for g in groups for s in g]
        return sum(xs) / len(xs) if xs else None
    return stat


def pooled_median_latency(groups):
    xs = [s["latency_s"] for g in groups for s in g if s["latency_s"] is not None]
    return float(np.median(xs)) if xs else None


def mean_of(groups):
    xs = [g for g in groups if g is not None]
    return float(np.mean(xs)) if xs else None


def median_of(groups):
    xs = [g for g in groups if g is not None]
    return float(np.median(xs)) if xs else None


def dumps(obj) -> str:
    return json.dumps(obj, indent=1, ensure_ascii=False, default=float)
