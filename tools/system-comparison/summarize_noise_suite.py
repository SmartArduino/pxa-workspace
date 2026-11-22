#!/usr/bin/env python3
"""Summarize every measured CPU workload and the repeated fill experiments."""
from pathlib import Path
import argparse
import itertools
import json
import math
import random
import statistics
import sys

sys.dont_write_bytecode = True
from pxa_fill_noise import quantile, save


def paired_time(pairs, kind, seed, resolution_ms):
    a = [p["A"]["cpu"][kind]["median_ms"] for p in pairs]
    b = [p["B"]["cpu"][kind]["median_ms"] for p in pairs]
    diffs = [math.log(y / x) for x, y in zip(a, b)]
    observed = statistics.mean(diffs)
    rng = random.Random(seed)
    bootstrap = [statistics.mean(rng.choices(diffs, k=len(diffs))) for _ in range(20000)]
    slot_changes = []
    for pair in pairs:
        first = pair["first"]["cpu"][kind]["median_ms"]
        second = pair["second"]["cpu"][kind]["median_ms"]
        slot_changes.append(math.log(second / first))
    null = []
    for indices in itertools.combinations(range(len(pairs)), len(pairs) // 2):
        ab = set(indices)
        null.append(statistics.mean(d if i in ab else -d for i, d in enumerate(slot_changes)))
    percent = lambda value: math.expm1(value) * 100
    output = {"name": pairs[0]["A"]["cpu"][kind]["name"],
              "baseline_geomean_ms": math.exp(statistics.mean(math.log(v) for v in a)),
              "candidate_geomean_ms": math.exp(statistics.mean(math.log(v) for v in b)),
              "time_change_percent": percent(observed),
              "paired_bootstrap_ci95_percent": [percent(quantile(bootstrap, q)) for q in (.025, .975)],
              "exact_balanced_randomization_p_two_sided":
                  sum(abs(v) >= abs(observed) - 1e-14 for v in null) / len(null),
              "paired_time_changes_percent": [percent(v) for v in diffs],
              "clock_resolution_ms": resolution_ms,
              "quantization_only_ratio_sensitivity_percent": [
                  percent(statistics.mean(math.log(max(y-resolution_ms, 1e-9)/(x+resolution_ms)) for x, y in zip(a, b))),
                  percent(statistics.mean(math.log((y+resolution_ms)/max(x-resolution_ms, 1e-9)) for x, y in zip(a, b)))],
              "baseline_run_medians_ms": a, "candidate_run_medians_ms": b}
    return output


def summarize(directory, system):
    frame = json.loads((directory / "summary.json").read_text())
    plan = frame["plan"]
    samples = [s for s in json.loads((directory / "samples.json").read_text()) if not s["warmup"]]
    pairs = []
    for index, order in enumerate(plan["order"], 1):
        group = sorted([s for s in samples if s["pair"] == index], key=lambda x: x["slot"])
        if len(group) != 2 or "".join(s["variant"] for s in group) != order:
            raise ValueError(f"Incomplete or reordered pair: {system} {index}")
        pair = {s["variant"]: s for s in group}
        pair.update({"first": group[0], "second": group[1], "order": order})
        pairs.append(pair)
    tests = [paired_time(pairs, kind, plan["seed"] + 100 + kind, 1 if system == "aster" else .001) for kind in range(5)]
    # CPU analyses were added after the original fill-only primary endpoint;
    # label them exploratory and control the five-CPU-test family per system.
    ordered = sorted(range(5), key=lambda i: tests[i]["exact_balanced_randomization_p_two_sided"])
    running = 0
    for rank, index in enumerate(ordered):
        running = min(1, max(running, (5-rank)*tests[index]["exact_balanced_randomization_p_two_sided"]))
        tests[index]["holm_adjusted_cpu_family_p"] = running
    stages = []
    for mode in (0, 1):
        item = {"mode": mode}
        for variant in ("A", "B"):
            selected = [s["modes"][mode] for s in samples if s["variant"] == variant]
            keys = ["paint_ms", "present_call_ms", "guest_submit_fps", "lcd_fps", "window_us"]
            if system == "aster":
                keys += ["native_rotate_ms", "native_transfer_ms", "counter_start_roundtrip_ms", "counter_end_roundtrip_ms"]
            item[variant] = {key: {"median": statistics.median(s[key] for s in selected),
                                   "range": [min(s[key] for s in selected), max(s[key] for s in selected)]}
                             for key in keys}
        stages.append(item)
    return {"directory": str(directory), "pairs": len(pairs), "formal_runs": len(samples),
            "fill_primary": frame["primary"], "fill_modes": frame["secondary_modes"],
            "cpu_exploratory": tests, "stages": stages,
            "failed_attempts": [str(p) for p in directory.rglob("failure.json")]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pxa", type=Path, required=True)
    parser.add_argument("--aster", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = {system: summarize(directory, system) for system, directory in (("pxa", args.pxa), ("aster", args.aster))}
    output["method"] = {"cpu_point_estimate": "Geometric mean of per-run medians (7 retained samples after 2 warmups)",
                        "inference_unit": "Randomized paired runs; never individual frames or inner-loop samples",
                        "cpu_inference": "Exploratory paired log time ratio; 20000 pair bootstrap resamples; exact balanced randomization; Holm across five CPU tests per system",
                        "timer_limit": "Statistical CPU intervals are conditional on recorded times. The separate +/- one clock quantum ratio sensitivity is a worst-case timing envelope, not a calibrated confidence interval.",
                        "system_comparison_limit": "Two physical devices, without swapping firmware. Descriptive cross-system comparisons do not isolate board differences.",
                        "aster_counter_clock": "Native completion deltas over host command/response midpoint intervals; round trips and independent guest clock rates retained."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    save(args.output, output)
    for system in ("pxa", "aster"):
        print(system, "fill", output[system]["fill_primary"]["fps_change_percent"],
              output[system]["fill_primary"]["paired_bootstrap_ci95_percent"])
        for item in output[system]["cpu_exploratory"]:
            print(item["name"], round(item["baseline_geomean_ms"], 3), "->",
                  round(item["candidate_geomean_ms"], 3), "ms", round(item["time_change_percent"], 3), "%")


if __name__ == "__main__":
    main()
