#!/usr/bin/env python3
"""Measures how fast and how well OpenAI's audio models recognise a bowl, against the DSP.

What: cuts each clip to the window the device would hear (2 s after the strike), sends it to
      OpenAI's fastest audio models and asks for the instrument, the note and the frequency:
        realtime   gpt-realtime-2.1-mini over one open WebSocket (the way a device would stream);
                   timed from the moment the audio is committed to the first and the last token
        chat       gpt-audio-mini through /v1/chat/completions (one HTTP request per clip)
      Pure tones with a known frequency are made here and give the pitch error in cents; the
      recordings give the instrument answer. Writes report.json and prints a table.
Why:  owner 25.10.2026: "könnten wir auch eine AI-basierte Detection machen und wie schnell wär
      das". The DSP on the board locks in 0.13 s (Strike) or 0.38 s (Flow) and never leaves the
      room; this shows what a cloud model adds or costs.
Pitfalls:
  * Costs money: a 2 s clip is a few dozen audio tokens (well under a cent per clip on the mini
    models). OPENAI_API_KEY comes from the environment only (a login shell), never printed.
  * The WebSocket is opened once and kept, as a device would; its setup time is reported apart.
  * Realtime audio must be 24 kHz mono PCM16; ffmpeg converts every clip.
Usage:  /bin/zsh -lc 'python3 tools/ai_detect_bench.py CLIP_DIR [CLIP_DIR ...]'
        tools/ai_detect_bench.py --dry-run CLIP_DIR    lists the clips and tones, calls nothing
Needs:  ffmpeg, the `websockets` package (e.g. ~/Coding/homeassistant/.venv/bin/python).
"""
import argparse
import asyncio
import base64
import io
import json
import math
import os
import struct
import subprocess
import sys
import time
import urllib.request
import wave
from pathlib import Path

RATE = 24000
OFFSET, LENGTH = 0.3, 2.0
REALTIME_MODEL = "gpt-realtime-2.1-mini"
CHAT_MODEL = "gpt-audio-mini"
OUT = Path.home() / "Library/Application Support/LeopardCode/chakra/ai_bench"
AUDIO = {".wav", ".ogg", ".flac", ".mp3", ".m4a"}
PROMPT = ("You hear a short sound recorded in a sound healing session. Reply with JSON only, no "
          'prose: {"instrument": one of "singing bowl", "gong", "drum", "rainstick", "shaker", '
          '"bell", "voice", "pure tone", "other"; "note": the strongest pitch as note name and '
          'octave like "A4", or null; "hz": its frequency in Hz as a number, or null}')
# Pure tones: the C-major chakra notes at A4 = 440 Hz, with soft harmonics like a bowl.
TONES = [("C4", 261.63), ("D4", 293.66), ("E4", 329.63), ("G4", 392.00), ("A4", 440.00), ("B4", 493.88)]


def pcm_of(path: Path) -> bytes:
    """24 kHz mono PCM16 of the window after the strike."""
    return subprocess.run(["ffmpeg", "-v", "error", "-ss", str(OFFSET), "-t", str(LENGTH), "-i", str(path),
                           "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
                          capture_output=True, check=True).stdout


def tone_pcm(hz: float) -> bytes:
    n = int(RATE * LENGTH)
    out = bytearray()
    for i in range(n):
        t = i / RATE
        v = sum(a * math.sin(2 * math.pi * hz * k * t) for k, a in ((1, 1.0), (2, 0.35), (3, 0.15)))
        out += struct.pack("<h", int(9000 * v * math.exp(-0.4 * t)))
    return bytes(out)


def wav_of(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def parse(text: str) -> dict:
    s = text[text.find("{"):text.rfind("}") + 1] if "{" in text else ""
    try:
        return json.loads(s)
    except ValueError:
        return {"raw": text[:120]}


def cents(hz, ref) -> float | None:
    try:
        return round(1200 * math.log2(float(hz) / ref), 1)
    except (TypeError, ValueError, ZeroDivisionError):
        return None


async def realtime_session(key: str):
    import websockets
    t0 = time.perf_counter()
    ws = await websockets.connect(f"wss://api.openai.com/v1/realtime?model={REALTIME_MODEL}",
                                  additional_headers={"Authorization": f"Bearer {key}"}, max_size=None)
    await ws.send(json.dumps({"type": "session.update", "session": {
        "type": "realtime", "output_modalities": ["text"], "instructions": PROMPT,
        "audio": {"input": {"format": {"type": "audio/pcm", "rate": RATE}, "turn_detection": None}}}}))
    while True:
        m = json.loads(await ws.recv())
        if m["type"] == "session.updated":
            break
        if m["type"] == "error":
            raise RuntimeError(m["error"].get("message"))
    return ws, time.perf_counter() - t0


async def realtime_ask(ws, pcm: bytes) -> dict:
    await ws.send(json.dumps({"type": "input_audio_buffer.clear"}))
    for i in range(0, len(pcm), 32000):
        await ws.send(json.dumps({"type": "input_audio_buffer.append",
                                  "audio": base64.b64encode(pcm[i:i + 32000]).decode()}))
    t0 = time.perf_counter()
    await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
    await ws.send(json.dumps({"type": "response.create", "response": {"conversation": "none"}}))
    first, text = None, ""
    while True:
        m = json.loads(await ws.recv())
        kind = m["type"]
        if kind in ("response.output_text.delta", "response.text.delta"):
            first = first or time.perf_counter() - t0
            text += m.get("delta", "")
        elif kind == "response.done":
            return {"first_s": round(first or 0, 3), "done_s": round(time.perf_counter() - t0, 3), **parse(text)}
        elif kind == "error":
            return {"error": m["error"].get("message")}


def chat_ask(key: str, pcm: bytes) -> dict:
    body = {"model": CHAT_MODEL, "modalities": ["text"], "messages": [{"role": "user", "content": [
        {"type": "text", "text": PROMPT},
        {"type": "input_audio", "input_audio": {"data": base64.b64encode(wav_of(pcm)).decode(), "format": "wav"}}]}]}
    req = urllib.request.Request("https://api.openai.com/v1/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        d = json.load(urllib.request.urlopen(req, timeout=60))
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}"}
    return {"done_s": round(time.perf_counter() - t0, 3), **parse(d["choices"][0]["message"].get("content") or "")}


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="+", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    clips = sorted(p for d in a.dirs for p in d.iterdir() if p.suffix.lower() in AUDIO)
    items = [(f"tone {n} {hz}", None, hz) for n, hz in TONES] + [(p.name, p, None) for p in clips]
    if a.dry_run:
        print("\n".join(f"  {name}" for name, _, _ in items))
        print(f"(dry run) {len(items)} items x 2 models, nothing sent")
        return
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not key:
        sys.exit("OPENAI_API_KEY not set: start in a login shell")
    ws, setup = await realtime_session(key)
    print(f"realtime WebSocket open in {setup:.2f} s ({REALTIME_MODEL})")
    rows, t_start = [], time.perf_counter()
    for i, (name, path, hz) in enumerate(items, 1):
        pcm = tone_pcm(hz) if hz else pcm_of(path)
        rt = await realtime_ask(ws, pcm)
        ch = chat_ask(key, pcm)
        row = {"clip": name, "true_hz": hz, "realtime": rt, "chat": ch}
        if hz:
            row["realtime"]["cents"] = cents(rt.get("hz"), hz)
            row["chat"]["cents"] = cents(ch.get("hz"), hz)
        rows.append(row)
        left = (time.perf_counter() - t_start) / i * (len(items) - i)
        print(f"[{i}/{len(items)} {100 * i // len(items)}% ~{left:.0f}s left] {name[:38]:38} "
              f"rt {rt.get('first_s', '-')}/{rt.get('done_s', '-')} s {rt.get('instrument', rt.get('error', '?'))} "
              f"{rt.get('note')} {rt.get('hz')} | chat {ch.get('done_s', '-')} s {ch.get('instrument', ch.get('error', '?'))} "
              f"{ch.get('note')} {ch.get('hz')}", flush=True)
    await ws.close()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "report.json").write_text(json.dumps({"setup_s": setup, "rows": rows}, indent=1))
    for m in ("realtime", "chat"):
        t = sorted(r[m]["done_s"] for r in rows if "done_s" in r[m])
        f = sorted(r[m]["first_s"] for r in rows if r[m].get("first_s"))
        errs = [abs(r[m]["cents"]) for r in rows if r.get("true_hz") and r[m].get("cents") is not None]
        print(f"{m:8} median done {t[len(t) // 2] if t else '-'} s"
              + (f", first token {f[len(f) // 2]} s" if f else "")
              + f", pure tones within 50 ct: {sum(e <= 50 for e in errs)}/{len(TONES)}")
    print(OUT / "report.json")


if __name__ == "__main__":
    asyncio.run(main())
