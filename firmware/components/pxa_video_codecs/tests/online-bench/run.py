#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Send volatile Wi-Fi credentials over USB CDC and capture one playback run."""

import argparse
import getpass
import time
from pathlib import Path

import serial


def run(port: str, ssid: str, output: Path, timeout: float) -> None:
    password = getpass.getpass("Wi-Fi password: ").encode()
    name = ssid.encode()
    if not 0 < len(name) <= 32 or len(password) > 63:
        raise SystemExit("Invalid Wi-Fi credential length")
    if any(delimiter in name or delimiter in password for delimiter in (b"\t", b"\n")):
        raise SystemExit("Wi-Fi credentials cannot contain a tab or newline")
    with serial.Serial(port, 115200, timeout=0.5, exclusive=True) as link:
        link.write(name + b"\t" + password + b"\n")
        del password
        deadline = time.monotonic() + timeout
        pending = b""
        summary = set()
        with output.open("w", encoding="utf-8", buffering=1) as log:
            while time.monotonic() < deadline:
                pending += link.read(link.in_waiting or 1)
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    line = raw.decode("utf-8", errors="replace").rstrip("\r")
                    log.write(line + "\n")
                    print(line, flush=True)
                    if line.startswith("NATIVE error ") or line.startswith("NATIVE credentials_invalid"):
                        raise SystemExit(f"Playback failed. Read {output}")
                    if line.startswith(("NATIVE video_result ", "NATIVE audio_result ", "NATIVE net_result ")):
                        summary.add(line.split()[1])
                    if line == "NATIVE done":
                        if summary != {"video_result", "audio_result", "net_result"}:
                            raise SystemExit(f"Incomplete benchmark. Read {output}")
                        return
    raise SystemExit(f"Benchmark timed out. Read {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--ssid", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=420)
    args = parser.parse_args()
    run(args.port, args.ssid, args.output, args.timeout)
