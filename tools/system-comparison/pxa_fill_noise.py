#!/usr/bin/env python3
"""Run a fixed, balanced randomized PXA crossover and analyze LCD FPS noise."""
from pathlib import Path
import argparse
import hashlib
import itertools
import json
import math
import random
import statistics
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from summarize import cpu

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PXADB = ROOT / "tools/pxadb/pxadb.py"


def fields(rows):
    return dict(item.split("=", 1) for row in rows for item in row.split(";") if "=" in item)


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def read_checksums(path):
    expected = {}
    pixel = None
    for line in path.read_text().splitlines():
        values = line.split()
        if values[0] == "cpu":
            expected[int(values[1])] = int(values[2])
        else:
            pixel = int(values[1])
    return expected, pixel


def measure(path, expected, pixel, identity):
    meta = json.loads((path / "metadata.json").read_text())
    if not meta["done"] or meta["scope"] != "fill":
        raise ValueError("Incomplete fill scope")
    detail = fields(meta["detail_info"])
    for key in ("serial", "version", "elf_sha256"):
        if detail[key] != identity[key]:
            raise RuntimeError(f"Device/firmware changed: {key}")
    records = json.loads((path / "records.json").read_text())
    kernels = cpu(records, expected)
    if not any(r.get("test") == "pixels" and int(r["sum"]) == pixel for r in records):
        raise ValueError("Missing/mismatched pixel checksum")
    modes = []
    for mode in (0, 1):
        windows = [r for r in records if r.get("test") == "display" and r["phase"] == [0, mode]]
        if len(windows) != 1:
            raise ValueError(f"Expected one LCD window for mode {mode}")
        counter = fields(windows[0]["data"])
        duration = int(counter["window_us"])
        if not 5_000_000 <= duration <= 7_000_000:
            raise ValueError(f"Invalid counter duration: {duration}")
        completed = int(counter["completed"])
        if completed == 0:
            # A zero result needs investigation, not retries until a fast run.
            raise RuntimeError("Zero LCD completions; stop and investigate visibility/presentation")
        ticks = [r for r in records if r.get("test") == "frames" and int(r["round"]) == 0
                 and int(r["mode"]) == mode and 2_000_000 <= int(r["elapsed_us"]) <= 7_500_000]
        if len(ticks) < 3:
            raise ValueError("Insufficient guest timing samples")
        first, last = ticks[0], ticks[-1]
        count = int(last["frames"]) - int(first["frames"])
        if count <= 0:
            raise RuntimeError("No successful guest submissions")
        modes.append({"mode": mode, "lcd_fps": completed * 1e6 / duration,
                      "completed": completed, "window_us": duration,
                      "paint_ms": (int(last["paint_us"]) - int(first["paint_us"])) / count / 1000,
                      "present_call_ms": (int(last["submit_us"]) - int(first["submit_us"])) / count / 1000,
                      "guest_submit_fps": count * 1e6 / (int(last["elapsed_us"]) - int(first["elapsed_us"]))})
    log = (path / "serial.log").read_text(errors="replace")
    return {"path": str(path), "modes": modes, "cpu": kernels,
            "memory_before": meta["memory_before"], "memory_after": meta["memory_after"],
            "background_logs": {name: log.count(name) for name in
                                ("ADC: ESP_ERR_TIMEOUT", "UART FIFO overflow", "DROP dropped=")}}


def quantile(values, p):
    ordered = sorted(values)
    position = p * (len(ordered) - 1)
    lo = int(position)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def effect(pairs, mode, seed):
    def value(sample):
        selected = sample["modes"] if mode is None else [sample["modes"][mode]]
        return statistics.mean(math.log(m["lcd_fps"]) for m in selected)
    av = [value(p["A"]) for p in pairs]
    bv = [value(p["B"]) for p in pairs]
    diffs = [b - a for a, b in zip(av, bv)]
    observed = statistics.mean(diffs)
    rng = random.Random(seed)
    bootstrap = [statistics.mean(rng.choices(diffs, k=len(diffs))) for _ in range(20000)]
    # Randomization inference respects the actual design: exactly half of
    # blocks are AB and half BA. Do not pretend phase samples are new trials.
    slot_changes = [value(p["second"]) - value(p["first"]) for p in pairs]
    permutations = []
    for ab_blocks in itertools.combinations(range(len(pairs)), len(pairs) // 2):
        chosen = set(ab_blocks)
        permutations.append(statistics.mean(d if i in chosen else -d for i, d in enumerate(slot_changes)))
    percent = lambda v: math.expm1(v) * 100
    output = {"baseline_geomean_fps": math.exp(statistics.mean(av)),
              "candidate_geomean_fps": math.exp(statistics.mean(bv)),
              "fps_change_percent": percent(observed),
              "paired_bootstrap_ci95_percent": [percent(quantile(bootstrap, q)) for q in (0.025, 0.975)],
              "exact_balanced_randomization_p_two_sided":
                  sum(abs(v) >= abs(observed) - 1e-14 for v in permutations) / len(permutations),
              "randomization_assignments": len(permutations),
              "paired_changes_percent": [percent(d) for d in diffs]}
    for label, values in (("baseline", av), ("candidate", bv)):
        fps = [math.exp(v) for v in values]
        output[label + "_run_range_fps"] = [min(fps), max(fps)]
        output[label + "_run_cv_percent"] = statistics.stdev(fps) / statistics.mean(fps) * 100
    output["effect_by_order_percent"] = {
        order: percent(statistics.mean(d for p, d in zip(pairs, diffs) if p["order"] == order))
        for order in ("AB", "BA")}
    return output


def analyze(output):
    plan = json.loads((output / "experiment.json").read_text())
    samples = json.loads((output / "samples.json").read_text())
    selected = [s for s in samples if not s["warmup"]]
    pairs = []
    for index, order in enumerate(plan["order"], 1):
        group = sorted([s for s in selected if s["pair"] == index], key=lambda s: s["slot"])
        if len(group) != 2:
            raise ValueError(f"Incomplete pair {index}")
        if "".join(s["variant"] for s in group) != order:
            raise ValueError("Observed order differs from precommitted order")
        pair = {s["variant"]: s for s in group}
        pair.update({"first": group[0], "second": group[1], "order": order})
        pairs.append(pair)
    result = {"plan": plan, "valid_pairs": len(pairs),
              "primary": effect(pairs, None, plan["seed"] + 1),
              "secondary_modes": [effect(pairs, m, plan["seed"] + 10 + m) for m in (0, 1)],
              "interpretation_limits": ["One physical board and these exact app/firmware builds.",
                                        "Bootstrap confidence intervals are approximate with a limited number of independent pairs.",
                                        "Secondary mode tests are exploratory; their p values are not multiplicity-adjusted.",
                                        "Guest wall-clock stages include scheduling/waiting. No per-frame TE/rotation/SPI traces were collected; available cumulative native counters remain in samples."]}
    result["cpu_secondary"] = []
    for kind in range(5):
        a = statistics.median(p["A"]["cpu"][kind]["median_ms"] for p in pairs)
        b = statistics.median(p["B"]["cpu"][kind]["median_ms"] for p in pairs)
        result["cpu_secondary"].append({"name": pairs[0]["A"]["cpu"][kind]["name"],
                                         "baseline_ms": a, "candidate_ms": b,
                                         "time_change_percent": (b / a - 1) * 100})
    save(output / "summary.json", result)
    print("PRIMARY", json.dumps(result["primary"]), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--checksums", type=Path)
    parser.add_argument("--port")
    parser.add_argument("--expected-serial")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=12)
    parser.add_argument("--seed", type=int, default=20261119)
    parser.add_argument("--attempts", type=int, choices=(1, 2, 3), default=2)
    parser.add_argument("--analyze-only", action="store_true")
    args = parser.parse_args()
    if args.analyze_only:
        analyze(args.output)
        return
    if not all((args.baseline, args.candidate, args.checksums, args.port, args.expected_serial)):
        parser.error("Package paths, checksums, port and expected serial are required")
    if args.pairs < 4 or args.pairs > 16 or args.pairs % 2:
        parser.error("--pairs must be an even number between 4 and 16")
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "experiment.json").exists():
        parser.error("Choose a fresh output directory; do not overwrite a precommitted experiment")
    info = subprocess.run([sys.executable, str(PXADB), "info", "--port", args.port],
                          capture_output=True, text=True, check=True, timeout=30).stdout
    (args.output / "initial-info.txt").write_text(info)
    identity = fields(info.strip().splitlines())
    if identity["serial"] != args.expected_serial:
        raise RuntimeError("Unexpected device serial")
    packages = {"A": args.baseline.resolve(), "B": args.candidate.resolve()}
    order = ["AB"] * (args.pairs // 2) + ["BA"] * (args.pairs // 2)
    random.Random(args.seed).shuffle(order)
    plan = {"seed": args.seed, "pairs": args.pairs, "order": order,
            "packages": {k: {"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                         for k, p in packages.items()}, "identity": identity,
            "primary": "Mean paired log FPS ratio B/A, averaged over constant and changing fill; report geometric percent change",
            "inference": "95% pair bootstrap interval (20000 resamples); exact two-sided balanced order randomization test",
            "fixed_sample_size": True, "warmup_runs": ["A", "B"], "settle_seconds": 1.5,
            "collector_sha256": hashlib.sha256((HERE / "run_pxa.py").read_bytes()).hexdigest(),
            "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "home_settle_seconds": 0.5,
            "native_window": "8-second phases; discard first 2 seconds, count actual LCD completions for about 6 seconds",
            "retry_rule": f"At most {args.attempts} attempts for transport, incomplete logs or invalid measurement windows; retain all failure logs. Never reject a nonzero FPS for being slow/fast. Stop for identity changes or zero display completions.",
            "firmware_changes": False, "reboots": False}
    save(args.output / "experiment.json", plan)
    print("COMMITTED ORDER", order, flush=True)
    expected, pixel = read_checksums(args.checksums)
    jobs = [(True, 0, i + 1, v) for i, v in enumerate(("A", "B"))]
    jobs += [(False, pair, slot, v) for pair, o in enumerate(order, 1) for slot, v in enumerate(o, 1)]
    samples = []
    save(args.output / "samples.json", samples)
    for warmup, pair, slot, variant in jobs:
        if hashlib.sha256((HERE / "run_pxa.py").read_bytes()).hexdigest() != plan["collector_sha256"]:
            raise RuntimeError("Collector changed during the experiment")
        stem = f'warmup-{variant}' if warmup else f'pair-{pair:02d}-{slot}-{variant}'
        for attempt in range(1, args.attempts + 1):
            directory = args.output / f"{stem}-attempt-{attempt}"
            directory.mkdir()
            try:
                with (directory / "install.log").open("w") as log:
                    subprocess.run([sys.executable, str(PXADB), "package", "install", str(packages[variant]),
                                    "--port", args.port, "--yes"], stdout=log, stderr=subprocess.STDOUT,
                                   check=True, timeout=60)
                time.sleep(plan["settle_seconds"])
                started = time.time()
                with (directory / "collector.log").open("w") as log:
                    subprocess.run([sys.executable, "-u", str(HERE / "run_pxa.py"), "--fill-only",
                                    "--port", args.port, "--output", str(directory / "run")],
                                   stdout=log, stderr=subprocess.STDOUT, check=True, timeout=75)
                sample = measure(directory / "run", expected, pixel, identity)
                sample.update({"warmup": warmup, "pair": pair, "slot": slot, "variant": variant,
                               "attempt": attempt, "host_start_unix": started, "host_end_unix": time.time()})
                samples.append(sample)
                save(args.output / "samples.json", samples)
                print(stem, "LCD FPS", [round(m["lcd_fps"], 3) for m in sample["modes"]], flush=True)
                break
            except RuntimeError:
                raise
            except Exception as error:
                save(directory / "failure.json", {"type": type(error).__name__, "error": str(error)})
                with (directory / "cleanup.log").open("w") as log:
                    subprocess.run([sys.executable, str(PXADB), "package", "stop", "pxa-system-comparison",
                                    "--port", args.port], stdout=log, stderr=subprocess.STDOUT, timeout=30)
                if attempt == args.attempts:
                    raise
                print(stem, "retry after", type(error).__name__, flush=True)
    analyze(args.output)


if __name__ == "__main__":
    main()
