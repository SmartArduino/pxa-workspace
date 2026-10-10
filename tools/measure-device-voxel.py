#!/usr/bin/env python3
"""Capture bounded device frame samples for an already installed PXA game."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct
import sys
import time
import zlib

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
    parser.add_argument("--wake-home", action="store_true",
                        help="keep an already unlocked device awake without tapping launcher icons")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--local-heap", action="store_true",
                        help="record ESP-IDF allocation minima for each launch and steady window")
    parser.add_argument("--minimum-warm-frames", type=int, default=0,
                        help="require this many accepted frames before capture (sequential frame IDs)")
    parser.add_argument("--pixel-search", action="store_true",
                        help="fixed-seed Pixel test package: reset selected app data each run, new warrior, twelve searches, verify save")
    args = parser.parse_args()
    if args.warmup < 0 or not 0 < args.seconds <= 30 or args.repeat < 1 or args.minimum_warm_frames < 0:
        parser.error("warmup >= 0, 0 < seconds <= 30, repeat >= 1 required")
    if args.pixel_search and (args.seconds < 4 or args.menu_tap or args.app not in
                             ("pxa-pixel-dungeon", "pxa-pixel-dungeon-cpp")):
        parser.error("--pixel-search requires a Pixel package and >= 4 seconds, without --menu-tap")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"app": args.app, "port": args.port, "warmup_seconds": args.warmup,
              "capture_seconds": args.seconds, "runs": [], "passed": False}
    if args.pixel_search:
        report["scene"] = {"name": "new-warrior-twelve-searches", "seed": "0x51ed270b",
                           "width": 296, "height": 240, "dpi": 160,
                           "safe_insets": [8, 10, 8, 10], "corner_radius": 58,
                           "search_pixel": [113, 213], "pointer_hold_ms": 80,
                           "music": "original-region-music"}
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
                # Statistics can span several DATA frames. Live launch logs
                # share the firmware's bounded TX queue; pause them while
                # collecting a complete response, outside the FPS window.
                quiet = client.log_subscribed and command.startswith(("MEMORY", "PERF"))
                if quiet:
                    client.unsubscribe_logs()
                frames = client.request(command, timeout=20)
                if quiet:
                    client.subscribe_logs()
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

            def pixel_tap(x, y):
                # Explicit edges span several touch/LVGL samples. INPUT SYNC
                # acknowledges router delivery, not Guest processing.
                request(f"INPUT POINTER DOWN {x} {y} 0")
                request("INPUT SYNC")
                pump(.08)
                request(f"INPUT POINTER UP {x} {y} 0")
                request("INPUT SYNC")

            def pixel_save(index):
                private = ("pxa-state/data/17982acd08493944059713aee9a56135dd4a080ecaaf7aa77df11fdf3102d171~"
                           + args.app + "/.pxa-storage")
                fs = pxadb.NormalFsClient(client)
                snapshots = []
                for kind, size, name in fs.list(private):
                    if kind != "F" or not name.startswith(".pxa-kv-"):
                        continue
                    path = args.output / f"save-{index}-{name.removeprefix('.pxa-')}"
                    fs.get(private + "/" + name, path)
                    raw = path.read_bytes()
                    if len(raw) < 24:
                        continue
                    magic, version, _, generation, length, crc = struct.unpack("<4sHHQII", raw[:24])
                    body = raw[24:]
                    if magic != b"PXKV" or version != 1 or len(body) != length or zlib.crc32(body) != crc:
                        continue
                    values = {}; at = 2
                    for _ in range(struct.unpack_from("<H", body)[0]):
                        n = body[at]; at += 1; key = body[at:at+n]; at += n
                        n = struct.unpack_from("<H", body, at)[0]; at += 2
                        values[key] = body[at:at+n]; at += n
                    if at != len(body):
                        raise RuntimeError("invalid KV snapshot length")
                    snapshots.append((generation, values))
                if not snapshots:
                    raise RuntimeError("no valid committed Pixel save")
                saved = max(snapshots, key=lambda row: row[0])[1][b"pixel-dungeon.save"]
                if len(saved) < 30 or struct.unpack_from("<H", saved, 28)[0] != 24:
                    raise RuntimeError("twelve searches did not commit twenty-four turns")
                if struct.unpack_from("<I", saved, 4)[0] != 0x51ed270b or saved[12:14] != bytes([1, 0]):
                    raise RuntimeError("wrong Pixel benchmark seed, floor or class")
                return {"turn": 24, "seed": "0x51ed270b", "depth": 1, "class": 0,
                        "sha256": hashlib.sha256(saved).hexdigest()}

            running = recording = heap_monitoring = False
            try:
                report["hello"] = client.hello()
                if "perf-raster" not in report["hello"] or "memory" not in report["hello"]:
                    raise RuntimeError("firmware requires perf-raster and memory capabilities")
                if args.local_heap and "heap-local" not in report["hello"]:
                    raise RuntimeError("firmware requires heap-local capability")
                client.subscribe_logs()
                report["packages_before"] = [f.payload for f in request("PACKAGES") if f.kind == "PKG"]
                if any("active=1" in p for p in report["packages_before"]):
                    raise RuntimeError("stop the active application before measuring")
                for index in range(args.repeat):
                    if args.wake_home:
                        request("INPUT KEY HOME")
                        pump(.5)
                    if args.unlock_swipe:
                        x1, y1, x2, y2 = args.unlock_swipe
                        # A tap on the launcher can start an app before the
                        # launch heap monitor. HOME wakes without selecting it.
                        request("INPUT KEY HOME")
                        pump(.3)
                        request(f"INPUT SWIPE {x1} {y1} {x2} {y2} 400 8")
                        # Launcher activation is asynchronous; check after it
                        # has settled, rather than accepting a pending launch.
                        pump(2)
                        packages = [f.payload for f in request("PACKAGES") if f.kind == "PKG"]
                        if any("active=1" in p for p in packages):
                            raise RuntimeError("unlock started an application before the launch monitor")
                    if args.pixel_search:
                        # Only the explicitly selected test game's private data.
                        request(f"PACKAGE clear-data {args.app}")
                        pump(1)
                    run = {"index": index, "memory_before": memory()}
                    if any(run["memory_before"]["surface"][key] for key in
                           ("frame", "scratch", "mailbox", "probe")):
                        raise RuntimeError("a surface is still alive before the launch monitor")
                    report["runs"].append(run)
                    request("PERF CLEAR")
                    if args.local_heap:
                        request("MEMORY START")
                        heap_monitoring = True
                        run["memory_launch_start"] = memory()
                    request(f"PACKAGE run {args.app}")
                    running = True
                    pump(args.warmup)
                    if args.pixel_search:
                        # Same native-verified layout in both 296x240, 160 DPI
                        # packages. Cold-start catalog -> empty slot -> warrior.
                        for x, y in ((148, 122), (148, 120), (62, 129)):
                            pixel_tap(x, y)
                            pump(1)
                        pump(args.warmup)
                    if args.menu_tap:
                        request(f"INPUT TAP {args.menu_tap[0]} {args.menu_tap[1]}")
                        request("INPUT SYNC")
                        pump(args.warmup)
                    active = [f.payload for f in request("PACKAGES")
                              if f.kind == "PKG" and "active=1" in f.payload]
                    if len(active) != 1 or active[0].split("\t", 1)[0].split(":", 1)[-1] != args.app:
                        raise RuntimeError("requested application is not the active application")
                    run["memory_warm"] = memory()
                    if args.local_heap:
                        request("MEMORY STOP")
                        heap_monitoring = False
                        request("MEMORY START")
                        heap_monitoring = True
                        run["memory_steady_start"] = memory()
                    request("PERF START")
                    recording = True
                    if args.pixel_search:
                        capture_started = time.monotonic()
                        run["search_input_seconds"] = []
                        for search in range(12):
                            pump(max(0, capture_started + (search+1)*.24 - time.monotonic()))
                            run["search_input_seconds"].append(time.monotonic() - capture_started)
                            pixel_tap(113, 213)
                        pump(max(0, capture_started + args.seconds - time.monotonic()))
                        run["capture_elapsed_seconds"] = time.monotonic() - capture_started
                    else:
                        pump(args.seconds)
                    run["probe"] = pxadb.info_properties(request("PERF STOP")[-1].payload)
                    recording = False
                    if any(int(run["probe"][key]) for key in
                           ("raster_overflow", "present_overflow")):
                        raise RuntimeError("probe overflow: shorten the capture window")
                    run["memory_capture"] = memory()
                    if args.local_heap:
                        request("MEMORY STOP")
                        heap_monitoring = False
                        for phase, start_key, end_key in (
                                ("launch", "memory_launch_start", "memory_warm"),
                                ("steady", "memory_steady_start", "memory_capture")):
                            before = run[start_key]["heap"]
                            after = run[end_key]["heap"]
                            run[phase + "_heap"] = {
                                kind + suffix: after[kind + "_total"] - after[kind + field]
                                for kind in ("sram", "psram")
                                for suffix, field in (("_used", "_free"), ("_peak_used", "_min"))}
                            run[phase + "_heap"].update({
                                kind + "_peak_delta": before[kind + "_free"] - after[kind + "_min"]
                                for kind in ("sram", "psram")})
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
                    if args.pixel_search and (int(capture.metadata["width"]) != 296 or
                                              int(capture.metadata["height"]) != 240):
                        raise RuntimeError("Pixel benchmark requires 296x240 display")
                    if not run["present_us"] or int(run["probe"]["surface_changed"]):
                        raise RuntimeError("no displayed frames or surface changed during capture")
                    if args.minimum_warm_frames:
                        warmed = int(capture.metadata["frame_id"]) - int(run["probe"]["present"]) - 1
                        run["minimum_observed_warm_frames"] = warmed
                        if warmed < args.minimum_warm_frames:
                            raise RuntimeError("insufficient warm frames: increase --warmup")
                    run["display_fps"] = 1e6 / run["present"]["mean_us"]
                    if args.pixel_search:
                        # C persists on a user back request before the Host
                        # suspends the component; C++ also saves on background.
                        request("INPUT KEY BACK")
                        pump(1)
                        request("INPUT KEY HOME")
                        pump(2)
                        run["committed_save"] = pixel_save(index)
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
                                        ("MEMORY STOP", heap_monitoring),
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
