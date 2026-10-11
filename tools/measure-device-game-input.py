#!/usr/bin/env python3
"""Measure sampled pointer-to-Guest/present/LCD completion on a game probe."""

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--app", required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--moves", type=int, default=48)
    parser.add_argument("--interval", type=float, default=.16)
    parser.add_argument("--ignore-top-rows", type=int, default=24,
                        help="exclude the device FPS overlay from visual response verification")
    parser.add_argument("--positions", type=int, nargs=4, default=[210, 120, 240, 120],
                        metavar=("X1", "Y1", "X2", "Y2"))
    args = parser.parse_args()
    if (args.repeat < 1 or args.moves < 1 or args.interval <= 0 or args.warmup < 0
            or args.ignore_top_rows < 0):
        parser.error("positive repeat, moves and interval required; warmup >= 0")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"app": args.app, "package_sha256": hashlib.sha256(args.package.read_bytes()).hexdigest(),
              "moves": args.moves, "interval_seconds": args.interval,
              "ignored_top_rows": args.ignore_top_rows,
              "positions": args.positions, "runs": [], "passed": False}
    start = time.monotonic()
    with (args.output / "serial.jsonl").open("w", buffering=1) as log:
        def record(kind, payload):
            log.write(json.dumps({"seconds": time.monotonic() - start,
                                  "kind": kind, "payload": payload}) + "\n")

        with pxadb.PxaDbClient(args.port, 10) as client:
            client.log_callback = lambda frame: record(frame.kind, frame.payload)
            client.raw_callback = lambda data: record("RAW", data.decode("utf-8", "replace"))

            def request(command):
                record("COMMAND", command)
                quiet = client.log_subscribed and command.startswith("PERF")
                if quiet:
                    client.unsubscribe_logs(timeout=3)
                frames = client.request(command, timeout=20)
                if quiet:
                    client.subscribe_logs()
                record("RESPONSE", [{"kind": f.kind, "payload": f.payload}
                                    for f in frames if f.kind != "DATA" or command.startswith("PERF INPUT")])
                return frames

            def pump(seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    client.pump_logs()
                    time.sleep(.01)

            def capture(name):
                # Transfer outside the measured input window. Quiet live logs
                # while the screenshot shares the bounded debug TX queue.
                client.unsubscribe_logs(timeout=3)
                for retry in range(6):
                    try:
                        shot = pxadb.screenshot_capture(request("SCREENSHOT"))
                        break
                    except pxadb.PxaDbError as error:
                        if not pxadb.screenshot_retryable(error) or retry == 5:
                            raise
                        pump(.2)
                pxadb.write_screenshot(shot, args.output / name)
                client.subscribe_logs()
                if shot.metadata.get("source") != "surface":
                    raise RuntimeError("the probe surface is not visible")
                height = int(shot.metadata["height"])
                stride = int(shot.metadata["stride"])
                if shot.metadata["format"] != "rgb565le" or args.ignore_top_rows >= height:
                    raise RuntimeError("visual verification requires RGB565 scene rows")
                return {"metadata": shot.metadata,
                        "pixels_sha256": hashlib.sha256(shot.data).hexdigest(),
                        "scene_pixels_sha256": hashlib.sha256(
                            shot.data[args.ignore_top_rows * stride:]).hexdigest()}

            running = holding = recording = False
            try:
                report["hello"] = client.hello()
                packages = [f.payload for f in request("PACKAGES") if f.kind == "PKG"]
                if any("active=1" in p for p in packages):
                    raise RuntimeError("stop the active application before measuring")
                client.subscribe_logs()
                x1, y1, x2, y2 = args.positions
                for index in range(args.repeat):
                    request("INPUT KEY HOME")
                    pump(.3)
                    request(f"PACKAGE run {args.app}")
                    running = True
                    pump(args.warmup)
                    row = {"index": index, "before": capture(f"before-{index}.png")}
                    # Discard startup/history, then track one explicit window.
                    request("PERF START")
                    recording = True
                    request("PERF INPUT")
                    request(f"INPUT POINTER DOWN {x1} {y1} 0")
                    holding = True
                    request("INPUT SYNC")
                    pump(args.interval)
                    for move in range(args.moves):
                        # End at the alternate position so the last displayed
                        # frame also proves a visible response to this input.
                        x, y = (x2, y2) if move % 2 == 0 else (x1, y1)
                        request(f"INPUT POINTER MOVE {x} {y} 0")
                        request("INPUT SYNC")
                        pump(args.interval)
                    request(f"INPUT POINTER MOVE {x2} {y2} 0")
                    request("INPUT SYNC")
                    pump(.4)
                    request(f"INPUT POINTER UP {x2} {y2} 0")
                    holding = False
                    request("INPUT SYNC")
                    pump(.5)
                    metrics = {}
                    for response in request("PERF INPUT"):
                        if response.kind in ("DATA", "OK") and response.payload:
                            metrics.update({k: int(v) for k, v in
                                            pxadb.info_properties(response.payload).items()})
                    request("PERF CLEAR")
                    recording = False
                    if any(metrics[p + "_count"] == 0 for p in ("guest", "present", "visible")):
                        raise RuntimeError("missing Guest/present/LCD completion samples")
                    row["metrics"] = metrics
                    row["after"] = capture(f"after-{index}.png")
                    if row["before"]["scene_pixels_sha256"] == row["after"]["scene_pixels_sha256"]:
                        raise RuntimeError("input did not change the displayed scene")
                    request(f"PACKAGE stop {args.app}")
                    running = False
                    pump(.3)
                    report["runs"].append(row)
                    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                    print(json.dumps({"index": index, "metrics": metrics}), flush=True)
                report["passed"] = True
            finally:
                if recording:
                    request("PERF CLEAR")
                if holding:
                    request("INPUT CANCEL")
                if running:
                    request(f"PACKAGE stop {args.app}")
                request("INPUT KEY HOME")
                (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
