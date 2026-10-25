#!/usr/bin/env python3
"""Proposes instrument labels for your own sound healing videos, and makes sheets to check them by eye.

What: for every video in OWN (exported from the Photos album "ChakraV Training") it
        1. cuts the sound into SEG-second segments and asks EfficientAT (dymn04_as, AudioSet)
           what sounds in each; the AudioSet classes are mapped to the lamp's classes
           (bowl, bell, gong, drum, rattle, rain, voice, silence, other), quiet segments are silence;
        2. draws a contact sheet: one frame per SHEET_EVERY seconds with its time and the proposed
           label under it, so a person (or Claude, looking at the sheet) sees which instrument is
           really played and corrects the label;
      and writes own_labels.json: [{video, start, end, label, top3, checked: false}]. A corrected
      segment gets checked: true (by hand in the file, or with --set VIDEO START LABEL).
      gate_bench.py reads the checked and unchecked segments as extra clips (--own).
Why:  owner 25.10.2026: "check Videos in Sound Healing Photos-Ordner von mir, die können wir auch
      zum Learning benutzen". 4 gongs from Wikimedia were too few: a temple gong was taken for a bowl.
Privacy: the videos show people at sessions. Everything stays in the private state folder (0700):
      the exports, the sheets and the labels. Nothing of it is ever committed, uploaded or shown in
      a public text; only counts and results are.
Pitfalls:
  * A segment often holds two instruments (a bowl ringing while a drum starts); the label is the
    one that leads, and "mixed" segments are best left out of training (label "other").
  * Phone videos are 48 kHz stereo; everything is mixed down to mono here.
Usage:  AI_VENV/bin/python tools/bench/label_own.py [--dry-run]
        AI_VENV/bin/python tools/bench/label_own.py --set IMG_1234.MOV 42 gong
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import bench_efficientat as eat  # noqa: E402

sys.path.insert(0, str(Path.home() / "Coding/company/tools"))
try:
    import progress
except ImportError:
    progress = None

STATE = Path.home() / "Library/Application Support/LeopardCode/chakra"
OWN = STATE / "own_media/chakrav_training"
REVIEW = STATE / "own_media/_review"
LABELS = STATE / "own_media/own_labels.json"
SEG, SHEET_EVERY = 2.0, 4.0
SILENCE_DB = -50.0
VIDEO = (".mov", ".mp4", ".m4v")
# AudioSet class -> the lamp's class (first match wins, in this order).
MAP = [("gong", {"Gong", "Tam-tam"}),
       ("bowl", {"Singing bowl", "Tuning fork", "Sine wave", "Ding"}),
       ("bell", {"Bell", "Church bell", "Jingle bell", "Chime", "Wind chime", "Tubular bells", "Glockenspiel",
                 "Change ringing (campanology)", "Vibraphone"}),
       ("drum", {"Drum", "Bass drum", "Snare drum", "Drum kit", "Tabla", "Timpani", "Drum roll", "Tambourine",
                 "Percussion", "Hands", "Clapping"}),
       ("rattle", {"Rattle (instrument)", "Maraca", "Shaker", "Rattle"}),
       ("rain", {"Rain", "Rain on surface", "Raindrop", "Water", "Stream", "Ocean", "Waves, surf"}),
       ("voice", {"Speech", "Singing", "Chant", "Humming", "Mantra", "Male singing", "Female singing", "Choir",
                  "Throat singing", "Child speech, kid speaking", "Conversation", "Narration, monologue",
                  "Breathing"})]
GENERIC = {"Music", "Musical instrument", "Inside, small room", "Inside, large room or hall", "Silence",
           "Reverberation", "Echo"}


def lamp_class(names: list[str]) -> str:
    for n in names:
        if n in GENERIC:
            continue
        for cls, members in MAP:
            if n in members:
                return cls
        return "other"
    return "other"


def audio(path: Path, rate: int) -> np.ndarray:
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-vn", "-ac", "1", "-ar", str(rate),
                          "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


def sheet(path: Path, segs: list[dict]) -> Path:
    """One frame per SHEET_EVERY s in a grid of 5, each with its time and proposed label."""
    REVIEW.mkdir(parents=True, exist_ok=True)
    out = REVIEW / f"{path.stem}.jpg"
    lab = {int(s["start"] // SHEET_EVERY): s["label"] for s in segs}
    # Tiles left to right, top to bottom, one every SHEET_EVERY s from 0 (Homebrew's ffmpeg has no
    # drawtext without freetype, so the times are the tiles' order; see labels_text()).
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(path), "-vf",
                    f"fps=1/{SHEET_EVERY:g},scale=320:-2,tile=5x{max(1, -(-len(lab) // 5))}", "-frames:v", "1",
                    "-q:v", "4", str(out)], check=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", type=Path, default=OWN, help="the videos' folder (default: the album export)")
    ap.add_argument("--set", nargs=3, metavar=("VIDEO", "START", "LABEL"), help="correct one segment by hand")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    labels = json.loads(LABELS.read_text()) if LABELS.exists() else []
    if a.set:
        video, start, label = a.set[0], float(a.set[1]), a.set[2]
        hit = [s for s in labels if s["video"] == video and s["start"] <= start < s["end"]]
        for s in hit:
            s.update(label=label, checked=True)
        LABELS.write_text(json.dumps(labels, indent=1))
        print(f"{len(hit)} segment(s) set to {label}")
        return
    videos = sorted(p for p in a.dir.iterdir() if p.suffix.lower() in VIDEO) if a.dir.exists() else []
    if a.dry_run:
        print(f"(dry run) {len(videos)} videos in {OWN}, segments of {SEG:g} s, sheets to {REVIEW}")
        return
    done = {s["video"] for s in labels}
    torch, names, get_mn, get_dymn, mel = eat.load_repo()
    model = eat.build("dymn04_as", get_mn, get_dymn)
    bar = progress.Bar("Label own videos", total=len(videos), unit="videos") if progress else None
    for v in videos:
        if v.name in done:
            if bar:
                bar.advance()
            continue
        w = audio(v, eat.RATE)
        size = int(eat.RATE * SEG)
        segs = []
        with torch.no_grad():
            for i in range(0, max(1, len(w) - size + 1), size):
                x = w[i:i + size]
                start = i / eat.RATE
                db = 20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-9)
                if db < SILENCE_DB:
                    segs.append({"video": v.name, "start": start, "end": start + SEG, "label": "silence",
                                 "top3": [], "db": round(float(db), 1), "checked": False})
                    continue
                logits, _ = model(mel(torch.from_numpy(x.copy())[None, :]).unsqueeze(0))
                p = torch.sigmoid(logits)[0].numpy()
                order = np.argsort(p)[::-1][:6]
                top = [names[j] for j in order]
                segs.append({"video": v.name, "start": start, "end": start + SEG, "label": lamp_class(top),
                             "top3": [[names[j], round(float(p[j]), 2)] for j in order[:3]],
                             "db": round(float(db), 1), "checked": False})
        labels += segs
        LABELS.write_text(json.dumps(labels, indent=1))
        s = sheet(v, segs)
        counts = {}
        for g in segs:
            counts[g["label"]] = counts.get(g["label"], 0) + 1
        print(f"{v.name}: {len(segs)} segments {counts}; sheet {s.name}", flush=True)
        if bar:
            bar.advance()
    if bar:
        bar.done()
    print(LABELS)


if __name__ == "__main__":
    main()
