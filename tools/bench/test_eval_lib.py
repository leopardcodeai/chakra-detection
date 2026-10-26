#!/usr/bin/env python3
"""Tests for eval_lib.py: onset timing on synthetic strikes, the order alignment, strike scoring, intervals.

What: plain asserts, no data files, a few seconds. Run before trusting a changed definition.
Why:  the report's latencies are only as good as the onset times; a 40 ms bias of the flux peak was
      found this way on 26.10.2026 and is corrected by eval_lib.ONSET_SHIFT_S.
Usage:  AI_VENV/bin/python tools/bench/test_eval_lib.py
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import eval_lib as E  # noqa: E402

RNG = np.random.default_rng(7)


def strike(x: np.ndarray, t0: float, f0: float, amp: float = 0.3) -> None:
    tt = np.arange(len(x) - int(t0 * E.RATE)) / E.RATE
    x[int(t0 * E.RATE):] += amp * np.exp(-tt) * (np.sin(2 * np.pi * f0 * tt) + 0.4 * np.sin(2 * np.pi * 2.7 * f0 * tt))


def test_onset_timing():
    for t0 in (1.0, 1.37, 2.011, 3.5):
        for f0 in (130.0, 250.0, 600.0):
            x = (1e-4 * RNG.standard_normal(E.RATE * 6)).astype(np.float32)
            strike(x, t0, f0)
            o = E.flux_onsets(x)
            assert o and abs(o[0]["t"] - t0) <= 0.01, (t0, f0, o[:1])


def test_align_order_finds_the_bowls():
    hz = [132.8, 148.0, 162.8, 176.5, 197.8, 218.0, 247.0]
    x = (1e-4 * RNG.standard_normal(E.RATE * 14)).astype(np.float32)
    times = [1.0, 2.2, 3.4, 4.6, 5.8, 7.0, 8.2]
    for t, f in zip(times, hz):
        strike(x, t, f, 0.15)
    strike(x, 10.5, 132.8, 0.05)  # a soft extra strike of the root bowl, out of order
    on = E.flux_onsets(x, 0.4, 1.0)
    al = E.align_order(x, on, hz, list(range(7)))
    assert [a["chakra"] for a in al] == list(range(7))
    assert all(abs(a["t"] - t) < 0.02 for a, t in zip(al, times)), [a["t"] for a in al]


def test_score_strikes():
    segs = [{"start": 1.4, "end": 2.5, "chakra": 0, "hz": 131}, {"start": 2.5, "end": 3.0, "chakra": 4, "hz": 196},
            {"start": 3.0, "end": 5.0, "chakra": 1, "hz": 147}]
    strikes = [{"t": 1.0, "chakra": 0}, {"t": 2.2, "chakra": 1}, {"t": 4.0, "chakra": 1}, {"t": 6.0, "chakra": 2}]
    s = E.score_strikes(segs, strikes, window_s=3.0)
    assert s[0]["right"] and s[0]["first_right"] and abs(s[0]["latency_s"] - 0.4) < 1e-9
    assert s[1]["right"] and not s[1]["first_right"] and abs(s[1]["latency_s"] - 0.8) < 1e-9  # Throat first
    assert s[2]["right"] and s[2]["already"] and s[2]["latency_s"] is None  # the same bowl again
    assert not s[3]["right"] and s[3]["latency_s"] is None


def test_intervals():
    v, lo, hi = E.bootstrap_ci([0.2, 0.4, 0.6, 0.8], E.mean_of, n=2000, seed=1)
    assert abs(v - 0.5) < 1e-9 and 0.2 <= lo < 0.5 < hi <= 0.8
    assert E.bootstrap_ci([0.5], E.mean_of)[1] is None
    lo, hi = E.wilson_ci(7, 7)
    assert 0.6 < lo < 0.7 and hi == 1.0


def test_light_metrics():
    segs = [{"start": 0.0, "end": 0.3, "chakra": 0, "hz": 0}, {"start": 0.3, "end": 0.6, "chakra": 1, "hz": 0},
            {"start": 0.6, "end": 0.9, "chakra": 2, "hz": 0}, {"start": 0.9, "end": 1.1, "chakra": 3, "hz": 0},
            {"start": 5.0, "end": 20.0, "chakra": 3, "hz": 0}]
    m = E.light_metrics(segs, 60.0)
    assert m["max_changes_1s"] == 4 and m["over_flash_limit"]
    assert abs(m["short_share"] - 0.8) < 1e-9


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"ok  {name}")
