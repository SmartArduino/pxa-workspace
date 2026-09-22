#!/usr/bin/env python3
"""PXADB2 binary service for the desktop product simulator."""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import os
import pathlib
import secrets
import shutil
import signal
import socket
import socketserver
import struct
import subprocess
import threading
import time
import zlib


FRAME_HEADER = struct.Struct("!IBBI")
FRAME_COMMAND = 1
FRAME_RESPONSE = 2
FRAME_UPLOAD_DATA = 3
MAX_FRAME_BYTES = 1024 * 1024
MAX_UPLOAD_BYTES = 128 * 1024 * 1024
MAX_CHUNK_BYTES = 64 * 1024
MAX_SCREENSHOT_BYTES = 32 * 1024 * 1024
# One FSREAD response carries this many raw bytes, split into DATA frames.
FS_READ_WINDOW_BYTES = 4 * MAX_CHUNK_BYTES


class ServiceError(RuntimeError):
    pass


def decode_path(value: str) -> str:
    try:
        return base64.b64decode(value.encode("ascii"), validate=True).decode("utf-8")
    except (UnicodeDecodeError, ValueError) as error:
        raise ServiceError("invalid base64 path") from error


def safe_identity(identity: str) -> bool:
    return (identity.startswith("pxa-") and len(identity) <= 64 and
            all(character.isascii() and (character.islower() or character.isdigit()
                or character in ".-_") for character in identity))


def parse_tcp_address(value: str) -> tuple[str, int]:
    host, separator, port_text = value.rpartition(":")
    if not separator or not host:
        raise argparse.ArgumentTypeError("TCP listener must be HOST:PORT")
    try:
        port = int(port_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("TCP listener port must be numeric") from error
    if port < 0 or port > 65535:
        raise argparse.ArgumentTypeError("TCP listener port must be in 0..65535")
    return host, port


def load_or_create_token(path: pathlib.Path) -> str:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        token = path.read_text(encoding="ascii").strip()
    except FileNotFoundError:
        token = secrets.token_urlsafe(32)
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="ascii") as file:
            file.write(token + "\n")
    if not token or any(character.isspace() for character in token):
        raise ServiceError("PXADB2 token file is invalid")
    os.chmod(path, 0o600)
    return token


def write_tcp_address(path: pathlib.Path, address: tuple[str, int]) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.part")
    temporary.write_text(f"{address[0]}:{address[1]}\n", encoding="ascii")
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)


class SimulatorPxaDb:
    def __init__(self, state_root: pathlib.Path, installer: pathlib.Path,
                 publisher_key: pathlib.Path,
                 control_socket: pathlib.Path | None = None) -> None:
        self.state_root = state_root
        self.installer = installer
        self.publisher_key = publisher_key
        self.control_socket = control_socket
        self.lock = threading.Lock()
        self.uploads: dict[int, tuple[pathlib.Path, pathlib.Path, int, str]] = {}
        self.state_root.mkdir(mode=0o700, parents=True, exist_ok=True)

    def control(self, command: str, timeout: float = 5.0) -> bytes:
        if self.control_socket is None:
            raise ServiceError("simulator_control_unavailable")
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.settimeout(timeout)
        try:
            connection.connect(str(self.control_socket))
            connection.sendall(command.encode("ascii") + b"\n")
            response = bytearray()
            while b"\n" not in response:
                part = connection.recv(256)
                if not part:
                    raise ServiceError("simulator_control_disconnected")
                response.extend(part)
                if len(response) > 256:
                    raise ServiceError("simulator_control_response_invalid")
            header, remainder = bytes(response).split(b"\n", 1)
            if header.startswith(b"ERR "):
                raise ServiceError(header[4:].decode("ascii", "replace"))
            if header.startswith(b"PNG "):
                try:
                    size = int(header[4:])
                except ValueError as error:
                    raise ServiceError("simulator_screenshot_invalid") from error
                if size <= 0 or size > MAX_SCREENSHOT_BYTES:
                    raise ServiceError("simulator_screenshot_invalid")
                data = bytearray(remainder)
                while len(data) < size:
                    part = connection.recv(size - len(data))
                    if not part:
                        raise ServiceError("simulator_screenshot_truncated")
                    data.extend(part)
                if len(data) != size:
                    raise ServiceError("simulator_screenshot_invalid")
                return bytes(data)
            if header != b"OK":
                raise ServiceError("simulator_control_response_invalid")
            return bytes(remainder)
        except (OSError, socket.timeout) as error:
            raise ServiceError("simulator_not_running") from error
        finally:
            connection.close()

    @staticmethod
    def coordinate(value: str) -> int:
        try:
            coordinate = int(value)
        except ValueError as error:
            raise ServiceError("invalid_input_coordinate") from error
        if coordinate < 0 or coordinate > 65535:
            raise ServiceError("invalid_input_coordinate")
        return coordinate

    def screenshot(self, connection: "BinaryHandler", sequence: int) -> None:
        png = self.control("SCREENSHOT", timeout=30.0)
        digest = hashlib.sha256(png).hexdigest()
        self.send(connection, sequence, "META",
                  f"format=png;bytes={len(png)};sha256={digest};source=simulator")
        for offset in range(0, len(png), MAX_CHUNK_BYTES):
            data = base64.b64encode(png[offset:offset + MAX_CHUNK_BYTES]).decode("ascii")
            self.send(connection, sequence, "DATA", f"{offset}\t{data}")
        self.send(connection, sequence, "OK")

    def input(self, arguments: list[str]) -> str:
        if not arguments:
            raise ServiceError("invalid_input_command")
        action = arguments[0].upper()
        if action == "CAPABILITIES" and len(arguments) == 1:
            self.control("CAPABILITIES")
            return "pointers=1;keys=back,home,volume-up,volume-down"
        if action == "SYNC" and len(arguments) == 1:
            self.control("SYNC")
            return ""
        if action == "CANCEL" and len(arguments) == 1:
            self.control("POINTER UP 0 0")
            return ""
        if action == "KEY" and len(arguments) == 2:
            key = arguments[1].upper().replace("-", "_")
            if key not in {"BACK", "HOME", "VOLUME_UP", "VOLUME_DOWN"}:
                raise ServiceError("unsupported_input_key")
            self.control(f"KEY {key}")
            return ""
        if action == "POINTER" and len(arguments) == 5:
            phase = arguments[1].upper()
            x = self.coordinate(arguments[2])
            y = self.coordinate(arguments[3])
            try:
                pointer_id = int(arguments[4])
            except ValueError as error:
                raise ServiceError("invalid_pointer_id") from error
            if phase not in {"DOWN", "MOVE", "UP", "CANCEL"} or pointer_id != 0:
                raise ServiceError("unsupported_pointer")
            self.control(f"POINTER {'UP' if phase == 'CANCEL' else phase} {x} {y}")
            return ""
        if action == "TAP" and len(arguments) == 3:
            x = self.coordinate(arguments[1])
            y = self.coordinate(arguments[2])
            self.control(f"TAP {x} {y}")
            return ""
        if action == "SWIPE" and len(arguments) == 7:
            x1 = self.coordinate(arguments[1])
            y1 = self.coordinate(arguments[2])
            x2 = self.coordinate(arguments[3])
            y2 = self.coordinate(arguments[4])
            try:
                duration_ms = int(arguments[5])
                steps = int(arguments[6])
            except ValueError as error:
                raise ServiceError("invalid_swipe_duration") from error
            if duration_ms < 0 or duration_ms > 60000 or steps < 1 or steps > 120:
                raise ServiceError("invalid_swipe_duration")
            self.control(f"POINTER DOWN {x1} {y1}")
            for step in range(1, steps + 1):
                if duration_ms:
                    time.sleep(duration_ms / steps / 1000.0)
                x = x1 + (x2 - x1) * step // steps
                y = y1 + (y2 - y1) * step // steps
                self.control(f"POINTER {'UP' if step == steps else 'MOVE'} {x} {y}")
            return ""
        raise ServiceError("invalid_input_command")

    def storage_path(self, remote: str) -> pathlib.Path:
        normalized = pathlib.PurePosixPath(remote.strip("/"))
        if str(normalized) in {"", "."}:
            return self.state_root
        if (not remote or normalized.is_absolute() or
                ".." in normalized.parts):
            raise ServiceError("path must remain inside simulator PXA storage")
        return self.state_root.joinpath(*normalized.parts)

    def send(self, connection: "BinaryHandler", sequence: int, kind: str,
             payload: str = "") -> None:
        connection.write_frame(FRAME_RESPONSE, sequence,
                               kind.encode("utf-8") + b"\0" + payload.encode("utf-8"))

    def package_list(self, connection: "BinaryHandler", sequence: int) -> None:
        packages = self.state_root / "packages"
        if packages.is_dir():
            for entry in sorted(packages.iterdir()):
                root = entry / "current" if (entry / "current").is_dir() else entry
                if entry.name.startswith(".") or not (root / "manifest.pxm").is_file():
                    continue
                self.send(connection, sequence, "PKG",
                          f"{entry.name}\t{entry.name}\tunknown\t"
                          "builtin=0;installed=1;staged=0;enabled=1;active=0")
        self.send(connection, sequence, "OK")

    def begin_upload(self, connection: "BinaryHandler", sequence: int,
                     arguments: list[str]) -> None:
        if len(arguments) not in {2, 3}:
            raise ServiceError("invalid_file_upload")
        destination = self.storage_path(decode_path(arguments[0]))
        try:
            size = int(arguments[1])
        except ValueError as error:
            raise ServiceError("invalid_file_size") from error
        if size < 0 or size > MAX_UPLOAD_BYTES:
            raise ServiceError("invalid_file_size")
        digest = arguments[2] if len(arguments) == 3 else ""
        if digest and (len(digest) != 64 or
                       any(character not in "0123456789abcdef" for character in digest)):
            raise ServiceError("invalid_file_digest")
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        temporary = destination.with_name(f".{destination.name}.pxadb2-part")
        temporary.unlink(missing_ok=True)
        temporary.touch(mode=0o600)
        self.uploads[id(connection)] = (destination, temporary, size, digest)
        self.send(connection, sequence, "READY", str(size))

    def write_upload(self, connection: "BinaryHandler", sequence: int,
                     payload: bytes) -> None:
        upload_key = id(connection)
        if upload_key not in self.uploads or len(payload) <= 8:
            raise ServiceError("no_file_upload")
        destination, temporary, total, expected_digest = self.uploads[upload_key]
        offset = struct.unpack("!Q", payload[:8])[0]
        data = payload[8:]
        current = temporary.stat().st_size
        if (offset != current or len(data) > MAX_CHUNK_BYTES or
                current + len(data) > total):
            raise ServiceError("invalid_file_offset")
        with temporary.open("ab") as file:
            file.write(data)
        remaining = total - (current + len(data))
        if remaining:
            self.send(connection, sequence, "READY", str(remaining))
            return
        if expected_digest:
            digest = hashlib.sha256(temporary.read_bytes()).hexdigest()
            if digest != expected_digest:
                temporary.unlink(missing_ok=True)
                del self.uploads[upload_key]
                raise ServiceError("file_digest_mismatch")
        os.replace(temporary, destination)
        del self.uploads[upload_key]
        self.send(connection, sequence, "OK")

    def file_list(self, connection: "BinaryHandler", sequence: int,
                  arguments: list[str]) -> None:
        if len(arguments) != 1:
            raise ServiceError("invalid_file_list")
        directory = self.storage_path(decode_path(arguments[0]))
        if not directory.is_dir():
            raise ServiceError("path_not_found")
        for entry in sorted(directory.iterdir(), key=lambda item: item.name):
            try:
                metadata = entry.stat()
            except OSError:
                continue
            kind = "D" if entry.is_dir() else "F"
            size = min(metadata.st_size, 0xFFFFFFFF)
            self.send(connection, sequence, "ENTRY",
                      f"{kind}\t{size}\t{entry.name}")
        self.send(connection, sequence, "OK")

    def file_digest(self, connection: "BinaryHandler", sequence: int,
                    arguments: list[str]) -> None:
        if len(arguments) != 1:
            raise ServiceError("invalid_file_digest")
        path = self.storage_path(decode_path(arguments[0]))
        if not path.is_file():
            raise ServiceError("file_not_found")
        digest = hashlib.sha256()
        size = 0
        try:
            with path.open("rb") as file:
                while chunk := file.read(MAX_CHUNK_BYTES):
                    digest.update(chunk)
                    size += len(chunk)
        except OSError as error:
            raise ServiceError("file_read_failed") from error
        self.send(connection, sequence, "OK",
                  f"sha256={digest.hexdigest()};bytes={size}")

    def file_read(self, connection: "BinaryHandler", sequence: int,
                  arguments: list[str]) -> None:
        if len(arguments) not in {1, 2}:
            raise ServiceError("invalid_file_read")
        path = self.storage_path(decode_path(arguments[0]))
        try:
            offset = int(arguments[1]) if len(arguments) == 2 else 0
        except ValueError as error:
            raise ServiceError("invalid_offset") from error
        if offset < 0:
            raise ServiceError("invalid_offset")
        if not path.is_file():
            raise ServiceError("file_not_found")
        if offset > path.stat().st_size:
            raise ServiceError("invalid_offset")
        try:
            with path.open("rb") as file:
                file.seek(offset)
                window = file.read(FS_READ_WINDOW_BYTES)
        except OSError as error:
            raise ServiceError("file_read_failed") from error
        eof = offset + len(window) >= path.stat().st_size
        for start in range(0, len(window), MAX_CHUNK_BYTES):
            chunk = window[start:start + MAX_CHUNK_BYTES]
            encoded = base64.b64encode(chunk).decode("ascii")
            checksum = zlib.crc32(chunk) & 0xFFFFFFFF
            self.send(connection, sequence, "DATA",
                      f"{offset + start}\t{checksum:08x}\t{encoded}")
        self.send(connection, sequence, "OK", "eof" if eof else "more")

    def file_remove_tree(self, connection: "BinaryHandler", sequence: int,
                         arguments: list[str]) -> None:
        if len(arguments) != 1:
            raise ServiceError("invalid_file_remove")
        path = self.storage_path(decode_path(arguments[0]))
        if path == self.state_root:
            raise ServiceError("invalid_path")
        try:
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
        except FileNotFoundError as error:
            raise ServiceError("file_not_found") from error
        except OSError as error:
            raise ServiceError("file_remove_failed") from error
        self.send(connection, sequence, "OK", "removed")

    def deploy(self, connection: "BinaryHandler", sequence: int, identity: str) -> None:
        if not safe_identity(identity):
            raise ServiceError("invalid_identity")
        inbox = self.state_root / "pxa-state" / "inbox"
        source = inbox / f"{identity}.pxa"
        if not source.is_file():
            source = inbox / identity
        if not source.exists():
            raise ServiceError("staged_package_not_found")
        with self.lock:
            completed = subprocess.run(
                [str(self.installer), "--storage-root", str(self.state_root),
                 "--publisher-key", str(self.publisher_key), "--source", str(source),
                 "--expected-id", identity],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=60, check=False,
            )
        if completed.returncode:
            detail = completed.stderr.strip().replace("\n", ": ")[:240]
            raise ServiceError(f"package_install_failed: {detail or 'unknown error'}")
        if self.control_socket is not None:
            try:
                self.control("CATALOG REFRESH")
            except ServiceError:
                pass
        self.send(connection, sequence, "OK", "installed")

    def dispatch(self, connection: "BinaryHandler", sequence: int,
                 command: str, arguments: list[str]) -> None:
        if command == "HELLO":
            transport = "tcp" if connection.server.require_token else "unix"  # type: ignore[attr-defined]
            self.send(connection, sequence, "OK",
                      f"protocol=2;transport={transport};fs_offset=1;"
                      f"fs_sha256=1;max_chunk={MAX_CHUNK_BYTES};binary=1;"
                      "simulator=1;screenshot=png;input=pointer,key")
        elif command == "INFO":
            self.send(connection, sequence, "OK", "kind=simulator;package_installer=posix;protocol=2;debug_control=1")
        elif command in {"PING", "BYE"}:
            self.send(connection, sequence, "OK", "pong" if command == "PING" else "disconnected")
        elif command in {"LOGSUB", "LOGUNSUB"}:
            self.send(connection, sequence, "OK", "subscribed" if command == "LOGSUB" else "unsubscribed")
        elif command == "PACKAGES":
            self.package_list(connection, sequence)
        elif command == "SCREENSHOT" and len(arguments) in {0, 1} and \
                (not arguments or arguments[0] == "AFTER_PRESENT"):
            self.screenshot(connection, sequence)
        elif command == "INPUT":
            self.send(connection, sequence, "OK", self.input(arguments))
        elif command == "FSPUT":
            self.begin_upload(connection, sequence, arguments)
        elif command == "FSMKDIR" and len(arguments) == 1:
            self.storage_path(decode_path(arguments[0])).mkdir(mode=0o700, parents=True, exist_ok=True)
            self.send(connection, sequence, "OK")
        elif command == "FSRM" and len(arguments) == 1:
            path = self.storage_path(decode_path(arguments[0]))
            if path == self.state_root:
                raise ServiceError("invalid_path")
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink(missing_ok=True)
            self.send(connection, sequence, "OK")
        elif command == "FSLIST" and len(arguments) == 1:
            self.file_list(connection, sequence, arguments)
        elif command == "FSSHA" and len(arguments) == 1:
            self.file_digest(connection, sequence, arguments)
        elif command == "FSREAD" and len(arguments) in {1, 2}:
            self.file_read(connection, sequence, arguments)
        elif command == "FSRMTREE" and len(arguments) == 1:
            self.file_remove_tree(connection, sequence, arguments)
        elif command == "PACKAGE" and len(arguments) == 2 and arguments[0] == "deploy":
            self.deploy(connection, sequence, arguments[1])
        else:
            raise ServiceError("unsupported_simulator_command")


class BinaryHandler(socketserver.BaseRequestHandler):
    def setup(self) -> None:
        self.authenticated = not self.server.require_token  # type: ignore[attr-defined]

    def read_exact(self, size: int) -> bytes | None:
        output = bytearray()
        while len(output) < size:
            received = self.request.recv(size - len(output))
            if not received:
                return None
            output.extend(received)
        return bytes(output)

    def write_frame(self, kind: int, sequence: int, payload: bytes) -> None:
        if len(payload) > MAX_FRAME_BYTES:
            raise ServiceError("response exceeds PXADB2 maximum frame size")
        self.request.sendall(FRAME_HEADER.pack(len(payload), kind, 0, sequence) + payload)

    def handle_command(self, sequence: int, payload: bytes) -> None:
        service: SimulatorPxaDb = self.server.service  # type: ignore[attr-defined]
        try:
            command_parts = payload.decode("utf-8").split(" ")
            command = command_parts[0]
            arguments = command_parts[1:]
            if not self.authenticated:
                expected = self.server.token  # type: ignore[attr-defined]
                if command != "AUTH" or len(arguments) != 1 or not hmac.compare_digest(arguments[0], expected):
                    raise ServiceError("authentication_required")
                self.authenticated = True
                service.send(self, sequence, "OK", "authenticated")
                return
            service.dispatch(self, sequence, command, arguments)
        except (ServiceError, UnicodeError, OSError) as error:
            service.send(self, sequence, "ERR", str(error))

    def handle(self) -> None:
        service: SimulatorPxaDb = self.server.service  # type: ignore[attr-defined]
        while True:
            header = self.read_exact(FRAME_HEADER.size)
            if header is None:
                return
            payload_size, kind, _flags, sequence = FRAME_HEADER.unpack(header)
            if payload_size > MAX_FRAME_BYTES:
                return
            payload = self.read_exact(payload_size)
            if payload is None:
                return
            try:
                if kind == FRAME_COMMAND:
                    self.handle_command(sequence, payload)
                elif kind == FRAME_UPLOAD_DATA and self.authenticated:
                    service.write_upload(self, sequence, payload)
                else:
                    raise ServiceError("invalid_frame_kind")
            except (ServiceError, OSError) as error:
                service.send(self, sequence, "ERR", str(error))


class UnixServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True


class TcpServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def configure_server(server: socketserver.BaseServer, service: SimulatorPxaDb,
                     token: str | None) -> None:
    server.service = service  # type: ignore[attr-defined]
    server.token = token  # type: ignore[attr-defined]
    server.require_token = token is not None  # type: ignore[attr-defined]


def main() -> int:
    parser = argparse.ArgumentParser(description="PXADB2 binary service for the desktop simulator")
    parser.add_argument("--state-root", required=True, type=pathlib.Path)
    parser.add_argument("--socket", required=True, type=pathlib.Path)
    parser.add_argument("--installer", required=True, type=pathlib.Path)
    parser.add_argument("--publisher-key", required=True, type=pathlib.Path)
    parser.add_argument("--control-socket", type=pathlib.Path)
    parser.add_argument("--tcp-listen", type=parse_tcp_address)
    parser.add_argument("--token-file", type=pathlib.Path)
    parser.add_argument("--tcp-address-file", type=pathlib.Path)
    arguments = parser.parse_args()
    if arguments.tcp_listen and not arguments.token_file:
        parser.error("--tcp-listen requires --token-file")
    if arguments.token_file and not arguments.tcp_listen:
        parser.error("--token-file requires --tcp-listen")
    if arguments.tcp_address_file and not arguments.tcp_listen:
        parser.error("--tcp-address-file requires --tcp-listen")
    state_root = arguments.state_root.resolve()
    socket_path = arguments.socket.resolve()
    socket_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if socket_path.parent.stat().st_uid != os.getuid():
        parser.error("socket directory must be owned by the current user")
    os.chmod(socket_path.parent, 0o700)
    if not arguments.installer.is_file() or not os.access(arguments.installer, os.X_OK):
        parser.error("installer is unavailable or not executable")
    if not arguments.publisher_key.is_file():
        parser.error("publisher key is unavailable")
    token = load_or_create_token(arguments.token_file.resolve()) if arguments.token_file else None
    socket_path.unlink(missing_ok=True)
    service = SimulatorPxaDb(
        state_root, arguments.installer.resolve(), arguments.publisher_key.resolve(),
        arguments.control_socket.resolve() if arguments.control_socket else None,
    )
    unix_server = UnixServer(str(socket_path), BinaryHandler)
    configure_server(unix_server, service, None)
    os.chmod(socket_path, 0o600)
    tcp_server = None
    if arguments.tcp_listen:
        tcp_server = TcpServer(arguments.tcp_listen, BinaryHandler)
        configure_server(tcp_server, service, token)
        if arguments.tcp_address_file:
            write_tcp_address(arguments.tcp_address_file.resolve(), tcp_server.server_address)
        threading.Thread(target=tcp_server.serve_forever, daemon=True).start()
    stopping = threading.Event()

    def stop(*_: object) -> None:
        if not stopping.is_set():
            stopping.set()
            threading.Thread(target=unix_server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        unix_server.serve_forever()
    finally:
        unix_server.server_close()
        if tcp_server is not None:
            tcp_server.shutdown()
            tcp_server.server_close()
        if arguments.tcp_address_file:
            arguments.tcp_address_file.unlink(missing_ok=True)
        socket_path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
