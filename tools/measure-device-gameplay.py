#!/usr/bin/env python3
"""Record bounded game/display/input windows; screenshots stay outside timing.

Uses device timestamps and distinct panel-completion intervals, not submission
FPS or UART round trips. The input association is Host telemetry, not proof of
physical touch-to-photon latency. Install a fresh test game before using this
tool; --start is the title-page tap, in logical display pixels.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def distribution(values):
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    return {"count": len(values), "mean_us": statistics.mean(values),
            "median_us": statistics.median(values),
            "p95_us": ordered[math.ceil(.95 * len(values)) - 1],
            "max_us": ordered[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--app", required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=4)
    parser.add_argument("--warmup", type=float, default=6)
    parser.add_argument("--start", type=int, nargs=2)
    parser.add_argument("--size", type=int, nargs=2, default=[800, 480])
    parser.add_argument("--scenes", nargs="+", choices=["idle", "turn", "walk", "charge"],
                        default=["idle", "turn"])
    args = parser.parse_args()
    if args.repeat < 1 or not 1 <= args.seconds <= 4 or args.warmup < 0:
        parser.error("repeat >= 1, 1 <= seconds <= 4, warmup >= 0 required")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"app": args.app, "package_sha256": hashlib.sha256(args.package.read_bytes()).hexdigest(),
              "seconds": args.seconds, "size": args.size, "start": args.start,
              "runs": [], "passed": False,
              "latency_scope": "injected device sample to Host Guest dispatch, associated submission and panel buffer completion; excludes physical controller and optical response"}
    epoch = time.monotonic()
    running = recording = monitoring = False
    with (args.output / "serial.jsonl").open("w", buffering=1) as log, \
            pxadb.PxaDbClient(args.port, 15) as client:
        def record(kind, payload):
            log.write(json.dumps({"seconds": time.monotonic() - epoch,
                                  "kind": kind, "payload": payload}) + "\n")

        client.log_callback = lambda frame: record(frame.kind, frame.payload)
        client.raw_callback = lambda data: record("RAW", data.decode("utf-8", "replace"))

        def request(command):
            record("COMMAND", command)
            frames = client.request(command, timeout=30)
            for frame in frames:
                if not command.startswith("SCREENSHOT") or frame.kind != "DATA":
                    record(frame.kind, frame.payload)
            return frames

        def pump(seconds):
            deadline = time.monotonic() + max(0, seconds)
            while time.monotonic() < deadline:
                client.pump_logs()
                time.sleep(min(.005, max(0, deadline - time.monotonic())))

        def properties(command):
            result = {}
            for frame in request(command):
                if frame.kind in ("DATA", "OK") and frame.payload:
                    result.update(pxadb.info_properties(frame.payload))
            return result

        def memory(command="MEMORY"):
            rows = {}
            for frame in request(command):
                if frame.kind == "DATA":
                    props = pxadb.info_properties(frame.payload)
                    scope = props.pop("scope")
                    name = props.pop("name", "")
                    rows[scope + (":" + name if name else "")] = {k: int(v) for k, v in props.items()}
            return rows

        def pointer(phase, x, y):
            request(f"INPUT POINTER {phase} {int(x)} {int(y)} 0")
            request("INPUT SYNC")

        def screenshot(name):
            for attempt in range(6):
                try:
                    shot = pxadb.screenshot_capture(request("SCREENSHOT JPEG"))
                    break
                except pxadb.PxaDbError as error:
                    if attempt == 5 or not (pxadb.screenshot_retryable(error) or "rate_limited" in str(error)):
                        raise
                    pump(.25)
            pxadb.write_screenshot(shot, args.output / (name + ".jpg"))
            if [int(shot.metadata[k]) for k in ("width", "height")] != args.size:
                raise RuntimeError("unexpected display geometry: " + str(shot.metadata))
            return shot.metadata

        def require_active():
            if not any(args.app in f.payload and "active=1" in f.payload
                       for f in request("PACKAGES") if f.kind == "PKG"):
                raise RuntimeError("test application exited; see serial.jsonl")

        try:
            report["hello"] = client.hello()
            if "heap-local" not in report["hello"]:
                raise RuntimeError("firmware must expose local heap minima")
            for frame in request("PACKAGES"):
                if frame.kind == "PKG" and "active=1" in frame.payload:
                    if args.app not in frame.payload:
                        raise RuntimeError("stop the other active application before benchmarking")
                    request(f"PACKAGE stop {args.app}")
                    pump(.5)
            client.subscribe_logs()
            w, h = args.size
            # The lock screen deliberately suspends presentation. An upward
            # swipe also refreshes activity without changing saved settings.
            pointer("DOWN", w * .5, h * .92)
            pointer("MOVE", w * .5, h * .2)
            pointer("UP", w * .5, h * .2)
            pump(.4)
            for repeat in range(args.repeat):
                # Restart for every scene: turn/walk cannot change the next
                # scene's initial map and camera. App defaults must be fixed.
                for scene in args.scenes:
                    request("INPUT KEY HOME")
                    pump(.3)
                    request("MEMORY START")
                    monitoring = True
                    launch_before = memory()
                    request(f"PACKAGE run {args.app}")
                    running = True
                    pump(args.warmup)
                    if args.start:
                        pointer("DOWN", *args.start)
                        pump(.15)
                        pointer("UP", *args.start)
                        pump(args.warmup)
                    startup = memory("MEMORY STOP")
                    monitoring = False
                    require_active()
                    before_shot = screenshot(f"{repeat}-{scene}-before")
                    pump(.4)
                    request("MEMORY START")
                    monitoring = True
                    request("PERF START")
                    recording = True
                    properties("PERF INPUT")
                    before = memory()
                    begin = time.monotonic()
                    if scene in ("turn", "walk"):
                        x, y = (w * .69, h * .45) if scene == "turn" else (w * .25, h * .6)
                        pointer("DOWN", x, y)
                        for step in range(48):
                            pump(begin + (step + 1) * args.seconds / 49 - time.monotonic())
                            dx = (w * .04 if step % 2 else 0) if scene == "turn" else 0
                            dy = 0 if scene == "turn" else (-h * .12 if step % 2 else h * .12)
                            pointer("MOVE", x + dx, y + dy)
                        pointer("UP", x + dx, y + dy)
                    elif scene == "charge":
                        pointer("DOWN", w * .5, h * .65)
                        pump(.45)
                        pointer("UP", w * .5, h * .65)
                    pump(begin + args.seconds - time.monotonic())
                    metrics = {k: int(v) for k, v in properties("PERF INPUT").items()}
                    perf = {k: int(v) for k, v in properties("PERF STOP").items()}
                    recording = False
                    steady = memory("MEMORY STOP")
                    monitoring = False
                    row = {"repeat": repeat, "scene": scene, "elapsed_seconds": time.monotonic() - begin,
                           "launch_before": launch_before, "startup": startup, "before": before,
                           "memory": steady, "input": metrics, "perf": perf,
                           "screenshot_before": before_shot}
                    report["runs"].append(row)
                    for kind in ("raster", "present"):
                        values = []
                        for offset in range(0, perf[kind], 16):
                            chunk = properties(f"PERF READ {kind} {offset}")
                            samples = [int(v) for v in chunk["values"].split(",") if v]
                            if int(chunk["offset"]) != offset or len(samples) != int(chunk["count"]):
                                raise RuntimeError("invalid probe page")
                            values.extend(samples)
                        if len(values) != perf[kind]:
                            raise RuntimeError("incomplete probe")
                        row[kind + "_us"] = values
                        row[kind] = distribution(values)
                    if any(perf[k] for k in ("raster_overflow", "present_overflow", "surface_changed")):
                        raise RuntimeError("invalid measurement window: " + str(perf))
                    require_active()
                    if not row["present"]["count"]:
                        raise RuntimeError("no completed game frames; measurement is invalid")
                    row["display_fps"] = 1e6 / row["present"]["mean_us"] if row["present"]["count"] else None
                    row["screenshot_after"] = screenshot(f"{repeat}-{scene}-after")
                    request("PERF CLEAR")
                    request(f"PACKAGE stop {args.app}")
                    running = False
                    pump(.4)
                    row["stopped"] = memory()
                    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                    print(json.dumps({k: row[k] for k in ("repeat", "scene", "display_fps", "raster", "input")}), flush=True)
            report["passed"] = True
        except Exception as error:
            report["error"] = str(error)
            raise
        finally:
            cleanup = (["PERF CLEAR"] if recording else [])
            cleanup += ["MEMORY STOP"] if monitoring else []
            cleanup += ["INPUT CANCEL"]
            cleanup += [f"PACKAGE stop {args.app}"] if running else []
            cleanup += ["INPUT KEY HOME"]
            for command in cleanup:
                try:
                    request(command)
                except Exception as error:
                    report.setdefault("cleanup_errors", []).append(str(error))
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
