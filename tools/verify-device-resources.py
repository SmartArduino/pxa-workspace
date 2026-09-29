#!/usr/bin/env python3
"""Run installed resource-scenes on a physical board with live log evidence.

Install the board-specific package first. This controls one serial connection,
subscribes before launch, and always stops the test app. Heap INFO is aggregate
free heap, not an attribution of resource budgets or a proof of leak freedom.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb

MARKERS = (
    "installed map 8193 bytes and EOF checked with full-width tokens",
    "music READY for accepted instance",
    "100 scenes completed; 20 file textures; four visible; no Guest pixel arrays",
    "100 prepared sound triggers; closing handle during final playback",
    "music STOPPED for matching instance",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--screenshot", action="store_true")
    args = parser.parse_args()
    if args.rounds < 1 or args.timeout <= 0:
        parser.error("rounds and timeout must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["PXADB_BAUD"] = str(args.baud)
    start = time.monotonic()
    result = {"port": args.port, "baud": args.baud, "rounds": [], "passed": False}
    seen = {}
    failures = []
    with (args.output / "serial.jsonl").open("w", buffering=1) as log:
        def record(kind, payload):
            now = time.monotonic() - start
            log.write(json.dumps({"seconds": now, "kind": kind, "payload": payload}) + "\n")
            for marker in MARKERS:
                if marker in payload:
                    seen.setdefault(marker, now)
            if any(message in payload for message in (
                "Package launch failed at", "scene bind/draw failed",
                "resource budget/load failure", "unbind/loading frame failed",
                "Guru Meditation", "assert failed")):
                failures.append(payload)

        with pxadb.PxaDbClient(args.port, timeout=10) as client:
            client.log_callback = lambda frame: record(frame.kind, frame.payload)
            client.raw_callback = lambda raw: record("RAW", raw.decode("utf-8", "replace"))

            def request(command, timeout=15):
                record("COMMAND", command)
                frames = client.request(command, timeout=timeout)
                for frame in frames:
                    if frame.kind != "DATA":
                        record(frame.kind, frame.payload)
                return [{"kind": f.kind, "payload": f.payload} for f in frames]

            def pump(seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    client.pump_logs()
                    time.sleep(0.02)

            running = False
            try:
                result["hello"] = client.hello()
                client.subscribe_logs()
                result["initial_info"] = request("INFO")
                for index in range(args.rounds):
                    seen.clear()
                    failures.clear()
                    row = {"round": index + 1, "before": request("INFO")}
                    result["rounds"].append(row)
                    launched = time.monotonic()
                    row["launch_seconds"] = launched - start
                    request("PACKAGE run pxa-resource-scenes")
                    running = True
                    captured = False
                    while time.monotonic() - launched < args.timeout and len(seen) < len(MARKERS):
                        pump(0.1)
                        if failures:
                            raise RuntimeError("; ".join(failures))
                        if args.screenshot and not captured and time.monotonic() - launched > 5:
                            captured = True
                            frames = client.request("SCREENSHOT", timeout=30)
                            shot = pxadb.screenshot_capture(frames)
                            pxadb.write_screenshot(shot, args.output / f"round-{index + 1}.png")
                            row["screenshot"] = shot.metadata
                    row["duration_seconds"] = time.monotonic() - launched
                    row["markers"] = dict(seen)
                    row["missing"] = [m for m in MARKERS if m not in seen]
                    row["finished_info"] = request("INFO")
                    request("PACKAGE stop pxa-resource-scenes")
                    # Stop is queued; check until the package is inactive.
                    deadline = time.monotonic() + 15
                    row["stopped"] = False
                    while time.monotonic() < deadline:
                        pump(0.5)
                        packages = request("PACKAGES")
                        matches = [f for f in packages if f["kind"] == "PKG" and "pxa-resource-scenes" in f["payload"]]
                        if matches and all("active=0" in f["payload"].split("\t")[-1].split(";") for f in matches):
                            row["stopped"] = True
                            running = False
                            break
                    pump(2)
                    row["after"] = request("INFO")
                    print(json.dumps(row), flush=True)
                    if row["missing"] or not row["stopped"]:
                        raise RuntimeError("resource test incomplete; inspect live serial log")
                result["passed"] = True
            except (Exception, KeyboardInterrupt) as error:
                result["error"] = str(error)
            finally:
                try:
                    if running:
                        request("PACKAGE stop pxa-resource-scenes")
                        pump(1)
                except Exception as error:
                    result["cleanup_error"] = str(error)
                (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["passed"] and "cleanup_error" not in result else 1


if __name__ == "__main__":
    sys.exit(main())
