#!/usr/bin/env python3
"""Compare startup heap windows in one test firmware with buffered_reference.c.

Uses B/S/S/B order, warms each app and keeps screenshots outside measurement.
The marker is removed on exit. Use only with installed QA apps on a test device.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pxadb"))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=2000000)
    parser.add_argument("--expected-serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=2)
    parser.add_argument("--apps", nargs="+", required=True)
    parser.add_argument("--wasm-apps", nargs="*", default=[])
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("pairs must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["PXADB_BAUD"] = str(args.baud)
    marker = "pxa-state/inbox/aot-buffered-reference.flag"
    report = {"passed": False, "rows": [], "errors": [], "apps": args.apps,
              "scope": "one firmware; ABBA; startup local minima; five running heap samples"}
    records = []

    def save():
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    with pxadb.PxaDbClient(args.port, 30) as client, \
            (args.output / "serial.jsonl").open("w", buffering=1) as log, \
            tempfile.TemporaryDirectory() as temporary:
        def record(kind, payload):
            event = {"seconds": time.monotonic(), "kind": kind, "payload": payload}
            records.append(event)
            log.write(json.dumps(event) + "\n")
            if isinstance(payload, str) and any(token in payload for token in (
                    "Guru Meditation", "assert failed", "PXA Guest exception",
                    "Package launch failed", "rst:0x")):
                report["errors"].append(event)

        client.log_callback = lambda frame: record(frame.kind, frame.payload)
        client.raw_callback = lambda data: record("RAW", data.decode(errors="replace"))
        hello = client.hello()
        report["hello"] = hello
        if pxadb.info_properties(hello).get("serial") != args.expected_serial:
            raise RuntimeError("unexpected device identity; no mutations sent")
        client.subscribe_logs()
        fs = pxadb.NormalFsClient(client)
        if any(name == marker.rsplit("/", 1)[1] for _, _, name in fs.list("pxa-state/inbox")):
            raise RuntimeError("reference marker already exists; remove it before testing")
        local_marker = Path(temporary) / "mode"
        local_marker.write_text("buffered\n")

        def command(text):
            record("COMMAND", text)
            frames = [{"kind": f.kind, "payload": f.payload}
                      for f in client.request(text, timeout=30)]
            record("RESPONSE", frames)
            return frames

        def pump(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                client.pump_logs()
                time.sleep(.01)

        def wait(app, active):
            deadline = time.monotonic() + 25
            while True:
                packages = [f["payload"] for f in command("PACKAGES")
                            if f["kind"] == "PKG" and ":" + app + "\t" in f["payload"]]
                if packages and all("active=" + str(int(active)) in
                                    p.split("\t")[-1].split(";") for p in packages):
                    return
                if time.monotonic() >= deadline:
                    raise RuntimeError("unexpected app state: " + app)
                pump(.1)

        running = None
        owns_marker = False
        try:
            if any(f["kind"] == "PKG" and "active=1" in f["payload"]
                   for f in command("PACKAGES")):
                raise RuntimeError("stop active apps before testing")
            for app in args.apps:
                running = app
                command("PACKAGE run " + app)
                wait(app, True)
                pump(2)
                command("PACKAGE stop " + app)
                wait(app, False)
                running = None
                pump(2)
                for pair in range(args.pairs):
                    for mode in ("buffered", "stream", "stream", "buffered"):
                        if mode == "buffered" and not owns_marker:
                            owns_marker = True
                            fs.put(local_marker, marker, progress=lambda *_: None)
                        elif mode == "stream" and owns_marker:
                            fs.remove(marker)
                            owns_marker = False
                        pump(.5)
                        row = {"app": app, "pair": pair, "mode": mode,
                               "before": command("MEMORY")}
                        report["rows"].append(row)
                        command("MEMORY START")
                        start_log = len(records)
                        begin = time.monotonic()
                        running = app
                        command("PACKAGE run " + app)
                        wait(app, True)
                        pump(1.2)
                        row["active_ms"] = (time.monotonic() - begin - 1.2) * 1000
                        row["window"] = command("MEMORY STOP")
                        row["steady"] = []
                        for _ in range(5):
                            row["steady"].append(command("MEMORY"))
                            pump(.12)
                        row["logs"] = [r["payload"] for r in records[start_log:]
                                       if r["kind"] == "LOG" and any(t in r["payload"] for t in
                                           ("AOT streamed:", "WAMR dynamic:", "AOT mapped:"))]
                        if app not in args.wasm_apps:
                            streamed = any("AOT streamed:" in line for line in row["logs"])
                            if streamed != (mode == "stream"):
                                raise RuntimeError("mode could not be verified from loader logs")
                        command("PACKAGE stop " + app)
                        wait(app, False)
                        running = None
                        pump(2)
                        row["after"] = command("MEMORY")
                        save()
            report["passed"] = not report["errors"]
        except Exception as error:
            report["error"] = str(error)
            raise
        finally:
            try:
                if running:
                    command("PACKAGE stop " + running)
            finally:
                if owns_marker:
                    fs.remove(marker)
                save()
        print(json.dumps({"passed": report["passed"], "rows": len(report["rows"])}))
        return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
