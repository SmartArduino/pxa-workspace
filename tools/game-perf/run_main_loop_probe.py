#!/usr/bin/env python3
"""Exercise an installed, signed Core 2.1 event-port probe on a real device."""
import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pxadb"))
import pxadb


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def fresh_probe_records(records, device_ms, app="pxa-main-loop-probe"):
    """A log subscription replays history; only accept this activation's logs."""
    result = []
    for row in records:
        stamp = re.match(r"^(\d+)\t", row["payload"])
        if (row["kind"] == "LOG" and stamp and int(stamp[1]) >= device_ms
                and f"[PXA app={app} " in row["payload"] and "MAINLOOP " in row["payload"]):
            result.append(row)
    return result


def heartbeat_progress(records):
    # Only this retained call emits alive, after checking its local stack
    # sentinel following native waits. Three distinct values prove continued
    # execution even if the bounded log queue drops a one-shot entered line.
    return sorted({int(match[1]) for row in records
                   if (match := re.search(r"MAINLOOP alive value=0 heartbeat=(\d+)", row["payload"]))})


def require_probe_canvas(shot):
    width, height = int(shot.metadata["width"]), int(shot.metadata["height"])
    require(shot.metadata["format"] == "rgb565le" and width == 320 and height == 240,
            "Unexpected probe capture format or dimensions")
    for x, y in ((32, 200), (280, 160), (280, 200)):
        offset = (y * width + x) * 2
        require(int.from_bytes(shot.data[offset:offset + 2], "little") == 0x122b,
                "Probe UI is covered or missing; expected its opaque blue canvas")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--replace-with", help="Installed app to validate queued replacement after native cancellation")
    args = parser.parse_args()
    if args.replace_with and not re.fullmatch(r"[a-z][a-z0-9._-]{0,63}", args.replace_with):
        parser.error("--replace-with requires a bare package ID")
    os.environ.setdefault("PXADB_BAUD", "2000000")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"qualified": False, "snapshots": {}, "phases": []}
    records = []
    app = "pxa-main-loop-probe"
    prepared = started = replacement_requested = False
    with (args.output / "serial.jsonl").open("w", buffering=1) as log, pxadb.PxaDbClient(args.port, 10) as client:
        def record(kind, payload):
            row = {"time": time.monotonic(), "kind": kind, "payload": payload}
            log.write(json.dumps(row) + "\n")
            records.append(row)

        client.log_callback = lambda frame: record(frame.kind, frame.payload)

        def request(command):
            record("COMMAND", command)
            frames = client.request(command, timeout=30)
            for frame in frames:
                if frame.kind != "DATA" or command != "SCREENSHOT":
                    record(frame.kind, frame.payload)
            return frames

        def pump(seconds):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                client.pump_logs()
                time.sleep(.02)

        def snapshot(name):
            report["snapshots"][name] = {
                cmd: [{"kind": f.kind, "payload": f.payload} for f in request(cmd)]
                for cmd in ("PACKAGES", "MEMORY", "INFO")
            }

        def screenshot(name):
            client.unsubscribe_logs(timeout=5)
            try:
                for retry in range(5):
                    try:
                        shot = pxadb.screenshot_capture(request("SCREENSHOT"))
                        break
                    except pxadb.PxaDbError as error:
                        if retry == 4 or not pxadb.screenshot_retryable(error):
                            raise
                        time.sleep(.2)
                pxadb.write_screenshot(shot, args.output / (name + ".png"))
                report.setdefault("screenshots", {})[name] = shot.metadata
                return shot
            finally:
                client.subscribe_logs()

        def active():
            return any(f.kind == "PKG" and f":{app}\t" in f.payload and "active=1" in f.payload
                       for f in request("PACKAGES"))

        def stop():
            began = time.monotonic()
            request(f"PACKAGE stop {app}")
            for _ in range(50):
                pump(.1)
                if not active():
                    report.setdefault("stop_seconds", []).append(time.monotonic() - began)
                    return
            raise RuntimeError("Probe did not leave its retained VM call")

        try:
            report["hello"] = client.hello()
            require(pxadb.info_properties(report["hello"])["serial"] == args.serial,
                    "Device serial does not match --serial")
            snapshot("before")
            require(not active(), "Start with the probe stopped")
            client.subscribe_logs()
            prepared = True
            request("INPUT KEY HOME")
            request("INPUT POINTER DOWN 160 185 0")
            for y in (163, 139, 115, 91, 67, 53):
                pump(.1); request(f"INPUT POINTER MOVE 160 {y} 0")
            request("INPUT POINTER UP 160 53 0")
            pump(.3)
            for cycle in range(3):
                begin = len(records)
                barrier = request("INPUT SYNC")
                device_ms = int(pxadb.info_properties(barrier[-1].payload)["device_us"]) // 1000
                started = True
                request(f"PACKAGE run {app}")
                pump(8)
                require(active(), "Probe is not active after launch")
                require(len(heartbeat_progress(fresh_probe_records(records[begin:], device_ms))) >= 3,
                        "No fresh progress inside the retained native wait loop")
                if cycle == 0:
                    for bits in (1, 2, 16):
                        request(f"INPUT CONTROLLER 0 1 {bits}"); pump(.2)
                        request("INPUT CONTROLLER 0 1 0"); pump(.2)
                    request("INPUT POINTER DOWN 285 180 0"); pump(.2)
                    request("INPUT POINTER UP 285 180 0"); pump(.4)
                    require_probe_canvas(screenshot("foreground"))
                    snapshot("running")
                    request("INPUT KEY HOME"); pump(3)
                    screenshot("background")
                    request(f"PACKAGE run {app}"); pump(3)
                    require_probe_canvas(screenshot("resumed"))
                phase = fresh_probe_records(records[begin:], device_ms)
                heartbeats = heartbeat_progress(phase)
                require(len(heartbeats) >= 3,
                        "Probe did not survive multiple waits")
                report["phases"].append({"cycle": cycle, "device_ms": device_ms,
                    "heartbeats": heartbeats,
                    "entered_log_received": any("MAINLOOP entered" in r["payload"] for r in phase)})
                if cycle == 1 and args.replace_with:
                    replacement_requested = True
                    request(f"PACKAGE run {args.replace_with}"); pump(5)
                    require(not active(), "Replacement did not cancel the old native loop")
                    require(any(f.kind == "PKG" and f":{args.replace_with}\t" in f.payload and "active=1" in f.payload
                                for f in request("PACKAGES")), "Queued replacement was lost")
                    request(f"PACKAGE stop {args.replace_with}"); pump(2)
                    replacement_requested = False
                    report["replacement"] = args.replace_with
                else:
                    stop(); pump(1)
                snapshot(f"stopped-{cycle}")
                print(f"cycle {cycle}: start, retained waits and terminal stop passed", flush=True)
            probe_logs = [r["payload"] for r in fresh_probe_records(records, report["phases"][0]["device_ms"])]
            for kind in ("pointer", "controller", "lifecycle"):
                require(any(f"MAINLOOP {kind}" in text for text in probe_logs), f"No {kind} event received")
            require(any("MAINLOOP lifecycle value=0" in text for text in probe_logs), "No background transition")
            require(any("MAINLOOP lifecycle value=1" in text for text in probe_logs), "No foreground transition")
            report["probe_logs"] = probe_logs
            report["qualified"] = True
        except BaseException as error:
            report["failure"] = {"type": type(error).__name__, "message": str(error)}
            raise
        finally:
            # Failed identity/initial-state guards are read-only. Cleanup only
            # touches input/apps after this invocation started using them.
            if prepared:
                try:
                    request("INPUT CONTROLLER 0 1 0")
                    if started and active():
                        request(f"PACKAGE stop {app}")
                    if replacement_requested:
                        request(f"PACKAGE stop {args.replace_with}")
                    pump(1)
                    request("INPUT KEY HOME")
                except Exception as error:
                    report["cleanup_error"] = str(error)
                    report["qualified"] = False
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    require(report["qualified"], "Probe cleanup failed: " + report.get("cleanup_error", "unknown"))


if __name__ == "__main__":
    main()
