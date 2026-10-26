#!/usr/bin/env python3
"""Trains and tests the gong head of the AI sound gate: is this "other" sound a gong?

What: cuts every bench clip (fetch_clips.py) and, with --own, every checked segment of your own videos
      (label_own.py) into 1 s windows half a second apart, the way tools/ai_gate.py hears them, and
      takes EfficientAT mn05's embedding of each. A small logistic head learns gong against
      everything else; clip-wise 5-fold (a window is judged by a head that never saw its clip)
      gives, per class, the share of sounding windows the gate would call "gong" (only windows the
      bowl head already calls "other" count, as in ai_gate.py). With --save the head trained on all
      windows goes to GONG_HEAD, which ai_gate.py reads; without it nothing is written.
Why:  owner 26.10.2026 said yes to "Gong: a slow swell, not a kick" (docs/SOUND_LIGHT_EXPERIENCE.md in
      ChakraV, recommendation 2). The bowl head only says bowl or other; AudioSet's own "Gong" class
      found 9 % of the gong windows (measured 26.10.2026), too few to rely on.
Pitfalls:
  * Window labels come from the clip's class; a gong clip with a bowl in it counts as gong.
  * The bowl head is the one in ai_models (gate_bench.py --save-head); retrain this head after it.
  * Own videos show people: only counts leave the private state folder, never the media.
Usage:  AI_VENV/bin/python tools/bench/gong_head.py [--own] [--save] [--dry-run]
        AI_VENV = ~/Library/Application Support/LeopardCode/chakra/ai_venv_eat
"""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import bench_efficientat as eat  # noqa: E402
import gate_bench as gb  # noqa: E402

try:
    import progress
except ImportError:
    progress = None

GONG_HEAD = gb.STATE / "ai_models/gate_gong_mn05.json"  # read by tools/ai_gate.py
WIN, HOP = 1.0, 0.5
SILENCE_DB = -55.0  # as ai_gate.py: quieter windows are "none" there and left out here


def db(x: np.ndarray) -> float:
    return float(20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-9))


def sources(own: bool) -> list[dict]:
    """Each source: a file, its class, and for own videos the checked segments to cut out."""
    out = [{"file": gb.CLIPS / c["file"], "class": c["class"], "segments": None} for c in gb.clips()]
    if own and gb.OWN_LABELS.exists():
        labels = json.loads(gb.OWN_LABELS.read_text())
        for video in sorted({s["video"] for s in labels}):
            p = next((d / video for d in gb.OWN_DIRS if (d / video).exists()), None)
            segs = [s for s in labels if s["video"] == video and s.get("checked") and s["label"] != "silence"]
            if p and segs:
                out.append({"file": p, "class": "own", "segments": segs})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--own", action="store_true", help="add the checked segments of your own videos")
    ap.add_argument("--save", action="store_true", help=f"write the head to {GONG_HEAD}")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    items = sources(a.own)
    if a.dry_run:
        print(Counter(i["class"] for i in items))
        print(f"(dry run) {len(items)} sources, 1 s windows; would {'write ' + str(GONG_HEAD) if a.save else 'write nothing'}")
        return
    torch, labels, get_mn, get_dymn, mel = eat.load_repo()
    model = eat.build("mn05_as", get_mn, get_dymn)
    bowl = json.loads(gb.HEAD.read_text())
    b_coef, b_bias = np.array(bowl["coef"]), float(bowl["intercept"])
    b_mean, b_scale = np.array(bowl["mean"]), np.array(bowl["scale"])
    size, hop = int(eat.RATE * WIN), int(eat.RATE * HOP)
    feats, ys, groups, kinds, other = [], [], [], [], []
    bar = progress.Bar("Gong head", total=len(items), unit="clips") if progress else None
    for gi, it in enumerate(items):
        w = gb.decode(it["file"], eat.RATE)
        for s in range(0, max(1, len(w) - size + 1), hop):
            x = w[s:s + size]
            if len(x) < size or db(x) < SILENCE_DB:
                continue
            if it["segments"] is None:
                kind = it["class"]
            else:  # an own video: the window takes the checked segment at its centre, or is left out
                t = (s + size / 2) / eat.RATE
                seg = next((g for g in it["segments"] if g["start"] <= t < g["end"]), None)
                if not seg:
                    continue
                kind = "own_" + seg["label"]
            with torch.no_grad():
                _, emb = model(mel(torch.from_numpy(np.ascontiguousarray(x))[None, :]).unsqueeze(0))
            e = emb[0].numpy().ravel()
            feats.append(e)
            ys.append(1 if kind in ("gong", "own_gong") else 0)
            groups.append(gi)
            kinds.append(kind)
            other.append(float(((e - b_mean) / b_scale) @ b_coef + b_bias) <= 0)  # the bowl head says other
        if bar:
            bar.advance()
    from sklearn.linear_model import LogisticRegression
    from sklearn.model_selection import GroupKFold
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler
    X, y, g, other = np.array(feats), np.array(ys), np.array(groups), np.array(other)
    # C 0.01: of 0.01, 0.05 and 0.5 the best gong share (26.10.2026: Commons gongs 45 %, own gongs 33 %)
    # for a little more false gong on drums (13 %) and voices (10 %), which only ever turns a small kick
    # into a slow swell. AudioSet's log-probabilities as features found more own gongs but 29 % of voices.
    new_head = lambda: make_pipeline(StandardScaler(), LogisticRegression(  # noqa: E731
        max_iter=3000, class_weight="balanced", C=0.01))
    pred = np.zeros(len(y), dtype=bool)
    for tr, te in GroupKFold(n_splits=min(5, len(set(g)))).split(X, y, g):
        pred[te] = new_head().fit(X[tr], y[tr]).predict(X[te]) == 1
    gong = pred & other
    print(f"{'class':16} {'windows':>7} {'other':>6} {'gong':>6}   (clip-wise 5-fold, gong = other and the gong head)")
    rows = {}
    for kind in sorted(set(kinds)):
        m = np.array([k == kind for k in kinds])
        rows[kind] = {"windows": int(m.sum()), "other": float(other[m].mean()), "gong": float(gong[m].mean())}
        print(f"{kind:16} {m.sum():7} {other[m].mean():6.0%} {gong[m].mean():6.0%}")
    if a.save:
        full = new_head().fit(X, y)
        sc, lr = full[0], full[1]
        GONG_HEAD.write_text(json.dumps({"model": "mn05_as", "window_s": WIN, "mean": sc.mean_.tolist(),
                                         "scale": sc.scale_.tolist(), "coef": lr.coef_[0].tolist(),
                                         "intercept": float(lr.intercept_[0]), "sources": len(items),
                                         "windows": int(len(y)), "folds": rows}))
        print(f"gong head saved: {GONG_HEAD}")


if __name__ == "__main__":
    main()
