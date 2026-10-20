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
            assert pxadb.info_properties(report["hello"])["serial"] == args.serial
            snapshot("before")
            assert not active(), "Start with the probe stopped"
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
                started = True
                request(f"PACKAGE run {app}")
                pump(8)
                assert active()
                assert any("MAINLOOP entered" in r["payload"] for r in records[begin:])
                if cycle == 0:
                    for bits in (1, 2, 16):
                        request(f"INPUT CONTROLLER 0 1 {bits}"); pump(.2)
                        request("INPUT CONTROLLER 0 1 0"); pump(.2)
                    request("INPUT POINTER DOWN 285 180 0"); pump(.2)
                    request("INPUT POINTER UP 285 180 0"); pump(.4)
                    screenshot("foreground")
                    snapshot("running")
                    request("INPUT KEY HOME"); pump(3)
                    screenshot("background")
                    request(f"PACKAGE run {app}"); pump(3)
                    screenshot("resumed")
                phase = records[begin:]
                assert sum("MAINLOOP alive" in r["payload"] for r in phase) >= 3
                report["phases"].append({"cycle": cycle, "heartbeats": sum("MAINLOOP alive" in r["payload"] for r in phase)})
                if cycle == 1 and args.replace_with:
                    replacement_requested = True
                    request(f"PACKAGE run {args.replace_with}"); pump(5)
                    assert not active(), "Replacement did not cancel the old native loop"
                    assert any(f.kind == "PKG" and f":{args.replace_with}\t" in f.payload and "active=1" in f.payload
                               for f in request("PACKAGES")), "Queued replacement was lost"
                    request(f"PACKAGE stop {args.replace_with}"); pump(2)
                    replacement_requested = False
                    report["replacement"] = args.replace_with
                else:
                    stop(); pump(1)
                snapshot(f"stopped-{cycle}")
                print(f"cycle {cycle}: start, retained waits and terminal stop passed", flush=True)
            probe_logs = [r["payload"] for r in records if "MAINLOOP " in r["payload"]]
            for kind in ("pointer", "controller", "lifecycle"):
                assert any(f"MAINLOOP {kind}" in text for text in probe_logs), kind
            assert any("MAINLOOP lifecycle value=0" in text for text in probe_logs)
            assert any("MAINLOOP lifecycle value=1" in text for text in probe_logs)
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
                    if started:
                        request(f"PACKAGE stop {app}")
                    if replacement_requested:
                        request(f"PACKAGE stop {args.replace_with}")
                    pump(1)
                    request("INPUT KEY HOME")
                except Exception as error:
                    report["cleanup_error"] = str(error)
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
