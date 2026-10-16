#!/usr/bin/env python3
"""Record bounded game/display/input windows; screenshots stay outside timing.

Uses device timestamps and distinct panel-completion intervals, not submission
FPS or UART round trips. The input association is Host telemetry, not proof of
physical touch-to-photon latency. The supplied manifest must match the installed
game; --start is the title-page tap, in logical display pixels.
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
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--package", type=Path)
    source.add_argument("--installed-manifest", type=Path,
                        help="snapshot of the installed manifest when its original package is unavailable")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=4)
    parser.add_argument("--warmup", type=float, default=6)
    parser.add_argument("--charge-seconds", type=float, default=.45,
                        help="requested hold duration; UART/sync time also affects actual device hold")
    parser.add_argument("--start", type=int, nargs=2)
    parser.add_argument("--prepare-tap", type=int, nargs=2, action="append", default=[],
                        help="tap a menu before --start; repeat to load the same saved world")
    parser.add_argument("--expected-serial", help="abort before input if HELLO identity differs")
    parser.add_argument("--size", type=int, nargs=2, default=[800, 480])
    parser.add_argument("--screenshot-format", choices=["jpeg", "raw"], default="jpeg",
                        help="raw captures the scanout buffer on hosts without direct-game JPEG capture")
    parser.add_argument("--scenes", nargs="+", choices=["idle", "turn", "walk", "charge"],
                        default=["idle", "turn"])
    args = parser.parse_args()
    if args.repeat < 1 or not 1 <= args.seconds <= 4 or args.warmup < 0:
        parser.error("repeat >= 1, 1 <= seconds <= 4, warmup >= 0 required")
    if not 0 < args.charge_seconds <= args.seconds:
        parser.error("0 < charge-seconds <= seconds required")
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = pxadb.read_package_manifest(args.package) if args.package else args.installed_manifest.read_bytes()
    manifest_sha256 = hashlib.sha256(manifest).hexdigest()
    report = {"app": args.app,
              "package_sha256": hashlib.sha256(args.package.read_bytes()).hexdigest() if args.package else None,
              "manifest_sha256": manifest_sha256,
              "seconds": args.seconds, "warmup": args.warmup,
              "charge_seconds": args.charge_seconds,
              "size": args.size, "start": args.start, "prepare_taps": args.prepare_tap,
              "screenshot_format": args.screenshot_format,
              "runs": [], "passed": False, "display_errors": [],
              "latency_scope": "injected device sample to Host Guest dispatch, associated submission and panel buffer completion; excludes physical controller and optical response"}
    epoch = time.monotonic()
    running = recording = monitoring = verified = False
    with (args.output / "serial.jsonl").open("w", buffering=1) as log, \
            pxadb.PxaDbClient(args.port, 15) as client:
        def record(kind, payload):
            event = {"seconds": time.monotonic() - epoch,
                     "kind": kind, "payload": payload}
            log.write(json.dumps(event) + "\n")
            if kind in ("LOG", "RAW") and any(message in payload for message in (
                    "send color failed", "flush failed (", "DMA TX underflow",
                    "Failed to allocate priv TX buffer", "Task watchdog got triggered",
                    "Guru Meditation", "assert failed")):
                report["display_errors"].append(event)

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
            command = "SCREENSHOT" if args.screenshot_format == "raw" else "SCREENSHOT JPEG"
            for attempt in range(6):
                try:
                    shot = pxadb.screenshot_capture(request(command))
                    break
                except pxadb.PxaDbError as error:
                    if attempt == 5 or not (pxadb.screenshot_retryable(error) or "rate_limited" in str(error)):
                        raise
                    pump(.25)
            suffix = ".png" if args.screenshot_format == "raw" else ".jpg"
            pxadb.write_screenshot(shot, args.output / (name + suffix))
            if [int(shot.metadata[k]) for k in ("width", "height")] != args.size:
                raise RuntimeError("unexpected display geometry: " + str(shot.metadata))
            return shot.metadata

        def require_active():
            if not any(args.app in f.payload and "active=1" in f.payload
                       for f in request("PACKAGES") if f.kind == "PKG"):
                raise RuntimeError("test application exited; see serial.jsonl")

        try:
            report["hello"] = client.hello()
            if args.expected_serial and pxadb.info_properties(report["hello"]).get("serial") != args.expected_serial:
                raise RuntimeError("unexpected device identity; no input or app commands sent")
            if "heap-local" not in report["hello"]:
                raise RuntimeError("firmware must expose local heap minima")
            packages = request("PACKAGES")
            installed = [frame.payload.split("\t")[0] for frame in packages
                         if frame.kind == "PKG" and "installed=1" in frame.payload
                         and frame.payload.split("\t")[0].split(":", 1)[-1] == args.app]
            if len(installed) != 1:
                raise RuntimeError("expected exactly one installed test application")
            manifest_path = "pxa-state/packages/" + installed[0].replace(":", "~", 1) + "/manifest.pxm"
            installed_digest = properties("FSSHA " + pxadb.client_encode_path(manifest_path))
            if installed_digest.get("sha256") != manifest_sha256:
                raise RuntimeError("installed package differs from the supplied manifest; no input or app commands sent")
            for frame in packages:
                if frame.kind == "PKG" and "active=1" in frame.payload:
                    if frame.payload.split("\t", 1)[0] != installed[0]:
                        raise RuntimeError("stop the other active application before benchmarking")
                    request(f"PACKAGE stop {args.app}")
                    pump(.5)
            verified = True
            client.subscribe_logs()
            w, h = args.size
            # The lock screen deliberately suspends presentation. An upward
            # swipe also refreshes activity without changing saved settings.
            # Wake first and pace samples: the router ACK does not guarantee
            # LVGL has polled the last position before the next arrives.
            request("INPUT KEY HOME")
            pump(.3)
            pointer("DOWN", w * .5, h * .77)
            pump(.12)
            for factor in (.68, .58, .48, .38, .28, .22):
                pointer("MOVE", w * .5, h * factor)
                pump(.12)
            pointer("UP", w * .5, h * .22)
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
                    for location in args.prepare_tap:
                        pointer("DOWN", *location)
                        pump(.15)
                        pointer("UP", *location)
                        pump(.6)
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
                    properties("PERF INPUT")
                    before = memory()
                    request("PERF START")
                    recording = True
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
                        pump(args.charge_seconds)
                        pointer("UP", w * .5, h * .65)
                    pump(begin + args.seconds - time.monotonic())
                    metrics = {k: int(v) for k, v in properties("PERF INPUT").items()}
                    perf = {k: int(v) for k, v in properties("PERF STOP").items()}
                    recording = False
                    # Bound the actual recording window. MEMORY STOP and its
                    # heap walk can take much longer under load; including that
                    # unrecorded time understated whole-window display FPS.
                    recording_seconds = time.monotonic() - begin
                    steady = memory("MEMORY STOP")
                    monitoring = False
                    row = {"repeat": repeat, "scene": scene, "elapsed_seconds": recording_seconds,
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
                    # A result screen can stop drawing before the window ends.
                    # Keep the active-frame interval rate and the whole-window
                    # rate distinct instead of attributing idle time to raster.
                    row["window_display_fps"] = row["present"]["count"] / row["elapsed_seconds"]
                    row["active_present_seconds"] = sum(row["present_us"]) / 1e6
                    row["screenshot_after"] = screenshot(f"{repeat}-{scene}-after")
                    request("PERF CLEAR")
                    request(f"PACKAGE stop {args.app}")
                    running = False
                    pump(.4)
                    row["stopped"] = memory()
                    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                    print(json.dumps({k: row[k] for k in ("repeat", "scene", "display_fps", "raster", "input")}), flush=True)
            if report["display_errors"]:
                raise RuntimeError("device/display errors occurred; see serial.jsonl")
            report["passed"] = True
        except Exception as error:
            report["error"] = str(error)
            raise
        finally:
            # STOP freezes samples but keeps the probe allocated for READ.
            # Release it even if reading/capturing fails after STOP.
            cleanup = ["PERF CLEAR"] if verified else []
            cleanup += ["MEMORY STOP"] if monitoring else []
            cleanup += ["INPUT CANCEL"] if verified else []
            cleanup += [f"PACKAGE stop {args.app}"] if running else []
            cleanup += ["INPUT KEY HOME"] if verified else []
            for command in cleanup:
                try:
                    request(command)
                except Exception as error:
                    report.setdefault("cleanup_errors", []).append(str(error))
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
