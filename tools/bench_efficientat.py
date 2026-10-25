#!/usr/bin/env python3
"""Measures the EfficientAT audio taggers (MobileNetV3 on AudioSet) as the bowl gate for the S31.

What: cuts each clip to the window the device hears (2 s after the strike, classifier_bench.pcm16k
      at 32 kHz), turns it into EfficientAT's own log-mel (AugmentMelSTFT: 128 mels, 25 ms window,
      10 ms hop, eval mode) and asks
        mn04_as    0.98 M parameters, mAP 43.2
        mn05_as    1.43 M parameters, mAP 44.3
        dymn04_as  1.97 M parameters, mAP 45.0 (dynamic convs, not exported)
      The top AudioSet class is reduced to the lamp's question with classifier_bench.BOWL_CLASSES:
      "bowl" (colour follows the pitch) or "other" (colour stays). Per model it prints right/total,
      the wrong clips with their top 3, ms per 2 s clip on this Mac, parameters and float size, and
      writes ai_bench/efficientat_<model>.json. Then it exports mn04 and mn05 to ONNX (opset 13,
      fixed 1 s input [1, 1, 128, 100] log-mel, output [1, 527] logits) into ai_models/ and checks
      that ONNX Runtime gives the same top class as PyTorch on three clips.
Why:  owner 25.10.2026: the chakra gate should run on the ESP32-S31's AI instructions; EfficientAT
      is the smallest AudioSet tagger family with a published mAP. One of four parallel model tests
      (YAMNet and CLAP are in classifier_bench.py).
Pitfalls:
  * The model code imports helpers/utils.py, which reads metadata/class_labels_indices.csv relative
    to the working directory, and torch.hub looks for the weights in ./resources: the script chdirs
    into the EfficientAT clone before importing it.
  * The mel front end is not part of the ONNX file (STFT does not map to ESP-DL); the chip has to
    reproduce AugmentMelSTFT: pre-emphasis 0.97, Hann 800, n_fft 1024, hop 320, Kaldi mel banks
    0 to 15000 Hz, log(x + 1e-5), then (x + 4.5) / 5. 1 s at 32 kHz gives 100 frames because the
    pre-emphasis eats one sample.
  * Times are the Mac's CPU (torch, default threads), mel included; the chip's time needs the board.
  * classifier_bench.py is shared with other agents: imported, never edited.
Setup (once):
  git clone --depth 1 https://github.com/fschmid56/EfficientAT "$STATE/ai_models/EfficientAT"
  weights from its release v0.0.1 into EfficientAT/resources
  uv venv --python 3.14 "$STATE/ai_venv_eat"; uv pip install torch torchaudio torchvision numpy
  onnx onnxruntime onnxscript
Usage:  "$STATE/ai_venv_eat/bin/python" tools/bench_efficientat.py CLIP_DIR [...]
          [--models mn04_as,mn05_as,dymn04_as] [--no-export] [--dry-run]
        with STATE = ~/Library/Application Support/LeopardCode/chakra
"""
import argparse
import contextlib
import io
import json
import os
import statistics
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from classifier_bench import AUDIO, BOWL_CLASSES, TONES, expected, pcm16k  # noqa: E402

STATE = Path.home() / "Library/Application Support/LeopardCode/chakra"
REPO = STATE / "ai_models/EfficientAT"
MODELS_OUT = STATE / "ai_models"
OUT = STATE / "ai_bench"
RATE = 32000
WEIGHTS = {"mn04_as": ("mn", 0.4, "mn04_as_mAP_432.pt"),
           "mn05_as": ("mn", 0.5, "mn05_as_mAP_443.pt"),
           "dymn04_as": ("dymn", 0.4, "dymn04_as.pt")}
EXPORT = ["mn04_as", "mn05_as"]
OPSET = 13
WINDOW_FRAMES = 100  # 1 s at 32 kHz after pre-emphasis and a centred STFT with hop 320
GENERIC = {"Music", "Musical instrument"}  # parent classes that say nothing about bowl or not


def load_repo():
    """Imports EfficientAT from the clone (it needs the clone as working directory)."""
    os.chdir(REPO)
    sys.path.insert(0, str(REPO))
    import torch
    from helpers.utils import labels
    from models.dymn.model import get_model as get_dymn
    from models.mn.model import get_model as get_mn
    from models.preprocess import AugmentMelSTFT
    with contextlib.redirect_stdout(io.StringIO()):  # it prints the fmax note
        mel = AugmentMelSTFT(n_mels=128, sr=RATE, win_length=800, hopsize=320).eval()
    return torch, labels, get_mn, get_dymn, mel


def build(name, get_mn, get_dymn):
    kind, width, _ = WEIGHTS[name]
    with contextlib.redirect_stdout(io.StringIO()):  # get_model prints the whole network
        if kind == "dymn":
            m = get_dymn(width_mult=width, pretrained_name=name)
        else:
            m = get_mn(width_mult=width, pretrained_name=name, head_type="mlp")
    return m.eval()


class Logits:
    """Wraps the model so it returns only the 527 logits (the export target)."""

    def __init__(self, torch, model):
        class W(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.m = model

            def forward(self, x):
                return self.m(x)[0]
        self.module = W().eval()


def verdict(label: str) -> str:
    return "bowl" if label in BOWL_CLASSES else "other"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="+", type=Path)
    ap.add_argument("--models", default=",".join(WEIGHTS))
    ap.add_argument("--no-export", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    names = [n for n in a.models.split(",") if n]
    clips = sorted(p for d in a.dirs for p in d.iterdir() if p.suffix.lower() in AUDIO)
    items = [(f"tone {n} {hz}", None, hz) for n, hz in TONES] + [(p.name, p, None) for p in clips]
    if a.dry_run:
        for name, _, _ in items:
            print(f"  {name:45} expect {expected(name)}")
        for n in names:
            w = REPO / "resources" / WEIGHTS[n][2]
            print(f"  {n:10} weights {'ok' if w.exists() else 'MISSING'} {w}")
        print(f"(dry run) {len(items)} items, models {names}, export "
              f"{[] if a.no_export else [m for m in EXPORT if m in names]}, nothing loaded or written")
        return

    torch, labels, get_mn, get_dymn, mel = load_repo()
    wavs = {name: pcm16k(path, hz, RATE) for name, path, hz in items}
    specs = {}
    with torch.no_grad():
        for name, w in wavs.items():
            specs[name] = mel(torch.from_numpy(np.ascontiguousarray(w))[None, :]).unsqueeze(0)
    OUT.mkdir(parents=True, exist_ok=True)
    summary = {}
    for n in names:
        model = build(n, get_mn, get_dymn)
        params = sum(p.numel() for p in model.parameters())
        rows = []
        with torch.no_grad():
            model(specs[items[0][0]])  # warm-up
            for name, path, hz in items:
                x = torch.from_numpy(np.ascontiguousarray(wavs[name]))[None, :]
                runs = []
                for _ in range(3):
                    t0 = time.perf_counter()
                    logits, _ = model(mel(x).unsqueeze(0))
                    runs.append((time.perf_counter() - t0) * 1000)
                p = torch.sigmoid(logits.float())[0].numpy()
                order = np.argsort(p)[::-1]
                top3 = [[labels[i], round(float(p[i]), 3)] for i in order[:3]]
                spec_i = next(i for i in order if labels[i] not in GENERIC)
                with torch.no_grad():
                    p1 = torch.sigmoid(model(specs[name][..., :WINDOW_FRAMES])[0])[0].numpy()
                exp = expected(name)
                v = verdict(labels[order[0]])
                rows.append({"clip": name, "expect": exp, "label": labels[order[0]], "top3": top3,
                             "score": round(float(p[order[0]]), 3), "ms": round(min(runs), 1),
                             "verdict": v, "right": v == exp,
                             "specific_label": labels[spec_i],
                             "specific_right": verdict(labels[spec_i]) == exp,
                             "label_1s": labels[int(np.argmax(p1))],
                             "right_1s": verdict(labels[int(np.argmax(p1))]) == exp})
        (OUT / f"efficientat_{n}.json").write_text(json.dumps(rows, indent=1, ensure_ascii=False))
        scored = [r for r in rows if r["expect"]]
        summary[n] = {"right": sum(r["right"] for r in scored), "total": len(scored),
                      "specific": sum(r["specific_right"] for r in scored),
                      "first_1s": sum(r["right_1s"] for r in scored),
                      "ms": statistics.median(r["ms"] for r in rows), "params": params,
                      "mb": params * 4 / 1e6}
        s = summary[n]
        print(f"\n== {n}: right {s['right']}/{s['total']} (top-1)  | ignoring Music/Musical instrument "
              f"{s['specific']}/{s['total']} | first 1 s only {s['first_1s']}/{s['total']}")
        print(f"   median {s['ms']:.1f} ms per 2 s clip (mel + model, Mac CPU), "
              f"{params / 1e6:.2f} M parameters, {s['mb']:.1f} MB float32")
        for r in rows:
            if not r["right"]:
                print(f"   NO  {r['clip'][:44]:44} {r['expect']:5} top3 " +
                      ", ".join(f"{lb} {sc:.2f}" for lb, sc in r["top3"]))
        print(f"   {OUT / f'efficientat_{n}.json'}")

        if a.no_export or n not in EXPORT:
            continue
        onnx_path = MODELS_OUT / f"efficientat_{n}_1s.onnx"
        wrapped = Logits(torch, model).module
        dummy = torch.zeros(1, 1, 128, WINDOW_FRAMES)
        torch.onnx.export(wrapped, (dummy,), str(onnx_path), opset_version=OPSET, dynamo=False,
                          input_names=["logmel"], output_names=["logits"])
        import onnxruntime as ort
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        checks = [items[0][0]] + [r["clip"] for r in rows if r["expect"] == "bowl"][-1:] + \
                 [r["clip"] for r in rows if r["expect"] == "other"][:1]
        ort_ms = []
        for name in checks:
            x = specs[name][..., :WINDOW_FRAMES].contiguous()
            with torch.no_grad():
                pt = wrapped(x)[0].numpy()
            t0 = time.perf_counter()
            ox = sess.run(None, {"logmel": x.numpy()})[0][0]
            ort_ms.append((time.perf_counter() - t0) * 1000)
            same = int(np.argmax(pt)) == int(np.argmax(ox))
            print(f"   onnx {'same' if same else 'DIFF'} top {labels[int(np.argmax(ox))]:22} "
                  f"max |dlogit| {float(np.max(np.abs(pt - ox))):.2e}  {name[:40]}")
        print(f"   {onnx_path} ({onnx_path.stat().st_size / 1e6:.1f} MB), input logmel [1, 1, 128, "
              f"{WINDOW_FRAMES}], output logits [1, 527], opset {OPSET}, ORT {min(ort_ms):.1f} ms per 1 s")

    print("\nmodel      right  no-generic  1s-only  ms/2s   params   float MB")
    for n, s in summary.items():
        print(f"{n:10} {s['right']:2}/{s['total']:<3} {s['specific']:2}/{s['total']:<7} "
              f"{s['first_1s']:2}/{s['total']:<4} {s['ms']:6.1f} {s['params'] / 1e6:6.2f} M {s['mb']:7.1f}")


if __name__ == "__main__":
    main()
