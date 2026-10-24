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
      never from the command line).
Why:  the board is on order; this tests the whole chain, microphone to lamp, on the Mac.
Pitfalls:
  * Only one client at a time; a new feeder replaces the old one.
  * The mic needs ffmpeg (brew install ffmpeg) and the terminal's microphone permission.
Usage:  chakra_feed.py demo | tone 341 [--seconds 4] | mic | wav FILE  [--light light.x] [--dry-run]
"""
import argparse
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
A4 = 440.0  # the app's default reference
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


def listen(sock, entity):
    buf = b""
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
                print(f"  light: {NAMES[c] if c >= 0 else 'silence'}  ({line.split(chr(9), 2)[2].replace(chr(9), ', ')})",
                      flush=True)
                if entity:
                    set_lamp(entity, line)
            elif line == "close":
                print("  closed: top bar held 3 s, the board would go back to Vibe Mode now", flush=True)


def ffmpeg(args):
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg missing: brew install ffmpeg")
    return subprocess.Popen(["ffmpeg", "-loglevel", "error", *args, "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
                            stdout=subprocess.PIPE)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=["demo", "tone", "mic", "wav"])
    ap.add_argument("arg", nargs="?", help="Hz for tone, file for wav")
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--light", help="a Home Assistant light entity to colour, e.g. light.wohnzimmer")
    ap.add_argument("--port", type=int, default=int(os.environ.get("CHAKRA_FEED_PORT", "8698")))
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
        print(f"would send to 127.0.0.1:{a.port}: {plan}" + (f"; lamp {a.light}" if a.light else ""))
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
    threading.Thread(target=listen, args=(sock, a.light), daemon=True).start()
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
            proc = ffmpeg(["-f", "avfoundation", "-i", ":0"] if a.mode == "mic" else ["-re", "-i", a.arg])
            print("microphone open, Ctrl-C stops" if a.mode == "mic" else f"playing {a.arg}", flush=True)
            while True:
                data = proc.stdout.read(HOP * 2)
                if not data:
                    break
                sock.sendall(data)
    except KeyboardInterrupt:
        pass
    time.sleep(0.5)
    sock.close()


if __name__ == "__main__":
    main()
