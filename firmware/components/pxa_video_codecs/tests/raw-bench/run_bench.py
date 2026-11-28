#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the benchmark over the Mosaico USB CDC port and save the serial log."""

import argparse
from pathlib import Path
import time

import serial


def run(port: str, output: Path, timeout: float) -> None:
    with serial.Serial(port, 115200, timeout=0.5, exclusive=True) as link:
        with output.open("w", encoding="utf-8", buffering=1) as log:
            deadline = time.monotonic() + timeout
            link.write(b"b")
            pending = b""
            results = 0
            failed = False
            while time.monotonic() < deadline:
                pending += link.read(link.in_waiting or 1)
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    line = raw.decode("utf-8", errors="replace").rstrip("\r")
                    log.write(line + "\n")
                    print(line, flush=True)
                    if line.startswith("RAW_H264 result "):
                        results += 1
                        failed |= not line.endswith("error=0")
                    if line == "RAW_H264 done":
                        if results != 8 or failed:
                            raise SystemExit(f"Incomplete benchmark. Read {output}")
                        return
    raise SystemExit(f"Benchmark timed out. Read {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=480)
    args = parser.parse_args()
    run(args.port, args.output, args.timeout)
