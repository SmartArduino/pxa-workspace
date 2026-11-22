#!/usr/bin/env python3
"""Plot completed randomized fill trials and their paired treatment effects."""
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
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    summary = json.loads((args.directory / "summary.json").read_text())
    samples = [s for s in json.loads((args.directory / "samples.json").read_text()) if not s["warmup"]]
    colors = {"A": "#3778bf", "B": "#db7834"}
    labels = {"A": "Default", "B": "BUILTINS"}
    fig, axes = plt.subplots(1, 3, figsize=(14.5, 5.3), gridspec_kw={"width_ratios": [1, 1, 1.15]})
    for mode, title in enumerate(("Constant fill", "Changing-color fill")):
        ax = axes[mode]
        for pair in range(1, summary["valid_pairs"] + 1):
            group = [s for s in samples if s["pair"] == pair]
            ax.plot([(s["pair"] - 1) * 2 + s["slot"] for s in group],
                    [s["modes"][mode]["lcd_fps"] for s in group], color="#c8c8c8", lw=1, zorder=1)
        for variant in ("A", "B"):
            group = [s for s in samples if s["variant"] == variant]
            ax.scatter([(s["pair"] - 1) * 2 + s["slot"] for s in group],
                       [s["modes"][mode]["lcd_fps"] for s in group],
                       color=colors[variant], label=labels[variant], s=30, zorder=2)
        ax.set(title=title, xlabel="Run order (warmups excluded)", ylabel="Completed LCD transfers / second")
        ax.grid(alpha=.2)
        ax.legend(frameon=False, fontsize=9)
    ax = axes[2]
    primary = summary["primary"]
    values = primary["paired_changes_percent"]
    positions = list(range(1, len(values) + 1))
    ax.scatter(values, positions, color="#666666", s=25, label="Each paired block")
    center = primary["fps_change_percent"]
    low, high = primary["paired_bootstrap_ci95_percent"]
    ax.errorbar(center, 0, xerr=[[max(0, center - low)], [max(0, high - center)]],
                fmt="o", color="#202020", capsize=5, lw=2, label="Overall + 95% paired bootstrap CI")
    ax.axvline(0, color="#444444", lw=1)
    if summary["plan"].get("system", "pxa") == "pxa":
        ax.axvline(-6, color="#c54949", ls="--", lw=1.2, label="Earlier single-run estimate (-6%)")
    ax.set(title="Combined fill FPS: BUILTINS vs default", xlabel="FPS change (%)", ylabel="Paired block")
    ax.set_yticks([0] + positions)
    ax.set_yticklabels(["Overall"] + [str(i) for i in positions])
    ax.invert_yaxis()
    ax.grid(alpha=.2)
    ax.legend(frameon=False, fontsize=8, loc="upper center", bbox_to_anchor=(.5, -.2))
    system = summary["plan"].get("system", "pxa").upper()
    fig.suptitle(f'{system}: fixed {summary["valid_pairs"]}-pair randomized crossover on one Pai Touch', fontsize=14)
    fig.tight_layout(rect=(0, 0.03, 1, .94))
    fig.text(.01, .005, "Actual device completion counters; two fill workloads per run. CI and inference use independent pairs, not individual frames.", fontsize=8, color="#555555")
    target = args.directory / "comparison.png"
    fig.savefig(target, dpi=160)
    print(target)


if __name__ == "__main__":
    main()
