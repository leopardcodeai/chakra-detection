#!/usr/bin/env python3
"""Takes ChakraA's README screenshots and the demo GIF in the simulator, with synthetic tones only.

What: starts sim.sh, feeds it tools/chakra_feed.py tones and saves through ChakraV's
      tools/sim_shot.py (headless Chrome, the canvas's own 480 x 480 PNG):
        screen_ready.png        before any tone
        screen_heart.png        a steady 174.6 Hz (F3, Heart)
        screen_crown.png        a steady 246.9 Hz (B3, Crown)
        screen_settings.png     the gear's first page
        screen_connections.png  the lamps
        demo.gif                the demo round, root to crown (chakra_feed.py demo), 360 px
      then stops everything it started. Only synthetic tones: no microphone, no recording.
Why:  the README shows the app as it is; the screenshots of 24.10.2026 still showed the old title,
      60 % brightness and the old spectrum (26.10.2026).
Pitfalls:
  * Needs ../chakrav (for sim_shot.py), uv (websockets) and ffmpeg for the GIF.
  * Uses the feed port 8698: not while another ChakraA simulator or the launcher's ChakraA runs.
    The preview's port is read from the simulator's "Preview:" line (3222 may belong to another app).
  * A first build of the backend takes up to a minute; the script waits for "Preview:".
  * A stored setting survives in the simulator's NVS file; the shots show what the app shows.
Usage:  tools/screens.py OUTDIR [--no-gif]
        tools/screens.py OUTDIR --dry-run
        then copy OUTDIR/screen_*.png and OUTDIR/demo.gif to docs/
"""
import argparse
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
SHOT = HERE.parent / "chakrav/tools/sim_shot.py"
FEED = HERE / "tools/chakra_feed.py"
UV = "uv"
GEAR = (444, 30)
TAB_CONNECTIONS = (350, 88)
STILLS = ["wait 8", "shot screen_ready"]


def tree(pid: int) -> list[int]:
    out = subprocess.run(["ps", "-axo", "pid=,ppid="], capture_output=True, text=True).stdout
    kids: dict[int, list[int]] = {}
    for row in out.split("\n"):
        p = row.split()
        if len(p) == 2:
            kids.setdefault(int(p[1]), []).append(int(p[0]))
    found, todo = [], [pid]
    while todo:
        for k in kids.get(todo.pop(), []):
            found.append(k)
            todo.append(k)
    return found


def stop(proc: subprocess.Popen) -> None:
    for sig in (signal.SIGINT, signal.SIGTERM):
        for pid in [proc.pid, *tree(proc.pid)]:
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        try:
            proc.wait(timeout=6)
            return
        except subprocess.TimeoutExpired:
            pass
    proc.kill()


def shoot(url: str, out: Path, steps: list[str]) -> None:
    subprocess.run([UV, "run", "-q", "--python", "3.14", "--with", "websockets", "python", str(SHOT),
                    "--url", url, "--out", str(out), *steps], check=True)


def feed(env: dict, *args: str) -> subprocess.Popen:
    return subprocess.Popen([sys.executable, str(FEED), *args, "--wait", "--port", "8698"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--no-gif", action="store_true", help="the stills only")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.dry_run:
        print(f"would start sim.sh, shoot ready, tone 174.6 (heart), tone 246.9 (crown), settings and "
              f"connections{'' if a.no_gif else ', then the demo round as demo.gif'} into {a.out}")
        return
    if not SHOT.exists():
        sys.exit(f"needs {SHOT} (the chakrav repository next to this one)")
    env = {k: v for k, v in os.environ.items() if not k.endswith("_TOKEN") and not k.endswith("_API_KEY")}
    env = {**env, "BROWSER": "true", "CHAKRA_SIM_SECONDS": "300", "CHAKRA_FEED_PORT": "8698"}
    env.pop("CHAKRA_LIGHT", None)
    a.out.mkdir(parents=True, exist_ok=True)
    log = open(a.out / "sim.log", "w")
    sim = subprocess.Popen(["sh", str(HERE / "sim.sh")], cwd=HERE, env=env, stdout=log, stderr=subprocess.STDOUT)
    feeds: list[subprocess.Popen] = []
    try:
        url, deadline = None, time.time() + 240
        while time.time() < deadline and not url:
            m = re.search(r"^Preview: (http://127\.0\.0\.1:\d+/)", (a.out / "sim.log").read_text(errors="replace"), re.M)
            url = m.group(1) if m else None
            time.sleep(0.5)
        if not url:
            sys.exit("the simulator printed no preview address, see sim.log")
        time.sleep(1.0)
        if "cannot listen" in (a.out / "sim.log").read_text(errors="replace"):
            sys.exit("8698 is taken (another ChakraA simulator?): the shots would show the wrong one")
        shoot(url, a.out, STILLS)
        for hz, name in (("174.6", "screen_heart"), ("246.9", "screen_crown")):
            feeds.append(feed(env, "tone", hz, "--seconds", "12"))
            shoot(url, a.out, ["wait 5", f"shot {name}"])
            feeds[-1].wait(timeout=30)
        shoot(url, a.out, [f"tap {GEAR[0]} {GEAR[1]}", "wait 1.5", "shot screen_settings",
                           f"tap {TAB_CONNECTIONS[0]} {TAB_CONNECTIONS[1]}", "wait 1.5", "shot screen_connections",
                           f"tap {GEAR[0]} {GEAR[1]}", "wait 1.5"])
        if not a.no_gif:
            frames = a.out / "gif"
            frames.mkdir(exist_ok=True)
            steps = ["wait 2"]
            for i in range(86):  # 7 bowls x 4.6 s: a frame every 0.4 s
                steps += ["wait 0.4", f"shot f{i:03d}"]
            # The shooter first, the round once Chrome is up: otherwise the root bowl passes unseen.
            shooter = subprocess.Popen([UV, "run", "-q", "--python", "3.14", "--with", "websockets", "python",
                                        str(SHOT), "--url", url, "--out", str(frames), *steps],
                                       stdout=subprocess.DEVNULL)
            time.sleep(4)
            feeds.append(feed(env, "demo"))
            if shooter.wait(timeout=180) != 0:
                sys.exit("sim_shot.py failed on the demo round")
            subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-framerate", "5", "-i", str(frames / "f%03d.png"),
                            "-vf", "scale=360:360:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];"
                                   "[b][p]paletteuse=dither=bayer", str(a.out / "demo.gif")], check=True)
    finally:
        for p in feeds:
            p.terminate()
        stop(sim)
        log.close()
    print(f"written to {a.out}")


if __name__ == "__main__":
    main()
