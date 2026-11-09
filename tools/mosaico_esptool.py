#!/usr/bin/env python3
"""ESP-IDF esptool wrapper for Mosaico's native USB reboot-to-download."""
# SPDX-License-Identifier: MIT
import argparse
import os
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports


def preserve_modem_lines_on_close(port):
    """Keep POSIX close from introducing an extra DTR/RTS transition."""
    if os.name == "posix":
        import termios
        settings = termios.tcgetattr(port.fileno())
        settings[2] &= ~termios.HUPCL
        termios.tcsetattr(port.fileno(), termios.TCSANOW, settings)


USB_VID = 0x303A
APP_PRODUCT = "ESP-Mosaico"
ROM_PRODUCT = "ESP32-S31"
MAC_PATTERN = re.compile(r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}")


def is_mosaico(port):
    return port.vid == USB_VID and port.product in (APP_PRODUCT, ROM_PRODUCT)


def select_port(device):
    ports = list(list_ports.comports())
    if device:
        real_device = os.path.realpath(device)
        return next((p for p in ports if os.path.realpath(p.device) == real_device), None)
    matches = [p for p in ports if is_mosaico(p)]
    if len(matches) != 1:
        raise RuntimeError("Specify -p with the target Mosaico port when zero or multiple boards are connected")
    return matches[0]


def request_download(device):
    # Device metadata can appear just before udev finishes assigning permissions.
    time.sleep(0.2)
    port = serial.Serial(port=None, baudrate=1200, timeout=0.5)
    port.dtr = True
    port.rts = False
    port.port = device
    with port:
        preserve_modem_lines_on_close(port)
        time.sleep(0.1)
        port.dtr = False


def wait_for_download(mac, timeout=15):
    deadline = time.monotonic() + timeout
    inaccessible = None
    while time.monotonic() < deadline:
        matches = [p for p in list_ports.comports()
                   if p.vid == USB_VID and p.product == ROM_PRODUCT
                   and (p.serial_number or "").lower() == mac]
        if len(matches) == 1:
            endpoint = matches[0].device
            # Enumeration can precede Linux udev's group/ACL update under load.
            # Do not open the port to probe it: DTR/RTS can change boot mode.
            if os.name != 'posix' or os.access(endpoint, os.R_OK | os.W_OK):
                time.sleep(0.2)
                return endpoint
            inaccessible = endpoint
        time.sleep(0.1)
    if inaccessible:
        raise RuntimeError(f"ROM detected, but serial port permissions are not ready: {inaccessible}")
    raise RuntimeError(f"Timed out waiting for ESP32-S31 download USB with MAC {mac}")


def download_arguments(arguments, device):
    # A second reset can boot Flash again after the software download flag is
    # consumed by ROM. Connect directly to the freshly enumerated downloader.
    result = ["--port", device, "--before", "no-reset"]
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in ("--port", "-p", "--before"):
            index += 2
            continue
        if argument.startswith(("--port=", "-p", "--before=")):
            index += 1
            continue
        result.append(argument)
        index += 1
    return result


def prepare_download(arguments):
    # Offline commands (elf2image, merge-bin) must remain usable without hardware.
    flash_commands = ("write-flash", "erase-flash", "write_flash", "erase_flash")
    if not any(command in arguments for command in flash_commands):
        return arguments
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("-p", "--port")
    parser.add_argument("--before", default="default-reset")
    options, _ = parser.parse_known_args(arguments)
    if options.before != "default-reset":
        return arguments
    port = select_port(options.port or os.environ.get("ESPPORT"))
    if port is None or not is_mosaico(port):
        return arguments
    if port.product == ROM_PRODUCT:
        return download_arguments(arguments, port.device)
    mac = (port.serial_number or "").lower()
    if not MAC_PATTERN.fullmatch(mac):
        raise RuntimeError("Mosaico application USB serial number must contain its MAC address")
    print(f"Mosaico {mac}: requesting USB download on {port.device}", flush=True)
    try:
        request_download(port.device)
    except (OSError, serial.SerialException) as error:
        # USB can disappear before the final control ioctl returns. The same
        # device must still reappear as ROM before esptool may proceed.
        print(f"Mosaico {mac}: USB control disconnected ({error}); waiting for ROM", flush=True)
    device = wait_for_download(mac)
    print(f"Mosaico {mac}: download USB ready on {device}", flush=True)
    return download_arguments(arguments, device)


def main(arguments=None):
    arguments = list(sys.argv[1:] if arguments is None else arguments)
    if arguments and arguments[0] == "esptool":
        arguments.pop(0)
    try:
        arguments = prepare_download(arguments)
        return subprocess.call([sys.executable, "-m", "esptool", *arguments])
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"Mosaico USB download failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
