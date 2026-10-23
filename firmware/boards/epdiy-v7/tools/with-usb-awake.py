#!/usr/bin/env python3
"""Keep a USB-UART bridge active while running a serial command on Linux."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("provide a command after --")
    tty = Path(args.port).resolve().name
    device = (Path("/sys/class/tty") / tty / "device").resolve()
    usb = next((parent for parent in (device, *device.parents)
                if (parent / "busnum").is_file() and (parent / "devnum").is_file()), None)
    if usb is None:
        parser.error(f"{args.port} is not a Linux USB serial device")
    bus = int((usb / "busnum").read_text())
    address = int((usb / "devnum").read_text())
    node = Path(f"/dev/bus/usb/{bus:03}/{address:03}")
    try:
        # Opening usbfs pins runtime PM without changing system power policy,
        # claiming interfaces, or touching UART DTR/RTS. CLOEXEC keeps it here.
        descriptor = os.open(node, os.O_RDONLY | os.O_CLOEXEC)
    except OSError as error:
        parser.error(f"cannot hold {node} open: {error}")
    try:
        return subprocess.call(command)
    except KeyboardInterrupt:
        return 130
    finally:
        os.close(descriptor)


if __name__ == "__main__":
    sys.exit(main())
