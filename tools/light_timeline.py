#!/usr/bin/env python3
"""Measures how the light would change over a recording: changes per minute, holds, flicker, dark gaps.

What: runs the app's own detection (tests/analyse.sh, the same C code as the board) over each recording,
      in Flow and in Strike, and reads the colour timeline it prints. Per recording and mode it reports
        changes/min   colour changes a minute (a new chakra, or colour to dark and back)
        hold          shortest and median time one colour stands
        short         colours that stand under SHORT seconds: what a person sees as flicker
        dark          share of the time without colour, and the longest dark gap between two colours
        fast          the most changes in any WINDOW-second stretch, against FLASH_LIMIT (photosensitivity
                      guidance: no more than three flashes a second; a calm room wants far fewer)
      One line per recording, and a summary; --json writes everything for a chart.
Why:  owner 26.10.2026: "check auch immer nicht nur die Erkennung, sondern auch Übergänge und so". The
      detection can be right and the room still restless: a colour that holds 0.3 s is a flicker, ten
      changes a minute feel like a disco, a long dark gap while a bowl rings feels broken.
Pitfalls:
  * The screen and the lamps fade (Flow about 1.5 s, Strike 0.1 s) and a strike kicks the brightness;
    this measures the colour decisions, not the fades. A change shorter than its fade is seen as a smear.
  * "dark" counts time between colours; a recording with long silences is dark there on purpose.
Usage:  tools/light_timeline.py FILE_OR_FOLDER... [--mode flow|strike|both] [--json OUT.json] [--dry-run]
"""
import argparse
import json
import re
import statistics
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
ANALYSE = HERE / "tests/analyse.sh"
MODES = {"flow": [], "strike": ["--lock", "128", "256"]}
SEG = re.compile(r"^\s*([\d.]+)-\s*([\d.]+) s\s+(\S.*?)\s{2,}")
SHORT, WINDOW, FLASH_LIMIT = 1.0, 10.0, 3.0
MEDIA = (".mov", ".mp4", ".m4v", ".wav", ".ogg", ".mp3", ".flac", ".m4a", ".webm")


def timeline(path: Path, mode: str) -> list[tuple[float, float, str]]:
    out = subprocess.run([str(ANALYSE), str(path), "notes", *MODES[mode]], capture_output=True, text=True).stdout
    segs = []
    for line in out.splitlines():
        m = SEG.match(line)
        if m:
            segs.append((float(m.group(1)), float(m.group(2)), m.group(3).strip()))
    return segs


def duration(path: Path) -> float:
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
                       capture_output=True, text=True)
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 0.0


def measure(segs: list[tuple[float, float, str]], length: float) -> dict:
    holds = [b - a for a, b, _ in segs]
    # A change: every start of a colour, except one that simply continues the colour before it.
    changes, prev_end, prev_name = [], None, None
    for a, b, name in segs:
        if not (prev_name == name and prev_end is not None and a - prev_end < 0.05):
            changes.append(a)
        if prev_end is not None and a - prev_end >= 0.05:
            changes.append(prev_end)  # colour to dark
        prev_end, prev_name = b, name
    changes.sort()
    gaps = [b[0] - a[1] for a, b in zip(segs, segs[1:]) if b[0] - a[1] > 0]
    fast = max((sum(1 for t in changes if c <= t < c + WINDOW) for c in changes), default=0)
    lit = sum(holds)
    return {"changes_per_min": round(len(changes) / max(length, 1) * 60, 1),
            "hold_min_s": round(min(holds), 2) if holds else None,
            "hold_median_s": round(statistics.median(holds), 2) if holds else None,
            "short": sum(1 for h in holds if h < SHORT),
            "dark_share": round(1 - lit / length, 2) if length else None,
            "longest_dark_gap_s": round(max(gaps), 1) if gaps else 0.0,
            "fast_per_10s": fast, "flash_ok": fast / WINDOW <= FLASH_LIMIT}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--mode", choices=["flow", "strike", "both"], default="both")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    files = []
    for p in a.paths:
        files += sorted(f for f in p.iterdir() if f.suffix.lower() in MEDIA) if p.is_dir() else [p]
    modes = ["flow", "strike"] if a.mode == "both" else [a.mode]
    if a.dry_run:
        print(f"(dry run) {len(files)} recordings, modes {', '.join(modes)}, through {ANALYSE}")
        return
    rows = {}
    print(f"{'recording':34} {'mode':6} {'chg/min':>7} {'hold min':>8} {'median':>6} {'short':>5} {'dark':>5} {'gap':>5} {'10s':>4}")
    for f in files:
        length = duration(f)
        for mode in modes:
            r = measure(timeline(f, mode), length)
            rows.setdefault(f.name, {})[mode] = r
            print(f"{f.name[:34]:34} {mode:6} {r['changes_per_min']:7} {str(r['hold_min_s']):>8} "
                  f"{str(r['hold_median_s']):>6} {r['short']:5} {str(r['dark_share']):>5} {r['longest_dark_gap_s']:5} "
                  f"{r['fast_per_10s']:4}{'' if r['flash_ok'] else '  FAST'}", flush=True)
    for mode in modes:
        vals = [rows[f][mode] for f in rows]
        if vals:
            print(f"summary {mode}: changes/min median {statistics.median(v['changes_per_min'] for v in vals)}, "
                  f"short colours {sum(v['short'] for v in vals)} in {len(vals)} recordings")
    if a.json:
        a.json.write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
