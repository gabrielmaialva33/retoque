"""How much of NR's change at WorkingScale 1.0 survives at 0.75 and 0.5, on a replayed game clip.

edit_s = NR output at scale s minus the game's frame (linear Rec.709, float16 RGBA, alpha ignored).
kept_s = <edit_s, edit_1> / <edit_1, edit_1>: the share of the full-scale change, in its direction.
corr_s = Pearson correlation of edit_s and edit_1 (shape agreement, scale-free).
The same on the fine band: edit minus a Gaussian blur (sigma 2 px), the detail the model adds.
Computed on log2 luminance as well, where the composition works (RenoDX ratio).
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter

CAPTURE = Path(sys.argv[1])
RUNS = Path(sys.argv[2])
SCALES = ["s100", "s75", "s50"]


def load(path, w, h, pitch):
    raw = np.fromfile(path, dtype=np.uint8)
    rows = raw.reshape(h, pitch)[:, : w * 8]
    return rows.view(np.float16).reshape(h, w, 4)[..., :3].astype(np.float32)


def luminance(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


capture = json.loads(CAPTURE.read_text())
runs = {s: {f["id"]: f for f in json.loads((RUNS / s / "run.json").read_text())["frames"]} for s in SCALES}
rows = []
for frame in capture["frames"]:
    fid = frame["id"]
    if not all(runs[s][fid]["kind"] == "every" for s in SCALES):
        continue
    c = frame["color"]
    game = load(CAPTURE.parent / c["path"], c["width"], c["height"], c["row_pitch"])
    outs = {}
    for s in SCALES:
        o = runs[s][fid]["output"]
        outs[s] = load(RUNS / s / o["path"], o["width"], o["height"], o["row_pitch"])
    lg = np.log2(np.maximum(luminance(game), 1e-4))
    row = {"id": fid}
    for space in ("rgb", "log2lum"):
        if space == "rgb":
            edits = {s: outs[s] - game for s in SCALES}
            fine = {s: e - gaussian_filter(e, sigma=(2, 2, 0)) for s, e in edits.items()}
        else:
            edits = {s: np.log2(np.maximum(luminance(outs[s]), 1e-4)) - lg for s in SCALES}
            fine = {s: e - gaussian_filter(e, sigma=2) for s, e in edits.items()}
        for band, d in (("all", edits), ("fine", fine)):
            ref = d["s100"].ravel().astype(np.float64)
            denom = float(ref @ ref)
            for s in ("s75", "s50"):
                x = d[s].ravel().astype(np.float64)
                row[f"{space}.{band}.kept.{s}"] = float(x @ ref) / denom if denom > 0 else float("nan")
                row[f"{space}.{band}.corr.{s}"] = float(np.corrcoef(x, ref)[0, 1])
                row[f"{space}.{band}.energy.{s}"] = float(np.sqrt((x @ x) / denom)) if denom > 0 else float("nan")
    row["mean_abs_log2_edit_s100"] = float(np.mean(np.abs(edits["s100"])))
    rows.append(row)
    print(fid, {k: round(v, 3) for k, v in row.items() if k != "id"}, flush=True)

keys = [k for k in rows[0] if k != "id"]
summary = {k: {"median": float(np.median([r[k] for r in rows])), "p10": float(np.percentile([r[k] for r in rows], 10)),
               "p90": float(np.percentile([r[k] for r in rows], 90))} for k in keys}
out = {"frames": len(rows), "summary": summary, "rows": rows}
(CAPTURE.parent.parent / "retention.json").write_text(json.dumps(out, indent=1))
print(json.dumps({"frames": len(rows), **{k: round(v["median"], 3) for k, v in summary.items()}}, indent=1))
