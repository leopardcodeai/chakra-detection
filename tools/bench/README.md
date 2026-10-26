# tools/bench: measuring ChakraA's audio side

Each script has a header (what, why, pitfalls, usage) and `--dry-run`. Data and results live in the
private state folder `~/Library/Application Support/LeopardCode/chakra/`, never in this repository;
only the public evaluation report is copied to `docs/evaluation/`.

| Script | What it does |
|---|---|
| `pipeline.py` | The whole evaluation as stages: fetch, own, detect, strikes, gate, light, report. One folder per run in `eval_runs/` with the config, git commits, data hashes and every stage's output. `--from`, `--to`, `--only`, `--run latest`, `--force`, `--dry-run`. |
| `pipeline.toml` | Its configuration: paths, the detector's Flow and Strike settings, strike window, gate model, light thresholds, report variants. |
| `eval_lib.py` | Definitions the stages share: flux onsets, the order alignment for derived strike times, strike scores, light measures, bootstrap and Wilson intervals. |
| `eval_report.py` | The report of a run: `report.md`, `report.json`, `report.html` and `.pdf` (docgen), figures as PNG and SVG in light and dark; a private and a public variant. |
| `strike_truth.json` | Ground truth for public YouTube tune-ups: chakra per chapter or spoken caption, how the strike times are derived, and how reliable each is. |
| `test_eval_lib.py` | Tests of the definitions on synthetic strikes (onset timing within 10 ms, order alignment, scoring, intervals). |
| `fetch_clips.py` | Freely licensed Wikimedia Commons clips per sound class, with licences in `sources.json`. |
| `export_album.py` | The owner's Photos album, one video at a time, into the private folder. |
| `label_own.py` | Proposed instrument labels and contact sheets for the own videos; `own_labels.json` with checked flags. |
| `gate_bench.py` | The detector with and without the local AI gate (EfficientAT, pretrained and trained head). |

Run it (the AI venv has numpy, scipy, matplotlib and torch; the script switches to it by itself):

```sh
tools/bench/pipeline.py --dry-run                     # what would run, nothing written
tools/bench/pipeline.py                               # everything, a new run folder
tools/bench/pipeline.py --from detect                 # no network, no Photos
tools/bench/pipeline.py --run latest --only report --force   # redraw the report of the last run
"$HOME/Library/Application Support/LeopardCode/chakra/ai_venv_eat/bin/python" tools/bench/test_eval_lib.py
```
