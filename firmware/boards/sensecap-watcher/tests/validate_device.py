#!/usr/bin/env python3
"""Validate Watcher UI/game transitions with actual completed screenshots.

Reads/logs the board identity before controlling an app. Requires an installed
Jump Jump 3D package. Keeps log capture active during screenshot DATA transfers.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import time

PROJECT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(PROJECT / "tools/pxadb"))
import pxadb

ERRORS = (
    "send color failed", "flush failed (", "DMA TX underflow",
    "Failed to allocate priv TX buffer", "Guru Meditation", "assert failed",
    "Task watchdog got triggered",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-serial", required=True)
    parser.add_argument("--baud", type=int, default=2000000)
    parser.add_argument("--app", default="pxa-jump-jump-3d")
    parser.add_argument("--restarts", type=int, default=3)
    parser.add_argument("--stress-captures", type=int, default=12)
    parser.add_argument("--jpeg-self-test", action="store_true",
                        help="also validate noisy-image encoding at the production capacity")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.restarts < 1 or args.stress_captures < 0:
        parser.error("restarts >= 1 and stress-captures >= 0 required")
    if (args.output / "report.json").exists():
        parser.error("use a fresh output directory to preserve previous evidence")
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["PXADB_BAUD"] = str(args.baud)
    report = {"passed": False, "app": args.app, "restarts": args.restarts,
              "screenshots": [], "errors": [], "cleanup_errors": []}
    running = monitoring = False
    epoch = time.monotonic()
    with pxadb.PxaDbClient(args.port, 15) as client, \
            (args.output / "serial.jsonl").open("w", buffering=1) as log:
        def record(kind, payload):
            row = {"seconds": time.monotonic() - epoch,
                   "kind": kind, "payload": payload}
            log.write(json.dumps(row) + "\n")
            if any(error in payload for error in ERRORS):
                report["errors"].append(row)

        client.log_callback = lambda frame: record(frame.kind, frame.payload)
        client.raw_callback = lambda data: record("RAW", data.decode("utf8", "replace"))

        def request(command):
            record("COMMAND", command)
            frames = client.request(command, timeout=15)
            for frame in frames:
                if not command.startswith("SCREENSHOT") or frame.kind != "DATA":
                    record(frame.kind, frame.payload)
            return frames

        def wait(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                client.pump_logs()
                time.sleep(min(.01, max(0, deadline - time.monotonic())))

        def active():
            return [frame.payload for frame in request("PACKAGES")
                    if frame.kind == "PKG" and "active=1" in frame.payload]

        def stop():
            nonlocal running
            request(f"PACKAGE stop {args.app}")
            deadline = time.monotonic() + 5
            while any(args.app in package for package in active()):
                if time.monotonic() >= deadline:
                    raise RuntimeError("asynchronous package stop did not finish")
                wait(.1)
            running = False
            wait(.2)

        def memory(command):
            rows = {}
            for frame in request(command):
                if frame.kind == "DATA":
                    properties = pxadb.info_properties(frame.payload)
                    scope = properties.pop("scope")
                    name = properties.pop("name", "")
                    rows[scope + (":" + name if name else "")] = {
                        key: int(value) for key, value in properties.items()}
            return rows

        def pointer(phase, x, y):
            request(f"INPUT POINTER {phase} {x} {y} 0")
            wait(.12)

        def shot(name):
            wait(.3)  # Respect the protocol's minimum capture interval.
            capture = pxadb.screenshot_capture(request("SCREENSHOT JPEG"))
            if (int(capture.metadata["width"]), int(capture.metadata["height"])) != (412, 412):
                raise RuntimeError("unexpected Watcher screenshot geometry")
            pxadb.write_screenshot(capture, args.output / (name + ".jpg"))
            report["screenshots"].append({"name": name, **capture.metadata})

        try:
            hello = client.hello()
            report["hello"] = hello
            if pxadb.info_properties(hello).get("serial") != args.expected_serial:
                raise RuntimeError("unexpected device serial; no control performed")
            if not any("board=sensecap-watcher" in frame.payload for frame in request("INFO")):
                raise RuntimeError("unexpected board; no control performed")
            if active():
                raise RuntimeError("stop the active app before running this validation")
            client.subscribe_logs()
            if args.jpeg_self_test:
                frames = request("JPEGTEST")
                details = next(pxadb.info_properties(frame.payload) for frame in frames
                               if frame.kind == "JPEGTEST")
                capture = pxadb.screenshot_capture(frames)
                if capture.metadata["source"] != "codec-self-test" or int(details["attempts"]) != 1:
                    raise RuntimeError("unexpected JPEG self-test result")
                pxadb.write_screenshot(capture, args.output / "jpeg-self-test.jpg")
                report["jpeg_self_test"] = {**details, **capture.metadata}
            report["before"] = memory("MEMORY")
            request("MEMORY START")
            monitoring = True
            request("INPUT KEY HOME")
            wait(.3)
            pointer("DOWN", 206, 315)
            for y in (280, 240, 200, 160, 120, 90):
                pointer("MOVE", 206, y)
            pointer("UP", 206, 90)
            wait(.5)
            shot("launcher")
            pointer("DOWN", 139, 145)
            pointer("UP", 139, 145)
            wait(.6)
            shot("settings")
            pointer("DOWN", 206, 315)
            for y in (280, 240, 200, 160, 120):
                pointer("MOVE", 206, y)
            pointer("UP", 206, 120)
            wait(.4)
            shot("settings-scrolled")
            request("INPUT KEY HOME")
            wait(.4)
            pointer("DOWN", 206, 75)
            for y in (110, 160, 210, 260, 315):
                pointer("MOVE", 206, y)
            pointer("UP", 206, 315)
            wait(.5)
            shot("shade")
            request("INPUT KEY HOME")
            wait(.4)
            for index in range(args.restarts):
                request(f"PACKAGE run {args.app}")
                running = True
                wait(3)
                shot(f"game-{index}")
                request("INPUT KEY HOME")
                wait(.4)
                shot(f"home-{index}")
                request(f"PACKAGE run {args.app}")
                wait(1)
                shot(f"resume-{index}")
                stop()
            if args.stress_captures:
                request(f"PACKAGE run {args.app}")
                running = True
                wait(3)
                for index in range(args.stress_captures):
                    wait(.3)
                    shot(f"framed-log-stress-{index}")
                stop()
            request("INPUT CANCEL")
            request("INPUT KEY HOME")
            wait(.5)
            report["memory"] = memory("MEMORY STOP")
            monitoring = False
            surface = report["memory"]["surface"]
            if any(surface[key] for key in ("frame", "mailbox", "probe")):
                raise RuntimeError("Surface/mailbox/probe leaked after stop")
            if report["errors"]:
                raise RuntimeError("device errors occurred; see serial.jsonl")
            report["passed"] = True
        except Exception as error:
            report["error"] = str(error)
            raise
        finally:
            # Do not control a device whose identity was rejected above.
            if monitoring or running:
                for command in (["INPUT CANCEL"] +
                                ([f"PACKAGE stop {args.app}"] if running else []) +
                                (["MEMORY STOP"] if monitoring else [])):
                    try:
                        request(command)
                    except Exception as error:
                        report["cleanup_errors"].append(str(error))
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Watcher UI, {args.restarts} restarts and {len(report['screenshots'])} screenshots passed")


if __name__ == "__main__":
    main()
