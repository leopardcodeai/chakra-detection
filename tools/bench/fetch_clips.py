#!/usr/bin/env python3
"""Fetches freely licensed instrument recordings from Wikimedia Commons for the sound-gate benchmark.

What: searches Commons (File namespace, audio only) per class, keeps files with a free licence
      (public domain, CC0, CC BY, CC BY-SA) under MAX_MB, and downloads them into
      ~/Library/Application Support/LeopardCode/chakra/bench_clips/<class>/. sources.json beside
      them records per file: class, the gate's expected answer, title, page URL, licence, author.
      Classes and what the lamp should do with them:
        bowl-like (the colour follows the pitch): singing_bowl, bell, tuning_fork
        other (the colour stays): gong, drum, rainstick, shaker, voice
Why:  owner 25.10.2026: "mach den Test für die Modelle noch bisschen umfangreicher ... und das
      alles auch in GitHub". 26 clips were too few to say anything per class.
Pitfalls:
  * The audio is never committed: the benchmark publishes results and links, not the files.
  * Commons answers a burst with an empty body, and upload.wikimedia.org with HTTP 429: API calls
    wait PAUSE_S, downloads DOWNLOAD_PAUSE_S, both back off and retry, with a descriptive
    User-Agent as Wikimedia asks (25.10.2026: 1.2 s between downloads got 110 of 123 refused).
  * "throat" alone matched bird recordings ("White-throated Sparrow"): class words are phrases.
  * A search hit can be the wrong instrument (a "gong" in a gamelan piece with drums). Titles
    with words from EXCLUDE are skipped; the rest is checked by ear for the published table.
  * Idempotent: a file already on disk is not fetched again; sources.json is rewritten whole.
Usage:  fetch_clips.py [--per-class 15] [--dry-run]
"""
import argparse
import json
import sys
import time
import urllib.parse
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path.home() / "Coding/company/tools"))
try:
    import progress
except ImportError:  # the bars are a convenience; the fetch works without them
    progress = None

OUT = Path.home() / "Library/Application Support/LeopardCode/chakra/bench_clips"
API = "https://commons.wikimedia.org/w/api.php"
UA = "LeopardCode-chakra-bench/1.0 (sound classifier benchmark; contact via github.com/leopardcodeai)"
PAUSE_S, DOWNLOAD_PAUSE_S, MAX_MB = 1.2, 4.0, 25
AUDIO = (".ogg", ".oga", ".wav", ".flac", ".mp3", ".opus")
FREE = ("public domain", "cc0", "cc by", "cc-by", "pd")
EXCLUDE = ("orchestra", "symphony", "song", "speech", "interview", "podcast", "national anthem")
# Searched in this order (a file found by an earlier class is not taken again), each with the
# words one of which the file name itself must contain: Commons searches descriptions too, so a
# drum search returns rainsticks and a chant search returns podcasts.
CLASSES = {
    "rainstick": ("other", ["rainstick", "rain stick", "palo de lluvia", "ocean drum"],
                  ["rainstick", "rain stick", "lluvia", "ocean drum"]),
    "shaker": ("other", ["shaker", "maraca", "rattle instrument"], ["shaker", "maraca", "rattle"]),
    "tuning_fork": ("bowl", ["tuning fork"], ["tuning fork", "tuning_fork", "stimmgabel"]),
    "singing_bowl": ("bowl", ["singing bowl", "tibetan bowl", "crystal singing bowl", "rin bell"],
                     ["bowl", "rin", "klangschale"]),
    "gong": ("other", ["gong", "tam-tam"], ["gong", "tam-tam", "tamtam"]),
    "bell": ("bowl", ["bell strike", "hand bell", "temple bell", "chime"], ["bell", "chime", "glocke"]),
    "drum": ("other", ["frame drum", "hand drum", "udu", "djembe", "bass drum"],
             ["drum", "udu", "djembe", "bęben", "trommel", "tabla", "bongo", "conga"]),
    "voice": ("other", ["humming", "chant", "throat singing", "om chant"],
              ["humming", "chant", "throat singing", "throat-singing", "mantra", "overtone singing"]),
}


def get(params: dict, tries: int = 4) -> dict:
    url = API + "?" + urllib.parse.urlencode({**params, "format": "json"})
    for i in range(tries):
        time.sleep(PAUSE_S * (2 ** i))
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=30) as r:
                body = r.read()
            if body.strip():
                return json.loads(body)
        except (OSError, ValueError):
            pass
    return {}


def search(term: str) -> list[str]:
    d = get({"action": "query", "list": "search", "srnamespace": 6, "srlimit": 50,
             "srsearch": f"{term} filetype:audio"})
    return [h["title"] for h in d.get("query", {}).get("search", [])]


def info(titles: list[str]) -> dict:
    d = get({"action": "query", "titles": "|".join(titles), "prop": "imageinfo",
             "iiprop": "url|size|extmetadata"})
    out = {}
    for p in d.get("query", {}).get("pages", {}).values():
        ii = (p.get("imageinfo") or [{}])[0]
        m = ii.get("extmetadata", {})
        out[p["title"]] = {"url": ii.get("url"), "page": ii.get("descriptionurl"), "bytes": ii.get("size", 0),
                           "licence": m.get("LicenseShortName", {}).get("value", ""),
                           "author": m.get("Artist", {}).get("value", "")[:200]}
    return out


def plan(per_class: int) -> list[dict]:
    picked, seen = [], set()
    for cls, (expect, terms, words) in CLASSES.items():
        titles = []
        for term in terms:
            for t in search(term):
                low = t.lower()
                if (t in seen or not low.endswith(AUDIO) or any(w in low for w in EXCLUDE)
                        or not any(w in low for w in words)):
                    continue
                titles.append(t)
                seen.add(t)
        meta = {}
        for i in range(0, len(titles), 40):
            meta.update(info(titles[i:i + 40]))
        n = 0
        for t in titles:
            m = meta.get(t, {})
            free = any(f in m.get("licence", "").lower() for f in FREE)
            if not m.get("url") or not free or m.get("bytes", 0) > MAX_MB * 2 ** 20:
                continue
            picked.append({"class": cls, "expect": expect, "title": t, **m,
                           "file": f"{cls}/{Path(urllib.parse.unquote(urllib.parse.urlsplit(m['url']).path)).name}"})
            n += 1
            if n >= per_class:
                break
        print(f"  {cls:13} {n:3} of {len(titles)} hits free and small enough", flush=True)
    return picked


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--per-class", type=int, default=15)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    items = plan(a.per_class)
    if a.dry_run:
        print(f"(dry run) would fetch {len(items)} files into {OUT}")
        return
    bar = progress.Bar("Commons clips", total=len(items), unit="files") if progress else None
    for it in items:
        dest = OUT / it["file"]
        if not dest.exists():
            dest.parent.mkdir(parents=True, exist_ok=True)
            for attempt in range(5):  # upload.wikimedia.org answers bursts with 429: wait and retry
                time.sleep(DOWNLOAD_PAUSE_S * (3 ** attempt))
                try:
                    req = urllib.request.Request(it["url"], headers={"User-Agent": UA})
                    with urllib.request.urlopen(req, timeout=120) as r:
                        dest.write_bytes(r.read())
                    it.pop("failed", None)
                    break
                except urllib.error.HTTPError as e:
                    it["failed"] = True
                    if e.code != 429:
                        print(f"  failed {it['title']}: {e}", file=sys.stderr)
                        break
                except OSError as e:
                    it["failed"] = True
                    print(f"  failed {it['title']}: {e}", file=sys.stderr)
                    break
            if it.get("failed"):
                print(f"  gave up {it['title']}", file=sys.stderr)
        if bar:
            bar.advance()
    if bar:
        bar.done()
    kept = [i for i in items if not i.get("failed")]
    (OUT / "sources.json").write_text(json.dumps(kept, indent=1, ensure_ascii=False) + "\n")
    print(f"{OUT}: {len(kept)} files, sources.json written")


if __name__ == "__main__":
    main()
