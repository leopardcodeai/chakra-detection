#!/usr/bin/env python3
"""Measures the Audio Spectrogram Transformer (AST) as the upper bound for the chakra sound gate.

What: asks MIT/ast-finetuned-audioset-10-10-0.4593 (AudioSet, 527 classes) what it hears in the
      same 2 s window classifier_bench.py cuts (pcm16k: 2 s after 0.3 s, plus the synthetic tones),
      maps its top class to "bowl" (in BOWL_CLASSES: the lamp colour follows the pitch) or "other"
      (the colour stays), and prints right/total, the top 3 per clip and ms per 2 s clip.
      With --model clap it runs classifier_bench's Clap class through the same loop, so both
      models write rows of one schema: {clip, expect, label, top3, score, ms, verdict, right}.
      Rows go to ~/Library/Application Support/LeopardCode/chakra/ai_bench/<model>.json.
Why:  owner 25.10.2026: four model tests in parallel for the gate; AST and CLAP are far too large
      for the ESP32-S31 and show what a big model on the Mac or the house Pi would add.
Pitfalls:
  * Times are the Mac's CPU (torch, one thread pool, no MPS); the first clip includes warm-up and
    is run twice, the second time counts.
  * AST pads every input to 1024 frames (10.24 s), so a 2 s clip costs as much as a 10 s one.
  * AST is multi-label (sigmoid scores, they do not sum to 1); CLAP's are a softmax over our labels.
  * Weights come from the Hugging Face cache (AST about 350 MB, CLAP about 600 MB on first run).
  * Only the top class decides; a bowl class in place 2 or 3 does not count.
Usage:  AI_VENV/bin/python tools/bench_ast.py CLIP_DIR [...] [--model ast|clap] [--dry-run]
        with AI_VENV = ~/Library/Application Support/LeopardCode/chakra/ai_venv_big
        (torch, transformers, numpy); ffmpeg must be in PATH.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from classifier_bench import AUDIO, BOWL_CLASSES, OUT, TONES, expected, pcm16k  # noqa: E402

AST_ID = "MIT/ast-finetuned-audioset-10-10-0.4593"


class Ast:
    RATE = 16000

    def __init__(self):
        import torch
        from transformers import ASTFeatureExtractor, ASTForAudioClassification
        self.torch = torch
        self.fe = ASTFeatureExtractor.from_pretrained(AST_ID)
        self.model = ASTForAudioClassification.from_pretrained(AST_ID).eval()
        self.labels = self.model.config.id2label
        self.top3 = []

    def ask(self, wav) -> tuple[str, float, float]:
        t0 = time.perf_counter()
        with self.torch.no_grad():
            x = self.fe(wav, sampling_rate=self.RATE, return_tensors="pt")
            p = self.model(**x).logits.sigmoid()[0]
        ms = (time.perf_counter() - t0) * 1000
        s, i = p.topk(3)
        self.top3 = [[self.labels[int(k)], round(float(v), 3)] for v, k in zip(s, i)]
        return self.top3[0][0], self.top3[0][1], ms

    def verdict(self, label: str) -> str:
        return "bowl" if label in BOWL_CLASSES else "other"


def size_of(model) -> tuple[int, float]:
    """Parameters and their size in MB as stored (fp32)."""
    n = sum(p.numel() for p in model.parameters())
    mb = sum(p.numel() * p.element_size() for p in model.parameters()) / 1e6
    return n, mb


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="+", type=Path)
    ap.add_argument("--model", choices=["ast", "clap"], default="ast")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    clips = sorted(p for d in a.dirs for p in d.iterdir() if p.suffix.lower() in AUDIO)
    items = [(f"tone {n} {hz}", None, hz) for n, hz in TONES] + [(p.name, p, None) for p in clips]
    out = OUT / f"{a.model}.json"
    if a.dry_run:
        for name, _, _ in items:
            print(f"  {name:45} expect {expected(name)}")
        print(f"(dry run) {len(items)} items, model {a.model}, would write {out}")
        return
    t0 = time.perf_counter()
    if a.model == "ast":
        model = Ast()
    else:
        from classifier_bench import Clap
        model = Clap()
    n, mb = size_of(model.model)
    print(f"{a.model} loaded in {time.perf_counter() - t0:.1f} s, {n / 1e6:.1f} M parameters, "
          f"{mb:.0f} MB fp32", flush=True)
    rows = []
    for i, (name, path, hz) in enumerate(items, 1):
        wav = pcm16k(path, hz, model.RATE)
        if i == 1:
            model.ask(wav)  # warm-up
        label, score, ms = model.ask(wav)
        v = model.verdict(label)
        exp = expected(name)
        rows.append({"clip": name, "expect": exp, "label": label, "top3": model.top3,
                     "score": round(score, 3), "ms": round(ms, 1), "verdict": v, "right": v == exp})
        top = ", ".join(f"{lbl} {s:.2f}" for lbl, s in model.top3)
        print(f"[{i}/{len(items)}] {'ok' if v == exp else 'NO'} {name[:38]:38} {str(exp):5} "
              f"{ms:5.0f} ms | {top}", flush=True)
    OUT.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(rows, indent=1, ensure_ascii=False))
    scored = [r for r in rows if r["expect"]]
    ms = sorted(r["ms"] for r in rows)
    print(f"{a.model}: right {sum(r['right'] for r in scored)}/{len(scored)}, "
          f"median {ms[len(ms) // 2]} ms per 2 s clip on this Mac (CPU)")
    for r in scored:
        if not r["right"]:
            print(f"  wrong: {r['clip']} expect {r['expect']}, heard {r['label']}")
    print(out)


if __name__ == "__main__":
    main()
