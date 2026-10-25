#!/usr/bin/env python3
"""Exports a Photos album one item at a time, so the first videos can be used while the rest come.

What: asks Photos (AppleScript) for the album's items and exports them one by one, as originals, into
      OUT (the private own_media folder). An item whose file name is already there is skipped, so a
      second run only fetches what is missing. Prints one line per file as it lands.
Why:  owner 25.10.2026: "immer nicht warten bis lange Jobs fertig sind, immer mal eine Datei rausnehmen".
      One export of the whole album wrote nothing for 55 minutes: Photos downloads every original from
      iCloud first and writes all files at the end.
Privacy: the videos show people at sessions. They go only into the private state folder (0700) and are
      never committed, uploaded or sent to a cloud model; label_own.py reads them there.
Pitfalls:
  * The first run asks once whether this app may control Photos (Automation permission).
  * An item still in iCloud takes as long as its download; the timeout per item is 30 minutes.
  * Photos sometimes returns without writing the file; the line says so, and a rerun fetches it.
  * Photos may name an export differently from the item's filename (an edited item, a HEVC video);
    then the skip check misses it and the file is exported again under the same name, harmlessly.
Usage:  export_album.py [--album "ChakraV Training"] [--dry-run]
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path.home() / "Coding/company/tools"))
try:
    import progress
except ImportError:
    progress = None

OUT = Path.home() / "Library/Application Support/LeopardCode/chakra/own_media/chakrav_training"


def osa(script: str, timeout: int = 120) -> str:
    r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(r.stderr.strip())
    return r.stdout.strip()


def q(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--album", default="ChakraV Training")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    album = f"album {q(a.album)}"
    names = osa(f'tell application "Photos" to get filename of every media item of {album}').split(", ")
    names = [n for n in names if n]
    have = {p.name for p in OUT.iterdir()} if OUT.exists() else set()
    todo = [(i + 1, n) for i, n in enumerate(names) if n not in have]
    print(f"{len(names)} items in {a.album}, {len(names) - len(todo)} already in {OUT.name}, {len(todo)} to export",
          flush=True)
    if a.dry_run or not todo:
        return
    OUT.mkdir(parents=True, exist_ok=True)
    os.chmod(OUT.parent, 0o700)
    bar = progress.Bar("Export album", total=len(todo), unit="videos") if progress else None
    for i, name in todo:
        try:
            osa(f'with timeout of 1800 seconds\ntell application "Photos" to export {{media item {i} of {album}}} '
                f'to (POSIX file {q(str(OUT))}) with using originals\nend timeout', timeout=1900)
            # Photos can return without writing the file (seen 25.10.2026 for 2 of 7): say so, a rerun retries.
            stem = Path(name).stem
            landed = any(p.stem == stem and p.suffix.lower() != ".aae" for p in OUT.iterdir())
            print(f"{name}: {'exported' if landed else 'not written by Photos, run again later'}", flush=True)
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            print(f"{name}: failed, {str(e)[:160]}", flush=True)
        if bar:
            bar.advance()
    if bar:
        bar.done()


if __name__ == "__main__":
    main()
