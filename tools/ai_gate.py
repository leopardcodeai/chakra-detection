#!/usr/bin/env python3
"""The AI sound gate on the Mac: hears the same audio as the app and says bowl, other or nothing.

What: reads raw signed 16-bit mono audio at 32 kHz on stdin (chakra_feed.py --ai pipes it in) and
      prints one line per verdict for the app's simulator (chakra_feed.py forwards it):
        sound<TAB>bowl|other|none<TAB><strike 0|1>
      Every 0.5 s it judges the last second with EfficientAT (mn05_as, AudioSet) and the small
      trained head from tools/bench/gate_bench.py (--save-head writes it to HEAD): bowl-like sounds
      let the colour follow the pitch, anything else keeps it. Between verdicts a jump in loudness
      (about 12 dB above the last half second) is a strike: it goes out at once with the last
      verdict, so a gong or a drum kicks the light without waiting for the model.
      Without the head file it falls back to the pretrained rule (best bowl-like class against the
      best other class), which the benchmark showed to be weaker on gongs.
Why:  owner 25.10.2026: "die App mit AI on/off", "smoothere Übergänge, und wenn doch so ein
      Schlag kommt, Helligkeit und Farbe als Kick reinbringen". On the board the same job is meant
      for mn05 w8a16 on the ESP32-S31 through ESP-DL (estimated 30 to 60 ms per window, not yet
      measured: the board is on order).
Pitfalls:
  * Needs the venv with torch (~/Library/Application Support/LeopardCode/chakra/ai_venv_eat) and
    the EfficientAT clone next to it; chakra_feed.py finds both.
  * The first verdict comes after one second of audio; until then the gate says none.
  * Quiet input (below SILENCE_DB) is "none" and never a strike.
Usage:  ffmpeg ... -ar 32000 -ac 1 -f s16le - | ai_gate.py            (normally via chakra_feed.py --ai)
        ai_gate.py --dry-run    says which model and head it would load
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_efficientat as eat  # noqa: E402
from classifier_bench import BOWL_CLASSES  # noqa: E402

STATE = Path.home() / "Library/Application Support/LeopardCode/chakra"
HEAD = STATE / "ai_models/gate_head_mn05.json"
MODEL = "mn05_as"
RATE = eat.RATE  # 32 kHz
WINDOW, STEP_S, CHUNK_S = 1.0, 0.5, 0.05
SILENCE_DB, STRIKE_DB = -55.0, 12.0
BOWL = set(BOWL_CLASSES) | {"Singing bowl", "Ding", "Chime", "Bell", "Tuning fork", "Sine wave"}
OTHER = {"Gong", "Drum", "Bass drum", "Snare drum", "Drum kit", "Tabla", "Percussion", "Rattle (instrument)",
         "Maraca", "Shaker", "Rain", "Speech", "Singing", "Chant", "Humming", "Timpani", "Tambourine"}


def db(x: np.ndarray) -> float:
    return 20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-9)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.dry_run:
        print(f"model {MODEL}, head {HEAD if HEAD.exists() else 'none (pretrained rule)'}")
        return
    torch, labels, get_mn, get_dymn, mel = eat.load_repo()
    model = eat.build(MODEL, get_mn, get_dymn)
    head = json.loads(HEAD.read_text()) if HEAD.exists() else None
    if head:
        coef, bias = np.array(head["coef"]), float(head["intercept"])
        mean, scale = np.array(head["mean"]), np.array(head["scale"])
    bowl_i = [i for i, l in enumerate(labels) if l in BOWL]
    other_i = [i for i, l in enumerate(labels) if l in OTHER]
    size, chunk = int(RATE * WINDOW), int(RATE * CHUNK_S)
    buf = np.zeros(0, dtype=np.float32)
    since, verdict, levels = 0, "none", []
    out = sys.stdout
    while True:
        raw = sys.stdin.buffer.read(chunk * 2)
        if not raw:
            return
        x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
        buf = np.concatenate([buf, x])[-size:]
        since += len(x)
        level = db(x)
        recent = levels[-10:]
        if recent and level > SILENCE_DB and level - float(np.median(recent)) > STRIKE_DB:
            out.write(f"sound\t{verdict}\t1\n")
            out.flush()
        levels = (levels + [level])[-20:]
        if since < int(RATE * STEP_S) or len(buf) < size:
            continue
        since = 0
        if db(buf) < SILENCE_DB:
            verdict = "none"
        else:
            with torch.no_grad():
                logits, emb = model(mel(torch.from_numpy(buf.copy())[None, :]).unsqueeze(0))
            if head:
                z = (emb[0].numpy().ravel() - mean) / scale
                verdict = "bowl" if float(z @ coef + bias) > 0 else "other"
            else:
                p = torch.sigmoid(logits)[0].numpy()
                verdict = "bowl" if p[bowl_i].max() >= p[other_i].max() else "other"
        out.write(f"sound\t{verdict}\t0\n")
        out.flush()


if __name__ == "__main__":
    main()
