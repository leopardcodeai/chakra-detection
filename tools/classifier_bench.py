#!/usr/bin/env python3
"""Measures local sound classifiers (YAMNet, CLAP) as a gate in front of the chakra DSP.

What: cuts each clip to the window the device hears (2 s after the strike) and asks
        yamnet  Google's YAMNet (AudioSet, 521 classes; MediaPipe's TFLite file), the candidate for
                the ESP32-S31: a MobileNet of Conv and DepthwiseConv, which ESP-DL runs on the
                chip's AI instructions after int8 quantization (esp-ppq)
        clap    LAION CLAP (laion/clap-htsat-unfused), zero-shot against our own labels; far too
                large for the chip, it shows what a bigger model would add
      what it hears. Each answer is reduced to the question the lamp needs: "bowl-like" (singing
      bowl, bell, chime, pure tone: the colour follows the pitch) or "other" (gong, drum,
      rainstick, shaker: the colour stays). The expected class comes from the clip's name.
      Prints a table with time per clip and writes report.json.
Why:  owner 25.10.2026: "las das mit YAMNet or CLAP jetzt testen", "die Modelle nutzen den
      AI-Beschleuniger des S31". The OpenAI audio models failed (tools/ai_detect_bench.py).
Pitfalls:
  * Times here are the Mac's CPU; the chip's time needs the board (ESP-DL profile).
  * Models live outside the repo: ~/Library/Application Support/LeopardCode/chakra/ai_models
    (YAMNet) and the Hugging Face cache (CLAP, about 600 MB on first run).
Usage:  AI_VENV/bin/python tools/classifier_bench.py CLIP_DIR [...] [--only yamnet|clap] [--dry-run]
        with AI_VENV = ~/Library/Application Support/LeopardCode/chakra/ai_venv (torch,
        transformers, ai-edge-litert, numpy)
"""
import argparse
import json
import math
import subprocess
import sys
import time
import zipfile
from pathlib import Path

import numpy as np

STATE = Path.home() / "Library/Application Support/LeopardCode/chakra"
YAMNET = STATE / "ai_models/yamnet.tflite"
OUT = STATE / "ai_bench"
OFFSET, LENGTH = 0.3, 2.0
AUDIO = {".wav", ".ogg", ".flac", ".mp3", ".m4a"}
TONES = [("C4", 261.63), ("E4", 329.63), ("A4", 440.00)]

# Expected answer per clip, from its file name.
EXPECT = [("bowl", "bowl"), ("rin", "bowl"), ("tone", "bowl"), ("gong", "other"), ("drum", "other"),
          ("beben", "other"), ("bęben", "other"), ("udu", "other"), ("shaker", "other"),
          ("rainstick", "other"), ("palo_de_lluvia", "other")]
# AudioSet classes that count as bowl-like for the lamp.
BOWL_CLASSES = {"Singing bowl", "Bell", "Church bell", "Jingle bell", "Bicycle bell", "Tuning fork",
                "Chime", "Wind chime", "Change ringing (campanology)", "Sine wave", "Glockenspiel",
                "Vibraphone", "Marimba, xylophone", "Mallet percussion", "Tubular bells"}
CLAP_LABELS = {"a singing bowl": "bowl", "a ringing bell": "bowl", "a pure sine tone": "bowl",
               "a gong": "other", "a drum": "other", "a rainstick": "other", "a shaker or rattle": "other",
               "a human voice": "other", "rain or water": "other"}


def expected(name: str) -> str | None:
    n = name.lower()
    if n.startswith("tone "):  # the synthetic tones; "Overtones" in a rainstick's name is no tone
        return "bowl"
    return next((v for k, v in EXPECT if k != "tone" and k in n), None)


def pcm16k(path: Path | None, hz: float | None, rate: int) -> np.ndarray:
    if hz:
        t = np.arange(int(rate * LENGTH)) / rate
        v = sum(a * np.sin(2 * math.pi * hz * k * t) for k, a in ((1, 1.0), (2, 0.35), (3, 0.15)))
        return (0.3 * v * np.exp(-0.4 * t)).astype(np.float32)
    raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", str(OFFSET), "-t", str(LENGTH), "-i", str(path),
                          "-ac", "1", "-ar", str(rate), "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


class Yamnet:
    RATE = 16000

    def __init__(self):
        from ai_edge_litert.interpreter import Interpreter
        self.labels = zipfile.ZipFile(YAMNET).read("yamnet_label_list.txt").decode().splitlines()
        self.it = Interpreter(model_path=str(YAMNET))
        self.inp = self.it.get_input_details()[0]
        self.out = self.it.get_output_details()[0]
        self.n = int(self.inp["shape"][-1])  # 15600 samples = 0.975 s; MediaPipe's file takes [15600]
        self.it.allocate_tensors()  # once: the input shape is fixed, re-allocating per frame skewed the time
        self.top3: list[tuple[str, float]] = []  # top 3 classes of the last ask(), for the report

    def ask(self, wav: np.ndarray) -> tuple[str, float, float]:
        """Mean scores over 0.975 s frames (hop 0.5 s); returns top class, its score, ms per frame."""
        frames = [wav[i:i + self.n] for i in range(0, max(1, len(wav) - self.n + 1), self.RATE // 2)]
        scores, spent = [], 0.0
        for f in frames:
            f = np.pad(f, (0, self.n - len(f)))
            self.it.set_tensor(self.inp["index"], f.reshape(self.inp["shape"]).astype(np.float32))
            t0 = time.perf_counter()
            self.it.invoke()
            spent += time.perf_counter() - t0
            scores.append(self.it.get_tensor(self.out["index"]).reshape(-1).copy())
        ms = spent * 1000 / len(frames)
        s = np.mean(scores, axis=0)
        order = np.argsort(s)[::-1][:3]
        self.top3 = [(self.labels[int(i)], float(s[i])) for i in order]
        top = int(order[0])
        return self.labels[top], float(s[top]), ms

    def verdict(self, label: str) -> str:
        return "bowl" if label in BOWL_CLASSES else "other"


class Clap:
    RATE = 48000

    def __init__(self):
        import torch
        from transformers import ClapModel, ClapProcessor
        self.torch = torch
        self.proc = ClapProcessor.from_pretrained("laion/clap-htsat-unfused")
        self.model = ClapModel.from_pretrained("laion/clap-htsat-unfused").eval()
        self.top3 = []
        with torch.no_grad():
            t = self.proc(text=list(CLAP_LABELS), return_tensors="pt", padding=True)
            self.text = self.unit(self.model.get_text_features(**t))

    @staticmethod
    def unit(e):
        """transformers 5 returns an output object whose pooler_output holds the projected embedding."""
        e = getattr(e, "pooler_output", e)
        return e / e.norm(dim=-1, keepdim=True)

    def ask(self, wav: np.ndarray) -> tuple[str, float, float]:
        t0 = time.perf_counter()
        with self.torch.no_grad():
            a = self.proc(audio=[wav], sampling_rate=self.RATE, return_tensors="pt")
            e = self.unit(self.model.get_audio_features(**a))
            p = (100 * e @ self.text.T).softmax(dim=-1)[0]
        ms = (time.perf_counter() - t0) * 1000
        labels = list(CLAP_LABELS)
        s, k = p.topk(3)
        self.top3 = [[labels[int(j)], round(float(v), 3)] for v, j in zip(s, k)]
        i = int(p.argmax())
        return labels[i], float(p[i]), ms

    def verdict(self, label: str) -> str:
        return CLAP_LABELS[label]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="+", type=Path)
    ap.add_argument("--only", choices=["yamnet", "clap"])
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    clips = sorted(p for d in a.dirs for p in d.iterdir() if p.suffix.lower() in AUDIO)
    items = [(f"tone {n} {hz}", None, hz) for n, hz in TONES] + [(p.name, p, None) for p in clips]
    names = [a.only] if a.only else ["yamnet", "clap"]
    if a.dry_run:
        for name, _, _ in items:
            print(f"  {name:45} expect {expected(name)}")
        print(f"(dry run) {len(items)} items, models {names}, nothing loaded")
        return
    models = {}
    for m in names:
        t0 = time.perf_counter()
        models[m] = Yamnet() if m == "yamnet" else Clap()
        print(f"{m} loaded in {time.perf_counter() - t0:.1f} s", flush=True)
    rows = []
    for i, (name, path, hz) in enumerate(items, 1):
        row = {"clip": name, "expect": expected(name)}
        line = f"[{i}/{len(items)}] {name[:34]:34} {str(row['expect']):5}"
        for m, model in models.items():
            label, score, ms = model.ask(pcm16k(path, hz, model.RATE))
            v = model.verdict(label)
            row[m] = {"label": label, "score": round(score, 3), "ms": round(ms, 1), "verdict": v,
                      "right": v == row["expect"]}
            line += f" | {m} {'ok ' if v == row['expect'] else 'NO '}{label[:22]:22} {score:.2f} {ms:5.0f} ms"
        rows.append(row)
        print(line, flush=True)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "classifier_report.json").write_text(json.dumps(rows, indent=1, ensure_ascii=False))
    for m in models:
        right = sum(r[m]["right"] for r in rows if r["expect"])
        total = sum(1 for r in rows if r["expect"])
        ms = sorted(r[m]["ms"] for r in rows)
        print(f"{m:7} right {right}/{total}, median {ms[len(ms) // 2]} ms per "
              f"{'0.975 s frame' if m == 'yamnet' else '2 s clip'} on this Mac")
    print(OUT / "classifier_report.json")


if __name__ == "__main__":
    main()
