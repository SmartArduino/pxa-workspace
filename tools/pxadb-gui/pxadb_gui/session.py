"""Single-connection device worker for the GUI.

PXADB is a request/response protocol over an exclusively opened transport, so
exactly one worker thread owns the connection. Input events take priority over
preview captures, and pointer MOVE events are coalesced on both ends: the GUI
keeps only the newest pending move, and the firmware input mailbox does the
same.
"""

from __future__ import annotations

import collections
import dataclasses
import errno
import pathlib
import subprocess
import threading
import time
from typing import Callable

from PySide6.QtCore import QObject, Signal

from .images import capture_to_image
from .library import pxadb


ROOT = pathlib.Path(__file__).resolve().parents[3]


@dataclasses.dataclass(frozen=True)
class DeviceChoice:
    label: str
    target: str
    info: str = ""
    simulator: str = ""  # PROFILE[@INSTANCE] for a local simulator

    def summary(self) -> str:
        if self.simulator:
            return f"simulator:{self.simulator}"
        return self.target


@dataclasses.dataclass(frozen=True)
class ScreenFrame:
    image: object
    metadata: dict[str, str]


# Firmware rejects captures that arrive before its minimum interval; retrying
# sooner only floods the device with rejected commands.
RATE_LIMIT_BACKOFF_SECONDS = 0.3

# If a drag never delivers its release (window manager stole the mouse, the
# widget vanished mid-gesture), release the device pointer instead of leaving
# it pressed and pausing the preview forever.
POINTER_STALL_SECONDS = 3.0

# A port that vanished (USB re-enumeration, unplug, reboot) reports one of
# these; pyserial wraps them in SerialException, and termios raises a bare
# error object whose first argument is the errno.
_TRANSPORT_ERRNOS = {
    errno.EIO, errno.ENXIO, errno.ENODEV, errno.ENOENT, errno.EBADF,
    errno.EPIPE, errno.ECONNRESET, errno.ESHUTDOWN,
}


def transport_failed(error: BaseException) -> bool:
    """True when the error means the port is gone and must be reopened."""
    errno_value = getattr(error, "errno", None)
    if errno_value is None and error.args and isinstance(error.args[0], int):
        errno_value = error.args[0]
    if errno_value in _TRANSPORT_ERRNOS:
        return True
    message = str(error)
    return any(fragment in message for fragment in (
        "device disconnected", "readiness", "Input/output error",
        "write failed", "short serial write", "PXADB2", "peer disconnected",
    ))


def reconnect_target(choice: DeviceChoice, serial: str,
                     timeout: float = 0.6) -> DeviceChoice | None:
    """Resolve the endpoint again after a reboot or USB re-enumeration.

    USB Serial/JTAG devices can come back under a different port name, so a
    serial-number match is preferred. Simulator sockets and TCP endpoints keep
    their address.
    """
    if choice.simulator or choice.target.startswith(("unix:", "tcp:")):
        return choice
    try:
        devices = pxadb.discover_devices(timeout)
    except (pxadb.PxaDbError, OSError, ValueError):
        devices = []
    if serial:
        for device in devices:
            if pxadb.info_properties(device.info).get("serial") == serial:
                return DeviceChoice(device.port, device.port, device.info)
    if len(devices) == 1:
        device = devices[0]
        return DeviceChoice(device.port, device.port, device.info)
    return None


def screenshot_request(device_info: str) -> str:
    """Prefer device-encoded JPEG when the firmware advertises it.

    JPEG frames are a fraction of the RGB565 burst, so the preview reaches the
    capture rate limit instead of the USB transfer time.
    """
    if "screenshot-jpeg" in pxadb.device_capabilities(device_info):
        return "SCREENSHOT JPEG"
    return "SCREENSHOT"


class InputQueue:
    """FIFO of device input commands with stale MOVE coalescing."""

    def __init__(self) -> None:
        self._events: collections.deque[tuple[str, str | None]] = collections.deque()

    def pointer(self, action: str, x: int, y: int) -> None:
        self._push(f"INPUT POINTER {action.upper()} {int(x)} {int(y)} 0",
                   "pointer-move" if action.lower() == "move" else None)

    def tap(self, x: int, y: int) -> None:
        self._push(f"INPUT TAP {int(x)} {int(y)}", None)

    def key(self, key: str) -> None:
        self._push(f"INPUT KEY {key.upper()}", None)

    def command(self, command: str) -> None:
        self._push(command, None)

    def _push(self, command: str, coalesce: str | None) -> None:
        if coalesce is not None:
            self._events = collections.deque(
                event for event in self._events if event[1] != coalesce)
        elif command.startswith("INPUT POINTER DOWN") or command == "INPUT CANCEL":
            # A new press makes queued moves from a previous gesture irrelevant.
            self._events = collections.deque(
                event for event in self._events if event[1] != "pointer-move")
        self._events.append((command, coalesce))

    def pop(self) -> str | None:
        if not self._events:
            return None
        return self._events.popleft()[0]

    def __len__(self) -> int:
        return len(self._events)


@dataclasses.dataclass
class _Job:
    name: str
    action: Callable[["DeviceSession"], object]
    needs_client: bool = True


class SessionSignals(QObject):
    connected = Signal(str)                 # HELLO payload
    disconnected = Signal()
    error = Signal(str)
    status = Signal(str)
    busy = Signal(bool)
    screenshot = Signal(object)             # ScreenFrame
    log_line = Signal(str)
    files_listed = Signal(str, object)      # path, list[(kind, size, name)]
    files_changed = Signal(str)
    packages_listed = Signal(object)        # list[list[str]]
    progress = Signal(str, int, int)        # label, done, total
    job_done = Signal(str, object)          # job name, payload


class DeviceSession:
    def __init__(self, signals: SessionSignals) -> None:
        self.signals = signals
        self._condition = threading.Condition()
        self._jobs: collections.deque[_Job] = collections.deque()
        self._inputs = InputQueue()
        self._log_buffer: list[str] = []
        self._stop = False
        self._thread: threading.Thread | None = None
        self._client = None
        self._target: DeviceChoice | None = None
        self._device_serial = ""
        self._simulator = ""
        self._timeout = 2.0
        self._preview = False
        self._interval = 0.6
        self._next_capture = 0.0
        self._pointer_active = False
        self._logs_enabled = False
        self._preview_error = ""
        self._auto_reconnect = False
        self._next_reconnect = 0.0
        self._reconnect_delay = 1.0
        self._last_pointer_us = 0.0

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, name="pxadb-session",
                                        daemon=True)
        self._thread.start()

    def shutdown(self) -> None:
        def action(session: "DeviceSession") -> object:
            session._close_client()
            session._stop = True
            return None

        self._enqueue("shutdown", action, needs_client=False)
        thread, self._thread = self._thread, None
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1.5)

    @property
    def connected(self) -> bool:
        return self._client is not None

    @property
    def simulator(self) -> str:
        return self._simulator

    @property
    def target(self) -> DeviceChoice | None:
        return self._target

    # -- connection --------------------------------------------------------

    def connect(self, choice: DeviceChoice, timeout: float = 2.0) -> None:
        def action(session: "DeviceSession") -> object:
            session._close_client()
            session._timeout = timeout
            client = pxadb.open_client(choice.target, timeout)
            client.log_callback = session._on_log
            client.raw_callback = session._on_raw
            session._client = client
            session._target = choice
            session._simulator = choice.simulator
            session._device_serial = pxadb.info_properties(
                client.device_info).get("serial", "")
            session._auto_reconnect = True
            session._reconnect_delay = 1.0
            with session._condition:
                session._next_reconnect = 0.0
            session.signals.connected.emit(client.device_info)
            return None

        self._enqueue("connect", action, needs_client=False)

    def disconnect(self) -> None:
        def action(session: "DeviceSession") -> object:
            session._auto_reconnect = False
            session._target = None
            with session._condition:
                session._next_reconnect = 0.0
            session._close_client()
            session.signals.disconnected.emit()
            return None

        self._enqueue("disconnect", action, needs_client=False)

    def _close_client(self) -> None:
        client, self._client = self._client, None
        self._simulator = ""
        if client is not None:
            try:
                client.close()
            except Exception:
                # The port may already be gone; closing stays best-effort.
                pass

    def _handle_transport_failure(self, error: BaseException) -> None:
        """Drop a dead connection and wait for the endpoint to come back."""
        if self._client is None:
            return
        target = self._target
        self._close_client()
        self.signals.disconnected.emit()
        if not self._auto_reconnect or target is None:
            self.signals.error.emit(str(error))
            return
        self._reconnect_delay = 1.0
        with self._condition:
            self._next_reconnect = time.monotonic() + self._reconnect_delay
            self._condition.notify()
        self.signals.status.emit(
            "device disconnected; waiting for it to come back")

    def _attempt_reconnect(self) -> None:
        target = self._target
        assert target is not None
        try:
            candidate = reconnect_target(target, self._device_serial)
        except Exception:
            candidate = None
        client = None
        if candidate is not None:
            try:
                client = pxadb.open_client(candidate.target, self._timeout)
            except (pxadb.PxaDbError, OSError, ValueError):
                client = None
        if client is None:
            self._reconnect_delay = min(self._reconnect_delay * 2.0, 5.0)
            with self._condition:
                self._next_reconnect = time.monotonic() + self._reconnect_delay
            return
        client.log_callback = self._on_log
        client.raw_callback = self._on_raw
        with self._condition:
            self._client = client
            self._target = candidate
            self._simulator = candidate.simulator
            self._next_reconnect = 0.0
            self._reconnect_delay = 1.0
        self._device_serial = pxadb.info_properties(
            client.device_info).get("serial", "")
        self.signals.connected.emit(client.device_info)
        self.signals.status.emit("reconnected")

    # -- preview and input -------------------------------------------------

    def set_preview(self, enabled: bool, interval: float) -> None:
        with self._condition:
            self._preview = enabled
            self._interval = max(0.05, interval)
            self._next_capture = 0.0
            self._condition.notify()

    def screenshot(self) -> None:
        def action(session: "DeviceSession") -> object:
            frame = session._capture_frame()
            if frame is not None:
                session.signals.screenshot.emit(frame)
            return None

        self._enqueue("screenshot", action)

    def pointer(self, action: str, x: int, y: int) -> None:
        with self._condition:
            self._inputs.pointer(action, x, y)
            self._last_pointer_us = time.monotonic()
            if action in {"down", "move"}:
                self._pointer_active = True
            elif action in {"up", "cancel"}:
                self._pointer_active = False
            self._condition.notify()

    def tap(self, x: int, y: int) -> None:
        with self._condition:
            self._inputs.tap(x, y)
            self._condition.notify()

    def key(self, name: str) -> None:
        with self._condition:
            self._inputs.key(name)
            self._condition.notify()

    def sync_input(self) -> None:
        with self._condition:
            self._inputs.command("INPUT SYNC")
            self._condition.notify()

    def set_logs(self, enabled: bool) -> None:
        def action(session: "DeviceSession") -> object:
            with session._condition:
                session._logs_enabled = enabled
            if enabled:
                session._client.subscribe_logs()
            else:
                session._client.unsubscribe_logs()
            return None

        self._enqueue("logcat on" if enabled else "logcat off", action)

    # -- files -------------------------------------------------------------

    def list_files(self, path: str) -> None:
        def action(session: "DeviceSession") -> object:
            entries = pxadb.NormalFsClient(session._client).list(path)
            session.signals.files_listed.emit(path, entries)
            return None

        self._enqueue("fs list", action)

    def make_directory(self, path: str) -> None:
        def action(session: "DeviceSession") -> object:
            pxadb.NormalFsClient(session._client).mkdir(path)
            session.signals.files_changed.emit(path)
            return path

        self._enqueue("fs mkdir", action)

    def remove_path(self, path: str, recursive: bool) -> None:
        def action(session: "DeviceSession") -> object:
            if recursive:
                pxadb.fs_remove_tree(pxadb.NormalFsClient(session._client),
                                     path, session._timeout)
            else:
                pxadb.NormalFsClient(session._client).remove(path)
            session.signals.files_changed.emit(path)
            return path

        self._enqueue("fs remove", action)

    def upload(self, local: str, remote: str) -> None:
        source = pathlib.Path(local)

        def action(session: "DeviceSession") -> object:
            def report(done: int, total: int) -> None:
                session.signals.progress.emit(f"upload {source.name}",
                                              int(done), int(total))

            pxadb.NormalFsClient(session._client).put(source, remote,
                                                      progress=report)
            session.signals.files_changed.emit(remote)
            return remote

        self._enqueue(f"upload {source.name}", action)

    def download(self, remote: str, local: str) -> None:
        destination = pathlib.Path(local)
        name = pathlib.PurePosixPath(remote).name or remote

        def action(session: "DeviceSession") -> object:
            def report(done: int, total: int | None) -> None:
                session.signals.progress.emit(f"download {name}", int(done),
                                              int(total or 0))

            pxadb.NormalFsClient(session._client).get(remote, destination,
                                                      progress=report)
            return str(destination)

        self._enqueue(f"download {name}", action)

    # -- packages and device actions --------------------------------------

    def list_packages(self) -> None:
        def action(session: "DeviceSession") -> object:
            frames = session._client.request("PACKAGES")
            packages = [frame.payload.split("\t", 3) for frame in frames
                        if frame.kind == "PKG"]
            session.signals.packages_listed.emit(packages)
            return None

        self._enqueue("package list", action)

    def install_package(self, local: str) -> None:
        source, identity = pxadb.package_source(local, None)

        def action(session: "DeviceSession") -> object:
            session.signals.status.emit(f"staging {identity}")
            pxadb.stage_package_files(pxadb.NormalFsClient(session._client),
                                      source, identity, session._timeout)
            session.signals.status.emit(f"installing {identity}")
            session._client.request(f"PACKAGE deploy {identity}", timeout=120.0)
            pxadb.clear_staged_source(session._client, identity)
            return identity

        self._enqueue(f"install {identity}", action)

    def package_action(self, action: str, identity: str) -> None:
        def run(session: "DeviceSession") -> object:
            session._client.request(f"PACKAGE {action} {identity}",
                                    timeout=60.0)
            return f"{action}: {identity}"

        self._enqueue(f"package {action}", run)

    def run_package(self, identity: str) -> None:
        selector = self._simulator

        def run(session: "DeviceSession") -> object:
            if selector:
                profile, separator, instance = selector.partition("@")
                command = [str(ROOT / "tools" / "simulator.sh"), "product",
                           "run", "--profile", profile]
                if separator:
                    command.extend(["--instance", instance])
                command.extend(["--installed", identity])
                subprocess.Popen(command, cwd=str(ROOT), start_new_session=True)
                return f"launched {identity} on {selector}"
            session._client.request(f"PACKAGE run {identity}", timeout=30.0)
            return f"run: {identity}"

        self._enqueue(f"run {identity}", run,
                      needs_client=not bool(selector))

    def reboot(self) -> None:
        def run(session: "DeviceSession") -> object:
            session._client.request("REBOOT")
            return "rebooting"

        self._enqueue("reboot", run)

    def poweroff(self) -> None:
        def run(session: "DeviceSession") -> object:
            try:
                session._client.request("POWEROFF")
            except pxadb.PxaDbError as error:
                if str(error) == "poweroff_not_supported":
                    raise pxadb.PxaDbError(
                        "this board does not support software power-off")
                raise
            return "powering off"

        self._enqueue("poweroff", run)

    # -- worker ------------------------------------------------------------

    def _enqueue(self, name: str, action: Callable[["DeviceSession"], object],
                 needs_client: bool = True) -> None:
        with self._condition:
            self._jobs.append(_Job(name, action, needs_client))
            self._condition.notify()

    def _run(self) -> None:
        while True:
            command = None
            job: _Job | None = None
            capture = False
            idle = False
            reconnect = False
            with self._condition:
                if self._stop:
                    return
                # A lost mouse release would otherwise keep the device pointer
                # pressed (rejecting later taps) and pause preview forever.
                if (self._pointer_active and
                        time.monotonic() - self._last_pointer_us >
                        POINTER_STALL_SECONDS):
                    self._pointer_active = False
                    self._inputs.command("INPUT CANCEL")
                reconnect = (self._client is None and self._target is not None
                             and self._auto_reconnect and
                             time.monotonic() >= self._next_reconnect)
                if not reconnect:
                    command = self._inputs.pop()
                    if command is None and self._jobs:
                        job = self._jobs.popleft()
                    if (command is None and job is None and self._preview and
                            not self._pointer_active and
                            time.monotonic() >= self._next_capture):
                        self._next_capture = time.monotonic() + self._interval
                        capture = True
                    if command is None and job is None and not capture:
                        idle = True
                        self._condition.wait(0.05)
            try:
                if reconnect:
                    self._attempt_reconnect()
                elif idle:
                    self.pump_logs()
                elif self._client is None and (
                        command is not None or
                        (job is not None and job.needs_client)):
                    if job is not None:
                        self.signals.error.emit(f"{job.name}: not connected")
                elif command is not None:
                    self._run_command(command)
                elif job is not None:
                    self._run_job(job)
                elif capture:
                    self._capture_preview()
            except Exception as error:
                # Keep the session thread alive no matter what a transport or
                # job does; a dead worker leaves the port locked and the UI
                # permanently stuck.
                self.signals.error.emit(f"session: {error}")
            self._flush_logs()

    def _run_command(self, command: str) -> None:
        assert self._client is not None
        try:
            self._client.request(command)
        except (pxadb.PxaDbError, OSError, ValueError) as error:
            if transport_failed(error):
                self._handle_transport_failure(error)
                return
            self.signals.error.emit(str(error))

    def _run_job(self, job: _Job) -> None:
        self.signals.busy.emit(True)
        try:
            payload = job.action(self)
        except (pxadb.PxaDbError, OSError, ValueError, RuntimeError) as error:
            if transport_failed(error):
                self._handle_transport_failure(error)
            else:
                self.signals.error.emit(f"{job.name}: {error}")
        else:
            self.signals.job_done.emit(job.name, payload)
        finally:
            self.signals.busy.emit(False)

    def _capture_preview(self) -> None:
        frame = self._capture_frame()
        if frame is not None:
            self.signals.screenshot.emit(frame)

    def _capture_frame(self) -> ScreenFrame | None:
        client = self._client
        if client is None:
            return None
        attempts = 2
        for attempt in range(attempts):
            try:
                frames = client.request(screenshot_request(client.device_info),
                                        timeout=120.0)
                capture = pxadb.screenshot_capture(frames)
            except (pxadb.PxaDbError, OSError, ValueError) as error:
                if (pxadb.screenshot_retryable(error) and
                        attempt + 1 < attempts):
                    # A dropped DATA line costs one frame; re-request it after
                    # the device's minimum capture interval instead of showing
                    # a transient link error in the log.
                    time.sleep(RATE_LIMIT_BACKOFF_SECONDS)
                    continue
                if "screenshot_rate_limited" in str(error):
                    with self._condition:
                        self._next_capture = time.monotonic() + max(
                            self._interval, RATE_LIMIT_BACKOFF_SECONDS)
                    return None
                if transport_failed(error):
                    self._handle_transport_failure(error)
                    return None
                message = f"screenshot: {error}"
                if message != self._preview_error:
                    self._preview_error = message
                    self.signals.error.emit(message)
                return None
            self._preview_error = ""
            return ScreenFrame(capture_to_image(capture), capture.metadata)
        return None

    def _on_log(self, frame) -> None:
        if frame.kind == "LOG":
            timestamp, separator, message = frame.payload.partition("\t")
            text = f"{timestamp:>10} {message}" if separator else frame.payload
        else:
            text = f"--- PXADB {frame.payload} ---"
        self._log_buffer.append(text)

    def _on_raw(self, raw: bytes) -> None:
        text = raw.decode("utf-8", "replace").rstrip("\r\n")
        if text:
            self._log_buffer.append(f"[serial] {text}")

    def _flush_logs(self) -> None:
        if not self._log_buffer:
            return
        batch = "\n".join(self._log_buffer)
        self._log_buffer.clear()
        self.signals.log_line.emit(batch)

    def pump_logs(self) -> None:
        """Drain spontaneous device output while the connection is idle."""
        client = self._client
        if client is not None:
            try:
                client.pump_logs()
            except (pxadb.PxaDbError, OSError):
                return
        self._flush_logs()


def scan_devices(timeout: float = 0.6) -> list[DeviceChoice]:
    """Probe USB and simulator endpoints. Blocking; call from a scan thread."""
    choices: list[DeviceChoice] = []
    try:
        for device in pxadb.discover_simulators(timeout):
            profile = device.port.removeprefix("simulator:")
            choices.append(DeviceChoice(f"simulator:{profile}",
                                        pxadb.simulator_socket(profile),
                                        device.info, profile))
    except (pxadb.PxaDbError, OSError, ValueError):
        pass
    try:
        for device in pxadb.discover_devices(timeout):
            choices.append(DeviceChoice(device.port, device.port, device.info))
    except (pxadb.PxaDbError, OSError, ValueError):
        pass
    return choices
