#!/usr/bin/env python3
"""Chakra with and without AI: how much wrong colour the DSP shows, and how much an AI gate removes.

What: for every clip in bench_clips/ (fetch_clips.py; sources.json gives the class and what the
      lamp should do) it
        1. runs the app's own detection (chakra_detect.c through tests/analyse_wav.c, Flow) and
           takes the time the lamp shows a colour;
        2. cuts the clip into windows (WINDOWS seconds long, half a window apart), asks
           EfficientAT (mn04, mn05, dymn04; AudioSet) for each window and opens the gate when
             pretrained  the best bowl-like AudioSet class beats the best "other" class
             trained     a small logistic head on the model's embedding, trained on these clips
                         with whole clips held out (5-fold by clip, never a window of the same clip)
        3. keeps the colour only where the gate is open.
      Reports per class: share of the clip's sounding time with a colour, without AI and with each
      gate; bowl-like classes should keep their colour, the others should lose it.
      Writes ai_bench/gate_bench.json and prints the tables.
Why:  owner 25.10.2026: "für den Comment nur Antwort für Audio mit und ohne AI, und dann die App
      mit AI on/off". The question a sound healer asks: does a gong or a drum still paint the room?
Pitfalls:
  * Window labels come from the clip's class; a "drum" clip with a bell in it counts as drum.
  * The trained head sees the same recordings' classes; the folds split by clip, so a held-out
    clip is never in training, but sessions in one room would still be easier than these.
  * Times are the Mac's CPU (EfficientAT in float). The S31 numbers are estimates elsewhere.
Usage:  AI_VENV/bin/python tools/bench/gate_bench.py [--models mn05_as] [--windows 0.5,1,2] [--save-head]
                                                   [--dry-run]
        AI_VENV = ~/Library/Application Support/LeopardCode/chakra/ai_venv_eat
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
import bench_efficientat as eat  # noqa: E402

sys.path.insert(0, str(HERE))
from classifier_bench import BOWL_CLASSES  # noqa: E402

sys.path.insert(0, str(Path.home() / "Coding/company/tools"))
try:
    import progress
except ImportError:
    progress = None

STATE = Path.home() / "Library/Application Support/LeopardCode/chakra"
CLIPS = STATE / "bench_clips"
OUT = STATE / "ai_bench/gate_bench.json"
HEAD = STATE / "ai_models/gate_head_mn05.json"  # read by tools/ai_gate.py
MAX_S = 30.0  # longer recordings are cut: the first 30 s say enough and keep the run short
STEP = 0.1  # timeline resolution in seconds
BOWL = set(BOWL_CLASSES) | {"Singing bowl", "Ding", "Chime", "Bell", "Tuning fork", "Sine wave"}
OTHER = {"Gong", "Drum", "Bass drum", "Snare drum", "Drum kit", "Tabla", "Percussion", "Rattle (instrument)",
         "Maraca", "Shaker", "Rain", "Rain on surface", "Raindrop", "Speech", "Singing", "Chant", "Humming",
         "Mantra", "Male singing", "Female singing", "Throat singing", "Timpani", "Tambourine",
         "Wood block", "Clapping", "Hands", "Water", "Crackle"}
SEG = re.compile(r"^\s*([\d.]+)-\s*([\d.]+) s\s+(\S+)")


def clips() -> list[dict]:
    """sources.json (fetch_clips.py) and extra_sources.json (the first benchmark's Commons clips),
    each file once: names are compared without a URL query an older fetch left on them."""
    out, seen = [], set()
    for name in ("extra_sources.json", "sources.json"):
        f = CLIPS / name
        for c in json.loads(f.read_text()) if f.exists() else []:
            key = Path(c["file"]).name.split("?")[0].replace(" ", "_").lower()
            if key not in seen and (CLIPS / c["file"]).exists():
                seen.add(key)
                out.append(c)
    # Files a running fetch has saved but not yet listed: the folder is the class.
    from fetch_clips import CLASSES
    for d in sorted(x for x in CLIPS.iterdir() if x.is_dir() and x.name in CLASSES):
        for f in sorted(d.iterdir()):
            key = f.name.split("?")[0].replace(" ", "_").lower()
            if key not in seen and f.is_file():
                seen.add(key)
                out.append({"class": d.name, "expect": CLASSES[d.name][0], "file": f"{d.name}/{f.name}",
                            "title": "File:" + f.name.split("?")[0].replace("_", " ")})
    return out


def decode(path: Path, rate: int) -> np.ndarray:
    raw = subprocess.run(["ffmpeg", "-v", "error", "-t", str(MAX_S), "-i", str(path), "-ac", "1", "-ar", str(rate),
                          "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


def dsp_timeline(path: Path, n: int) -> np.ndarray:
    """True where the app's detection shows a colour, at STEP resolution (Flow, notes at 440)."""
    wav = STATE / "ai_bench/_dsp.wav"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-t", str(MAX_S), "-i", str(path), "-ac", "1", "-ar", "16000",
                    "-c:a", "pcm_s16le", str(wav)], check=True)
    out = subprocess.run([str(APP / "build-test/analyse_wav"), str(wav)], capture_output=True, text=True).stdout
    on = np.zeros(n, dtype=bool)
    for line in out.splitlines():
        m = SEG.match(line)
        if m and m.group(3) != "none":
            on[int(float(m.group(1)) / STEP):int(float(m.group(2)) / STEP) + 1] = True
    return on


def sounding(w16: np.ndarray, n: int) -> np.ndarray:
    """True where the clip is not near silence: the share is taken of the time something plays."""
    hop = int(16000 * STEP)
    rms = np.array([np.sqrt(np.mean(w16[i * hop:(i + 1) * hop] ** 2) + 1e-12) for i in range(n)])
    return rms > max(rms.max() * 0.03, 1e-4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--models", default="mn04_as,mn05_as,dymn04_as")
    ap.add_argument("--windows", default="0.5,1,2")
    ap.add_argument("--save-head", action="store_true", help="save the mn05 1 s head for tools/ai_gate.py")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    models = a.models.split(",")
    windows = [float(x) for x in a.windows.split(",")]
    items = clips()
    if a.dry_run:
        from collections import Counter
        print(Counter(c["class"] for c in items))
        print(f"(dry run) {len(items)} clips x models {models} x windows {windows}, nothing run")
        return
    subprocess.run(["cc", "-std=c11", "-O2", f"-I{APP}/components/app_chakra/include",
                    str(APP / "components/app_chakra/chakra_detect.c"), str(APP / "tests/analyse_wav.c"), "-lm",
                    "-o", str(APP / "build-test/analyse_wav")], check=True)
    torch, labels, get_mn, get_dymn, mel = eat.load_repo()
    bowl_i = [i for i, l in enumerate(labels) if l in BOWL]
    other_i = [i for i, l in enumerate(labels) if l in OTHER]

    bar = progress.Bar("Gate bench", total=len(items) * (1 + len(models) * len(windows)), unit="runs") \
        if progress else None
    base = []
    for c in items:  # the DSP alone, once per clip
        p = CLIPS / c["file"]
        w16 = decode(p, 16000)
        n = max(1, int(len(w16) / 16000 / STEP))
        base.append({"clip": c["file"], "class": c["class"], "expect": c["expect"], "n": n,
                     "dsp": dsp_timeline(p, n), "sound": sounding(w16, n), "w32": decode(p, eat.RATE)})
        if bar:
            bar.advance()

    results = {"clips": len(items), "models": {}}
    for m in models:
        model = eat.build(m, get_mn, get_dymn)
        for win in windows:
            size, hop = int(eat.RATE * win), int(eat.RATE * win / 2)
            per_clip, feats, ys, groups, t_ms = [], [], [], [], []
            for gi, b in enumerate(base):
                w = b["w32"]
                starts = list(range(0, max(1, len(w) - size + 1), hop))
                probs, embs = [], []
                with torch.no_grad():
                    for s in starts:
                        x = torch.from_numpy(np.ascontiguousarray(np.pad(w[s:s + size], (0, max(0, size - len(w[s:s + size]))))))[None, :]
                        t0 = time.perf_counter()
                        logits, emb = model(mel(x).unsqueeze(0))
                        t_ms.append((time.perf_counter() - t0) * 1000)
                        probs.append(torch.sigmoid(logits)[0].numpy())
                        embs.append(emb[0].numpy().ravel())
                probs = np.array(probs)
                gate_pre = probs[:, bowl_i].max(1) >= probs[:, other_i].max(1)
                per_clip.append({"starts": starts, "pre": gate_pre})
                feats += embs
                ys += [1 if b["expect"] == "bowl" else 0] * len(embs)
                groups += [gi] * len(embs)
                if bar:
                    bar.advance()
            # The trained head, clip-wise 5-fold: every window is judged by a head that never saw its clip.
            from sklearn.linear_model import LogisticRegression
            from sklearn.model_selection import GroupKFold
            from sklearn.pipeline import make_pipeline
            from sklearn.preprocessing import StandardScaler
            X, y, g = np.array(feats), np.array(ys), np.array(groups)
            pred = np.zeros(len(y), dtype=bool)
            new_head = lambda: make_pipeline(StandardScaler(), LogisticRegression(  # noqa: E731
                max_iter=3000, class_weight="balanced", C=0.5))
            for tr, te in GroupKFold(n_splits=5).split(X, y, g):
                pred[te] = new_head().fit(X[tr], y[tr]).predict(X[te]) == 1
            if a.save_head and m == "mn05_as" and win == 1.0:
                # The head ai_gate.py uses: trained on every clip (the folds above are its test).
                full = new_head().fit(X, y)
                sc, lr = full[0], full[1]
                HEAD.write_text(json.dumps({"model": m, "window_s": win, "mean": sc.mean_.tolist(),
                                            "scale": sc.scale_.tolist(), "coef": lr.coef_[0].tolist(),
                                            "intercept": float(lr.intercept_[0]), "clips": len(base),
                                            "windows": int(len(y))}))
                print(f"head saved: {HEAD}", flush=True)
            k = 0
            rows = []
            for b, pc in zip(base, per_clip):
                gates = {"pre": np.zeros(b["n"], bool), "trained": np.zeros(b["n"], bool)}
                for j, s in enumerate(pc["starts"]):
                    # A window's verdict holds from when it is complete until the next one is.
                    a0 = int((s + size) / eat.RATE / STEP)
                    a1 = int((s + size + hop) / eat.RATE / STEP)
                    gates["pre"][a0:a1] = pc["pre"][j]
                    gates["trained"][a0:a1] = pred[k + j]
                k += len(pc["starts"])
                snd = b["sound"].sum() or 1
                share = lambda on: float((on & b["sound"]).sum() / snd)  # noqa: E731
                rows.append({"clip": b["clip"], "class": b["class"], "expect": b["expect"],
                             "no_ai": share(b["dsp"]), "ai_pretrained": share(b["dsp"] & gates["pre"]),
                             "ai_trained": share(b["dsp"] & gates["trained"])})
            key = f"{m}@{win:g}s"
            results["models"][key] = {"rows": rows, "ms_per_window": float(np.median(t_ms)),
                                      "window_accuracy_trained": float((pred == (y == 1)).mean())}
    if bar:
        bar.done()
    OUT.write_text(json.dumps(results, indent=1))
    # Tables: mean share of sounding time with a colour, per class.
    classes = sorted({b["class"] for b in base}, key=lambda c: (base[[x["class"] for x in base].index(c)]["expect"], c))
    for key, r in results["models"].items():
        print(f"\n== {key}  ({r['ms_per_window']:.0f} ms per window on the Mac, trained head "
              f"{100 * r['window_accuracy_trained']:.0f} % of windows right)")
        print(f"   {'class':13} {'lamp should':11} {'n':>3} {'no AI':>7} {'AI pre':>7} {'AI trained':>10}")
        for c in classes:
            rs = [x for x in r["rows"] if x["class"] == c]
            mean = lambda k: 100 * sum(x[k] for x in rs) / len(rs)  # noqa: E731
            print(f"   {c:13} {'colour' if rs[0]['expect'] == 'bowl' else 'no colour':11} {len(rs):3} "
                  f"{mean('no_ai'):6.0f}% {mean('ai_pretrained'):6.0f}% {mean('ai_trained'):9.0f}%")
    print(OUT)


if __name__ == "__main__":
    main()
