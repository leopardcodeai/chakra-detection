#!/usr/bin/env python3
"""Feeds audio into the chakra app's simulator and acts on its light commands.

What: streams signed 16-bit mono 16 kHz audio to the simulator (127.0.0.1:8698, see
      components/app_chakra/host/platform_pc.c) in real time, from
        demo          a round through the seven chakras with singing-bowl-like tones
        tone HZ       one tone (harmonics optional) for --seconds
        mic           the Mac's microphone through ffmpeg (macOS asks for the permission once)
        wav FILE      any file ffmpeg can read
      and prints what the simulator sends back. With --light ENTITY each chakra also colours a
      real lamp through Home Assistant's REST API (HA_URL and HA_TOKEN from the environment,
      never from the command line), at most one command every LAMP_GAP seconds (the newest
      waits, older ones in between are dropped): a Hue bridge behind Home Assistant takes about 10
      commands a second. With --lamp URL each light command goes to a screen lamp
      instead: the protocol line as it came, POSTed to URL/light (a page that fills the screen with
      the colour, e.g. on http://127.0.0.1:3310), so no real lamp lights up; the "reading" lines
      (chakra names, note, Hz, cents) go there too, for the page's text.
      With --ai (mic and wav) the same audio also goes to tools/ai_gate.py (the AI sound gate,
      EfficientAT on the Mac, in the venv AI_VENV), whose verdicts ("sound bowl|other|gong|none
      strike") are forwarded to the simulator's second port (the audio port + 1); they act only
      while the app's "AI sound gate" setting is On.
Why:  the board is on order; this tests the whole chain, microphone to lamp, on the Mac.
Pitfalls:
  * Only one client at a time; a new feeder replaces the old one.
  * The app sends one command per colour change and two per kick or gong swell (one up, one down,
    each with its fade), so the lamp itself fades; the screen lamp page takes every line at once.
  * The mic needs ffmpeg (brew install ffmpeg) and the terminal's microphone permission.
Usage:  chakra_feed.py demo | tone 341 [--seconds 4] | mic | wav FILE  [--light light.x | --lamp URL] [--ai]
                       [--dry-run]
"""
import argparse
import array
import json
import math
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request

RATE, HOP = 16000, 1024
AI_RATE = 32000  # EfficientAT's rate; the app gets every pair of samples averaged down to RATE
AI_VENV = os.path.expanduser("~/Library/Application Support/LeopardCode/chakra/ai_venv_eat/bin/python")
A4 = 440.0  # the app's default reference
LAMP_GAP = 0.125  # seconds between two commands to a real lamp (chakra_light.h CHAKRA_LAMP_GAP_MS)
# Note per chakra in the default note mapping (C, D, E, F, G, A, B), octave 3.
DEMO_NOTES = [0, 2, 4, 5, 7, 9, 11]
NAMES = ["Root", "Sacral", "Solar plexus", "Heart", "Throat", "Third eye", "Crown"]


def note_hz(note, octave=3):
    midi = (octave + 1) * 12 + note
    return A4 * 2 ** ((midi - 69) / 12)


def bowl(freq, seconds, amp=0.35):
    """A struck bowl: fast attack, slow decay, inharmonic partials, a slight beat."""
    partials = [(1.0, 1.0), (2.71, 0.45), (5.15, 0.18)]
    n = int(seconds * RATE)
    out = bytearray()
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / 0.01) * math.exp(-t / (seconds * 0.6))
        s = sum(a * math.sin(2 * math.pi * freq * r * t + 0.3 * math.sin(2 * math.pi * 1.5 * t)) for r, a in partials)
        v = int(max(-1.0, min(1.0, amp * env * s / 1.6)) * 32767)
        out += v.to_bytes(2, "little", signed=True)
    return bytes(out)


def silence(seconds):
    return bytes(2 * int(seconds * RATE))


def paced(sock, data):
    """Sends in hops at the real rate."""
    step = HOP * 2
    t0 = time.monotonic()
    for k in range(0, len(data), step):
        sock.sendall(data[k:k + step])
        delay = t0 + (k // step + 1) * HOP / RATE - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def set_lamp(entity, line):
    url = os.environ.get("HA_URL")
    token = os.environ.get("HA_TOKEN")
    if not url or not token:
        print("  (no HA_URL/HA_TOKEN in the environment, lamp not set)", flush=True)
        return
    parts = line.split("\t")
    _, chakra, rgb, bright = parts[:4]
    transition = float(parts[4]) if len(parts) > 4 else 0.6
    if int(chakra) < 0:
        return  # silence: the lamp keeps its colour, as on the board
    body = {"entity_id": entity, "rgb_color": [int(rgb[0:2], 16), int(rgb[2:4], 16), int(rgb[4:6], 16)],
            "brightness_pct": int(bright), "transition": transition}
    req = urllib.request.Request(url.rstrip("/") + "/api/services/light/turn_on", data=json.dumps(body).encode(),
                                 headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            print(f"  lamp {entity}: HTTP {r.status}", flush=True)
    except OSError as e:
        print(f"  lamp {entity}: {e}", flush=True)


class Paced:
    """Sends to a real lamp from its own thread, at most one command every LAMP_GAP seconds: the
    newest line waits, any older one still waiting is dropped (only the latest state matters)."""

    def __init__(self, send):
        self.send, self.line, self.cv = send, None, threading.Condition()
        threading.Thread(target=self.run, daemon=True).start()

    def put(self, line):
        with self.cv:
            self.line = line
            self.cv.notify()

    def run(self):
        last = 0.0
        while True:
            with self.cv:
                while self.line is None:
                    self.cv.wait()
            time.sleep(max(0.0, last + LAMP_GAP - time.monotonic()))
            with self.cv:
                line, self.line = self.line, None
            last = time.monotonic()
            self.send(line)


def set_virtual(url, line):
    """The protocol line as it came, to the screen lamp page (POST /light); nothing in the house."""
    req = urllib.request.Request(url.rstrip("/") + "/light", data=line.encode(), method="POST",
                                 headers={"Content-Type": "text/plain"})
    try:
        with urllib.request.urlopen(req, timeout=2) as r:
            r.read()
    except OSError as e:
        print(f"  screen lamp {url}: {e}", flush=True)


def listen(sock, entity, lamp=None):
    buf = b""
    real = Paced(lambda line: set_lamp(entity, line)) if entity and not lamp else None
    while True:
        try:
            chunk = sock.recv(4096)
        except OSError:
            return
        if not chunk:
            return
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode(errors="replace")
            if line.startswith("light\t"):
                c = int(line.split("\t")[1])
                print(f"  light: {NAMES[c] if 0 <= c < len(NAMES) else 'silence' if c < 0 else 'fixed colour'}  ({line.split(chr(9), 2)[2].replace(chr(9), ', ')})",
                      flush=True)
                if lamp:
                    set_virtual(lamp, line)
                elif real:
                    real.put(line)
            elif (line.startswith("reading\t") or line.startswith("bars\t")) and lamp:
                set_virtual(lamp, line)  # names, note, Hz, cents and the spectrum for the lamp page
            elif line == "close":
                print("  closed: top bar held 3 s, the board would go back to Vibe Mode now", flush=True)


def ffmpeg(args, rate=RATE):
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg missing: brew install ffmpeg")
    return subprocess.Popen(["ffmpeg", "-loglevel", "error", *args, "-ac", "1", "-ar", str(rate), "-f", "s16le", "-"],
                            stdout=subprocess.PIPE)


def start_gate(port):
    """tools/ai_gate.py with its stdout forwarded line by line to the simulator's AI port; None (and
    the feeder runs on without it) when the venv is missing."""
    if not os.path.exists(AI_VENV):
        print(f"no AI venv at {AI_VENV}: running without the local AI sound gate", flush=True)
        return None
    gate = subprocess.Popen([AI_VENV, os.path.join(os.path.dirname(os.path.abspath(__file__)), "ai_gate.py")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def forward():
        conn = None
        for line in gate.stdout:
            for _ in range(2):
                try:
                    conn = conn or socket.create_connection(("127.0.0.1", port), timeout=5)
                    conn.sendall(line)
                    break
                except OSError:
                    conn = None
            text = line.decode().strip().split("\t")
            if len(text) == 3 and text[2] == "1":
                print(f"  AI: strike ({text[1]})", flush=True)
    threading.Thread(target=forward, daemon=True).start()
    return gate


def halve(data: bytes) -> bytes:
    """32 kHz to 16 kHz: the mean of each pair (a gentle low-pass, enough below the app's 1100 Hz)."""
    x = array.array("h", data[:len(data) // 4 * 4])
    y = array.array("h", ((x[i] + x[i + 1]) // 2 for i in range(0, len(x), 2)))
    return y.tobytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=["demo", "tone", "mic", "wav"])
    ap.add_argument("arg", nargs="?", help="Hz for tone, file for wav")
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--light", help="a Home Assistant light entity to colour, e.g. light.wohnzimmer")
    ap.add_argument("--lamp", metavar="URL", help="a screen lamp page instead of a real lamp, e.g. http://127.0.0.1:3310")
    ap.add_argument("--loop", action="store_true", help="wav: play the file again and again (a looped test video)")
    ap.add_argument("--ai", action="store_true", help="also run the AI sound gate (mic and wav only)")
    ap.add_argument("--port", type=int, default=int(os.environ.get("CHAKRA_FEED_PORT", "8698")))
    ap.add_argument("--speaker", metavar="FLAGFILE",
                    help="wav: also play the sound on the Mac's speakers while FLAGFILE exists (the launcher's Sound button)")
    ap.add_argument("--wait", action="store_true", help="retry for 2 min until the simulator listens")
    ap.add_argument("--dry-run", action="store_true", help="say what would be sent, connect to nothing")
    a = ap.parse_args()
    if a.mode == "tone" and not a.arg:
        ap.error("tone needs a frequency")
    if a.mode == "wav" and not a.arg:
        ap.error("wav needs a file")
    if a.dry_run:
        plan = {"demo": f"{a.rounds} round(s): " + ", ".join(f"{n} {note_hz(k):.1f} Hz" for n, k in zip(NAMES, DEMO_NOTES)),
                "tone": f"{a.arg} Hz for {a.seconds} s", "mic": "the default microphone until Ctrl-C",
                "wav": f"{a.arg} in real time"}[a.mode]
        print(f"would send to 127.0.0.1:{a.port}: {plan}" + (f"; screen lamp {a.lamp}" if a.lamp else
                                                              f"; lamp {a.light}" if a.light else ""))
        return
    for attempt in range(120 if a.wait else 1):
        try:
            sock = socket.create_connection(("127.0.0.1", a.port), timeout=5)
            break
        except OSError:
            if attempt == (119 if a.wait else 0):
                sys.exit(f"no simulator on 127.0.0.1:{a.port} (./sim.sh first)")
            time.sleep(1)
    sock.settimeout(None)
    threading.Thread(target=listen, args=(sock, None if a.lamp else a.light, a.lamp), daemon=True).start()
    try:
        if a.mode == "demo":
            for _ in range(a.rounds):
                for name, note in zip(NAMES, DEMO_NOTES):
                    f = note_hz(note)
                    print(f"{name}: {f:.1f} Hz", flush=True)
                    paced(sock, bowl(f, 4.0) + silence(0.6))
            paced(sock, silence(2.0))
        elif a.mode == "tone":
            paced(sock, bowl(float(a.arg), a.seconds))
            paced(sock, silence(2.0))
        else:
            src = ["-f", "avfoundation", "-i", ":0"] if a.mode == "mic" else \
                ["-re", *(["-stream_loop", "-1"] if a.loop else []), "-i", a.arg, "-vn"]
            gate = start_gate(a.port + 1) if a.ai else None
            halving = gate is not None  # the rate stays what ffmpeg was started with, even if the gate dies
            proc = ffmpeg(src, AI_RATE if halving else RATE)
            # The same samples to the Mac's speakers (owner 26.10.2026: "dass ich den Sound auch höre, bzw.
            # an/aus schalten kann"): in step with the picture, silence while the flag file is gone.
            speaker = None
            if a.speaker and a.mode == "wav" and shutil.which("ffplay"):
                speaker = subprocess.Popen(["ffplay", "-loglevel", "error", "-nodisp", "-autoexit", "-f", "s16le",
                                            "-ch_layout", "mono", "-ar", str(AI_RATE if halving else RATE), "-i", "-"],
                                           stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            heard_on, checked = False, 0.0
            print(("microphone open, Ctrl-C stops" if a.mode == "mic" else f"playing {a.arg}")
                  + (" (with the AI sound gate)" if gate else ""), flush=True)
            while True:
                data = proc.stdout.read(HOP * 2 * (2 if halving else 1))
                if not data:
                    break
                if gate:
                    try:
                        gate.stdin.write(data)
                        gate.stdin.flush()
                    except OSError:
                        gate = None  # the gate ended: the app goes on with the detection alone
                if speaker:
                    if time.monotonic() - checked > 0.3:
                        heard_on, checked = os.path.exists(a.speaker), time.monotonic()
                    try:
                        speaker.stdin.write(data if heard_on else bytes(len(data)))
                    except OSError:
                        speaker = None  # the player ended: the app goes on without the speakers
                if halving:
                    data = halve(data)
                sock.sendall(data)
    except KeyboardInterrupt:
        pass
    time.sleep(0.5)
    sock.close()


if __name__ == "__main__":
    main()
