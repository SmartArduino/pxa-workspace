#!/usr/bin/env python3
"""Compare run variation and paired fill effects for both actual devices."""
from pathlib import Path
import argparse
import json
import math
import statistics
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    args = parser.parse_args()
    summary = json.loads(args.summary.read_text())
    fig, axes = plt.subplots(1, 2, figsize=(11.5, 5.1), gridspec_kw={"width_ratios": [1.2, 1]})
    palette = {"A": "#3778bf", "B": "#db7834"}
    positions = {"pxa": {"A": 1, "B": 2}, "aster": {"A": 4, "B": 5}}
    ax = axes[0]
    for system in ("pxa", "aster"):
        data = [s for s in json.loads((Path(summary[system]["directory"]) / "samples.json").read_text()) if not s["warmup"]]
        for pair in range(1, summary[system]["pairs"] + 1):
            group = sorted([s for s in data if s["pair"] == pair], key=lambda s: s["variant"])
            points = [math.exp(statistics.mean(math.log(m["lcd_fps"]) for m in s["modes"])) for s in group]
            offset = (pair - (summary[system]["pairs"] + 1) / 2) * .022
            xs = [positions[system][s["variant"]] + offset for s in group]
            ax.plot(xs, points, color="#cccccc", lw=.8, zorder=1)
            for s, x, y in zip(group, xs, points):
                ax.scatter(x, y, color=palette[s["variant"]], s=22, zorder=3)
        for variant in ("A", "B"):
            values = [math.exp(statistics.mean(math.log(m["lcd_fps"]) for m in s["modes"]))
                      for s in data if s["variant"] == variant]
            x = positions[system][variant]
            ax.plot([x-.22, x+.22], [statistics.median(values)]*2, color="#333333", lw=2, zorder=2)
    ax.set_xticks([1, 2, 4, 5], ["PXA\ndefault", "PXA\nBUILTINS", "Aster\ndefault", "Aster\nBUILTINS"])
    ax.set(xlim=(.5, 5.5), ylabel="Geometric mean LCD FPS (two fill scenes)", title="Independent run variation")
    ax.grid(axis="y", alpha=.2)
    ax = axes[1]
    for row, system in enumerate(("pxa", "aster")):
        data = summary[system]["fill_primary"]
        point = data["fps_change_percent"]
        lo, hi = data["paired_bootstrap_ci95_percent"]
        ax.errorbar(point, row, xerr=[[max(0, point-lo)], [max(0, hi-point)]],
                    fmt="o", color="#222222", capsize=5, lw=2)
        ax.annotate(f"{point:+.2f}%  [{lo:+.2f}%, {hi:+.2f}%]", (point, row),
                    xytext=(0, 16), textcoords="offset points", ha="center", fontsize=9)
    ax.axvline(0, color="#444444", lw=1)
    ax.axvline(-6, color="#c54949", lw=1.2, ls="--", label="Earlier PXA single-run estimate (-6%)")
    ax.set_yticks([0, 1], ["PXA", "Aster"])
    ax.set(ylim=(1.55, -.55), xlabel="BUILTINS vs default: FPS change (%)", title="Paired effect and 95% bootstrap interval")
    ax.grid(alpha=.2)
    ax.legend(loc="upper center", bbox_to_anchor=(.5, -.18), frameon=False, fontsize=8)
    fig.suptitle("Repeated measurements on two Pai Touch devices", fontsize=14)
    fig.tight_layout(rect=(0, .055, 1, .94))
    fig.text(.01, .012, "12 randomized paired blocks per system; warmup runs excluded. Native completion counters; no firmware swapping between devices.", fontsize=8, color="#555555")
    target = args.summary.with_name("noise-comparison.png")
    fig.savefig(target, dpi=160)
    print(target)


if __name__ == "__main__":
    main()
