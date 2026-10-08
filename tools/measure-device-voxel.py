#!/usr/bin/env python3
"""Capture bounded device frame samples for an already installed PXA game."""

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


def summary(values):
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    return {"count": len(values), "mean_us": statistics.mean(values),
            "median_us": statistics.median(values),
            "p95_us": ordered[math.ceil(len(values) * .95) - 1],
            "min_us": ordered[0], "max_us": ordered[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--app", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--menu-tap", type=int, nargs=2)
    parser.add_argument("--unlock-swipe", type=int, nargs=4,
                        help="wake and unlock before each run: X1 Y1 X2 Y2")
    parser.add_argument("--package", type=Path)
    args = parser.parse_args()
    if args.warmup < 0 or not 0 < args.seconds <= 30 or args.repeat < 1:
        parser.error("warmup >= 0, 0 < seconds <= 30, repeat >= 1 required")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"app": args.app, "port": args.port, "warmup_seconds": args.warmup,
              "capture_seconds": args.seconds, "runs": [], "passed": False}
    if args.package:
        report["local_package_sha256"] = hashlib.sha256(args.package.read_bytes()).hexdigest()
    started = time.monotonic()
    with (args.output / "serial.jsonl").open("w", buffering=1) as log:
        def record(kind, payload):
            log.write(json.dumps({"seconds": time.monotonic() - started,
                                  "kind": kind, "payload": payload}) + "\n")

        with pxadb.PxaDbClient(args.port, 10) as client:
            client.log_callback = lambda frame: record(frame.kind, frame.payload)
            client.raw_callback = lambda data: record("RAW", data.decode("utf-8", "replace"))

            def request(command):
                record("COMMAND", command)
                frames = client.request(command, timeout=20)
                record("RESPONSE", [{"kind": f.kind, "payload": f.payload}
                                    for f in frames if f.kind != "DATA"])
                return frames

            def pump(seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    client.pump_logs()
                    time.sleep(.02)

            def memory():
                output = {}
                for frame in request("MEMORY"):
                    if frame.kind != "DATA":
                        continue
                    props = pxadb.info_properties(frame.payload)
                    scope = props.pop("scope")
                    name = props.pop("name", "")
                    output[scope + (":" + name if name else "")] = {
                        key: int(value) for key, value in props.items()}
                return output

            running = recording = False
            try:
                report["hello"] = client.hello()
                if "perf-raster" not in report["hello"] or "memory" not in report["hello"]:
                    raise RuntimeError("firmware requires perf-raster and memory capabilities")
                client.subscribe_logs()
                report["packages_before"] = [f.payload for f in request("PACKAGES") if f.kind == "PKG"]
                if any("active=1" in p for p in report["packages_before"]):
                    raise RuntimeError("stop the active application before measuring")
                for index in range(args.repeat):
                    if args.unlock_swipe:
                        x1, y1, x2, y2 = args.unlock_swipe
                        request(f"INPUT TAP {x1} {y1}")
                        pump(.3)
                        request(f"INPUT SWIPE {x1} {y1} {x2} {y2} 400 8")
                        pump(.5)
                    run = {"index": index, "memory_before": memory()}
                    report["runs"].append(run)
                    request("PERF CLEAR")
                    request(f"PACKAGE run {args.app}")
                    running = True
                    pump(args.warmup)
                    if args.menu_tap:
                        request(f"INPUT TAP {args.menu_tap[0]} {args.menu_tap[1]}")
                        request("INPUT SYNC")
                        pump(args.warmup)
                    run["memory_warm"] = memory()
                    request("PERF START")
                    recording = True
                    pump(args.seconds)
                    run["probe"] = pxadb.info_properties(request("PERF STOP")[-1].payload)
                    recording = False
                    run["memory_capture"] = memory()
                    for kind in ("raster", "present"):
                        values = []
                        count = int(run["probe"][kind])
                        for offset in range(0, count, 16):
                            props = pxadb.info_properties(request(f"PERF READ {kind} {offset}")[-1].payload)
                            if int(props["offset"]) != offset:
                                raise RuntimeError("probe returned the wrong sample offset")
                            chunk = [int(v) for v in props["values"].split(",") if v]
                            if len(chunk) != int(props["count"]):
                                raise RuntimeError("probe returned a malformed sample count")
                            values.extend(chunk)
                        if len(values) != count:
                            raise RuntimeError("probe sample count mismatch")
                        run[kind + "_us"] = values
                        run[kind] = summary(values)
                    capture = pxadb.screenshot_capture(request("SCREENSHOT JPEG"))
                    pxadb.write_screenshot(capture, args.output / f"frame-{index}.jpg")
                    run["screenshot"] = capture.metadata
                    if not run["present_us"] or int(run["probe"]["surface_changed"]):
                        raise RuntimeError("no displayed frames or surface changed during capture")
                    run["display_fps"] = 1e6 / run["present"]["mean_us"]
                    request("PERF CLEAR")
                    request(f"PACKAGE stop {args.app}")
                    running = False
                    pump(2)
                    run["memory_after"] = memory()
                    packages = [f.payload for f in request("PACKAGES") if f.kind == "PKG"]
                    if any("active=1" in p for p in packages):
                        raise RuntimeError("application did not stop")
                    print(json.dumps({"run": index, "fps": run["display_fps"],
                                      "raster": run["raster"], "present": run["present"]}), flush=True)
                report["passed"] = True
            except (Exception, KeyboardInterrupt) as error:
                report["error"] = str(error)
                print(str(error), file=sys.stderr)
            finally:
                for command, needed in [("PERF STOP", recording), ("PERF CLEAR", True),
                                        (f"PACKAGE stop {args.app}", running)]:
                    if needed:
                        try:
                            request(command)
                        except Exception as error:
                            report.setdefault("cleanup_errors", []).append(str(error))
                report["elapsed_seconds"] = time.monotonic() - started
                (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] and not report.get("cleanup_errors") else 1


if __name__ == "__main__":
    sys.exit(main())
