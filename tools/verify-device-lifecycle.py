#!/usr/bin/env python3
"""Verify a running PXA app pauses at idle lock and resumes after unlock.

Use a firmware that logs Host UI lifecycle transitions. The app should keep
running longer than the configured idle lock timeout. No display request is
sent while waiting for the lock, because that may refresh UI activity.
"""

import argparse
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--app", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--verify-frames", action="store_true",
                        help="require no raster/present samples while locked and frames after unlock")
    parser.add_argument("--unlock-swipe", type=int, nargs=4,
                        metavar=("X1", "Y1", "X2", "Y2"), required=True)
    parser.add_argument("--unlock-duration-ms", type=int, default=400)
    parser.add_argument("--unlock-steps", type=int, default=8)
    parser.add_argument("--wake-tap", type=int, nargs=2,
                        metavar=("X", "Y"))
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    if args.unlock_duration_ms <= 0 or args.unlock_steps <= 0:
        parser.error("unlock duration and steps must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["PXADB_BAUD"] = str(args.baud)
    start = time.monotonic()
    report = {"port": args.port, "baud": args.baud, "app": args.app,
              "passed": False, "lifecycle": []}
    launch_at = None

    with (args.output / "serial.jsonl").open("w", buffering=1) as log:
        def record(kind, payload):
            entry = {"seconds": time.monotonic() - start,
                     "kind": kind, "payload": payload}
            log.write(json.dumps(entry) + "\n")
            if (kind == "LOG" and "UI lifecycle:" in payload and
                    launch_at is not None and entry["seconds"] >= launch_at):
                report["lifecycle"].append(entry)
            if (kind == "LOG" and "Application foregrounded:" in payload and
                    f":{args.app}" in payload and launch_at is not None and
                    entry["seconds"] >= launch_at):
                report["bridge_foreground"] = entry

        with pxadb.PxaDbClient(args.port, 10) as client:
            client.log_callback = lambda frame: record(frame.kind, frame.payload)
            client.raw_callback = lambda data: record(
                "RAW", data.decode("utf-8", "replace"))

            def request(command, timeout=20):
                record("COMMAND", command)
                frames = client.request(command, timeout=timeout)
                record("RESPONSE", [{"kind": frame.kind,
                                     "payload": frame.payload}
                                    for frame in frames if frame.kind != "DATA"])
                return frames

            def pump(seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    client.pump_logs()
                    time.sleep(.02)

            def has_transition(foreground, interactive, after=0):
                return any(
                    event["seconds"] >= after and
                    f"foreground={foreground}" in event["payload"] and
                    "requested=1" in event["payload"] and
                    f"display_interactive={interactive}" in event["payload"]
                    for event in report["lifecycle"])

            running = False
            try:
                report["hello"] = client.hello()
                client.subscribe_logs()
                pump(.2)  # Drain prior buffered log frames before launch.
                launch_at = time.monotonic() - start
                request(f"PACKAGE run {args.app}")
                running = True
                deadline = time.monotonic() + args.timeout
                next_keepalive = time.monotonic() + 10
                while time.monotonic() < deadline and not has_transition(0, 0):
                    pump(.1)
                    if time.monotonic() >= next_keepalive:
                        request("INFO")
                        next_keepalive += 10
                # The startup foreground log is transition-only and may be
                # absent. An effective 1->0 lock transition proves that this
                # instance was foreground; Bridge confirms its identity.
                if not (has_transition(1, 1) or
                        report.get("bridge_foreground")) or not has_transition(0, 0):
                    raise RuntimeError("foreground or idle-lock transition missing")
                report["lock_seconds"] = time.monotonic() - start
                if args.verify_frames:
                    # A synchronous Guest call and the bounded raster queue
                    # may already be in flight at the lifecycle transition.
                    # Let those complete, then check the steady locked state.
                    report["pipeline_settle_seconds"] = 1
                    pump(1)
                    request("PERF CLEAR")
                    request("PERF START")
                pump(5)
                if args.verify_frames:
                    paused = pxadb.info_properties(request("PERF STOP")[-1].payload)
                    report["paused_probe"] = paused
                    if int(paused["raster"]) or int(paused["present"]):
                        raise RuntimeError("frames continued while locked")
                    request("PERF CLEAR")
                locked = pxadb.screenshot_capture(request("SCREENSHOT JPEG"))
                pxadb.write_screenshot(locked, args.output / "locked.jpg")
                report["locked_frame"] = locked.metadata
                pump(5)
                locked_again = pxadb.screenshot_capture(
                    request("SCREENSHOT JPEG"))
                pxadb.write_screenshot(locked_again,
                                       args.output / "locked-second.jpg")
                report["locked_second_frame"] = locked_again.metadata
                if args.wake_tap is not None:
                    wake_x, wake_y = args.wake_tap
                    request(f"INPUT TAP {wake_x} {wake_y}")
                    pump(.5)
                x1, y1, x2, y2 = args.unlock_swipe
                request(f"INPUT SWIPE {x1} {y1} {x2} {y2} "
                        f"{args.unlock_duration_ms} {args.unlock_steps}")
                resume_deadline = time.monotonic() + 15
                while time.monotonic() < resume_deadline and not has_transition(
                        1, 1, report["lock_seconds"]):
                    pump(.1)
                if not has_transition(1, 1, report["lock_seconds"]):
                    raise RuntimeError("foreground transition missing after unlock")
                if args.verify_frames:
                    request("PERF CLEAR")
                    request("PERF START")
                pump(3)
                if args.verify_frames:
                    resumed = pxadb.info_properties(request("PERF STOP")[-1].payload)
                    report["resumed_probe"] = resumed
                    if not int(resumed["present"]):
                        raise RuntimeError("no displayed frames after unlock")
                    request("PERF CLEAR")
                unlocked = pxadb.screenshot_capture(request("SCREENSHOT JPEG"))
                pxadb.write_screenshot(unlocked, args.output / "unlocked.jpg")
                report["unlocked_frame"] = unlocked.metadata
                request(f"PACKAGE stop {args.app}")
                running = False
                pump(2)
                report["passed"] = True
            except (Exception, KeyboardInterrupt) as error:
                report["error"] = str(error)
            finally:
                try:
                    if running:
                        request(f"PACKAGE stop {args.app}")
                    packages = []
                    cleanup_deadline = time.monotonic() + 10
                    while time.monotonic() < cleanup_deadline:
                        pump(.2)
                        packages = request("PACKAGES")
                        current = [frame.payload for frame in packages
                                   if frame.kind == "PKG" and
                                   f":{args.app}\t" in frame.payload]
                        if len(current) == 1 and "active=0" in current[0]:
                            break
                    report["packages"] = [
                        {"kind": frame.kind, "payload": frame.payload}
                        for frame in packages if frame.kind != "DATA"]
                    matches = [frame.payload for frame in packages
                               if frame.kind == "PKG" and
                               f":{args.app}\t" in frame.payload]
                    if len(matches) != 1 or "active=0" not in matches[0]:
                        report["passed"] = False
                        report["cleanup_error"] = "test app still active"
                except Exception as error:
                    report["passed"] = False
                    report["cleanup_error"] = str(error)
                (args.output / "report.json").write_text(
                    json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
