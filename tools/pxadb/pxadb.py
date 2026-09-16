#!/usr/bin/env python3
"""PXADB host client for PXA firmware."""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import pathlib
import socket
import shlex
import struct
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass
from typing import Iterable

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - handled by main for an actionable error
    serial = None
    list_ports = None


PROTOCOL = "PXADB1"
PROTOCOL_BINARY = "PXADB2"
ESPRESSIF_USB_VID = 0x303A
BINARY_HEADER = struct.Struct("!IBBI")
BINARY_COMMAND = 1
BINARY_RESPONSE = 2
BINARY_UPLOAD_DATA = 3
BINARY_MAX_PAYLOAD = 1024 * 1024


class PxaDbError(RuntimeError):
    pass


class BinarySocketTransport:
    """PXADB2 length-prefixed transport for local sockets and authenticated TCP."""

    def __init__(self, target: str) -> None:
        if target.startswith("unix:"):
            path = target[len("unix:"):]
            self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            address: str | tuple[str, int] = path
        elif target.startswith("tcp:"):
            address_text = target[len("tcp:"):].split("|", 1)[0]
            host, separator, port_text = address_text.rpartition(":")
            if not separator or not host:
                raise PxaDbError("TCP PXADB target must be HOST:PORT")
            try:
                address = (host, int(port_text))
            except ValueError as error:
                raise PxaDbError("TCP PXADB port must be numeric") from error
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        else:
            raise PxaDbError(f"unsupported PXADB2 transport: {target}")
        self.socket.settimeout(0.2)
        try:
            self.socket.connect(address)
        except OSError as error:
            self.socket.close()
            raise PxaDbError(
                f"unable to connect to PXADB2 service: {target}; "
                "start it with tools/simulator.sh service start"
            ) from error

    def write_frame(self, kind: int, flags: int, sequence: int, payload: bytes) -> None:
        if len(payload) > BINARY_MAX_PAYLOAD:
            raise PxaDbError("PXADB2 payload exceeds the protocol maximum")
        try:
            self.socket.sendall(BINARY_HEADER.pack(len(payload), kind, flags, sequence) + payload)
        except OSError as error:
            raise PxaDbError(f"PXADB2 write failed: {error}") from error

    def read_exact(self, size: int) -> bytes | None:
        output = bytearray()
        while len(output) < size:
            try:
                received = self.socket.recv(size - len(output))
            except socket.timeout:
                return None
            except OSError as error:
                raise PxaDbError(f"PXADB2 read failed: {error}") from error
            if not received:
                raise PxaDbError("PXADB2 peer disconnected")
            output.extend(received)
        return bytes(output)

    def read_frame(self) -> tuple[int, int, int, bytes] | None:
        header = self.read_exact(BINARY_HEADER.size)
        if header is None:
            return None
        payload_size, kind, flags, sequence = BINARY_HEADER.unpack(header)
        if payload_size > BINARY_MAX_PAYLOAD:
            raise PxaDbError("PXADB2 peer sent an oversized payload")
        payload = self.read_exact(payload_size)
        if payload is None:
            return None
        return kind, flags, sequence, payload

    def close(self) -> None:
        self.socket.close()


@dataclass(frozen=True)
class Frame:
    sequence: int
    kind: str
    payload: str


@dataclass(frozen=True)
class Device:
    port: str
    description: str
    info: str


def info_properties(value: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for field in value.split(";"):
        key, separator, item = field.partition("=")
        if separator and key:
            properties[key] = item
    return properties


def file_sha256(source: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with source.open("rb") as input_file:
        while chunk := input_file.read(64 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def decode_payload(value: str) -> str:
    if value == "-":
        return ""
    try:
        return base64.b64decode(value.encode("ascii"), validate=True).decode("utf-8", "replace")
    except (UnicodeError, ValueError) as error:
        raise PxaDbError(f"invalid PXADB payload: {error}") from error


def parse_frame(line: bytes) -> Frame | None:
    try:
        text = line.decode("utf-8", "replace").strip()
    except UnicodeError:
        return None
    fields = text.split(" ", 3)
    if len(fields) != 4 or fields[0] != PROTOCOL:
        return None
    try:
        sequence = int(fields[1])
    except ValueError:
        return None
    try:
        return Frame(sequence, fields[2], decode_payload(fields[3]))
    except PxaDbError:
        # ROM and early boot output share the CDC stream. A coincidental
        # PXADB1-looking text line must not break an otherwise valid session.
        return None


def print_log_frame(frame: Frame) -> None:
    if frame.kind == "LOG":
        timestamp, separator, message = frame.payload.partition("\t")
        print(f"{timestamp:>10} {message}" if separator else frame.payload,
              flush=True)
    elif frame.kind == "DROP":
        print(f"--- PXADB {frame.payload} ---", file=sys.stderr, flush=True)


def print_raw_serial_line(raw: bytes) -> None:
    text = raw.decode("utf-8", "replace").rstrip("\r\n")
    if text:
        print(f"[serial] {text}", file=sys.stderr, flush=True)


class PxaDbClient:
    def __init__(self, port: str, timeout: float = 2.0) -> None:
        self.binary_transport: BinarySocketTransport | None = None
        self.tcp_token = ""
        if port.startswith(("unix:", "tcp:")):
            self.binary_transport = BinarySocketTransport(port)
            if port.startswith("tcp:") and "|" in port:
                self.tcp_token = port.split("|", 1)[1]
            self.timeout = timeout
            self.sequence = 0
            self.pending_frames: list[Frame] = []
            self.log_subscribed = False
            self.device_info = ""
            return
        if serial is None:
            raise PxaDbError("pyserial is required; install with: pip install -e tools/pxadb")
        try:
            baudrate = int(os.environ.get("PXADB_BAUD", "115200"))
        except ValueError:
            raise PxaDbError("PXADB_BAUD must be an integer baud rate")
        self.serial = serial.Serial(
            port,
            baudrate=baudrate,
            timeout=0.2,
            # PXADB commands fit in one small OS write. In pyserial's finite
            # timeout mode, CDC ACM waits for the endpoint to become writable
            # again even after that complete write, which stalls when firmware
            # has no PXADB reader. Non-blocking mode returns the actual count.
            write_timeout=0,
            exclusive=True,
        )
        self.timeout = timeout
        self.sequence = 0
        self.pending_frames: list[Frame] = []
        self.log_subscribed = False
        self.device_info = ""

    def close(self) -> None:
        if self.log_subscribed:
            try:
                self.unsubscribe_logs()
            except (PxaDbError, OSError):
                pass
        if getattr(self, "binary_transport", None) is not None:
            self.binary_transport.close()
            return
        # Do not let the CDC ACM driver's closing_wait delay a failed HELLO by
        # roughly 30 seconds when bytes remain queued for inactive firmware.
        try:
            self.serial.reset_output_buffer()
        except (AttributeError, OSError):
            pass
        self.serial.close()

    def __enter__(self) -> "PxaDbClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _next_sequence(self) -> int:
        self.sequence += 1
        return self.sequence

    def _write_command(self, sequence: int, command: str) -> None:
        if getattr(self, "binary_transport", None) is not None:
            self.binary_transport.write_frame(BINARY_COMMAND, 0, sequence,
                                              command.encode("utf-8"))
            return
        payload = f"{PROTOCOL} {sequence} {command}\n".encode("ascii")
        # Non-blocking serial writes can accept only part of a large upload
        # command; keep writing until the whole line left the host.
        offset = 0
        deadline = time.monotonic() + max(self.timeout, 2.0)
        while offset < len(payload):
            written = self.serial.write(payload[offset:])
            if written:
                offset += written
                continue
            if time.monotonic() >= deadline:
                raise PxaDbError(
                    f"short serial write: expected {len(payload)} bytes, "
                    f"wrote {offset}"
                )
            time.sleep(0.005)

    def request_until(
        self, command: str, terminal_kinds: set[str], timeout: float | None = None
    ) -> list[Frame]:
        request_timeout = self.timeout if timeout is None else timeout
        deadline = time.monotonic() + request_timeout
        sequence = self._next_sequence()
        self._write_command(sequence, command)
        if getattr(self, "binary_transport", None) is not None:
            return self._collect_binary_frames(sequence, terminal_kinds, command,
                                               deadline)
        frames: list[Frame] = []
        while time.monotonic() < deadline:
            raw = self.serial.readline()
            if not raw:
                continue
            frame = parse_frame(raw)
            if frame is None:
                if self.log_subscribed:
                    print_raw_serial_line(raw)
                continue
            if self.log_subscribed and frame.kind in {"LOG", "DROP"}:
                print_log_frame(frame)
                continue
            if frame.sequence != sequence:
                self.pending_frames.append(frame)
                continue
            if frame.kind == "ERR":
                raise PxaDbError(frame.payload or "device rejected the request")
            if frame.kind in terminal_kinds:
                return frames + [frame]
            frames.append(frame)
        operation = command.split(" ", 1)[0]
        raise PxaDbError(
            f"device did not respond to {operation}; verify that PXADB is "
            "enabled and the port is not in use"
        )

    def _collect_binary_frames(self, sequence: int, terminal_kinds: set[str],
                               operation: str, deadline: float) -> list[Frame]:
        assert self.binary_transport is not None
        frames: list[Frame] = []
        while time.monotonic() < deadline:
            raw = self.binary_transport.read_frame()
            if raw is None:
                continue
            kind, _flags, response_sequence, payload = raw
            if kind != BINARY_RESPONSE:
                continue
            response_kind, separator, response_payload = payload.decode(
                "utf-8", "replace").partition("\0")
            if not separator:
                raise PxaDbError("PXADB2 response is malformed")
            frame = Frame(response_sequence, response_kind, response_payload)
            if response_sequence != sequence:
                self.pending_frames.append(frame)
                continue
            if frame.kind == "ERR":
                raise PxaDbError(frame.payload or "device rejected the request")
            if frame.kind in terminal_kinds:
                return frames + [frame]
            frames.append(frame)
        raise PxaDbError(f"PXADB2 peer did not respond to {operation.split(' ', 1)[0]}")

    def send_binary_upload(self, offset: int, payload: bytes) -> list[Frame]:
        if self.binary_transport is None:
            raise PxaDbError("binary upload requires a PXADB2 transport")
        sequence = self._next_sequence()
        self.binary_transport.write_frame(
            BINARY_UPLOAD_DATA, 0, sequence, struct.pack("!Q", offset) + payload
        )
        return self._collect_binary_frames(sequence, {"READY", "OK"},
                                           "UPLOAD_DATA", time.monotonic() + 30.0)

    def request(self, command: str, timeout: float | None = None) -> list[Frame]:
        return self.request_until(command, {"OK"}, timeout)

    def hello(self, attempts: int = 3) -> str:
        error: PxaDbError | None = None
        deadline = time.monotonic() + self.timeout
        for attempt in range(attempts):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            attempt_timeout = remaining / (attempts - attempt)
            try:
                if getattr(self, "binary_transport", None) is not None and self.tcp_token:
                    self.request("AUTH " + self.tcp_token, timeout=attempt_timeout)
                    self.tcp_token = ""
                frames = self.request("HELLO", timeout=attempt_timeout)
                return frames[-1].payload
            except PxaDbError as caught:
                error = caught
                if attempt + 1 < attempts:
                    remaining = max(0.0, deadline - time.monotonic())
                    time.sleep(min(0.01, remaining / 2.0))
        assert error is not None
        raise error

    def subscribe_logs(self) -> None:
        if self.log_subscribed:
            return
        self.log_subscribed = True
        try:
            self.request("LOGSUB")
        except Exception:
            self.log_subscribed = False
            raise

    def unsubscribe_logs(self) -> None:
        if not self.log_subscribed:
            return
        try:
            self.request("LOGUNSUB", timeout=0.5)
        finally:
            self.log_subscribed = False

    def logcat(self, dump_only: bool) -> int:
        self.subscribe_logs()
        last_ping = time.monotonic()
        try:
            while True:
                if getattr(self, "binary_transport", None) is not None:
                    if dump_only:
                        return 0
                    if time.monotonic() - last_ping >= 10.0:
                        self.request("PING")
                        last_ping = time.monotonic()
                    time.sleep(0.1)
                    continue
                raw = self.serial.readline()
                if not raw:
                    if dump_only:
                        return 0
                    if time.monotonic() - last_ping >= 10.0:
                        self.request("PING")
                        last_ping = time.monotonic()
                    continue
                frame = parse_frame(raw)
                if frame is None:
                    print_raw_serial_line(raw)
                    continue
                if frame.kind in {"LOG", "DROP"}:
                    print_log_frame(frame)
                if dump_only and not self.serial.in_waiting:
                    return 0
        except KeyboardInterrupt:
            return 0
        finally:
            try:
                self.unsubscribe_logs()
            except (PxaDbError, serial.SerialException):
                pass


def open_client(port: str, timeout: float, stream_logs: bool = False) -> PxaDbClient:
    client = PxaDbClient(port, timeout)
    try:
        client.device_info = client.hello()
        if stream_logs:
            client.subscribe_logs()
    except Exception:
        client.close()
        raise
    return client


def candidate_ports():
    if list_ports is None:
        raise PxaDbError("pyserial is required; install with: pip install -e tools/pxadb")
    ports = sorted(list_ports.comports(), key=lambda candidate: candidate.device)
    espressif_ports = [port for port in ports if port.vid == ESPRESSIF_USB_VID]
    if espressif_ports:
        return espressif_ports
    usb_ports = [port for port in ports if port.vid is not None]
    return usb_ports or ports


def discover_devices(timeout: float) -> list[Device]:
    devices: list[Device] = []
    for port in candidate_ports():
        try:
            with open_client(port.device, timeout) as client:
                devices.append(Device(port.device, port.description, client.device_info))
        except (PxaDbError, serial.SerialException, OSError):
            continue
    return devices


def discover_simulators(timeout: float) -> list[Device]:
    socket_root = pathlib.Path(os.environ.get(
        "PXA_SIMULATOR_SOCKET_ROOT", f"/tmp/pxa-simulator-{os.getuid()}"
    ))
    if not socket_root.is_dir():
        return []
    devices: list[Device] = []
    for path in sorted(socket_root.glob("*.sock")):
        profile = path.stem
        try:
            if simulator_socket(profile) != f"unix:{path}":
                continue
            with open_client(f"unix:{path}", timeout) as client:
                properties = info_properties(client.device_info)
                if properties.get("simulator") != "1":
                    continue
                devices.append(Device(f"simulator:{profile}", "desktop simulator",
                                      client.device_info))
        except (argparse.ArgumentTypeError, PxaDbError, OSError):
            continue
    return devices


def resolve_pxadb_port(port: str | None, timeout: float) -> str:
    if port:
        return port

    simulators = discover_simulators(timeout)
    try:
        usb_devices = discover_devices(timeout)
    except PxaDbError:
        usb_devices = []
    endpoints = simulators + usb_devices
    if len(endpoints) == 1:
        endpoint = endpoints[0]
        if endpoint.port.startswith("simulator:"):
            return simulator_socket(endpoint.port.removeprefix("simulator:"))
        return endpoint.port
    if len(endpoints) > 1:
        choices = "\n".join(f"  {device.port}\t{device.info}" for device in endpoints)
        raise PxaDbError(
            "multiple PXADB endpoints found; use --port or --simulator:\n"
            f"{choices}"
        )

    ports = candidate_ports()
    if len(ports) == 1:
        return ports[0].device
    if not ports:
        raise PxaDbError(
            "no PXADB USB device found; connect the device and enable PXADB mode"
        )
    if not usb_devices:
        choices = "\n".join(f"  {candidate.device}\t{candidate.description}" for candidate in ports)
        raise PxaDbError(f"multiple USB devices found; use --port:\n{choices}")
    choices = "\n".join(f"  {device.port}\t{device.info}" for device in usb_devices)
    raise PxaDbError(f"multiple PXADB devices found; use --port:\n{choices}")


def simulator_socket(profile: str) -> str:
    profile_name, separator, instance_name = profile.partition("@")
    if (not profile_name or len(profile) > 96 or profile.count("@") > 1 or
            any(character not in "abcdefghijklmnopqrstuvwxyz0123456789-"
                for character in profile_name) or
            (separator and (not instance_name or any(
                character not in "abcdefghijklmnopqrstuvwxyz0123456789-"
                for character in instance_name)))):
        raise argparse.ArgumentTypeError(
            "simulator selector must be PROFILE or PROFILE@INSTANCE; names use lowercase letters, digits or hyphens"
        )
    socket_root = pathlib.Path(os.environ.get(
        "PXA_SIMULATOR_SOCKET_ROOT", f"/tmp/pxa-simulator-{os.getuid()}"
    ))
    return f"unix:{socket_root / (profile + '.sock')}"


def command_devices(arguments: argparse.Namespace) -> int:
    devices = discover_simulators(arguments.timeout)
    try:
        devices.extend(discover_devices(arguments.timeout))
    except PxaDbError:
        pass
    print("List of devices attached")
    for device in devices:
        print(f"{device.port}\tdevice\t{device.info}")
    return 0


def command_info(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        frames = client.request("INFO")
        print(frames[-1].payload)
    return 0


def command_doctor(arguments: argparse.Namespace) -> int:
    port = resolve_pxadb_port(arguments.port, arguments.timeout)
    with open_client(port, arguments.timeout) as client:
        info = client.request("INFO")[-1].payload
        hello = client.device_info
    properties = info_properties(hello)
    print(f"port={port}")
    print(f"hello={hello}")
    print(f"info={info}")
    print(f"upload_resume={'yes' if properties.get('fs_offset') == '1' else 'no'}")
    print(f"max_chunk={properties.get('max_chunk', 'legacy=192')}")
    return 0


def command_logcat(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout), arguments.timeout) as client:
        return client.logcat(arguments.dump)


def command_package_list(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        frames = client.request("PACKAGES")
    packages = [frame.payload.split("\t", 3) for frame in frames if frame.kind == "PKG"]
    if not packages:
        print("No PXA packages")
        return 0
    print("ID\tNAME\tVERSION\tSTATE")
    for package in packages:
        print("\t".join(package))
    return 0


def command_package_action(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        client.request(f"PACKAGE {arguments.package_action} {arguments.identity}")
    print(f"{arguments.package_action}: {arguments.identity}")
    return 0


def package_source(package: str, identity_override: str | None) -> tuple[pathlib.Path, str]:
    source = pathlib.Path(package).resolve()
    is_directory_package = (
        source.is_dir()
        and (source / "manifest.pxm").is_file()
        and (source / "signature.pxs").is_file()
    )
    is_container = source.is_file() and source.suffix == ".pxa"
    if not is_directory_package and not is_container:
        raise PxaDbError("package must be a .pxa file or a signed Package directory")
    identity = identity_override or (source.stem if is_container else source.name)
    if (
        not identity.startswith("pxa-")
        or len(identity) > 64
        or any(not (character.isascii() and (character.islower() or character.isdigit() or character in ".-_"))
               for character in identity)
    ):
        raise PxaDbError(
            "package identity must start with pxa- and contain only lowercase ASCII letters, digits, '.', '_' or '-'"
        )
    return source, identity


class NormalFsClient:
    def __init__(self, client: PxaDbClient) -> None:
        self.client = client

    def close(self) -> None:
        self.client.close()

    def list(self, remote_path: str) -> list[tuple[str, int, str]]:
        frames = self.client.request(f"FSLIST {client_encode_path(remote_path)}", timeout=10.0)
        entries: list[tuple[str, int, str]] = []
        for frame in frames:
            if frame.kind != "ENTRY":
                continue
            kind, size, name = frame.payload.split("\t", 2)
            entries.append((kind, int(size), name))
        return entries

    def get(self, remote_path: str, destination: pathlib.Path) -> None:
        frames = self.client.request(f"FSGET {client_encode_path(remote_path)}", timeout=120.0)
        with destination.open("wb") as output:
            for frame in frames:
                if frame.kind == "DATA":
                    try:
                        output.write(base64.b64decode(frame.payload.encode("ascii"), validate=True))
                    except ValueError as error:
                        raise PxaDbError(f"invalid file data from device: {error}") from error

    def put(self, source: pathlib.Path, remote_path: str) -> None:
        total = source.stat().st_size
        digest = file_sha256(source)
        properties = info_properties(self.client.device_info)
        offset_transfer = properties.get("fs_offset") == "1"
        binary_transfer = getattr(self.client, "binary_transport", None) is not None
        try:
            chunk_size = int(properties.get("max_chunk", "192"))
        except ValueError:
            chunk_size = 192
        if binary_transfer:
            chunk_size = min(64 * 1024, chunk_size)
        if chunk_size <= 0:
            chunk_size = 192
        last_progress = -1
        recoveries = 0

        def begin_upload() -> Frame:
            nonlocal recoveries
            while True:
                try:
                    frames = self.client.request_until(
                        f"FSPUT {client_encode_path(remote_path)} {total} {digest}",
                        {"READY", "OK"},
                        timeout=10.0,
                    )
                    return frames[-1]
                except PxaDbError:
                    recoveries += 1
                    if recoveries >= 3:
                        raise
                    time.sleep(0.1)

        print(f"uploading {source} -> PXA storage/{remote_path} ({total} bytes)",
              file=sys.stderr, flush=True)
        terminal = begin_upload()
        with source.open("rb") as input_file:
            while terminal.kind == "READY":
                try:
                    remaining = int(terminal.payload)
                except ValueError as error:
                    raise PxaDbError("invalid upload state from device") from error
                if remaining < 0 or remaining > total:
                    raise PxaDbError("device reported an invalid upload offset")
                offset = total - remaining
                input_file.seek(offset)
                chunk = input_file.read(min(chunk_size, remaining))
                if not chunk:
                    raise PxaDbError("device requested more data than the source contains")
                encoded = base64.b64encode(chunk).decode("ascii")
                try:
                    if binary_transfer:
                        frames = self.client.send_binary_upload(offset, chunk)
                    else:
                        command = (f"FSDATA {offset} {encoded}" if offset_transfer
                                   else f"FSDATA {encoded}")
                        frames = self.client.request_until(
                            command, {"READY", "OK"}, timeout=30.0
                        )
                    terminal = frames[-1]
                except PxaDbError:
                    # The digest identifies an upload. A new device resumes at
                    # its confirmed offset; legacy firmware restarts safely.
                    terminal = begin_upload()
                    continue
                progress = 100 if total == 0 else (offset + len(chunk)) * 100 // total
                progress = min(100, (progress // 10) * 10)
                if progress > last_progress:
                    print(f"upload {progress}%", file=sys.stderr, flush=True)
                    last_progress = progress
        if terminal.kind != "OK":
            raise PxaDbError("file upload did not complete")
        if last_progress < 100:
            print("upload 100%", file=sys.stderr, flush=True)

    def mkdir(self, remote_path: str) -> None:
        self.client.request(f"FSMKDIR {client_encode_path(remote_path)}", timeout=10.0)

    def remove(self, remote_path: str) -> None:
        self.client.request(f"FSRM {client_encode_path(remote_path)}", timeout=30.0)


def open_file_client(port: str | None, timeout: float,
                     stream_logs: bool = False):
    return NormalFsClient(open_client(resolve_pxadb_port(port, timeout), timeout,
                                      stream_logs))


def fs_mkdir(client: NormalFsClient, remote_path: str) -> None:
    client.mkdir(remote_path)


def fs_remove(client: NormalFsClient, remote_path: str, timeout: float) -> None:
    client.remove(remote_path)


def command_fs(arguments: argparse.Namespace) -> int:
    client = open_file_client(arguments.port, arguments.timeout,
                              arguments.logcat)
    try:
        if arguments.fs_action == "ls":
            for kind, size, name in client.list(arguments.path):
                print(f"{kind}\t{size}\t{name}")
        elif arguments.fs_action == "push":
            client.put(pathlib.Path(arguments.local), arguments.remote)
        elif arguments.fs_action == "pull":
            client.get(arguments.remote, pathlib.Path(arguments.local))
        elif arguments.fs_action == "rm":
            fs_remove(client, arguments.remote, arguments.timeout)
        elif arguments.fs_action == "mkdir":
            fs_mkdir(client, arguments.remote)
    finally:
        client.close()
    return 0


def client_encode_path(path: str) -> str:
    normalized = path.strip("/") or "."
    if normalized.startswith("../") or "/../" in normalized or normalized == "..":
        raise PxaDbError("path must remain inside PXA storage")
    return base64.b64encode(normalized.encode("utf-8")).decode("ascii")


def iter_package_files(directory: pathlib.Path) -> Iterable[pathlib.Path]:
    for path in sorted(directory.rglob("*")):
        if path.is_file():
            yield path


def stage_package(source: pathlib.Path, identity: str, port: str | None,
                  timeout: float, stream_logs: bool = False) -> None:
    inbox = "pxa-state/inbox"
    root = f"{inbox}/{identity}"
    client = open_file_client(port, timeout, stream_logs)
    try:
        for directory in ("pxa-state", inbox):
            try:
                fs_mkdir(client, directory)
            except Exception:
                pass
        if source.is_file():
            remote = f"{root}.pxa"
            for stale in (root, remote):
                try:
                    fs_remove(client, stale, timeout)
                except Exception:
                    pass
            client.put(source, remote)
            return
        try:
            fs_remove(client, root, timeout)
        except Exception:
            pass
        current = ""
        for segment in root.split("/"):
            current = segment if not current else f"{current}/{segment}"
            try:
                fs_mkdir(client, current)
            except Exception:
                pass
        directories = sorted({path.parent for path in iter_package_files(source) if path.parent != source})
        for directory in directories:
            relative = directory.relative_to(source).as_posix()
            try:
                fs_mkdir(client, f"{root}/{relative}")
            except Exception:
                pass
        for path in iter_package_files(source):
            relative = path.relative_to(source).as_posix()
            client.put(path, f"{root}/{relative}")
    finally:
        client.close()


def command_package_stage(arguments: argparse.Namespace) -> int:
    source, identity = package_source(arguments.directory, arguments.identity)
    stage_package(source, identity, arguments.port, arguments.timeout,
                  arguments.logcat)
    print(f"staged {identity}; run: pxadb package install {identity}")
    return 0


def command_package_install(arguments: argparse.Namespace) -> int:
    def deploy(client: PxaDbClient, identity: str) -> None:
        packages = client.request("PACKAGES")
        installed = any(
            frame.kind == "PKG"
            and frame.payload.split("\t", 1)[0] == identity
            and "installed=1" in frame.payload.rsplit("\t", 1)[-1].split(";")
            for frame in packages
        )
        if installed and not arguments.yes:
            if not sys.stdin.isatty():
                raise PxaDbError(
                    "replacing an installed package requires --yes; the file may be a downgrade"
                )
            answer = input(
                "This may downgrade the app or roll back its signer; newer private data may be incompatible. Continue? [y/N] "
            )
            if answer.strip().lower() not in {"y", "yes"}:
                raise PxaDbError("installation cancelled")
        client.request(f"PACKAGE deploy {identity}", timeout=60.0)

    source = pathlib.Path(arguments.package)
    if source.is_dir() or source.is_file():
        local_package, identity = package_source(arguments.package, arguments.identity)
        print(f"staging: {identity}", file=sys.stderr, flush=True)
        stage_package(local_package, identity, arguments.port,
                      arguments.timeout, arguments.logcat)
        print(f"verifying and installing: {identity}", file=sys.stderr,
              flush=True)
        with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                         arguments.timeout, arguments.logcat) as client:
            deploy(client, identity)
        print(f"installed: {identity}")
        return 0

    if arguments.identity:
        raise PxaDbError("--identity is only valid when installing a local package")
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        deploy(client, arguments.package)
    print(f"install: {arguments.package}")
    return 0


def command_package_run(arguments: argparse.Namespace) -> int:
    port = resolve_pxadb_port(arguments.port, arguments.timeout)
    if not port.startswith("unix:"):
        raise PxaDbError(
            "package run is only available for a local simulator; "
            "use tools/simulator.sh product run for a package directory"
        )
    socket_path = pathlib.Path(port.removeprefix("unix:"))
    selector = socket_path.stem
    if simulator_socket(selector) != port:
        raise PxaDbError("package run requires a simulator socket selected with --simulator")
    profile, separator, instance = selector.partition("@")
    simulator_script = pathlib.Path(__file__).resolve().parents[1] / "simulator.sh"
    if not simulator_script.is_file():
        raise PxaDbError(f"simulator launcher is unavailable: {simulator_script}")
    command = [str(simulator_script), "product", "run", "--profile", profile]
    if separator:
        command.extend(["--instance", instance])
    command.extend(["--installed", arguments.identity])
    return subprocess.run(command, check=False).returncode


def command_reboot(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        client.request("REBOOT")
    return 0


def print_input_ack(frames: list[Frame]) -> None:
    terminal = next((frame for frame in reversed(frames) if frame.kind == "OK"), None)
    if terminal is not None and terminal.payload:
        print(terminal.payload)


def command_input_capabilities(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        print_input_ack(client.request("INPUT CAPABILITIES"))
    return 0


def command_input_pointer(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        frames = client.request(
            f"INPUT POINTER {arguments.action.upper()} {arguments.x} "
            f"{arguments.y} {arguments.pointer_id}"
        )
        print_input_ack(frames)
    return 0


def command_input_tap(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        print_input_ack(client.request(f"INPUT TAP {arguments.x} {arguments.y}"))
    return 0


def command_input_swipe(arguments: argparse.Namespace) -> int:
    request_timeout = max(arguments.timeout, arguments.duration_ms / 1000.0 + 2.0)
    with open_client(resolve_pxadb_port(arguments.port, request_timeout),
                     request_timeout, arguments.logcat) as client:
        frames = client.request(
            f"INPUT SWIPE {arguments.x1} {arguments.y1} {arguments.x2} "
            f"{arguments.y2} {arguments.duration_ms} {arguments.steps}",
            timeout=request_timeout,
        )
        print_input_ack(frames)
    return 0


def command_input_key(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        print_input_ack(client.request(f"INPUT KEY {arguments.key.upper()}"))
    return 0


def command_input_control(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        print_input_ack(client.request(f"INPUT {arguments.input_action.upper()}"))
    return 0


def command_sync(arguments: argparse.Namespace) -> int:
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        print_input_ack(client.request("INPUT SYNC"))
    return 0


@dataclass(frozen=True)
class Screenshot:
    data: bytes
    metadata: dict[str, str]


def screenshot_capture(frames: list[Frame]) -> Screenshot:
    metadata: dict[str, str] = {}
    output = bytearray()
    for frame in frames:
        if frame.kind == "META":
            metadata = info_properties(frame.payload)
            continue
        if frame.kind != "DATA":
            continue
        encoded = frame.payload
        offset_text, separator, chunk = frame.payload.partition("\t")
        if separator:
            try:
                offset = int(offset_text)
            except ValueError as error:
                raise PxaDbError("invalid screenshot chunk offset") from error
            if offset != len(output):
                raise PxaDbError(
                    f"screenshot chunk offset mismatch: expected {len(output)}, got {offset}"
                )
            encoded = chunk
        try:
            output.extend(base64.b64decode(encoded.encode("ascii"), validate=True))
        except ValueError as error:
            raise PxaDbError(f"invalid screenshot data from device: {error}") from error
    if not output:
        raise PxaDbError("device returned an empty screenshot")
    expected_size = metadata.get("bytes")
    if expected_size is not None:
        try:
            parsed_size = int(expected_size)
        except ValueError as error:
            raise PxaDbError("invalid screenshot size metadata") from error
        if parsed_size != len(output):
            raise PxaDbError(
                f"screenshot size mismatch: expected {expected_size}, got {len(output)}"
            )
    expected_hash = metadata.get("sha256")
    actual = hashlib.sha256(output).hexdigest()
    if expected_hash is not None and expected_hash.lower() != actual:
        raise PxaDbError("screenshot SHA-256 mismatch")
    return Screenshot(bytes(output), metadata)


def screenshot_bytes(frames: list[Frame]) -> bytes:
    return screenshot_capture(frames).data


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def rgb565le_to_png(data: bytes, width: int, height: int, stride: int) -> bytes:
    if width <= 0 or height <= 0 or stride < width * 2 or len(data) < stride * height:
        raise PxaDbError("invalid RGB565 screenshot dimensions")
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        row = data[y * stride:y * stride + width * 2]
        for x in range(width):
            value = row[x * 2] | (row[x * 2 + 1] << 8)
            red = (value >> 11) & 0x1F
            green = (value >> 5) & 0x3F
            blue = value & 0x1F
            scanlines.extend(((red << 3) | (red >> 2),
                              (green << 2) | (green >> 4),
                              (blue << 3) | (blue >> 2)))
    signature = b"\x89PNG\r\n\x1a\n"
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (signature + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", zlib.compress(bytes(scanlines), 6)) +
            png_chunk(b"IEND", b""))


def write_screenshot(capture: Screenshot, destination: pathlib.Path,
                     raw: bool = False) -> None:
    pixel_format = capture.metadata.get("format", "jpeg")
    if pixel_format == "rgb565le" and not raw:
        try:
            width = int(capture.metadata["width"])
            height = int(capture.metadata["height"])
            stride = int(capture.metadata.get("stride", width * 2))
        except (KeyError, ValueError) as error:
            raise PxaDbError("incomplete RGB565 screenshot metadata") from error
        destination.write_bytes(rgb565le_to_png(capture.data, width, height, stride))
    else:
        destination.write_bytes(capture.data)


def command_screenshot(arguments: argparse.Namespace) -> int:
    request = "SCREENSHOT AFTER_PRESENT" if arguments.after_present else "SCREENSHOT"
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        frames = client.request(request, timeout=120.0)
    capture = screenshot_capture(frames)
    destination = pathlib.Path(arguments.output)
    write_screenshot(capture, destination, arguments.raw)
    details = ";".join(
        f"{key}={value}" for key, value in capture.metadata.items()
        if key in {"width", "height", "frame_id", "device_us", "source"}
    )
    print(f"saved screenshot: {destination}" + (f" ({details})" if details else ""))
    return 0


def run_device_scenario(client: PxaDbClient, script: pathlib.Path) -> None:
    for line_number, source_line in enumerate(script.read_text().splitlines(), 1):
        try:
            arguments = shlex.split(source_line, comments=True)
        except ValueError as error:
            raise PxaDbError(f"{script}:{line_number}: {error}") from error
        if not arguments:
            continue
        command = arguments[0].lower()
        try:
            if command == "wait" and len(arguments) == 2:
                time.sleep(int(arguments[1]) / 1000.0)
            elif command == "tap" and len(arguments) == 3:
                client.request(f"INPUT TAP {arguments[1]} {arguments[2]}")
            elif command == "swipe" and len(arguments) == 6:
                duration = int(arguments[5])
                client.request(
                    "INPUT SWIPE " + " ".join(arguments[1:]) + " 12",
                    timeout=max(client.timeout, duration / 1000.0 + 2.0),
                )
            elif command == "pointer" and len(arguments) == 4:
                client.request(
                    f"INPUT POINTER {arguments[1].upper()} "
                    f"{arguments[2]} {arguments[3]} 0"
                )
            elif command == "key" and len(arguments) == 2:
                client.request(f"INPUT KEY {arguments[1].upper()}")
            elif command == "sync" and len(arguments) == 1:
                client.request("INPUT SYNC")
            elif command == "screenshot" and len(arguments) == 2:
                capture = screenshot_capture(
                    client.request("SCREENSHOT", timeout=120.0)
                )
                destination = pathlib.Path(arguments[1])
                write_screenshot(capture, destination,
                                 destination.suffix.lower() == ".rgb565")
            else:
                raise ValueError("unsupported command or argument count")
        except (PxaDbError, OSError, ValueError) as error:
            try:
                client.request("INPUT CANCEL", timeout=1.0)
            except (PxaDbError, OSError):
                pass
            raise PxaDbError(f"{script}:{line_number}: {error}") from error


def end_control_session(client: PxaDbClient) -> None:
    for command in ("INPUT CANCEL", "BYE"):
        try:
            client.request(command, timeout=1.0)
        except (PxaDbError, OSError):
            pass


def command_run(arguments: argparse.Namespace) -> int:
    script = pathlib.Path(arguments.script).resolve()
    if not script.is_file():
        raise PxaDbError(f"scenario does not exist: {script}")
    with open_client(resolve_pxadb_port(arguments.port, arguments.timeout),
                     arguments.timeout, arguments.logcat) as client:
        try:
            run_device_scenario(client, script)
        finally:
            end_control_session(client)
    return 0


def add_connection_arguments(parser: argparse.ArgumentParser,
                             allow_logcat: bool = True) -> None:
    try:
        default_baud = int(os.environ.get("PXADB_BAUD", "115200"))
    except ValueError:
        default_baud = 115200
    parser.add_argument(
        "--port",
        default=os.environ.get("PXADB_PORT"),
        help="USB Serial/JTAG port; auto-detects one PXADB device when omitted",
    )
    parser.add_argument(
        "--baud", type=int, default=default_baud,
        help="serial baud rate for UART transports (default 115200, "
             "override with PXADB_BAUD); ignored by USB Serial/JTAG",
    )
    parser.add_argument(
        "--simulator", dest="port", type=simulator_socket, metavar="PROFILE[@INSTANCE]",
        help="connect to the local simulator PXADB service for PROFILE or PROFILE@INSTANCE",
    )
    parser.add_argument("--connect", metavar="HOST:PORT",
                        help="connect to an authenticated PXADB2 TCP service")
    parser.add_argument("--token", metavar="FILE",
                        help="read the PXADB2 TCP authentication token from FILE")
    parser.add_argument("--timeout", type=float, default=2.0, help="request timeout in seconds")
    if allow_logcat:
        parser.add_argument(
            "--logcat", action="store_true",
            help="stream structured device logs while the operation runs",
        )
    else:
        parser.set_defaults(logcat=False)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="pxadb", description="PXADB development client")
    subcommands = parser.add_subparsers(dest="command", required=True)

    devices = subcommands.add_parser("devices", help="list PXADB devices")
    devices.add_argument("--timeout", type=float, default=0.6, help="probe timeout in seconds")
    devices.set_defaults(handler=command_devices)

    info = subcommands.add_parser("info", help="print device information")
    add_connection_arguments(info)
    info.set_defaults(handler=command_info)

    doctor = subcommands.add_parser("doctor", help="diagnose a PXADB connection")
    add_connection_arguments(doctor, allow_logcat=False)
    doctor.set_defaults(handler=command_doctor)

    logcat = subcommands.add_parser("logcat", help="stream structured ESP logs")
    add_connection_arguments(logcat, allow_logcat=False)
    logcat.add_argument("--dump", action="store_true", help="drain currently buffered logs and exit")
    logcat.set_defaults(handler=command_logcat)

    reboot = subcommands.add_parser("reboot", help="restart normal firmware")
    add_connection_arguments(reboot)
    reboot.set_defaults(handler=command_reboot)

    sync = subcommands.add_parser(
        "sync", help="wait until all earlier injected input entered the router"
    )
    add_connection_arguments(sync)
    sync.set_defaults(handler=command_sync)

    run = subcommands.add_parser("run", help="run one .pxauto scenario on a device")
    add_connection_arguments(run)
    run.add_argument("script")
    run.set_defaults(handler=command_run)

    screenshot = subcommands.add_parser(
        "screenshot", help="capture the final visible display frame"
    )
    add_connection_arguments(screenshot)
    screenshot.add_argument("output", nargs="?", default="screenshot.png")
    screenshot.add_argument(
        "--after-present", action="store_true",
        help="wait for a frame completed after the request",
    )
    screenshot.add_argument(
        "--raw", action="store_true",
        help="write logical little-endian RGB565 instead of PNG",
    )
    screenshot.set_defaults(handler=command_screenshot)

    input_command = subcommands.add_parser(
        "input", help="inject logical input through the system router"
    )
    input_commands = input_command.add_subparsers(dest="input_action", required=True)
    input_capabilities = input_commands.add_parser(
        "capabilities", help="show supported coordinates, pointers and keys"
    )
    add_connection_arguments(input_capabilities)
    input_capabilities.set_defaults(handler=command_input_capabilities)
    input_pointer = input_commands.add_parser(
        "pointer", aliases=["touch"], help="inject one pointer edge/sample"
    )
    add_connection_arguments(input_pointer)
    input_pointer.add_argument("action", choices=("down", "move", "up", "cancel"))
    input_pointer.add_argument("x", type=int)
    input_pointer.add_argument("y", type=int)
    input_pointer.add_argument("--id", dest="pointer_id", type=int, default=0)
    input_pointer.set_defaults(handler=command_input_pointer)
    input_tap = input_commands.add_parser("tap", help="perform a device-timed tap")
    add_connection_arguments(input_tap)
    input_tap.add_argument("x", type=int)
    input_tap.add_argument("y", type=int)
    input_tap.set_defaults(handler=command_input_tap)
    input_swipe = input_commands.add_parser(
        "swipe", help="perform a device-timed swipe"
    )
    add_connection_arguments(input_swipe)
    input_swipe.add_argument("x1", type=int)
    input_swipe.add_argument("y1", type=int)
    input_swipe.add_argument("x2", type=int)
    input_swipe.add_argument("y2", type=int)
    input_swipe.add_argument("--duration-ms", type=int, default=300)
    input_swipe.add_argument("--steps", type=int, default=12)
    input_swipe.set_defaults(handler=command_input_swipe)
    input_key = input_commands.add_parser("key", help="inject a system key")
    add_connection_arguments(input_key)
    input_key.add_argument(
        "key", choices=("back", "home", "volume-up", "volume-down")
    )
    input_key.set_defaults(handler=command_input_key)
    for action in ("cancel", "sync"):
        input_control = input_commands.add_parser(action)
        add_connection_arguments(input_control)
        input_control.set_defaults(handler=command_input_control)

    package = subcommands.add_parser("package", help="manage signed PXA packages")
    package_commands = package.add_subparsers(dest="package_command", required=True)
    package_list = package_commands.add_parser("list", help="list package state")
    add_connection_arguments(package_list)
    package_list.set_defaults(handler=command_package_list)
    install = package_commands.add_parser(
        "install", help="install a staged package ID or a local .pxa Package"
    )
    add_connection_arguments(install)
    install.add_argument(
        "package", help="package ID, .pxa file, or signed Package directory"
    )
    install.add_argument(
        "--identity", help="PXA app ID override when installing a local package"
    )
    install.add_argument(
        "--yes", action="store_true",
        help="confirm replacement or downgrade without an interactive prompt"
    )
    install.set_defaults(handler=command_package_install)
    package_run = package_commands.add_parser(
        "run", help="launch an installed package in the local product simulator"
    )
    add_connection_arguments(package_run, allow_logcat=False)
    package_run.add_argument("identity", help="installed PXA app ID")
    package_run.set_defaults(handler=command_package_run)
    for action in ("uninstall", "enable", "disable", "clear-data"):
        action_parser = package_commands.add_parser(action, help=f"{action} a PXA package")
        add_connection_arguments(action_parser)
        action_parser.add_argument("identity")
        action_parser.set_defaults(handler=command_package_action, package_action=action)
    stage = package_commands.add_parser("stage", help="stage a signed package through PXADB")
    add_connection_arguments(stage)
    stage.add_argument("directory", help=".pxa file or signed Package directory")
    stage.add_argument("--identity", help="PXA app id; defaults to the package filename")
    stage.set_defaults(handler=command_package_stage)

    filesystem = subcommands.add_parser("fs", help="manage files through PXADB")
    fs_commands = filesystem.add_subparsers(dest="fs_action", required=True)
    fs_ls = fs_commands.add_parser("ls")
    add_connection_arguments(fs_ls)
    fs_ls.add_argument("path", nargs="?", default=".")
    fs_ls.set_defaults(handler=command_fs)
    fs_push = fs_commands.add_parser("push")
    add_connection_arguments(fs_push)
    fs_push.add_argument("local")
    fs_push.add_argument("remote")
    fs_push.set_defaults(handler=command_fs)
    fs_pull = fs_commands.add_parser("pull")
    add_connection_arguments(fs_pull)
    fs_pull.add_argument("remote")
    fs_pull.add_argument("local")
    fs_pull.set_defaults(handler=command_fs)
    fs_rm = fs_commands.add_parser("rm")
    add_connection_arguments(fs_rm)
    fs_rm.add_argument("remote")
    fs_rm.set_defaults(handler=command_fs)
    fs_mkdir = fs_commands.add_parser("mkdir")
    add_connection_arguments(fs_mkdir)
    fs_mkdir.add_argument("remote")
    fs_mkdir.set_defaults(handler=command_fs)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    arguments = parser.parse_args(argv)
    baud = getattr(arguments, "baud", None)
    if baud is not None:
        if baud <= 0:
            parser.error("--baud must be a positive integer")
        # PxaDbClient reads PXADB_BAUD; keep a single source of truth.
        os.environ["PXADB_BAUD"] = str(baud)
    connect = getattr(arguments, "connect", None)
    token_file = getattr(arguments, "token", None)
    if connect:
        if not token_file:
            parser.error("--connect requires --token FILE")
        try:
            token = pathlib.Path(token_file).read_text(encoding="ascii").strip()
        except OSError as error:
            parser.error(f"unable to read PXADB2 token: {error}")
        if not token or any(character.isspace() for character in token):
            parser.error("PXADB2 token is invalid")
        arguments.port = f"tcp:{connect}|{token}"
    elif token_file:
        parser.error("--token is only valid with --connect")
    try:
        return arguments.handler(arguments)
    except (PxaDbError, OSError, serial.SerialException if serial is not None else RuntimeError) as error:
        print(f"pxadb: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
