#!/usr/bin/env python3
"""Run the same fixed fill crossover on Aster using native completion counters."""
from pathlib import Path
import argparse
import hashlib
import json
import random
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import serial
from serial.tools import list_ports
from pxa_fill_noise import save, read_checksums, analyze
from summarize import cpu

HERE = Path(__file__).resolve().parent


def measure(path, expected, pixel, identity):
    meta = json.loads((path / "metadata.json").read_text())
    if not meta["done"] or meta["scope"] != "fill":
        raise ValueError("Incomplete Aster fill run")
    if (meta.get("usb_serial") or "").lower() != identity.lower():
        raise RuntimeError("Aster device serial changed")
    records = json.loads((path / "records.json").read_text())
    kernels = cpu(records, expected)
    if not any(r.get("test") == "pixels" and int(r["sum"]) == pixel for r in records):
        raise ValueError("Missing/mismatched Aster pixel checksum")
    modes = []
    for mode in (0, 1):
        endpoints = {}
        for reason in ("start", "end"):
            group = [r for r in records if r.get("test") == "display" and r.get("phase") == [0, mode]
                     and r.get("reason") == reason]
            if len(group) != 1:
                raise ValueError(f"Expected one {reason} counter for mode {mode}")
            endpoints[reason] = group[0]
        start, end = endpoints["start"], endpoints["end"]
        midpoint = lambda r: (r["request_host_time"] + r["host_time"]) / 2
        duration = midpoint(end) - midpoint(start)
        if not 5 <= duration <= 7:
            raise ValueError(f"Invalid native counter observation duration: {duration}")
        completed = int(end["completed"]) - int(start["completed"])
        if completed <= 0:
            raise RuntimeError("No Aster LCD completions; investigate without dropping slow trials")
        ticks = [r for r in records if r.get("test") == "frames" and int(r["round"]) == 0
                 and int(r["mode"]) == mode and 2_000_000 <= int(r["elapsed_us"]) <= 7_500_000]
        if len(ticks) < 3:
            raise ValueError("Insufficient guest timing samples")
        first, last = ticks[0], ticks[-1]
        count = int(last["frames"]) - int(first["frames"])
        if count <= 0:
            raise RuntimeError("No successful Aster presents")
        modes.append({"mode": mode, "lcd_fps": completed / duration,
                      "completed": completed, "window_us": duration * 1e6,
                      "counter_start_roundtrip_ms": (start["host_time"] - start["request_host_time"]) * 1000,
                      "counter_end_roundtrip_ms": (end["host_time"] - end["request_host_time"]) * 1000,
                      "paint_ms": (int(last["paint_us"]) - int(first["paint_us"])) / count / 1000,
                      "present_call_ms": (int(last["submit_us"]) - int(first["submit_us"])) / count / 1000,
                      "guest_submit_fps": count * 1e6 / (int(last["elapsed_us"]) - int(first["elapsed_us"])),
                      "native_rotate_ms": (int(end["rotate_us"]) - int(start["rotate_us"])) / completed / 1000,
                      "native_transfer_ms": (int(end["transfer_us"]) - int(start["transfer_us"])) / completed / 1000})
    return {"path": str(path), "modes": modes, "cpu": kernels,
            "memory": [r for r in records if r.get("test") == "memory"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--checksums", type=Path, required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=12)
    parser.add_argument("--seed", type=int, default=20261119)
    args = parser.parse_args()
    if args.pairs < 4 or args.pairs > 16 or args.pairs % 2:
        parser.error("Use an even pair count from 4 to 16")
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "experiment.json").exists():
        parser.error("Choose a fresh output directory")
    import os
    port_info = next((p for p in list_ports.comports() if p.device == os.path.realpath(args.port)), None)
    if not port_info or (port_info.serial_number or "").lower() != args.expected_serial.lower():
        raise RuntimeError("Unexpected Aster device")
    with serial.Serial(None, 115200, timeout=.1, exclusive=True) as device:
        device.dtr = True
        device.rts = False
        device.port = args.port
        device.open()
        device.write(b'\nASTERPING\naster port selftest\n')
        end = time.monotonic() + 2
        received = bytearray()
        while time.monotonic() < end:
            received += device.read(max(1, device.in_waiting))
    (args.output / "preflight.log").write_bytes(received)
    if b"PONG " not in received or b"product=aster target=xtensa" not in received or b"passed=0xff" not in received:
        raise RuntimeError("Aster protocol/selftest preflight failed")
    packages = {"A": args.baseline.resolve(), "B": args.candidate.resolve()}
    order = ["AB"] * (args.pairs // 2) + ["BA"] * (args.pairs // 2)
    random.Random(args.seed).shuffle(order)
    plan = {"system": "aster", "seed": args.seed, "pairs": args.pairs, "order": order,
            "identity": {"usb_serial": args.expected_serial, "protocol": "ASTERFS package transport (PONG probe)", "port_selftest": "0xff",
                         "runtime_firmware_hash_available": False},
            "packages": {k: {"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for k, p in packages.items()},
            "collector_sha256": hashlib.sha256((HERE / "run_aster.py").read_bytes()).hexdigest(),
            "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "analysis_sha256": hashlib.sha256((HERE / "pxa_fill_noise.py").read_bytes()).hexdigest(),
            "primary": "Mean paired log FPS ratio B/A averaged over constant/changing fill",
            "inference": "Same pair bootstrap and exact balanced order randomization as the PXA experiment",
            "native_window": "8-second phases; counter requests after 2 seconds and at phase end, about 6 seconds",
            "clock": "Host monotonic command/response midpoints; round trips retained; independent guest device-clock rate retained",
            "fixed_sample_size": True, "warmup_runs": ["A", "B"], "reboots": False, "firmware_changes": False,
            "retry_rule": "Two attempts for transport/incomplete logs or invalid counter windows; preserve failures; no speed-based exclusion; stop on identity changes or zero completions"}
    save(args.output / "experiment.json", plan)
    print("COMMITTED ORDER", order, flush=True)
    expected, pixel = read_checksums(args.checksums)
    jobs = [(True, 0, i + 1, v) for i, v in enumerate(("A", "B"))]
    jobs += [(False, pair, slot, v) for pair, o in enumerate(order, 1) for slot, v in enumerate(o, 1)]
    samples = []
    save(args.output / "samples.json", samples)
    for warmup, pair, slot, variant in jobs:
        if hashlib.sha256((HERE / "run_aster.py").read_bytes()).hexdigest() != plan["collector_sha256"]:
            raise RuntimeError("Aster collector changed during experiment")
        stem = f'warmup-{variant}' if warmup else f'pair-{pair:02d}-{slot}-{variant}'
        for attempt in (1, 2):
            target = args.output / f"{stem}-attempt-{attempt}"
            target.mkdir()
            try:
                started = time.time()
                with (target / "collector.log").open("w") as log:
                    subprocess.run([sys.executable, "-u", str(HERE / "run_aster.py"), "--fill-only", "--port", args.port,
                                    "--expected-serial", args.expected_serial, "--package", str(packages[variant]),
                                    "--output", str(target / "run")], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
                sample = measure(target / "run", expected, pixel, args.expected_serial)
                sample.update({"warmup": warmup, "pair": pair, "slot": slot, "variant": variant,
                               "attempt": attempt, "host_start_unix": started, "host_end_unix": time.time()})
                samples.append(sample)
                save(args.output / "samples.json", samples)
                print(stem, "LCD FPS", [round(m["lcd_fps"], 3) for m in sample["modes"]], flush=True)
                break
            except RuntimeError:
                raise
            except Exception as error:
                save(target / "failure.json", {"type": type(error).__name__, "error": str(error)})
                if attempt == 2:
                    raise
                print(stem, "retry after", type(error).__name__, flush=True)
    analyze(args.output)


if __name__ == "__main__":
    main()
