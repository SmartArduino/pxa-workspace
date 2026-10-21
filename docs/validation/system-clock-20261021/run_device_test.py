#!/usr/bin/env python3
"""Capture a GameKit clock before/after idle lock and a software restart."""
import argparse
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/pxadb"))
from pxadb import PxaDbClient, info_properties, screenshot_capture, write_screenshot

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--board", required=True, choices=["doit-gamekit", "doit-gamekit-1"])
parser.add_argument("--serial", required=True)
parser.add_argument("--elf-sha256", required=True)
args = parser.parse_args()
OUTPUT = Path(__file__).resolve().parent / args.board
OUTPUT.mkdir(exist_ok=True)
os.environ["PXADB_BAUD"] = "2000000"
PORT = args.port
EXPECTED_ELF = args.elf_sha256
report = {"port": PORT, "expected_elf_sha256": EXPECTED_ELF, "captures": []}


def connect():
    client = PxaDbClient(PORT, timeout=5)
    try:
        client.hello()
        info = info_properties(client.request("INFO")[-1].payload)
        assert info["board"] == args.board and info["serial"] == args.serial
        assert info["elf_sha256"] == EXPECTED_ELF, info["elf_sha256"]
    except BaseException:
        client.close()
        raise
    report["device"] = info
    return client


def capture(client, name):
    result = screenshot_capture(client.request("SCREENSHOT AFTER_PRESENT", timeout=15))
    write_screenshot(result, OUTPUT / (name + ".png"))
    sample = {"name": name, "host_monotonic": time.monotonic(), **result.metadata}
    report["captures"].append(sample)
    (OUTPUT / "device-test.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Captured {name}: device_us={sample['device_us']}", flush=True)
    return sample


def wait_boot_seconds(sample, seconds):
    deadline = sample["host_monotonic"] + seconds - int(sample["device_us"]) / 1_000_000
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        print(f"Waiting for uptime {seconds}s: {remaining:.0f}s remaining", flush=True)
        time.sleep(min(20, remaining))


with connect() as client:
    first = capture(client, "live-before-restart")
    wait_boot_seconds(first, 75)
    capture(client, "after-minute")
    wait_boot_seconds(first, 135)
    capture(client, "locked")
    report["restart_host_monotonic"] = time.monotonic()
    client.request("REBOOT")

for attempt in range(15):
    time.sleep(1)
    try:
        client = connect()
        break
    except Exception:
        if attempt == 14:
            raise

with client:
    restored = capture(client, "restored")
    wait_boot_seconds(restored, int(restored["device_us"]) / 1_000_000 + 65)
    capture(client, "restored-ticking")
    client.request("INPUT SWIPE 160 4 160 215 400 16")
    time.sleep(1)
    capture(client, "notification-date")
    client.request("INPUT SWIPE 160 215 160 4 350 16")
    client.request("INPUT CANCEL")

report["capture_sequence_complete"] = True
report["visual_review"] = "pending"
(OUTPUT / "device-test.json").write_text(json.dumps(report, indent=2) + "\n")
print("Clock capture sequence complete; screenshots require visual review.", flush=True)
