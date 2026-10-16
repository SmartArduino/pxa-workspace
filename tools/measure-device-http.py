#!/usr/bin/env python3
"""Verify the HTTP SDK example and measure local heap minima per launch.

Install the matching http-stream package first. Result files remain reliable
when PXADB's bounded log queue drops messages. Screenshots are taken after
the measured allocation window, and boot-wide minima are never used.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--case", choices=("full", "buffered", "streamed"), default="full")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if args.repeat < 1 or args.timeout <= 0:
        parser.error("positive repeat and timeout required")
    args.output.mkdir(parents=True, exist_ok=True)
    rows, logs = [], []
    monitoring = False
    with pxadb.PxaDbClient(args.port, 15) as client:
        client.log_callback = lambda frame: logs.append(frame.payload)
        client.raw_callback = lambda data: logs.append(data.decode(errors="replace").strip())
        device = client.hello()
        client.subscribe_logs()
        fs = pxadb.NormalFsClient(client)
        def result_path():
            directories = [name for kind, _, name in fs.list("pxa-state/data")
                           if kind == "D" and name.endswith("~pxa-http-stream")]
            if len(directories) > 1:
                raise pxadb.PxaDbError("multiple http-stream app data directories")
            if not directories:
                return None
            directory = f"pxa-state/data/{directories[0]}"
            return f"{directory}/result.txt" if any(
                name == "result.txt" for _, _, name in fs.list(directory)) else None

        def request(command):
            return client.request(command, timeout=20)

        def pump(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                client.pump_logs()
                time.sleep(.005)

        def memory(command="MEMORY"):
            result = {}
            for frame in request(command):
                if frame.kind != "DATA":
                    continue
                fields = pxadb.info_properties(frame.payload)
                scope = fields.pop("scope")
                name = fields.pop("name", "")
                result[scope + (":" + name if name else "")] = {k: int(v) for k, v in fields.items()}
            return result

        def stop():
            try:
                request("PACKAGE stop pxa-http-stream")
            except pxadb.PxaDbRejectedError as error:
                if "not_running" not in str(error):
                    raise
            pump(1)

        try:
            for index in range(args.repeat):
                stop()
                previous = ""
                remote = result_path()
                if remote:
                    destination = args.output / "previous-result.txt"
                    fs.get(remote, destination)
                    previous = destination.read_text()
                request("MEMORY START")
                monitoring = True
                row = {"run": index, "before": memory(), "samples": []}
                rows.append(row)
                seen = len(logs)
                request("PACKAGE run pxa-http-stream")
                deadline = time.monotonic() + args.timeout
                result = ""
                next_file_check = time.monotonic()
                destination = args.output / f"result-{index}.txt"
                while time.monotonic() < deadline:
                    pump(.2)
                    row["samples"].append(memory())
                    new_logs = "\n".join(logs[seen:])
                    if time.monotonic() >= next_file_check or "HTTP_STREAM ALL PASS" in new_logs or "HTTP_STREAM BASELINE" in new_logs:
                        next_file_check = time.monotonic() + 1
                        remote = result_path()
                        if remote:
                            fs.get(remote, destination)
                            result = destination.read_text()
                    completed = ("HTTP_STREAM ALL PASS" in result if args.case == "full"
                                 else f"HTTP_STREAM BASELINE mode={1 if args.case == 'buffered' else 2} " in result)
                    if result != previous and (completed or "HTTP_STREAM FAILED" in result):
                        break
                row["result"] = result
                row["passed"] = result != previous and ("HTTP_STREAM ALL PASS" in result if args.case == "full"
                                 else f"HTTP_STREAM BASELINE mode={1 if args.case == 'buffered' else 2} " in result and "PASS" in result)
                row["after"] = memory("MEMORY STOP")
                monitoring = False
                row["peak_delta"] = {kind: row["before"]["heap"][kind + "_free"] - row["after"]["heap"][kind + "_min"]
                                     for kind in ("sram", "psram")}
                row["stable_delta"] = {kind: row["before"]["heap"][kind + "_free"] - row["after"]["heap"][kind + "_free"]
                                       for kind in ("sram", "psram")}
                capture = pxadb.screenshot_capture(request("SCREENSHOT"))
                pxadb.write_screenshot(capture, args.output / f"screen-{index}.png")
                (args.output / "report.json").write_text(json.dumps({"device": device, "case": args.case,
                    "minimum_scope": "local, includes initialization; screenshots excluded",
                    "runs": rows}, indent=2) + "\n")
                (args.output / "runtime.log").write_text("\n".join(logs))
                print(json.dumps({k: row[k] for k in ("run", "passed", "result", "peak_delta", "stable_delta")}), flush=True)
                if not row["passed"]:
                    return 1
        finally:
            if monitoring:
                memory("MEMORY STOP")
            stop()
            request("INPUT KEY HOME")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
