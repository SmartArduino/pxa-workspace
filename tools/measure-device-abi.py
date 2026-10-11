#!/usr/bin/env python3
"""Measure the ABI control benchmark, including fresh allocation peak windows."""

import argparse
import hashlib
import json
import re
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb

SAMPLE = re.compile(r"ABI_BENCH path=(submit|call) sample=(\d+) iterations=(\d+) "
                    r"elapsed_ns=(\d+) errors=(\d+) transport_bytes=(\d+)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--app", required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("repeat must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"app": args.app, "package_sha256": hashlib.sha256(args.package.read_bytes()).hexdigest(),
              "runs": [], "passed": False}
    seen = set()
    collected = []
    started = time.monotonic()
    with (args.output / "serial.jsonl").open("w", buffering=1) as log:
        def record(kind, payload):
            log.write(json.dumps({"seconds": time.monotonic() - started,
                                  "kind": kind, "payload": payload}) + "\n")

        with pxadb.PxaDbClient(args.port, 10) as client:
            def on_log(frame):
                record(frame.kind, frame.payload)
                match = SAMPLE.search(frame.payload)
                # LOG subscriptions replay the ring. Use the device timestamp
                # and whole payload, not sample index (which restarts on resume).
                if match and frame.payload not in seen:
                    seen.add(frame.payload)
                    path, index, count, elapsed, errors, size = match.groups()
                    collected.append({"path": path, "index": int(index), "iterations": int(count),
                                      "elapsed_ns": int(elapsed), "errors": int(errors),
                                      "transport_bytes": int(size)})

            client.log_callback = on_log
            client.raw_callback = lambda data: record("RAW", data.decode("utf-8", "replace"))

            def request(command):
                record("COMMAND", command)
                quiet = client.log_subscribed and command.startswith("MEMORY")
                if quiet:
                    client.unsubscribe_logs(timeout=3)
                frames = client.request(command, timeout=20)
                if quiet:
                    client.subscribe_logs()
                record("RESPONSE", [{"kind": f.kind, "payload": f.payload} for f in frames])
                return frames

            def pump(seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    client.pump_logs()
                    time.sleep(.01)

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

            def window(phase):
                nonlocal monitoring
                collected.clear()
                # Drain logs from the preceding phase before the new launch.
                pump(.1)
                collected.clear()
                request("MEMORY START")
                monitoring = True
                before = memory()
                request(f"PACKAGE run {args.app}")
                deadline = time.monotonic() + 15
                while len(collected) < 32 and time.monotonic() < deadline:
                    pump(.05)
                after = memory()
                request("MEMORY STOP")
                monitoring = False
                samples = list(collected)
                if len(samples) != 32 or sorted(s["index"] for s in samples) != list(range(32)):
                    raise RuntimeError(f"{phase}: expected 32 distinct samples, got {len(samples)}")
                if any(s["errors"] or s["elapsed_ns"] <= 0 for s in samples):
                    raise RuntimeError(f"{phase}: control operation or clock failed")
                per_call = [s["elapsed_ns"] / s["iterations"] for s in samples]
                heap = {}
                for kind in ("sram", "psram"):
                    b, a = before["heap"], after["heap"]
                    heap[kind] = {"stable_used": a[kind + "_total"] - a[kind + "_free"],
                                  "true_peak_used": a[kind + "_total"] - a[kind + "_min"],
                                  "allocation_delta": b[kind + "_free"] - a[kind + "_free"],
                                  "true_peak_delta": b[kind + "_free"] - a[kind + "_min"]}
                return {"samples": samples, "ns_per_call_median": statistics.median(per_call),
                        "ns_per_call_mean": statistics.mean(per_call), "memory_start": before,
                        "memory_end": after, "heap": heap}

            running = monitoring = False
            try:
                report["hello"] = client.hello()
                if "heap-local" not in report["hello"]:
                    raise RuntimeError("firmware requires heap-local")
                report["packages_before"] = [f.payload for f in request("PACKAGES") if f.kind == "PKG"]
                if any("active=1" in p for p in report["packages_before"]):
                    raise RuntimeError("stop the active application before measuring")
                client.subscribe_logs()
                pump(.1)
                for index in range(args.repeat):
                    request("INPUT KEY HOME")
                    pump(.2)
                    running = True
                    row = {"index": index, "launch": window("launch")}
                    request("INPUT KEY HOME")
                    pump(.3)
                    row["steady"] = window("steady")
                    request(f"PACKAGE stop {args.app}")
                    running = False
                    pump(.3)
                    row["stopped"] = memory()
                    report["runs"].append(row)
                    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                    print(json.dumps({"run": index, "launch_ns": row["launch"]["ns_per_call_median"],
                                      "steady_ns": row["steady"]["ns_per_call_median"]}), flush=True)
                report["passed"] = True
            finally:
                if monitoring:
                    request("MEMORY STOP")
                if running:
                    request(f"PACKAGE stop {args.app}")
                request("INPUT KEY HOME")
                (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
