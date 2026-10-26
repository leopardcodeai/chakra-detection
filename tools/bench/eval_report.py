#!/usr/bin/env python3
"""The report of an evaluation run (pipeline.py, stage report): numbers with n and intervals, figures, two variants.

What: reads a run folder's stages/*.json, manifest.json and data/recordings.json and writes
        report_<variant>/report.md     for the blog and GitHub (figures switch light and dark)
        report_<variant>/report.json   every number of the report, with its n and interval
        report_<variant>/report.html   the same through docgen (design_agent house style), and .pdf
        report_<variant>/figures/      PNG and SVG, each light and -dark
        report_<variant>/commons_sources.csv   title, page, licence and author of every Commons clip
      variant "private": everything, the owner's recordings by name.
      variant "public":  Commons and YouTube only; the owner's recordings appear as counts and
                         results of groups, never by name, picture, frame or sound.
Why:  owner 26.10.2026: "wissenschaftliche Auswertung für Blog und so". A blog number needs its n,
      its interval and its definition next to it, and the public text must not carry private media.
Charts (dataviz skill): dot and whisker, one axis, thin marks, legend above, series told apart by
      colour and marker shape (blue circle, orange square, aqua diamond: categorical slots 1 to 3,
      validated light and dark), recessive grid, every number also in a table.
Pitfalls:
  * leak_check() fails the public variant when an own file name, its stem or the private state
    path shows up in any text file of it; the PDF is rendered from the checked HTML.
  * Business-text rule: no dashes in prose; ranges are written "a to b", intervals "[a, b]".
Usage:  imported by pipeline.py; alone: eval_report.py RUN_DIR [--variant public] [--no-pdf]
"""
from __future__ import annotations

import csv
import html
import json
import math
import re
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import eval_lib as E  # noqa: E402

THEMES = {
    "light": {"surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781", "grid": "#e1e0d9",
              "axis": "#c3c2b7", "series": ["#2a78d6", "#eb6834", "#1baf7a"]},
    "dark": {"surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781", "grid": "#2c2c2a",
             "axis": "#383835", "series": ["#3987e5", "#d95926", "#199e70"]},
}
MARKERS = ["o", "s", "D"]
MODE_NAMES = {"flow": "Flow", "strike": "Strike"}
GATE_NAMES = {"no_ai": "No AI", "ai_pretrained": "AI, pretrained", "ai_trained": "AI, trained head"}
GROUP_NAMES = {
    "own/marked": "Own rounds, hand-marked strikes", "own/round": "Own rounds, derived strike times",
    "youtube/chapters": "YouTube, chapters (3 strikes a bowl)", "youtube/announced": "YouTube, spoken announcements",
    "youtube/tuneup": "YouTube tune-ups", "youtube/on_body": "YouTube sessions on the body",
    "youtube/concert": "YouTube concerts", "own/session": "Own sessions",
    "commons/bowl": "Commons, bowl-like sounds", "commons/other": "Commons, other sounds",
}
CLASS_NAMES = {"singing_bowl": "Singing bowl", "bell": "Bell", "tuning_fork": "Tuning fork", "gong": "Gong",
               "voice": "Voice", "drum": "Drum", "shaker": "Shaker", "rainstick": "Rain stick"}
CLASS_ORDER = ["singing_bowl", "bell", "tuning_fork", "gong", "voice", "drum", "shaker", "rainstick"]


# ------------------------------------------------------------------ numbers

class Stats:
    def __init__(self, cfg: dict):
        self.n = int(cfg["run"].get("bootstrap", 10000))
        self.seed = int(cfg["run"].get("seed", 26102026))

    def ci(self, groups, stat):
        v, lo, hi = E.bootstrap_ci(groups, stat, self.n, self.seed)
        return {"value": v, "low": lo, "high": hi}


def pct(x) -> str:
    return "n/a" if x is None or (isinstance(x, float) and math.isnan(x)) else f"{100 * x:.0f} %"


def fmt_ci(d: dict, kind: str = "pct") -> str:
    if d is None or d.get("value") is None:
        return "n/a"
    f = (lambda x: f"{100 * x:.0f}") if kind == "pct" else (lambda x: f"{x:.2f}") if kind == "s" else (lambda x: f"{x:.1f}")
    unit = " %" if kind == "pct" else " s" if kind == "s" else ""
    core = f(d["value"]) + unit
    if d.get("low") is None:
        return core
    return f"{core} [{f(d['low'])}, {f(d['high'])}]"


# ------------------------------------------------------------------ figures

def dot_ci_figure(path: Path, title: str, rows: list[str], series: list[str], values: dict, xlabel: str,
                  xlim: tuple | None = None, ref: float | None = None, ref_label: str = "",
                  percent: bool = False) -> list[str]:
    """Horizontal dot and whisker: one row per label, one mark per series. values[(row, series)] =
    {value, low, high}. Writes PNG and SVG in light and dark; returns the file names."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    names = []
    for theme_name, th in THEMES.items():
        plt.rcParams.update({"font.family": ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"], "font.size": 10,
                             "svg.fonttype": "none"})
        h = 1.5 + len(rows) * (0.28 + 0.16 * len(series))
        fig, ax = plt.subplots(figsize=(7.2, h), dpi=200)
        fig.patch.set_facecolor(th["surface"])
        ax.set_facecolor(th["surface"])
        span = 0.5 if len(series) > 1 else 0.0
        offs = np.linspace(-span / 2, span / 2, len(series)) if len(series) > 1 else [0.0]
        for j, s in enumerate(series):
            for i, r in enumerate(rows):
                v = values.get((r, s))
                if not v or v.get("value") is None:
                    continue
                y = i + offs[j]
                thin = bool(v.get("thin"))  # an interval of another kind (Wilson over strikes): thin and hollow
                if v.get("low") is not None:
                    ax.plot([v["low"], v["high"]], [y, y], color=th["series"][j], lw=1 if thin else 2,
                            solid_capstyle="round", zorder=2)
                ax.plot([v["value"]], [y], marker=MARKERS[j], ms=7, color=th["series"][j], zorder=3,
                        markerfacecolor=th["surface"] if thin else th["series"][j],
                        markeredgecolor=th["series"][j] if thin else th["surface"],
                        markeredgewidth=1.5, linestyle="none",
                        label=s if i == next((k for k, rr in enumerate(rows) if values.get((rr, s))), 0) else None)
        if ref is not None:
            ax.axvline(ref, color=th["muted"], lw=1, zorder=1)
            ax.text(ref, len(rows) - 0.45, " " + ref_label, color=th["ink2"], fontsize=8, va="bottom")
        ax.set_yticks(range(len(rows)))
        ax.set_yticklabels(rows, color=th["ink"])
        ax.invert_yaxis()
        ax.set_ylim(len(rows) - 0.5, -0.6 if len(series) > 1 else -0.5)
        if xlim:
            ax.set_xlim(*xlim)
        if percent:
            from matplotlib.ticker import FuncFormatter
            ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{100 * x:.0f} %"))
        ax.set_xlabel(xlabel, color=th["ink2"])
        ax.tick_params(length=0)
        for lab in ax.get_xticklabels():
            lab.set_color(th["ink2"])
        for lab in ax.get_yticklabels():
            lab.set_color(th["ink"])
        ax.grid(axis="x", color=th["grid"], lw=0.8)
        ax.set_axisbelow(True)
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(th["axis"])
        top = 1 - 0.8 / h  # title and legend take a fixed 0.8 inch, whatever the height
        fig.text(0.01, 1 - 0.12 / h, title, ha="left", va="top", color=th["ink"], fontsize=12, fontweight="bold")
        if len(series) > 1:
            leg = fig.legend(loc="upper left", ncol=len(series), frameon=False, fontsize=9,
                             bbox_to_anchor=(0.0, 1 - 0.42 / h), handletextpad=0.3, columnspacing=1.6)
            for t in leg.get_texts():
                t.set_color(th["ink"])
        fig.tight_layout(rect=(0, 0, 1, top))
        suffix = "" if theme_name == "light" else "-dark"
        for ext in ("png", "svg"):
            out = path.parent / f"{path.name}{suffix}.{ext}"
            fig.savefig(out, facecolor=th["surface"])
            names.append(out.name)
        plt.close(fig)
    return names


def strip_figure(path: Path, title: str, groups: dict, xlabel: str, stats: dict) -> list[str]:
    """Latencies as points (one row per series), with the median and its interval drawn over them."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    names = []
    rng = np.random.default_rng(1)
    labels = list(groups)
    for theme_name, th in THEMES.items():
        fig, ax = plt.subplots(figsize=(7.2, 1.4 + 0.9 * len(labels)), dpi=200)
        fig.patch.set_facecolor(th["surface"])
        ax.set_facecolor(th["surface"])
        for j, lab in enumerate(labels):
            xs = np.array(groups[lab])
            ys = j + rng.uniform(-0.18, 0.18, len(xs))
            ax.scatter(xs, ys, s=16, color=th["series"][j], alpha=0.55, edgecolors="none", marker=MARKERS[j], zorder=2)
            st = stats[lab]
            if st.get("value") is not None:
                if st.get("low") is not None:
                    ax.plot([st["low"], st["high"]], [j - 0.32, j - 0.32], color=th["ink"], lw=2, zorder=3)
                ax.plot([st["value"]], [j - 0.32], marker="|", ms=12, mew=2, color=th["ink"], zorder=4)
                ax.text(st["value"], j - 0.42, f" median {st['value']:.2f} s", color=th["ink2"], fontsize=8,
                        va="bottom")
        ax.set_yticks(range(len(labels)))
        ax.set_yticklabels([f"{lab} (n = {len(groups[lab])})" for lab in labels])
        ax.invert_yaxis()
        ax.set_xlabel(xlabel, color=th["ink2"])
        ax.tick_params(length=0)
        for lab in ax.get_xticklabels():
            lab.set_color(th["ink2"])
        for lab in ax.get_yticklabels():
            lab.set_color(th["ink"])
        ax.grid(axis="x", color=th["grid"], lw=0.8)
        ax.set_axisbelow(True)
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(th["axis"])
        ax.set_xlim(left=0)
        hh = 1.4 + 0.9 * len(labels)
        fig.text(0.01, 1 - 0.12 / hh, title, ha="left", va="top", color=th["ink"], fontsize=12, fontweight="bold")
        fig.tight_layout(rect=(0, 0, 1, 1 - 0.55 / hh))
        suffix = "" if theme_name == "light" else "-dark"
        for ext in ("png", "svg"):
            out = path.parent / f"{path.name}{suffix}.{ext}"
            fig.savefig(out, facecolor=th["surface"])
            names.append(out.name)
        plt.close(fig)
    return names


# ------------------------------------------------------------------ documents

class Doc:
    """Blocks rendered twice: Markdown for the blog and GitHub, HTML for docgen."""

    def __init__(self):
        self.blocks: list[tuple] = []

    def h(self, level: int, text: str):
        self.blocks.append(("h", level, text))

    def p(self, text: str):
        self.blocks.append(("p", text))

    def ul(self, items: list[str]):
        self.blocks.append(("ul", items))

    def table(self, head: list[str], rows: list[list[str]]):
        self.blocks.append(("table", head, rows))

    def fig(self, name: str, caption: str):
        self.blocks.append(("fig", name, caption))

    def markdown(self, title: str, subtitle: str) -> str:
        out = [f"# {title}", "", f"*{subtitle}*", ""]
        for b in self.blocks:
            if b[0] == "h":
                out += ["#" * (b[1] + 1) + " " + b[2], ""]
            elif b[0] == "p":
                out += [b[1], ""]
            elif b[0] == "ul":
                out += [f"- {x}" for x in b[1]] + [""]
            elif b[0] == "table":
                out += ["| " + " | ".join(b[1]) + " |", "|" + "|".join("---" for _ in b[1]) + "|"]
                out += ["| " + " | ".join(r) + " |" for r in b[2]] + [""]
            elif b[0] == "fig":
                out += [f'<picture><source media="(prefers-color-scheme: dark)" srcset="figures/{b[1]}-dark.svg">'
                        f'<img src="figures/{b[1]}.svg" alt="{html.escape(b[2])}" width="720"></picture>', "",
                        f"*{b[2]}*", ""]
        return "\n".join(out)

    @staticmethod
    def inline(text: str) -> str:
        t = html.escape(text)
        t = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", t)
        t = re.sub(r"`(.+?)`", r"<code>\1</code>", t)
        t = re.sub(r"\[(.+?)\]\((.+?)\)", r'<a href="\2">\1</a>', t)
        return t

    def html_body(self) -> str:
        out = []
        for b in self.blocks:
            if b[0] == "h":
                out.append(f"<h{b[1] + 1}>{self.inline(b[2])}</h{b[1] + 1}>")
            elif b[0] == "p":
                out.append(f"<p>{self.inline(b[1])}</p>")
            elif b[0] == "ul":
                out.append("<ul>" + "".join(f"<li>{self.inline(x)}</li>" for x in b[1]) + "</ul>")
            elif b[0] == "table":
                head = "".join(f"<th>{self.inline(x)}</th>" for x in b[1])
                rows = "".join("<tr>" + "".join(f"<td>{self.inline(x)}</td>" for x in r) + "</tr>" for r in b[2])
                out.append(f"<table><tr>{head}</tr>{rows}</table>")
            elif b[0] == "fig":
                out.append(f'<figure><img src="figures/{b[1]}.png" alt="" style="max-width:100%">'
                           f"<figcaption>{self.inline(b[2])}</figcaption></figure>")
        return "\n".join(out)


def leak_check(folder: Path, recs: list[dict]) -> None:
    """Fails when an own recording's name or the private state path appears in the public folder."""
    bad = set()
    for r in recs:
        if r["set"] == "own":
            name = Path(r["path"]).name
            bad |= {name, Path(name).stem}
    bad |= {"own_media", "Application Support/LeopardCode", "chakrav_training"}
    bad = {b for b in bad if len(b) >= 6}
    for f in folder.rglob("*"):
        if f.suffix.lower() in (".md", ".json", ".html", ".svg", ".csv", ".txt"):
            text = f.read_text(errors="ignore")
            hits = [b for b in bad if b in text]
            if hits:
                raise SystemExit(f"public report leaks private names {hits[:3]} in {f}")


# ------------------------------------------------------------------ the report

def build(run: Path, cfg: dict, variant: str, recs: list[dict] | None = None, pdf: bool = True) -> Path:
    public = variant == "public"
    if recs is None:
        recs = json.loads((run / "data/recordings.json").read_text())
    by_id = {r["id"]: r for r in recs}
    manifest = json.loads((run / "manifest.json").read_text())
    det = json.loads((run / "stages/detect.json").read_text())
    stk = json.loads((run / "stages/strikes.json").read_text())
    lig = json.loads((run / "stages/light.json").read_text())
    gate_c = json.loads((run / "stages/gate_commons.json").read_text()) if (run / "stages/gate_commons.json").exists() else None
    gate_o = json.loads((run / "stages/gate_own.json").read_text()) if (run / "stages/gate_own.json").exists() else None
    out = run / f"report_{variant}"
    figs = out / "figures"
    figs.mkdir(parents=True, exist_ok=True)
    S = Stats(cfg)
    R: dict = {"variant": variant, "run": run.name, "definitions": {}, "results": {}}
    doc = Doc()

    def label(rid: str) -> str:
        r = by_id[rid]
        if r["set"] == "own":
            return Path(r["path"]).name if not public else "own recording"
        if r["set"] == "youtube":
            return rid.split("/", 1)[1]
        return r.get("title", rid).replace("File:", "")

    # ---------------- data
    live = [r for r in recs if not r.get("duplicate_of") and r["id"] in det["recordings"]]
    hours = {s: sum(det["recordings"][r["id"]]["duration_s"] for r in live if r["set"] == s) / 3600
             for s in ("commons", "youtube", "own")}
    count = {s: sum(r["set"] == s for r in live) for s in ("commons", "youtube", "own")}
    R["data"] = {"recordings": count, "hours": {k: round(v, 2) for k, v in hours.items()},
                 "hashes": manifest.get("data"), "duplicates_left_out": [r["id"] for r in recs if r.get("duplicate_of")]}

    # ---------------- strikes
    srec = stk["recordings"]
    modes = list(det["args"]["modes"])

    def strike_group(rid: str) -> str:
        t = srec[rid]["truth"]["kind"]
        return {"marked": "own/marked", "round": "own/round", "chapters": "youtube/chapters",
                "announced": "youtube/announced"}.get(t, "")

    groups_order = ["own/marked", "own/round", "youtube/chapters", "youtube/announced"]
    strike_tab, strike_vals, lat_points, lat_stats = [], {}, {}, {}
    R["results"]["strikes"] = {}
    for g in groups_order + ["all", "all_public"]:
        ids = [rid for rid in srec if (strike_group(rid) == g or (g == "all" and strike_group(rid))
                                       or (g == "all_public" and strike_group(rid).startswith("youtube")))]
        if not ids:
            continue
        gr = {}
        for mode in modes:
            per = [srec[rid]["modes"][mode]["strikes"] for rid in ids]
            n_str = sum(len(p) for p in per)
            k = sum(s["right"] for p in per for s in p)
            lats = [s["latency_s"] for p in per for s in p if s["latency_s"] is not None]
            acc = S.ci(per, E.pooled_rate("right"))
            first = S.ci(per, E.pooled_rate("first_right"))
            lat = S.ci(per, E.pooled_median_latency)
            wl, wh = E.wilson_ci(k, n_str)
            gr[mode] = {"recordings": len(ids), "strikes": n_str, "right": k, "accuracy": acc,
                        "accuracy_wilson": {"low": wl, "high": wh}, "first_colour_right": first,
                        "latency_median_s": lat, "latency_n": len(lats),
                        "latency_mean_s": float(np.mean(lats)) if lats else None,
                        "latency_p90_s": float(np.percentile(lats, 90)) if lats else None,
                        "already_shown": sum(s["already"] for p in per for s in p)}
            if g in ("own/round",):
                conf = [[s for s, gt in zip(srec[rid]["modes"][mode]["strikes"], srec[rid]["truth"]["strikes"])
                         if not gt.get("uncertain")] for rid in ids]
                gr[mode]["confident_only"] = {"strikes": sum(len(c) for c in conf),
                                              "accuracy": S.ci(conf, E.pooled_rate("right"))}
            if g in groups_order and (not public or g.startswith("youtube")):
                shown = acc if acc.get("low") is not None else {"value": acc["value"], "low": wl, "high": wh,
                                                                  "thin": True}
                strike_vals[(GROUP_NAMES[g] + f"\n{len(ids)} rec., {n_str} strikes", MODE_NAMES[mode])] = shown
            if g == ("all_public" if public else "all"):
                lat_points[MODE_NAMES[mode]] = lats
                lat_stats[MODE_NAMES[mode]] = lat
        R["results"]["strikes"][g] = gr
        if g in groups_order:
            for mode in modes:
                x = gr[mode]
                strike_tab.append([GROUP_NAMES[g], MODE_NAMES[mode], str(x["recordings"]), str(x["strikes"]),
                                   f"{x['right']} of {x['strikes']}", fmt_ci(x["accuracy"]),
                                   fmt_ci(x["first_colour_right"]),
                                   f"{fmt_ci(x['latency_median_s'], 's')} (n = {x['latency_n']})"])
    # Ground-truth validation: the derived times against checked marks.
    valid = []
    for rid in srec:
        v = srec[rid]["truth"].get("validation")
        if v and v.get("aligned_minus_mark_s"):
            d = np.abs(v["aligned_minus_mark_s"])
            valid.append({"recording": label(rid), "strikes": len(d), "median_abs_s": float(np.median(d)),
                          "max_abs_s": float(d.max()), "within_0_1_s": int((d <= 0.1).sum()),
                          "marks_from": v.get("marks_from", "")})
    R["results"]["truth_validation"] = valid
    seg_rows = []
    for rid in srec:
        if not srec[rid]["truth"].get("segments") or by_id[rid]["set"] != "youtube":
            continue
        row = [label(rid), srec[rid]["truth"]["kind"].replace("_", " "), str(len(srec[rid]["truth"]["segments"]))]
        R["results"].setdefault("youtube_segments", {})[rid] = {}
        for mode in modes:
            sh = [s["share"] for s in srec[rid]["modes"][mode]["segments"]]
            R["results"]["youtube_segments"][rid][mode] = {"segments": len(sh), "mean_share": float(np.mean(sh)),
                                                           "per_segment": sh}
            row.append(pct(float(np.mean(sh))))
        seg_rows.append(row)

    # ---------------- colour by class (detector, Flow and Strike, over the full recording)
    cls_tab = []
    R["results"]["classes"] = {}
    for c in CLASS_ORDER:
        ids = [r["id"] for r in live if r["set"] == "commons" and r["class"] == c]
        if not ids:
            continue
        expect = by_id[ids[0]]["expect"]
        row = {"recordings": len(ids), "should_colour": expect == "bowl"}
        for mode in modes:
            vals = [det["recordings"][i]["modes"][mode]["lit_sounding_share"] for i in ids]
            row[mode] = S.ci(vals, E.mean_of)
        R["results"]["classes"][c] = row
        cls_tab.append([CLASS_NAMES[c], str(len(ids)), "yes" if expect == "bowl" else "no"] +
                       [fmt_ci(row[m]) for m in modes])

    # ---------------- the AI gate (gate_bench, mn05 on 2 s windows, first 30 s of each clip)
    gate_tab, gate_vals, gate_sum = [], {}, {}
    if gate_c:
        key = next(iter(gate_c["models"]))
        gm = gate_c["models"][key]
        R["results"]["gate"] = {"model": key, "ms_per_window": gm["ms_per_window"],
                                "window_accuracy_trained": gm["window_accuracy_trained"], "clips": gate_c["clips"],
                                "classes": {}}
        for c in CLASS_ORDER:
            rows = [x for x in gm["rows"] if x["class"] == c]
            if not rows:
                continue
            entry = {"recordings": len(rows)}
            for k in GATE_NAMES:
                entry[k] = S.ci([x[k] for x in rows], E.mean_of)
                gate_vals[(f"{CLASS_NAMES[c]} (n = {len(rows)})", GATE_NAMES[k])] = entry[k]
            R["results"]["gate"]["classes"][c] = entry
            gate_tab.append([CLASS_NAMES[c], str(len(rows)), "yes" if rows[0]["expect"] == "bowl" else "no"] +
                            [fmt_ci(entry[k]) for k in GATE_NAMES])
        for part, want in (("should_colour", "bowl"), ("should_not", "other")):
            rows = [x for x in gm["rows"] if x["expect"] == want]
            gate_sum[part] = {"recordings": len(rows), **{k: S.ci([x[k] for x in rows], E.mean_of) for k in GATE_NAMES}}
        R["results"]["gate"]["summary"] = gate_sum
    own_gate = {}
    if gate_o:
        key = next(iter(gate_o["models"]))
        rows = gate_o["models"][key]["rows"]
        for part in ("own_bowl", "own_other"):
            rs = [x[part] for x in rows if x.get("class") == "own" and x.get(part)]
            if rs:
                own_gate[part] = {"recordings": len(rs), **{k: S.ci([x[k] for x in rs], E.mean_of) for k in GATE_NAMES}}
        R["results"]["gate_own_segments"] = own_gate

    # ---------------- light
    def light_group(r: dict) -> str:
        if r["set"] == "commons":
            return "commons/bowl" if r["expect"] == "bowl" else "commons/other"
        return r["group"]

    light_order = ["commons/bowl", "commons/other", "youtube/tuneup", "youtube/on_body", "youtube/concert",
                   "own/round", "own/session"]
    light_tab, light_vals, light_short = [], {}, {}
    R["results"]["light"] = {}
    for g in light_order:
        ids = [r["id"] for r in live if light_group(r) == g and r["id"] in lig["recordings"]]
        if not ids:
            continue
        entry = {"recordings": len(ids)}
        for mode in modes:
            ms = [lig["recordings"][i][mode] for i in ids]
            cpm = S.ci([m["changes_per_min"] for m in ms], E.median_of)
            short = S.ci([m["short_share"] for m in ms if m["short_share"] is not None], E.median_of)
            over = sum(m["over_flash_limit"] for m in ms)
            flash10 = sum(not m["flash_ok"] for m in ms)
            entry[mode] = {"changes_per_min_median": cpm, "short_share_median": short,
                           "colours": sum(m["colours"] for m in ms),
                           "recordings_over_3_per_s_in_1s": over, "recordings_over_3_per_s_in_10s": flash10,
                           "max_changes_1s": max(m["max_changes_1s"] for m in ms),
                           "hold_median_s_median": E.median_of([m["hold_median_s"] for m in ms])}
            if not public or not g.startswith("own"):
                light_vals[(GROUP_NAMES[g] + f" (n = {len(ids)})", MODE_NAMES[mode])] = cpm
                light_short[(GROUP_NAMES[g] + f" (n = {len(ids)})", MODE_NAMES[mode])] = short
            light_tab.append([GROUP_NAMES[g], MODE_NAMES[mode], str(len(ids)), str(entry[mode]["colours"]),
                              fmt_ci(cpm, "n"), fmt_ci(short), f"{over} of {len(ids)}",
                              str(entry[mode]["max_changes_1s"])])
        R["results"]["light"][g] = entry

    # ---------------- figures
    fig_names = {}
    if strike_vals:
        rows = list(dict.fromkeys(r for r, _ in strike_vals))
        fig_names["strikes"] = dot_ci_figure(figs / "strikes", "Strikes that show the right chakra", rows,
                                             [MODE_NAMES[m] for m in modes], strike_vals,
                                             "share of strikes, 95 % interval", (-0.02, 1.02), percent=True)
    if lat_points:
        fig_names["latency"] = strip_figure(figs / "latency", "From the strike to the colour",
                                            lat_points, "seconds from the onset to the right colour", lat_stats)
    if gate_vals:
        rows = list(dict.fromkeys(r for r, _ in gate_vals))
        fig_names["gate"] = dot_ci_figure(figs / "gate", "Colour while a sound plays, with and without the AI gate",
                                          rows, list(GATE_NAMES.values()), gate_vals,
                                          "share of sounding time with a colour (Commons, first 30 s)", (-0.02, 1.02),
                                          percent=True)
    if light_vals:
        rows = list(dict.fromkeys(r for r, _ in light_vals))
        fig_names["light"] = dot_ci_figure(figs / "light", "How often the colour changes", rows,
                                           [MODE_NAMES[m] for m in modes], light_vals,
                                           "colour changes a minute (median, 95 % bootstrap interval)")
        fig_names["short"] = dot_ci_figure(figs / "short", "Colours that stand under 3 s", rows,
                                           [MODE_NAMES[m] for m in modes], light_short,
                                           "share of colours shorter than 3 s (median over recordings)", (-0.02, 1.02),
                                           percent=True)
    R["figures"] = fig_names

    # ---------------- text
    git = manifest.get("git", {})
    ch = git.get("chakra", {})
    hashes = manifest.get("data", {})
    st_all = R["results"]["strikes"].get("all_public" if public else "all", {})
    subtitle = (f"Run {run.name}, chakra {str(ch.get('commit'))[:7]}"
                f"{' with local changes' if ch.get('dirty_files') else ''}, {variant} variant")
    flow, strike = st_all.get("flow", {}), st_all.get("strike", {})
    doc.h(1, "Summary")
    head = []
    if flow:
        head.append(f"**Strikes:** Flow shows the right chakra for {flow['right']} of {flow['strikes']} strikes "
                    f"({fmt_ci(flow['accuracy'])}), Strike for {strike['right']} of {strike['strikes']} "
                    f"({fmt_ci(strike['accuracy'])}); {flow['recordings']} recordings"
                    f"{' (YouTube only)' if public else ''}.")
        head.append(f"**Latency** from the onset to the right colour, median: Flow {fmt_ci(flow['latency_median_s'], 's')}"
                    f" (n = {flow['latency_n']}), Strike {fmt_ci(strike['latency_median_s'], 's')} (n = {strike['latency_n']}).")
    if public:
        om = [R["results"]["strikes"].get(g, {}) for g in ("own/marked", "own/round")]
        if any(om):
            parts = []
            for mode in modes:
                k = sum(g[mode]["right"] for g in om if g)
                n = sum(g[mode]["strikes"] for g in om if g)
                nrec = sum(g[mode]["recordings"] for g in om if g)
                parts.append(f"{MODE_NAMES[mode]} {k} of {n}")
            head.append(f"**The owner's own rounds** (seven bowls struck root to crown, {nrec} recordings, shown "
                        f"here as numbers only): {', '.join(parts)} strikes right.")
    if gate_sum:
        a, b = gate_sum["should_colour"], gate_sum["should_not"]
        head.append(f"**AI gate** (mn05, {R['results']['gate']['clips']} Commons clips): sounds that should colour keep "
                    f"{fmt_ci(a['no_ai'])} without AI, {fmt_ci(a['ai_pretrained'])} pretrained, {fmt_ci(a['ai_trained'])} "
                    f"with the trained head; sounds that should not colour drop from {fmt_ci(b['no_ai'])} to "
                    f"{fmt_ci(b['ai_pretrained'])} and {fmt_ci(b['ai_trained'])} (n = {a['recordings']} and {b['recordings']}).")
    ses = R["results"]["light"].get("youtube/on_body") or {}
    if ses:
        head.append(f"**Light in real sessions** ({ses['recordings']} YouTube sessions): "
                    f"Flow {fmt_ci(ses['flow']['changes_per_min_median'], 'n')} colour changes a minute, "
                    f"Strike {fmt_ci(ses['strike']['changes_per_min_median'], 'n')}; recordings with more than three "
                    f"changes in any second: Flow {ses['flow']['recordings_over_3_per_s_in_1s']}, "
                    f"Strike {ses['strike']['recordings_over_3_per_s_in_1s']}.")
    doc.ul(head)
    doc.p("Every number carries its n. Intervals are 95 % percentile bootstrap intervals over recordings "
          f"({S.n} draws, seed {S.seed}): recordings, not strikes or windows, are the independent unit.")

    doc.h(1, "Data")
    rows = [["Wikimedia Commons clips", str(count["commons"]), f"{hours['commons']:.2f}",
             "free licences, 8 classes (fetch_clips.py)", f"`{hashes.get('commons', {}).get('sha256', '')[:12]}`"],
            ["YouTube videos", str(count["youtube"]), f"{hours['youtube']:.2f}",
             "tune-ups, sessions on the body, concerts; not redistributed", f"`{hashes.get('youtube', {}).get('sha256', '')[:12]}`"],
            ["Own recordings", str(count["own"]), f"{hours['own']:.2f}",
             "the owner's rounds and sessions; private, counted only" if public else "private (own_media)",
             f"`{hashes.get('own', {}).get('sha256', '')[:12]}`"]]
    doc.table(["Set", "Recordings", "Hours", "What", "Data list hash"], rows)
    dups = R["data"]["duplicates_left_out"]
    cleaning = manifest.get("data_cleaning", {})
    R["data"]["cleaning"] = cleaning
    doc.p(f"Left out as duplicates: {', '.join(d.split('/', 1)[1] for d in dups) or 'none'} (the same audio as "
          "u1KjQTAHSlo under another channel, shifted by 1.7 s). Commons clips are single recordings of one "
          "instrument, not sessions; the YouTube and own sessions are the nearest thing to a room.")
    if cleaning.get("commons_pronunciations_left_out"):
        doc.p(f"**Data cleaning:** {cleaning['commons_pronunciations_left_out']} Commons files found by the word "
              "searches are recordings of someone saying the word ('shaker', 'gong', 'maraca': Lingua Libre and "
              "Wiktionary pronunciations), not the instrument. They are left out here; the README table of "
              "25 October still counted 12 of them among its 91 clips (11 as shakers, 1 as a gong).")

    doc.h(1, "Method")
    doc.ul([
        "**Detector:** the app's own C code (`chakra_detect.c` through `tests/analyse_wav.c`, built into the run "
        "folder), notes at A = 440 Hz, sensitivity 8. **Flow** locks a tone after 384 ms and switches after 1500 ms; "
        "**Strike** after 128 and 256 ms. Both have the strike path (spectral flux) that lets a new bowl take over a "
        "louder ringing one.",
        "**Onsets** (reference, independent of the detector's decisions): peaks of the rectified log-magnitude "
        "spectral flux, 60 to 4000 Hz, 16 ms hops, corrected by 42 ms so a synthetic strike is found within 10 ms.",
        "**A strike is right** when its chakra is shown at some time from 0.1 s before the onset until the next "
        "strike or 3 s, whichever comes first. **First colour right**: the first colour that starts in that window is "
        "the right one. **Latency**: from the onset to the start of the right colour; a strike whose colour already "
        "showed (the same bowl again) counts for accuracy, not for latency.",
        "**Colour share:** the share of the sounding time (RMS above 3 % of the loudest 0.1 s) with any colour. "
        "The gate comparison uses gate_bench.py's own time base: the first 30 s of each clip.",
        "**Light:** colour changes a minute (a new chakra, or colour to dark), the share of colours that stand "
        "under 3 s, and the most changes in any 1 s window against the photosensitivity guidance of at most three "
        "flashes a second (light_timeline.py).",
        "**AI gate:** EfficientAT mn05 on 2 s windows; pretrained = the best bowl-like AudioSet class beats the best "
        "other class; trained = a logistic head on the embedding, 5-fold by clip (a clip is never in its own training).",
    ])
    doc.h(2, "Ground truth and how far to trust it")
    gt_rows = [
        ["Own rounds, hand-marked", "a person marked each strike and its bowl (frames, or flux onsets checked "
         "against a contact sheet)", "the flux onset found by the order alignment, when it lies within 1.6 s of the mark",
         "chakra high; time high where the marks were checked (see validation)"],
        ["Own rounds, derived", "the seven bowls are struck root to crown (the owner's set, tuned C3 to B3)",
         "onsets aligned to that order by dynamic programming: each bowl takes the onset where its fundamental "
         "gained most new energy", "chakra high; time medium, validated on the round with checked marks"],
        ["YouTube, chapters (-ar9vsmFhJU)", "the player's chapter list, root to crown, 'each bowl is struck 3 times'",
         "the 3 strongest onsets of each chapter, at least 4 s apart", "chakra high; time medium"],
        ["YouTube, announced (u1KjQTAHSlo)", "automatic captions: the seller names each bowl's chakra and note",
         "the strongest onset after the sentence and before the next one", "chakra high; time medium"],
        ["YouTube, crystal chapters (8Qhh2VF1Oz4)", "chapter numbers read as chakra numbers, as the description says",
         "none (bowls rung on): only the share of chapter time with the intended colour", "medium"],
    ]
    doc.table(["Source", "Chakra from", "Strike time from", "Reliability"], gt_rows)
    if valid:
        vrows = [[v["recording"] if not public else f"own round {i + 1}", str(v["strikes"]),
                  f"{v['median_abs_s']:.2f} s", f"{v['max_abs_s']:.2f} s", f"{v['within_0_1_s']} of {v['strikes']}",
                  v["marks_from"] if not public else ("checked marks" if v["median_abs_s"] < 0.2 else "marks from 1 fps frames")]
                 for i, v in enumerate(valid)]
        doc.p("**Validation of the derived strike times:** the order alignment, run without the marks, against the "
              "hand marks of the two marked rounds.")
        doc.table(["Round", "Strikes", "Median offset", "Largest", "Within 0.1 s", "Marks from"], vrows)
        doc.p("Where the marks were checked against the flux, the alignment finds the same strikes within a few "
              "hundredths of a second; where they came from video frames at one a second, the frames lag the sound by "
              "up to 1.5 s, so those marks give the bowl but not the time.")

    doc.h(1, "Results")
    doc.h(2, "Strikes")
    if public:
        strike_tab = [r for r in strike_tab if not r[0].startswith("Own")] + \
                     [r for r in strike_tab if r[0].startswith("Own")]
    doc.table(["Ground truth", "Mode", "Recordings", "Strikes", "Right", "Accuracy [95 % CI]",
               "First colour right", "Latency, median [95 % CI]"], strike_tab)
    rr = R["results"]["strikes"].get("own/round", {})
    if rr:
        f = rr[modes[0]]
        doc.p(f"Own rounds with derived times: {f['strikes'] - f['confident_only']['strikes']} of {f['strikes']} "
              f"strikes are flagged uncertain (their bowl gained under 10 % of the new energy). Without them, "
              + ", ".join(f"{MODE_NAMES[m]} {fmt_ci(rr[m]['confident_only']['accuracy'])}" for m in modes) + ".")
    if "strikes" in fig_names:
        doc.fig("strikes", "Share of strikes that show the right chakra, Flow and Strike. Filled marks and thick lines: "
                "95 % bootstrap interval over recordings; hollow marks and thin lines: a group of one recording, "
                "Wilson interval over its strikes" + (" (YouTube; the own rounds are in the table)." if public else "."))
    if "latency" in fig_names:
        doc.fig("latency", "Each point is one strike whose colour changed to the right chakra; the bar is the median "
                "with its 95 % bootstrap interval over recordings.")
    if seg_rows:
        doc.p("**Time with the intended colour** in the YouTube tune-ups, mean over their chapters or announced bowls:")
        doc.table(["Video", "Truth", "Segments"] + [MODE_NAMES[m] for m in modes], seg_rows)
        doc.p("-ar9vsmFhJU's root bowl rings at G#3 (208 Hz): by note the app calls it Throat, so the root chapter "
              "scores near zero by design. The app's answer is to learn your bowls (Settings, mapping 'your bowls').")

    doc.h(2, "Colour by sound class")
    doc.table(["Sound", "Recordings", "Should colour"] + [f"{MODE_NAMES[m]}, colour share" for m in modes], cls_tab)
    doc.p("Mean share of the sounding time with any colour, whole recordings, 95 % bootstrap interval over recordings.")
    if gate_tab:
        g = R["results"]["gate"]
        doc.h(2, "No AI, pretrained gate, trained head")
        doc.table(["Sound", "Recordings", "Should colour"] + list(GATE_NAMES.values()), gate_tab)
        doc.p(f"gate_bench.py, {g['model']}, {g['clips']} clips; the trained head gets {100 * g['window_accuracy_trained']:.0f} % "
              f"of windows right on clips it never saw; {g['ms_per_window']:.0f} ms a window on the Mac's CPU.")
        if "gate" in fig_names:
            doc.fig("gate", "Share of sounding time with a colour per class: no AI, the pretrained gate and the trained head.")
        if own_gate:
            rows = []
            for part, name in (("own_bowl", "Own segments, bowl or bell (should colour)"),
                               ("own_other", "Own segments, other sounds (should not)")):
                if part in own_gate:
                    x = own_gate[part]
                    rows.append([name, str(x["recordings"])] + [fmt_ci(x[k]) for k in GATE_NAMES])
            doc.p("The owner's recordings, segments a person checked (2 s labels), gate trained with them in the folds:")
            doc.table(["Segments", "Recordings"] + list(GATE_NAMES.values()), rows)

    doc.h(2, "Flow against Strike")
    cmp_rows = []
    for name, getter, kind in [
        ("Strikes right" + (" (YouTube)" if public else ""), lambda m: st_all.get(m, {}).get("accuracy"), "pct"),
        ("First colour right", lambda m: st_all.get(m, {}).get("first_colour_right"), "pct"),
        ("Latency, median", lambda m: st_all.get(m, {}).get("latency_median_s"), "s"),
        ("Singing bowls, colour share", lambda m: R["results"]["classes"].get("singing_bowl", {}).get(m), "pct"),
        ("Gongs, colour share", lambda m: R["results"]["classes"].get("gong", {}).get(m), "pct"),
        ("Sessions, changes a minute", lambda m: (R["results"]["light"].get("youtube/on_body") or {}).get(m, {}).get("changes_per_min_median"), "n"),
        ("Concerts, changes a minute", lambda m: (R["results"]["light"].get("youtube/concert") or {}).get(m, {}).get("changes_per_min_median"), "n"),
        ("Sessions, colours under 3 s", lambda m: (R["results"]["light"].get("youtube/on_body") or {}).get(m, {}).get("short_share_median"), "pct"),
    ]:
        cmp_rows.append([name] + [fmt_ci(getter(m), kind) for m in modes])
    doc.table(["Measure"] + [MODE_NAMES[m] for m in modes], cmp_rows)
    R["results"]["flow_vs_strike"] = {r[0]: dict(zip(modes, r[1:])) for r in cmp_rows}

    doc.h(2, "Light")
    doc.table(["Recordings", "Mode", "n", "Colours", "Changes a minute, median", "Colours under 3 s, median",
               "Over 3 changes in 1 s", "Most changes in 1 s"], light_tab)
    if "light" in fig_names:
        doc.fig("light", "Colour changes a minute per group of recordings, Flow and Strike: median and 95 % bootstrap "
                "interval over recordings.")
    if "short" in fig_names:
        doc.fig("short", "Share of colours that stand under 3 s (median over recordings).")

    doc.h(1, "Limitations")
    doc.ul([
        "**Small n.** The strike results rest on " + (f"{R['results']['strikes'].get('all', {}).get('flow', {}).get('recordings', 0)}"
                                                      if not public else f"{flow.get('recordings', 0)}") +
        " recordings; with so few, a bootstrap interval over recordings is coarse and can collapse to a point when "
        "every recording scores the same. Wilson intervals over strikes are in report.json; they assume independent "
        "strikes, which they are not.",
        "**Commons clips are not sessions.** They are short recordings of one instrument, often close up; a room with "
        "several bowls, voices and a gong at once is harder. The YouTube sessions and concerts have no ground truth "
        "and serve only the light measures.",
        "**Labels by one person.** The own segment labels, the strike marks and the check of the derived times were "
        "made by one person; nobody measured agreement between two.",
        "**Teacher-labelled data.** Unchecked labels of the own videos were proposed by an AudioSet model (EfficientAT "
        "dymn04); the gate's trained head uses only checked segments, but its pretrained teacher shaped what was "
        "checked. The YouTube chakra labels are what the players say their bowls are, not measured pitches.",
        "**One set of own bowls, one room.** All own rounds use the same seven bowls; another set, mallet or room "
        "may behave differently.",
        "**Not the board.** The detector runs on the Mac from audio files; the ESP32-S31's microphone, its gain and "
        "its timing are not measured here, and the lamps add their own delay.",
    ])

    doc.h(1, "Reproduce")
    chv = git.get("chakrav", {})
    doc.ul([
        f"chakra (ChakraA) commit `{str(ch.get('commit'))[:12]}` ({ch.get('subject', '')})"
        + (f", with local changes in {len(ch.get('dirty_files', []))} files (diff hash `{str(ch.get('diff_sha256'))[:12]}`)"
           if ch.get("dirty_files") else ""),
        f"chakrav commit `{str(chv.get('commit'))[:12]}`",
        f"config hash `{manifest.get('config_sha256', '')[:12]}`; detector sources hash "
        f"`{str(manifest.get('code', {}).get('detector_sources_sha256', ''))[:12]}`; data hash `{hashes.get('all', '')[:12]}`",
        f"tools: Python {manifest.get('tools', {}).get('python')}, numpy {manifest.get('tools', {}).get('numpy')}, "
        f"torch {manifest.get('tools', {}).get('torch')}, {manifest.get('tools', {}).get('ffmpeg', '')}",
        "command: `tools/bench/pipeline.py` (stages fetch, own, detect, strikes, gate, light, report); "
        "`--run latest --from report --force` redraws only the report.",
    ])

    doc.h(1, "Sources")
    yt = [r for r in live if r["set"] == "youtube"]
    doc.p("YouTube (watched for evaluation only, nothing redistributed): " +
          "; ".join(f"[{r['id'].split('/', 1)[1]}]({r['page']})" + (f" ({r['channel']})" if r.get("channel") else "")
                    for r in yt) + ".")
    doc.p(f"Wikimedia Commons: {count['commons']} clips under free licences, listed with title, page, licence and author "
          "in commons_sources.csv beside this report.")
    with open(out / "commons_sources.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["class", "title", "page", "licence", "author"])
        for r in live:
            if r["set"] == "commons":
                w.writerow([r["class"], r["title"].replace("File:", ""), r.get("page") or "", r.get("licence") or "",
                            re.sub(r"<[^>]+>", "", r.get("author") or "")])

    # ---------------- per-recording appendix
    rows = []
    for rid in sorted(srec):
        if public and by_id[rid]["set"] == "own":
            continue
        for mode in modes:
            sc = srec[rid]["modes"][mode]["strikes"]
            if sc:
                rows.append([label(rid), MODE_NAMES[mode], f"{sum(s['right'] for s in sc)} of {len(sc)}",
                             " ".join(("+" if s["right"] else "x") + E.CHAKRAS[s["chakra"]].split()[0][:4] for s in sc)])
    if rows:
        doc.h(2, "Per recording")
        doc.table(["Recording", "Mode", "Right", "Strikes in order (+ right, x wrong)"], rows)
    R["results"]["per_recording"] = {rid: {m: srec[rid]["modes"][m]["strikes"] for m in modes} for rid in srec
                                     if not (public and by_id[rid]["set"] == "own")}
    R["provenance"] = {"git": {"chakra": {k: ch.get(k) for k in ("commit", "subject", "diff_sha256")},
                               "chakrav": {k: chv.get(k) for k in ("commit", "subject")}},
                       "dirty_files": len(ch.get("dirty_files", [])), "config_sha256": manifest.get("config_sha256"),
                       "data": hashes, "tools": manifest.get("tools"),
                       "detector_sources_sha256": manifest.get("code", {}).get("detector_sources_sha256")}

    title = "ChakraA: how well the app hears a singing bowl"
    (out / "report.md").write_text(doc.markdown(title, subtitle))
    (out / "report.json").write_text(E.dumps(R))
    page = None
    try:
        sys.path.insert(0, str(Path.home() / "Coding/company/tools/design_agent"))
        import docgen
        page = docgen.seite(title, subtitle, meta=["LeopardCode.AI", f"Run {run.name}", variant], content=doc.html_body(),
                            sprache="en", zusatz_stil="figure img{width:100%} td,th{font-size:9pt}")
    except Exception as exc:  # docgen is the house style; without it the Markdown still stands
        print(f"  docgen not available ({exc}); report.html skipped", flush=True)
    if page:
        (out / "report.html").write_text(page)
        if public:
            leak_check(out, recs)
        if pdf:
            try:
                docgen.rendere_pdf(out / "report.html")
            except Exception as exc:
                print(f"  PDF not rendered ({exc})", flush=True)
    if public:
        leak_check(out, recs)
    print(f"  report {variant}: {out}", flush=True)
    return out


def main():
    import argparse
    import tomllib
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run", type=Path)
    ap.add_argument("--variant", choices=["private", "public"], default="private")
    ap.add_argument("--no-pdf", action="store_true")
    a = ap.parse_args()
    cfg = tomllib.loads((a.run / "config.toml").read_text())
    build(a.run, cfg, a.variant, pdf=not a.no_pdf)


if __name__ == "__main__":
    main()
