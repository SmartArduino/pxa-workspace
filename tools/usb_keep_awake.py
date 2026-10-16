#!/usr/bin/env python3
"""Keep a native USB device awake while a build/flash command runs.

Holding its usbfs node read-only pins Linux runtime PM without opening CDC,
changing DTR/RTS, or modifying system power settings. Follow re-enumeration at
the same physical port, including application CDC -> ROM CDC transitions.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import threading


def usb_parent(port: str, tty_root: Path = Path('/sys/class/tty')) -> Path | None:
    if not port or not sys.platform.startswith('linux'):
        return None
    device = (tty_root/Path(os.path.realpath(port)).name/'device').resolve()
    return next((p for p in (device, *device.parents) if (p/'idVendor').is_file()), None)


class UsbKeepAwake:
    def __init__(self, device: Path | None, usbfs: Path = Path('/dev/bus/usb')):
        self.device, self.usbfs = device, usbfs
        self.fd: int | None = None
        self.token: tuple[int, int] | None = None
        self.stop = threading.Event()
        self.thread: threading.Thread | None = None

    def update(self) -> None:
        if self.device is None:
            return
        try:
            vendor = int((self.device/'idVendor').read_text().strip(), 16)
            product = int((self.device/'idProduct').read_text().strip(), 16)
            if (vendor, product) not in ((0x303a, 0x0020), (0x303a, 0x4001)):
                self.close_fd()
                return
            token = (int((self.device/'busnum').read_text()),
                     int((self.device/'devnum').read_text()))
            if token == self.token:
                return
            self.close_fd()
            node = self.usbfs/f'{token[0]:03}'/f'{token[1]:03}'
            self.fd = os.open(node, os.O_RDONLY | os.O_CLOEXEC)
            self.token = token
            print(f'USB keep-awake: {node} (read-only; CDC left untouched)', flush=True)
        except (OSError, ValueError):
            # A reset briefly removes sysfs/node entries and udev permissions.
            # Retry without claiming the CDC port or touching another device.
            self.close_fd()

    def close_fd(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        self.token = None

    def watch(self) -> None:
        while not self.stop.wait(.05):
            self.update()

    def __enter__(self) -> UsbKeepAwake:
        self.update()
        if self.device is not None:
            self.thread = threading.Thread(target=self.watch, daemon=True)
            self.thread.start()
        return self

    def __exit__(self, *_: object) -> None:
        self.stop.set()
        if self.thread is not None:
            self.thread.join()
        self.close_fd()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a command is required after --')
    with UsbKeepAwake(usb_parent(args.port)):
        env = {**os.environ, 'PXA_USB_GUARD_ACTIVE': '1'}
        return subprocess.call(command, env=env)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
