#!/usr/bin/env python3
"""The audio evaluation of ChakraA as one reproducible pipeline: data, detector, strikes, AI gate, light, report.

What: runs these stages, each a separate, idempotent step (config: tools/bench/pipeline.toml):
        fetch    Wikimedia Commons clips (fetch_clips.py; only missing files are fetched)
        own      the owner's videos: Photos export (export_album.py, off by default) and labels
                 for new videos (label_own.py); both skip what is there
        detect   the C detector (chakra_detect.c through tests/analyse_wav.c, built into the run
                 folder) over every recording, Flow and Strike; colour share of the sounding time
        strikes  the detector against ground-truth strikes: right chakra yes/no, the first new
                 colour, latency from the onset to the colour (eval_lib.py has the definitions)
                   own rounds    hand marks (own_events.json) snapped to flux onsets, or the known
                                 order root to crown aligned to flux onsets by the bowls' pitches
                   YouTube       chapters or spoken captions give the chakra (strike_truth.json)
        gate     gate_bench.py: no AI, the pretrained EfficientAT gate, the trained head; Commons
                 only (public), and again with the owner's checked segments (private)
        light    light_timeline.py's measures on every timeline: changes a minute, short colours,
                 flashes against three a second
        report   report.md, report.json, report.html and .pdf, figures as PNG and SVG, light and
                 dark; a private variant and a public one (eval_report.py)
      Every run writes into its own folder, eval_runs/<date>_<time>/ in the private state folder:
      config.toml (a copy), manifest.json (git commits of chakra and chakrav, versions of the tools,
      hashes of the code that ran and of the data lists, per stage its inputs and timing),
      data/recordings.json, stages/<stage>.json, logs/, report_private/, report_public/.
Why:  owner 26.10.2026: "bau alles mit Training und so als Pipelines und wiederverwendbar, und
      wissenschaftliche Auswertung für Blog und so". The README's numbers came from runs nobody could
      repeat; now one command makes them, with sample sizes and intervals, from recorded inputs.
Privacy: the owner's videos show people at sessions. Their names, frames and audio stay in the
      private run folder; the public report holds only Commons and YouTube material, with the own
      results as numbers. A check fails the report stage when an own file name reaches it.
Pitfalls:
  * Runs under the AI venv (numpy, scipy, matplotlib, torch): started with another Python it
    re-executes itself under run.python from the config.
  * --from a later stage needs the earlier stages' outputs: use --run latest (or a run folder) to
    continue a run; a new run starts at its first stage.
  * gate_bench.py writes its own fixed file (ai_bench/gate_bench.json) and compiles into
    build-test/; the gate stage copies the result into the run right after each call. Do not run
    two gate stages at once.
  * The detector's outputs are cached by recording, binary and arguments (eval_cache/detect);
    a changed chakra_detect.c gives a new binary hash and so new runs.
  * fetch can find new Commons files, which changes the data hash: that is the point of the hash.
Usage:  tools/bench/pipeline.py [--config tools/bench/pipeline.toml] [--from STAGE] [--to STAGE]
                                [--run latest|DIR] [--force] [--dry-run]
        tools/bench/pipeline.py --from detect          # no network, no Photos
        tools/bench/pipeline.py --run latest --from report --force   # redraw the report only
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
STAGES = ["fetch", "own", "detect", "strikes", "gate", "light", "report"]
MEDIA = (".mov", ".mp4", ".m4v", ".wav", ".ogg", ".oga", ".mp3", ".flac", ".m4a", ".webm", ".opus")


def expand(p: str | Path, base: Path | None = None) -> Path:
    q = Path(os.path.expanduser(str(p)))
    return q if q.is_absolute() or base is None else base / q


def ensure_python(cfg: dict) -> None:
    """Re-executes under the configured venv when numpy, scipy or matplotlib are missing here."""
    try:
        import matplotlib  # noqa: F401
        import numpy  # noqa: F401
        import scipy  # noqa: F401
    except ImportError:
        py = expand(cfg["run"]["python"])
        if py.exists() and Path(sys.executable).resolve() != py.resolve():
            os.execv(str(py), [str(py), str(Path(__file__).resolve()), *sys.argv[1:]])
        sys.exit(f"numpy, scipy and matplotlib are needed; set run.python in the config ({py})")


def sha_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def sha_json(obj) -> str:
    return sha_text(json.dumps(obj, sort_keys=True, default=str))


def git_state(repo: Path) -> dict:
    def g(*args):
        r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None
    if not (repo / ".git").exists():
        return {"repo": str(repo), "present": False}
    dirty = [line[3:] for line in (g("status", "--porcelain") or "").splitlines()]
    diff = g("diff", "HEAD") or ""
    return {"repo": repo.name, "commit": g("rev-parse", "HEAD"), "branch": g("rev-parse", "--abbrev-ref", "HEAD"),
            "subject": g("log", "-1", "--format=%s"), "dirty_files": dirty,
            "diff_sha256": sha_text(diff) if diff else None,
            "remote": g("remote", "get-url", "origin")}


def tool_versions() -> dict:
    out = {"python": platform.python_version(), "platform": platform.platform()}
    from importlib.metadata import PackageNotFoundError, version
    for mod in ("numpy", "scipy", "matplotlib", "scikit-learn", "torch"):
        try:
            out[mod] = version(mod)  # without importing: torch takes seconds to import
        except PackageNotFoundError:
            out[mod] = None
    for cmd in (["ffmpeg", "-version"], ["cc", "--version"]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        out[cmd[0]] = (r.stdout.splitlines() or [""])[0]
    return out


class Ctx:
    """What every stage needs: the config, the run folder, the data paths, logging and bars."""

    def __init__(self, cfg: dict, cfg_path: Path, run_dir: Path, dry: bool, force: bool):
        self.cfg, self.cfg_path, self.run, self.dry, self.force = cfg, cfg_path, run_dir, dry, force
        self.state = expand(cfg["data"]["state"])
        self.cache = expand(cfg["data"]["cache"], self.state)
        self.py = str(expand(cfg["run"]["python"])) if expand(cfg["run"]["python"]).exists() else sys.executable
        sys.path.insert(0, str(Path.home() / "Coding/company/tools"))
        try:
            import progress
            self.progress = progress
        except ImportError:
            self.progress = None
        self.manifest_path = run_dir / "manifest.json"
        self.manifest = json.loads(self.manifest_path.read_text()) if self.manifest_path.exists() else {}

    def say(self, msg: str) -> None:
        print(msg, flush=True)
        if not self.dry:
            (self.run / "logs").mkdir(parents=True, exist_ok=True)
            with open(self.run / "logs/pipeline.log", "a") as f:
                f.write(f"{dt.datetime.now().isoformat(timespec='seconds')} {msg}\n")

    def bar(self, phase: str, total: int, unit: str):
        if self.progress and not self.dry:
            return self.progress.Bar(phase, total=max(1, total), unit=unit)
        return None

    def env(self) -> dict:
        return self.progress.child_env() if self.progress else dict(os.environ)

    def call(self, cmd: list[str], log: str) -> None:
        """A child process with the bars passed on and its output into logs/<log>.log."""
        self.say("  $ " + " ".join(cmd))
        if self.dry:
            return
        with open(self.run / f"logs/{log}.log", "a") as f:
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=self.env(), cwd=str(APP))
        if r.returncode:
            raise RuntimeError(f"{cmd[1] if len(cmd) > 1 else cmd[0]} ended with {r.returncode}; see logs/{log}.log")

    def save_manifest(self) -> None:
        if not self.dry:
            self.manifest_path.write_text(json.dumps(self.manifest, indent=1, ensure_ascii=False))

    def stage_file(self, name: str) -> Path:
        return self.run / "stages" / f"{name}.json"

    def load(self, name: str) -> dict:
        p = self.stage_file(name)
        if not p.exists():
            raise SystemExit(f"stage {name} has no output in {self.run}; run it first (or --run latest)")
        return json.loads(p.read_text())

    def write(self, name: str, obj: dict) -> None:
        p = self.stage_file(name)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(obj, indent=1, ensure_ascii=False, default=float))


# ------------------------------------------------------------------ the data list

def inventory(ctx: Ctx) -> list[dict]:
    """Every recording the stages use, with its set, group and whether it may appear by name in public."""
    d = ctx.cfg["data"]
    recs: list[dict] = []
    sys.path.insert(0, str(HERE))
    sys.path.insert(0, str(HERE.parent))
    from fetch_clips import PRONUNCIATION
    from gate_bench import clips  # the same Commons list the gate stage uses (pronunciations left out)
    commons = expand(d["commons"], ctx.state)
    listed = []
    for name in ("extra_sources.json", "sources.json"):
        f = commons / name
        listed += json.loads(f.read_text()) if f.exists() else []
    spoken = sorted({c["title"] for c in listed if PRONUNCIATION.match(c["title"].lower())})
    ctx.manifest["data_cleaning"] = {"commons_pronunciations_left_out": len(spoken), "titles": spoken,
                                     "rule": "fetch_clips.PRONUNCIATION: Lingua Libre and Wiktionary word recordings"}
    for c in clips():
        recs.append({"id": "commons/" + c["file"], "set": "commons", "group": f"commons/{c['class']}",
                     "class": c["class"], "expect": c["expect"], "path": str(commons / c["file"]),
                     "title": c.get("title", ""), "page": c.get("page"), "licence": c.get("licence"),
                     "author": c.get("author"), "public": True})
    yt = expand(d["youtube"], ctx.state)
    truth = json.loads(expand(d["truth_public"], APP).read_text())["videos"]
    meta = json.loads((yt / "sources.json").read_text()) if (yt / "sources.json").exists() else {}
    for vid, m in meta.items():
        p = yt / f"{vid}.mp4"
        if not p.exists():
            continue
        kind = "tuneup" if m.get("title") else (m.get("kind") or "other").replace(" ", "_")
        t = truth.get(vid, {})
        recs.append({"id": f"youtube/{vid}", "set": "youtube", "group": f"youtube/{kind}", "path": str(p),
                     "title": m.get("title") or m.get("what", ""), "channel": m.get("channel"),
                     "page": f"https://www.youtube.com/watch?v={vid}", "public": True,
                     "duplicate_of": f"youtube/{t['duplicate_of']}" if t.get("duplicate_of") else None})
    events = json.loads(expand(d["own_events"], ctx.state).read_text()) if expand(d["own_events"], ctx.state).exists() else {}
    seen = set()
    for od in d["own_dirs"]:
        folder = expand(od, ctx.state)
        for p in sorted(folder.iterdir()) if folder.exists() else []:
            if p.suffix.lower() not in MEDIA or p.name in seen:
                continue
            seen.add(p.name)
            e = events.get(p.name, {})
            use = (e.get("use") or "").lower()
            if use.startswith("none") or use.startswith("excluded"):
                continue
            group = "own/round" if (e.get("strikes") or e.get("order")) else "own/session"
            recs.append({"id": f"own/{p.name}", "set": "own", "group": group, "path": str(p),
                         "title": "", "public": False})
    return recs


def data_hashes(ctx: Ctx, recs: list[dict]) -> dict:
    import eval_lib as E
    hp = ctx.cache / "hashes.json"
    cache = json.loads(hp.read_text()) if hp.exists() else {}
    bar = ctx.bar("Hash data", len(recs), "files")
    for r in recs:
        r["sha256"] = E.sha256_file(Path(r["path"]), cache)
        r["bytes"] = Path(r["path"]).stat().st_size
        if bar:
            bar.advance()
    if bar:
        bar.done()
    if not ctx.dry:
        ctx.cache.mkdir(parents=True, exist_ok=True)
        hp.write_text(json.dumps(cache))
    out = {}
    for s in ("commons", "youtube", "own"):
        rows = sorted((r["id"], r["sha256"]) for r in recs if r["set"] == s)
        out[s] = {"recordings": len(rows), "sha256": sha_json(rows)}
    out["all"] = sha_json([out[s]["sha256"] for s in ("commons", "youtube", "own")])
    return out


# ------------------------------------------------------------------ stages

def stage_fetch(ctx: Ctx) -> dict:
    c = ctx.cfg["stages"]["fetch"]
    if not c.get("enabled", True):
        return {"skipped": "disabled in config"}
    ctx.call([ctx.py, str(HERE / "fetch_clips.py"), "--per-class", str(c.get("per_class", 15))], "fetch")
    return {"ran": "fetch_clips.py"}


def stage_own(ctx: Ctx) -> dict:
    c = ctx.cfg["stages"]["own"]
    done = []
    if c.get("export"):
        ctx.call([ctx.py, str(HERE / "export_album.py"), "--album", c.get("album", "ChakraV Training")], "own")
        done.append("export_album.py")
    if c.get("label", True):
        ctx.call([ctx.py, str(HERE / "label_own.py")], "own")
        done.append("label_own.py")
    return {"ran": done}


def build_detector(ctx: Ctx) -> tuple[Path, str]:
    """analyse_wav built from the current sources into the run folder; returns it and the sources' hash."""
    srcs = [APP / "components/app_chakra/chakra_detect.c", APP / "components/app_chakra/include/chakra_detect.h",
            APP / "tests/analyse_wav.c"]
    h = sha_json([[s.name, hashlib.sha256(s.read_bytes()).hexdigest()] for s in srcs])
    out = ctx.run / "build/analyse_wav"
    if not ctx.dry:
        out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        f"-I{APP}/components/app_chakra/include", str(srcs[0]), str(srcs[2]), "-lm", "-o", str(out)],
                       check=True)
    return out, h


def stage_detect(ctx: Ctx, recs: list[dict]) -> dict:
    import eval_lib as E
    from concurrent.futures import ThreadPoolExecutor, as_completed
    c = ctx.cfg["stages"]["detect"]
    binary, src_hash = build_detector(ctx)
    ctx.manifest.setdefault("code", {})["detector_sources_sha256"] = src_hash
    base = [c.get("mapping", "notes"), "--a4", str(c.get("a4_hz", 440))]
    modes = {k: list(v) for k, v in c["modes"].items()}
    todo = [r for r in recs if not r.get("duplicate_of")]
    ctx.say(f"  {len(todo)} recordings x {len(modes)} modes ({', '.join(modes)}), {len(recs) - len(todo)} duplicates left out")
    if ctx.dry:
        return {}
    det_cache = ctx.cache / "detect"
    det_cache.mkdir(parents=True, exist_ok=True)

    def one(r: dict) -> tuple[str, dict]:
        wav = E.decode(Path(r["path"]), ctx.cache / "wav")
        x = E.read_wav(wav)
        length = len(x) / E.RATE
        snd = E.sounding_mask(x)
        res = {"duration_s": round(length, 2), "sounding_s": round(float(snd.sum()) * 0.1, 1), "modes": {}}
        for mode, extra in modes.items():
            args = base + extra
            key = sha_json([E.file_key(Path(r["path"])), src_hash, args])[:20]
            cf = det_cache / f"{key}.txt"
            if cf.exists():
                text = cf.read_text()
            else:
                text = subprocess.run([str(binary), str(wav), *args], capture_output=True, text=True, check=True).stdout
                cf.write_text(text)
            segs = E.parse_timeline(text)
            lit = E.lit_mask(segs, len(snd))
            res["modes"][mode] = {
                "segments": segs,
                "lit_share": round(float(lit.mean()), 4),
                "lit_sounding_share": round(float((lit & snd).sum() / max(1, snd.sum())), 4),
            }
            share = {}
            for s in segs:
                share[E.KEYS[s["chakra"]]] = share.get(E.KEYS[s["chakra"]], 0.0) + s["end"] - s["start"]
            res["modes"][mode]["shown"] = {k: round(v / max(length, 1e-9), 4) for k, v in share.items()}
        return r["id"], res

    out = {}
    bar = ctx.bar("Detector over recordings", len(todo), "recordings")
    with ThreadPoolExecutor(max_workers=int(ctx.cfg["run"].get("workers", 4))) as ex:
        futs = {ex.submit(one, r): r for r in todo}
        for f in as_completed(futs):
            rid, res = f.result()
            out[rid] = res
            if bar:
                bar.advance()
    if bar:
        bar.done()
    return {"args": {"base": base, "modes": modes}, "detector_sources_sha256": src_hash,
            "recordings": dict(sorted(out.items()))}


def own_truth(ctx: Ctx, r: dict, x, events: dict, bowl_hz: list[float], onsets: list[dict]) -> dict | None:
    import eval_lib as E
    c = ctx.cfg["stages"]["strikes"]
    e = events.get(Path(r["path"]).name, {})
    order = [E.chakra_index(k) for k in e.get("order", [])]
    marks = e.get("strikes") or []
    if not order and not marks:
        return None
    early = [o for o in onsets if o["t"] < c["round_window_s"]]
    aligned = E.align_order(x, early, bowl_hz, order or [m["chakra"] for m in marks]) if bowl_hz else []
    if marks:
        # Hand marks give the chakra; the time is the aligned onset when it lies within mark_snap_s of
        # its mark (the two sources agree), else the nearest onset, else the mark itself.
        strikes, diffs = [], []
        for i, m in enumerate(marks):
            a = aligned[i] if i < len(aligned) else None
            if a and abs(a["t"] - m["t"]) <= c["mark_snap_s"]:
                t, how = a["t"], "aligned onset, agrees with the mark"
            else:
                near = min(onsets, key=lambda o: abs(o["t"] - m["t"]), default=None)
                t, how = (near["t"], "nearest onset") if near and abs(near["t"] - m["t"]) <= c["mark_snap_s"] \
                    else (m["t"], "mark")
            diffs.append(round(t - m["t"], 3))
            strikes.append({"t": t, "chakra": int(m["chakra"]), "how": how,
                            "gain": a["gain"] if a else None, "mark_t": m["t"]})
        return {"kind": "marked", "strikes": strikes,
                "chakra_source": "hand marks (own_events.json): " + e.get("checked_by", ""),
                "reliability": "chakra high (marked by a person); time: the flux onset nearest the mark",
                "validation": {"onset_minus_mark_s": diffs,
                               "marks_from": e.get("checked_by", "")}}
    strikes = [{"t": a["t"], "chakra": a["chakra"], "how": "order aligned to onsets", "gain": a["gain"],
                "uncertain": a["gain"] < c["uncertain_gain"]} for a in aligned]
    return {"kind": "round", "strikes": strikes,
            "chakra_source": e.get("order_source", "known order"),
            "reliability": "chakra high (the order is known); time derived (onsets aligned to the order by the "
                           "bowls' pitches), validated on the round with checked marks"}


def youtube_truth(ctx: Ctx, t: dict, onsets: list[dict]) -> dict | None:
    import eval_lib as E
    kind = t.get("kind")
    if kind not in ("chapters", "announced", "chapters_sustained"):
        return None
    segs = [{"start": float(s["start"]), "end": float(s["end"]), "chakra": E.chakra_index(s["chakra"])}
            for s in t["segments"]]
    strikes = []
    n = int(t.get("strikes_per_segment", 0))
    for s in segs if n else []:
        a = s["start"] + float(t.get("skip_after_cue_s", 0.0))
        for o in E.strongest_in(onsets, a, s["end"], n, float(t.get("min_gap_s", 1.0))):
            strikes.append({"t": o["t"], "chakra": s["chakra"], "how": t["strike_source"].split(":")[0],
                            "strength": o["strength"]})
    return {"kind": kind, "strikes": strikes, "segments": segs, "chakra_source": t.get("chakra_source"),
            "strike_source": t.get("strike_source"), "reliability": t.get("reliability"), "note": t.get("note")}


def stage_strikes(ctx: Ctx, recs: list[dict]) -> dict:
    import eval_lib as E
    c = ctx.cfg["stages"]["strikes"]
    if ctx.dry:
        ctx.say("  ground truth: own rounds (own_events.json) and YouTube (strike_truth.json); scored per mode")
        return {}
    det = ctx.load("detect")["recordings"]
    d = ctx.cfg["data"]
    events_p = expand(d["own_events"], ctx.state)
    events = json.loads(events_p.read_text()) if events_p.exists() else {}
    # The own bowls' pitches: the round whose marks carry Hz (measured, IMG_2145 on 26.10.2026).
    bowl_hz = []
    for e in events.values():
        hz = [s.get("hz") for s in e.get("strikes", [])]
        if len(hz) == 7 and all(hz):
            bowl_hz = [float(h) for h in hz]
    truth = json.loads(expand(d["truth_public"], APP).read_text())["videos"]
    todo = []
    for r in recs:
        if r.get("duplicate_of") or r["id"] not in det:
            continue
        if r["set"] == "own" and r["group"] == "own/round":
            todo.append(r)
        elif r["set"] == "youtube" and truth.get(r["id"].split("/", 1)[1], {}).get("kind") in (
                "chapters", "announced", "chapters_sustained"):
            todo.append(r)
    ctx.say(f"  {len(todo)} recordings with ground truth ({sum(r['set'] == 'own' for r in todo)} own)")
    out = {}
    bar = ctx.bar("Strikes against ground truth", len(todo), "recordings")
    for r in todo:
        x = E.read_wav(E.decode(Path(r["path"]), ctx.cache / "wav"))
        onsets = E.flux_onsets(x, c["onset_min_gap_s"], c["onset_prominence"])
        gt = own_truth(ctx, r, x, events, bowl_hz, onsets) if r["set"] == "own" else \
            youtube_truth(ctx, truth[r["id"].split("/", 1)[1]], onsets)
        if not gt:
            continue
        if r["set"] == "own" and gt["kind"] == "marked" and bowl_hz:
            # Validation of the derivation used for the rounds: align the order without the marks.
            early = [o for o in onsets if o["t"] < c["round_window_s"]]
            al = E.align_order(x, early, bowl_hz, [s["chakra"] for s in gt["strikes"]])
            gt["validation"]["aligned_minus_mark_s"] = [round(a["t"] - s["mark_t"], 3)
                                                        for a, s in zip(al, gt["strikes"])]
        res = {"set": r["set"], "group": r["group"], "truth": gt, "modes": {}}
        for mode, m in det[r["id"]]["modes"].items():
            segs = m["segments"]
            res["modes"][mode] = {"strikes": E.score_strikes(segs, gt["strikes"], c["window_s"], c["early_s"])}
            if gt.get("segments"):
                res["modes"][mode]["segments"] = [
                    {**s, "share": round(E.segment_share(segs, s["start"], s["end"], s["chakra"]), 4)}
                    for s in gt["segments"]]
        out[r["id"]] = res
        if bar:
            bar.advance()
    if bar:
        bar.done()
    return {"bowl_hz_own": bowl_hz, "config": c, "recordings": out}


def stage_gate(ctx: Ctx) -> dict:
    c = ctx.cfg["stages"]["gate"]
    if c.get("save_head"):
        raise SystemExit("gate.save_head is true: an evaluation run must not replace the app's head")
    result = ctx.state / "ai_bench/gate_bench.json"
    base = [ctx.py, str(HERE / "gate_bench.py"), "--models", c["models"], "--windows", str(c["windows"])]
    out = {}
    for name, extra in [("commons", [])] + ([("own", ["--own"])] if c.get("own") else []):
        ctx.call(base + extra, f"gate_{name}")
        if not ctx.dry:
            dst = ctx.run / f"stages/gate_{name}.json"
            shutil.copy2(result, dst)
            out[name] = str(dst.relative_to(ctx.run))
    return {"runs": out, "models": c["models"], "windows": c["windows"]}


def stage_light(ctx: Ctx, recs: list[dict]) -> dict:
    import eval_lib as E
    c = ctx.cfg["stages"]["light"]
    if ctx.dry:
        ctx.say(f"  light_timeline measures on every timeline, short under {c['short_s']} s")
        return {}
    det = ctx.load("detect")["recordings"]
    out = {}
    for r in recs:
        if r["id"] not in det:
            continue
        d = det[r["id"]]
        out[r["id"]] = {mode: E.light_metrics(m["segments"], d["duration_s"], c["short_s"], c["flash_limit_per_s"])
                        for mode, m in d["modes"].items()}
    return {"config": c, "recordings": out}


def stage_report(ctx: Ctx, recs: list[dict]) -> dict:
    import eval_report
    c = ctx.cfg["stages"]["report"]
    if ctx.dry:
        ctx.say(f"  variants {c['variants']}, public copy to {c.get('public_copy')}")
        return {}
    made = {}
    for variant in c["variants"]:
        folder = eval_report.build(ctx.run, ctx.cfg, variant, recs, pdf=c.get("pdf", True))
        made[variant] = str(folder.relative_to(ctx.run))
    if "public" in c["variants"] and c.get("public_copy"):
        dst = APP / c["public_copy"]
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(ctx.run / "report_public", dst)
        eval_report.leak_check(dst, recs)
        made["public_copy"] = str(dst)
    return made


# ------------------------------------------------------------------ the chain

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", type=Path, default=HERE / "pipeline.toml")
    ap.add_argument("--from", dest="start", choices=STAGES, default=STAGES[0])
    ap.add_argument("--to", dest="stop", choices=STAGES, default=STAGES[-1])
    ap.add_argument("--only", choices=STAGES, help="one stage alone (same as --from S --to S)")
    ap.add_argument("--run", help="continue in this run folder ('latest' for the newest) instead of a new one")
    ap.add_argument("--force", action="store_true", help="redo stages whose inputs did not change")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    cfg = tomllib.loads(a.config.read_text())
    ensure_python(cfg)
    sys.path.insert(0, str(HERE))
    if a.only:
        a.start = a.stop = a.only
    chain = STAGES[STAGES.index(a.start):STAGES.index(a.stop) + 1]
    root = expand(cfg["run"]["root"])
    if a.run:
        run_dir = (root / "latest").resolve() if a.run == "latest" else expand(a.run)
        if not run_dir.exists():
            raise SystemExit(f"no run folder {run_dir}")
    else:
        run_dir = root / dt.datetime.now().strftime("%Y-%m-%d_%H%M%S")
    ctx = Ctx(cfg, a.config, run_dir, a.dry_run, a.force)
    ctx.say(f"{'(dry run) ' if a.dry_run else ''}run {run_dir}: {' -> '.join(chain)}")
    if not a.dry_run:
        for sub in ("stages", "logs", "data"):
            (run_dir / sub).mkdir(parents=True, exist_ok=True)
        os.chmod(run_dir, 0o700)
        if not a.run:
            shutil.copy2(a.config, run_dir / "config.toml")
            link = root / "latest"
            if link.is_symlink() or link.exists():
                link.unlink()
            link.symlink_to(run_dir.name)
        m = ctx.manifest
        code = [HERE / f for f in ("pipeline.py", "eval_lib.py", "eval_report.py", "gate_bench.py", "fetch_clips.py",
                                   "label_own.py", "strike_truth.json")] + [HERE.parent / "light_timeline.py",
                                                                            HERE.parent / "bench_efficientat.py"]
        state = {"git": {"chakra": git_state(APP), "chakrav": git_state(expand(cfg["data"]["chakrav_repo"]))},
                 "code": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in code if p.exists()}}
        if not a.run:  # the run's identity is fixed when it starts
            m["created"] = dt.datetime.now().isoformat(timespec="seconds")
            m["argv"] = sys.argv[1:]
            m["config_sha256"] = hashlib.sha256((run_dir / "config.toml").read_bytes()).hexdigest()
            m["tools"] = tool_versions()
            m["git"] = state["git"]
            m["code"] = state["code"]
        else:  # a continued run keeps its start and records what continued it
            m.setdefault("continued", []).append({"at": dt.datetime.now().isoformat(timespec="seconds"),
                                                  "argv": sys.argv[1:], **state})
        ctx.save_manifest()

    chain_bar = ctx.bar("Evaluation pipeline", len(chain), "stages")
    recs = None
    for name in chain:
        t0 = time.time()
        ctx.say(f"== {name}")
        if name in ("fetch", "own"):
            result = stage_fetch(ctx) if name == "fetch" else stage_own(ctx)
        else:
            if recs is None:  # after fetch and own: the data as it is now
                recs = inventory(ctx)
                hashes = data_hashes(ctx, recs) if not a.dry_run else {}
                ctx.say(f"  data: {sum(r['set'] == 'commons' for r in recs)} Commons, "
                        f"{sum(r['set'] == 'youtube' for r in recs)} YouTube, {sum(r['set'] == 'own' for r in recs)} own")
                if not a.dry_run:
                    ctx.manifest["data"] = hashes
                    (run_dir / "data/recordings.json").write_text(json.dumps(recs, indent=1, ensure_ascii=False))
            inputs = sha_json([cfg["stages"].get(name), ctx.manifest.get("data", {}).get("all"),
                               ctx.manifest.get("code"),
                               [hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((run_dir / "stages").glob("*.json"))
                                if STAGES.index(p.stem.split("_")[0]) < STAGES.index(name)] if not a.dry_run else []])
            prev = ctx.manifest.get("stages", {}).get(name, {})
            if not a.force and not a.dry_run and prev.get("inputs_sha256") == inputs and prev.get("status") == "done":
                ctx.say(f"  unchanged inputs, kept ({prev.get('finished')}); --force redoes it")
                if chain_bar:
                    chain_bar.advance()
                continue
            fn = {"detect": stage_detect, "strikes": stage_strikes, "light": stage_light,
                  "report": stage_report}.get(name)
            result = stage_gate(ctx) if name == "gate" else fn(ctx, recs)
            if not a.dry_run and name in ("detect", "strikes", "light"):
                ctx.write(name, result)
            if not a.dry_run:
                ctx.manifest.setdefault("stages", {}).setdefault(name, {})["inputs_sha256"] = inputs
        if not a.dry_run:
            st = ctx.manifest.setdefault("stages", {}).setdefault(name, {})
            st.update({"status": "done", "finished": dt.datetime.now().isoformat(timespec="seconds"),
                       "seconds": round(time.time() - t0, 1),
                       "result": result if name in ("fetch", "own", "gate", "report") else "stages/" + name + ".json"})
            ctx.save_manifest()
        ctx.say(f"  {name} done in {time.time() - t0:.0f} s")
        if chain_bar:
            chain_bar.advance()
    if chain_bar:
        chain_bar.done()
    ctx.say(f"run folder: {run_dir}")


if __name__ == "__main__":
    main()
