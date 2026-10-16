#!/usr/bin/env python3
"""Fast PXA development loop for the desktop PXA system and devices."""

from __future__ import annotations

import argparse
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time
import tomllib
from collections.abc import Iterable

from pxadb.pxadb import PxaDbError, package_manifest_identity


ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_TOOL = ROOT / "tools" / "app.sh"
SIMULATOR_TOOL = ROOT / "tools" / "simulator.sh"
PXADB_TOOL = ROOT / "tools" / "pxadb" / "pxadb.py"
BOARD_TARGET_TOOL = ROOT / "tools" / "board-target.cmake"
IGNORED_DIRECTORIES = {".git", "__pycache__", "build", "dist", "node_modules", "out"}


class DevError(RuntimeError):
    """An expected development setup or command failure."""


class LogWriter:
    def __init__(self, path: pathlib.Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.file = path.open("a", encoding="utf-8", buffering=1)
        self.lock = threading.Lock()

    def write(self, message: str) -> None:
        with self.lock:
            print(message, end="", flush=True)
            self.file.write(message)

    def close(self) -> None:
        self.file.close()


def command_label(command: Iterable[str]) -> str:
    return " ".join(command)


def board_target(board: str) -> str:
    metadata = ROOT / "firmware" / "boards" / board / "board.cmake"
    try:
        result = subprocess.run(
            ["cmake", "--log-level=ERROR", f"-DPXA_BOARD_METADATA={metadata}",
             "-P", str(BOARD_TARGET_TOOL)],
            check=True, capture_output=True, text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) else str(error)
        raise DevError(f"cannot read target for board '{board}': {detail}") from error
    return result.stdout.strip()


def stream_command(command: list[str], log: LogWriter) -> None:
    log.write(f"\n$ {command_label(command)}\n")
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    last_line = ""
    for line in process.stdout:
        log.write(line)
        if line.strip():
            last_line = line.strip()
    status = process.wait()
    if status:
        raise DevError(
            f"command failed with exit code {status}: {command_label(command)}; {last_line}"
        )


def stop_process(process: subprocess.Popen[str] | None, label: str, log: LogWriter,
                 timeout: float = 5) -> None:
    if process is None:
        return
    log.write(f"[dev] stopping {label}\n")
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return
    # The shell wrapper can exit before SDL finishes handling SIGTERM. Wait
    # for its entire private process group, including already orphaned runners.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    log.write(f"[dev] {label} did not exit; killing its process group\n")
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def pipe_process(process: subprocess.Popen[str], label: str, log: LogWriter) -> None:
    assert process.stdout is not None
    for line in process.stdout:
        log.write(f"[{label}] {line}")
    status = process.wait()
    if status:
        log.write(f"[dev] {label} exited with status {status}\n")


def start_logged_process(command: list[str], label: str, log: LogWriter) -> subprocess.Popen[str]:
    log.write(f"\n$ {command_label(command)}\n")
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        start_new_session=True,
    )
    threading.Thread(target=pipe_process, args=(process, label, log), daemon=True).start()
    return process


def app_source_root(app_id: str, supplied_root: str | None) -> pathlib.Path:
    if supplied_root:
        root = pathlib.Path(supplied_root).expanduser().resolve()
    else:
        catalog = ROOT / "local" / "apps.toml"
        if catalog.is_file():
            with catalog.open("rb") as source:
                entry = tomllib.load(source).get("apps", {}).get(app_id, {})
            value = entry.get("source_root") if isinstance(entry, dict) else None
        else:
            value = None
        if isinstance(value, str) and value:
            root = pathlib.Path(value).expanduser().resolve()
        elif (ROOT / "local/pxa-apps" / app_id / "package.json").is_file():
            root = (ROOT / "local/pxa-apps").resolve()
        else:
            raise DevError(
                f"no source root for '{app_id}'; clone local/pxa-apps or pass --source-root"
            )
    app_dir = root / app_id
    if not app_dir.is_dir():
        raise DevError(f"app source directory is unavailable: {app_dir}")
    return root


def watched_files(app_dir: pathlib.Path) -> dict[pathlib.Path, tuple[int, int]]:
    snapshot: dict[pathlib.Path, tuple[int, int]] = {}
    for parent, directories, files in os.walk(app_dir):
        directories[:] = [name for name in directories if name not in IGNORED_DIRECTORIES]
        for name in files:
            path = pathlib.Path(parent, name)
            try:
                stat = path.stat()
            except FileNotFoundError:
                continue
            snapshot[path] = (stat.st_mtime_ns, stat.st_size)
    return snapshot


class Developer:
    def __init__(self, args: argparse.Namespace, log: LogWriter, source_root: pathlib.Path) -> None:
        self.args = args
        self.log = log
        self.source_root = source_root
        self.app_id = args.app_id
        self.package_id: str | None = None
        self.app_dir = source_root / args.app_id
        self.runner: subprocess.Popen[str] | None = None
        self.logcat: subprocess.Popen[str] | None = None

    @property
    def simulator_id(self) -> str:
        instance = self.args.instance
        return f"{self.args.profile}@{instance}" if instance else self.args.profile

    def simulator_command(self, *arguments: str) -> list[str]:
        command = [str(SIMULATOR_TOOL), *arguments, "--profile", self.args.profile]
        if self.args.instance:
            command.extend(["--instance", self.args.instance])
        return command

    def pxadb(self, *arguments: str) -> list[str]:
        command = [sys.executable, str(PXADB_TOOL), *arguments]
        if self.args.mode == "sim":
            command.extend(["--simulator", self.simulator_id])
        elif self.args.port:
            command.extend(["--port", self.args.port])
        if self.args.mode == "device":
            baud = self.args.baud
            # CH343P UART on DOIT GameKit runs PXADB at 2 Mbaud. Explicit
            # --baud and PXADB_BAUD retain precedence over this board default.
            if baud is None and self.args.board == "doit-gamekit" and not os.environ.get("PXADB_BAUD"):
                baud = 2000000
            if baud is not None:
                command.extend(["--baud", str(baud)])
        return command

    def build(self) -> None:
        target = "simulator" if self.args.mode == "sim" else board_target(self.args.board)
        command = [
            str(APP_TOOL), "build", self.app_id,
            "--board", self.args.board,
            "--target", target,
            "--aot-only",
            "--source-root", str(self.source_root),
        ]
        stream_command(command, self.log)

    @property
    def package_path(self) -> pathlib.Path:
        return ROOT / "local" / "app-output" / self.args.board / f"pxa-{self.app_id}.pxa"

    def start_simulator_service(self) -> None:
        stream_command(
            self.simulator_command("service", "start"),
            self.log,
        )

    def install(self) -> None:
        command = self.pxadb("package", "install", str(self.package_path), "--yes")
        stream_command(command, self.log)

    def start_app(self) -> None:
        if self.package_id is None:
            raise DevError("package identity is unavailable; build the package first")
        if self.args.mode == "sim":
            self.runner = start_logged_process(
                self.simulator_command("ui", "run") + ["--", "--launch", self.package_id],
                "simulator",
                self.log,
            )
        else:
            # The catalog is synchronized on the LVGL owner after installation.
            # Give that owner a bounded window to register the new package.
            for attempt in range(10):
                try:
                    stream_command(self.pxadb("package", "run", self.package_id), self.log)
                    break
                except DevError as error:
                    if "package_launch_failed" not in str(error) or attempt == 9:
                        raise
                    time.sleep(0.3)
            if not self.args.no_logcat:
                self.logcat = start_logged_process(
                    self.pxadb("logcat"), "logcat", self.log
                )

    def deploy(self) -> None:
        self.log.write(f"\n[dev] building {self.app_id} for {self.args.mode}\n")
        self.build()
        if not self.package_path.is_file():
            raise DevError(f"package build did not produce {self.package_path}")
        try:
            candidate_id = package_manifest_identity(self.package_path)
        except (OSError, PxaDbError) as error:
            raise DevError(f"cannot read built package identity: {error}") from error
        previous_id = self.package_id
        stop_process(self.runner, "simulator", self.log)
        self.runner = None
        stop_process(self.logcat, "logcat", self.log)
        self.logcat = None
        try:
            self.install()
        except DevError:
            if previous_id is not None:
                self.package_id = previous_id
                self.start_app()
            raise
        self.package_id = candidate_id
        self.start_app()
        self.log.write(f"[dev] running {self.package_id}\n")

    def close(self) -> None:
        stop_process(self.runner, "simulator", self.log)
        stop_process(self.logcat, "logcat", self.log)

    def watch(self) -> None:
        interval = self.args.interval
        snapshot = watched_files(self.app_dir)
        self.log.write(f"[dev] watching {self.app_dir} (interval {interval:g}s)\n")
        pending_at: float | None = None
        try:
            while True:
                time.sleep(interval)
                current = watched_files(self.app_dir)
                if current != snapshot:
                    snapshot = current
                    pending_at = time.monotonic()
                    self.log.write("[dev] source change detected\n")
                if pending_at is not None and time.monotonic() - pending_at >= self.args.debounce:
                    pending_at = None
                    try:
                        self.deploy()
                    except DevError as error:
                        self.log.write(f"[dev] update failed: {error}\n")
        except KeyboardInterrupt:
            self.log.write("\n[dev] stopped\n")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="tools/dev.sh",
        description="Build, deploy and run a PXA App with an optional source watcher.",
    )
    parser.add_argument("mode", choices=("sim", "device"), help="full desktop PXA system or ESP device")
    parser.add_argument(
        "app_id",
        help="App source directory name; the installed ID is read from the built package",
    )
    parser.add_argument("--source-root", help="parent directory containing the App directory")
    parser.add_argument("--board", default="pai-touch", help="PXA board profile (default: pai-touch)")
    parser.add_argument("--profile", help="simulator profile (default: --board)")
    parser.add_argument("--instance", help="isolated simulator instance (separate settings and App data)")
    parser.add_argument("--port", help="PXADB USB Serial/JTAG port for device mode")
    parser.add_argument("--baud", type=int, help="PXADB UART baud rate for device mode")
    parser.add_argument("--watch", action="store_true", help="rebuild and restart after source changes")
    parser.add_argument("--no-logcat", action="store_true", help="do not start device logcat after deployment")
    parser.add_argument("--log-file", help="write combined tool and runtime output to this file")
    parser.add_argument("--interval", type=float, default=0.35, help="watch polling interval in seconds (default: 0.35)")
    parser.add_argument("--debounce", type=float, default=0.30, help="quiet time before rebuilding in seconds (default: 0.30)")
    args = parser.parse_args()
    if args.interval <= 0 or args.debounce < 0:
        parser.error("--interval must be positive and --debounce must not be negative")
    if args.mode == "sim" and args.port:
        parser.error("--port is only valid in device mode")
    if args.mode == "sim" and args.baud:
        parser.error("--baud is only valid in device mode")
    if args.mode == "device" and args.profile is not None:
        parser.error("--profile is only valid in sim mode")
    if args.mode == "device" and args.instance is not None:
        parser.error("--instance is only valid in sim mode")
    if args.baud is not None and args.baud <= 0:
        parser.error("--baud must be positive")
    if args.mode == "sim" and args.profile is None:
        args.profile = args.board
    return args


def request_shutdown(_signal_number: int, _frame: object) -> None:
    """Turn terminal and supervisor shutdown signals into normal cleanup."""
    raise KeyboardInterrupt


def main() -> int:
    signal.signal(signal.SIGINT, request_shutdown)
    signal.signal(signal.SIGTERM, request_shutdown)
    args = parse_arguments()
    try:
        source_root = app_source_root(args.app_id, args.source_root)
        log_path = pathlib.Path(args.log_file).expanduser().resolve() if args.log_file else (
            ROOT / "local" / "dev-logs" / args.mode / f"{args.app_id}.log"
        )
        log = LogWriter(log_path)
        log.write(f"[dev] log file: {log_path}\n")
        developer = Developer(args, log, source_root)
        try:
            if args.mode == "sim":
                developer.start_simulator_service()
            developer.deploy()
            if args.watch:
                developer.watch()
            elif args.mode == "sim":
                log.write("[dev] simulator is running; press Ctrl-C to stop\n")
                try:
                    assert developer.runner is not None
                    developer.runner.wait()
                except KeyboardInterrupt:
                    log.write("\n[dev] stopped\n")
            elif args.mode == "device" and not args.no_logcat:
                log.write("[dev] streaming device logs; press Ctrl-C to stop\n")
                try:
                    assert developer.logcat is not None
                    developer.logcat.wait()
                except KeyboardInterrupt:
                    log.write("\n[dev] stopped\n")
        finally:
            developer.close()
            log.close()
        return 0
    except DevError as error:
        print(f"dev: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
