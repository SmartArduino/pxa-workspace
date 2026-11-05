"""Check board-profile forwarding through the real simulator shell entry point."""

import base64
import json
import os
import pathlib
import shutil
import subprocess
import socket
import struct
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parent.parent


class SimulatorFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        (self.root / "tools").mkdir()
        (self.root / "simulator/profiles").mkdir(parents=True)
        self.script = self.root / "tools/simulator.sh"
        shutil.copy(ROOT / "tools/simulator.sh", self.script)
        self.profile = self.root / "simulator/profiles/esp-mosaico.toml"
        shutil.copy(ROOT / "simulator/profiles/esp-mosaico.toml", self.profile)
        pxsys = self.root / "pxsys"
        (pxsys / "simulator/desktop").mkdir(parents=True)
        (pxsys / "simulator/desktop/CMakeLists.txt").touch()
        self.package = self.root / "package"
        self.package.mkdir()
        self.key = self.root / "publisher.der"
        self.key.touch()
        self.output = self.root / "argv.json"
        self.build = self.root / "build/simulator/esp-mosaico"
        self.build.mkdir(parents=True)
        for runner in ("pxsys_desktop_simulator", "pxsys_product_simulator"):
            stub = self.build / runner
            stub.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, pathlib, sys\n"
                "pathlib.Path(os.environ['PXA_TEST_ARGV']).write_text(json.dumps(sys.argv))\n"
            )
            stub.chmod(0o755)
        self.environment = dict(
            os.environ, PXSYS_ROOT=str(pxsys), CMAKE=shutil.which("true"),
            PYTHON=sys.executable, PXA_SIMULATOR_AUTOSTART_PXADB="0",
            PXA_SIMULATOR_SOCKET_ROOT=str(self.root / "sockets"),
            PXA_TEST_ARGV=str(self.output))

    def command(self, mode: str, action: str, *extra: str) -> list[str]:
        return ["bash", str(self.script), mode, action, "--profile", "esp-mosaico",
                "--publisher-key", str(self.key), *extra]

    def run_simulator(self, mode: str, action: str, *extra: str, check=True):
        return subprocess.run(self.command(mode, action, *extra),
                              env=self.environment, check=check,
                              capture_output=True, text=True, timeout=20)


class SimulatorArgumentsTest(SimulatorFixture):
    def test_mosaico_display_profile_reaches_both_runners(self) -> None:
        for mode, runner in (("ui", "pxsys_desktop_simulator"),
                             ("product", "pxsys_product_simulator")):
            with self.subTest(mode=mode):
                self.run_simulator(mode, "run", "--package", str(self.package))
                args = json.loads(self.output.read_text())
                self.assertEqual(pathlib.Path(args[0]).name, runner)
                for flag, value in (("--width", "480"), ("--height", "480"),
                                    ("--density-dpi", "305"), ("--corner-radius", "58"),
                                    ("--safe-insets", "12,12,12,12"), ("--locale", "zh-CN")):
                    self.assertEqual(args[args.index(flag) + 1], value)

    def test_invalid_profiles_fail_before_starting_a_runner(self) -> None:
        profiles = (
            ("[desktop]\nwidth=true\nheight=480\n", "desktop.width"),
            ("[desktop]\nwidth=480\nheight=480\ncorner_radius=true\n", "desktop.corner_radius"),
            ("[desktop]\nwidth=480\nheight=480\nsafe_insets=[true,0,0,0]\n", "desktop.safe_insets"),
            ("[desktop]\nwidth=480\nheight=480\nsafe_insets=[240,0,240,0]\n", "non-empty display area"),
            ("[desktop]\nwidth=480\nheight=480\nround=1\n", "desktop.round"),
            ("[desktop]\nwidth=480\nheight=480\ngestures=1\n", "desktop.gestures"),
            ("[desktop]\nwidth=480\nheight=480\nnavigation_bar=1\n", "desktop.navigation_bar"),
            ("[desktop]\nwidth=", "TOMLDecodeError"),
        )
        for content, message in profiles:
            with self.subTest(message=message):
                self.profile.write_text(content)
                result = self.run_simulator("ui", "run", check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertNotIn("unbound variable", result.stderr)
                self.assertFalse(self.output.exists())


class SimulatorServiceTest(SimulatorFixture):
    def setUp(self) -> None:
        super().setUp()
        shutil.copy(ROOT / "tools/simulator_pxadb.py", self.root / "tools/simulator_pxadb.py")
        shutil.copytree(ROOT / "tools/pxadb", self.root / "tools/pxadb",
                        ignore=shutil.ignore_patterns("__pycache__"))
        installer = self.build / "pxsys_package_installer"
        installer.write_text("#!/bin/sh\nexit 0\n")
        installer.chmod(0o755)
        self.state = self.root / "local/simulator/esp-mosaico"
        self.addCleanup(self.stop_service)

    def stop_service(self) -> None:
        self.run_simulator("service", "stop", check=False)

    def pid(self) -> str:
        return (self.state / "pxadb.pid").read_text().strip()

    def test_reuses_healthy_service_and_refreshes_changed_sources(self) -> None:
        self.run_simulator("service", "start")
        original = self.pid()
        result = self.run_simulator("service", "start")
        self.assertIn("already running", result.stdout)
        self.assertEqual(self.pid(), original)
        with (self.root / "tools/simulator_pxadb.py").open("a") as source:
            source.write("\n# source update\n")
        result = self.run_simulator("service", "start")
        self.assertIn("Refreshing", result.stdout)
        self.assertNotEqual(self.pid(), original)
        self.run_simulator("service", "status")

    def test_restart_preserves_ui_control_socket(self) -> None:
        self.run_simulator("service", "start")
        path = self.root / "sockets/esp-mosaico.control.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as control:
            control.bind(str(path))
            self.run_simulator("service", "stop")
            self.assertTrue(path.is_socket())
            self.run_simulator("service", "start")
            self.assertTrue(path.is_socket())

    def test_refresh_preserves_tcp_listener_and_token(self) -> None:
        self.run_simulator("service", "start", "--listen", "127.0.0.1:auto")
        endpoint = (self.state / "pxadb2.tcp").read_text()
        token = (self.state / "pxadb2.token").read_text()
        (self.state / "pxadb.revision").write_text("stale")
        self.run_simulator("service", "start")
        self.assertEqual((self.state / "pxadb2.tcp").read_text(), endpoint)
        self.assertEqual((self.state / "pxadb2.token").read_text(), token)

    def test_tcp_listener_authenticates_each_connection(self) -> None:
        self.run_simulator("service", "start", "--listen", "127.0.0.1:auto")
        endpoint = (self.state / "pxadb2.tcp").read_text().strip()
        command = [sys.executable, str(self.root / "tools/pxadb/pxadb.py"), "doctor",
                   "--connect", endpoint, "--timeout", "1", "--token"]
        accepted = subprocess.run(command + [str(self.state / "pxadb2.token")],
                                  capture_output=True, text=True, timeout=5)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        incorrect_token = self.root / "incorrect-token"
        incorrect_token.write_text("incorrect-token\n")
        rejected = subprocess.run(command + [str(incorrect_token)],
                                  capture_output=True, text=True, timeout=5)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("authentication_required", rejected.stderr)

    def test_stop_cleans_an_upload_from_a_connected_client(self) -> None:
        self.run_simulator("service", "start")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(2)
            connection.connect(str(self.root / "sockets/esp-mosaico.sock"))
            path = base64.b64encode(b"upload.bin")
            command = b"FSPUT " + path + b" 100"
            connection.sendall(struct.pack("!IBBI", len(command), 1, 0, 1) + command)
            with connection.makefile("rb") as stream:
                size, _kind, _flags, _sequence = struct.unpack("!IBBI", stream.read(10))
                self.assertIn(b"READY\0", stream.read(size))
                self.assertTrue(list(self.state.glob("*.pxadb2-part")))
                self.run_simulator("service", "stop")
        self.assertEqual(list(self.state.glob("*.pxadb2-part")), [])

    def test_legacy_capability_follows_installer_build_option(self) -> None:
        self.run_simulator("service", "start")
        original = self.pid()
        (self.build / "CMakeCache.txt").write_text("PXA_ENABLE_LEGACY_CONTAINER:BOOL=TRUE\n")
        self.run_simulator("service", "start")
        self.assertNotEqual(self.pid(), original)
        result = subprocess.run([
            sys.executable, str(self.root / "tools/pxadb/pxadb.py"), "doctor",
            "--port", "unix:" + str(self.root / "sockets/esp-mosaico.sock"),
        ], check=True, capture_output=True, text=True, timeout=5)
        self.assertIn("package_legacy=1", result.stdout)

    def test_build_failure_keeps_existing_service_available(self) -> None:
        self.run_simulator("service", "start")
        original = self.pid()
        self.environment["CMAKE"] = shutil.which("false")
        result = self.run_simulator("service", "start", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.pid(), original)
        self.run_simulator("service", "status")

    def test_service_recovers_after_its_socket_is_lost(self) -> None:
        self.run_simulator("service", "start")
        original = self.pid()
        (self.root / "sockets/esp-mosaico.sock").unlink()
        self.run_simulator("service", "start")
        self.assertNotEqual(self.pid(), original)
        self.run_simulator("service", "status")

    def test_simultaneous_starts_share_one_service(self) -> None:
        command = self.command("service", "start")
        with subprocess.Popen(command, env=self.environment, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) as first:
            second = self.run_simulator("service", "start")
            stdout, stderr = first.communicate(timeout=20)
            self.assertEqual(first.returncode, 0, stderr)
        self.assertEqual((stdout + second.stdout).count("service started:"), 1)
        self.assertEqual((stdout + second.stdout).count("already running:"), 1)

    def test_stale_pid_does_not_signal_an_unrelated_process(self) -> None:
        with subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"]) as unrelated:
            try:
                self.state.mkdir(parents=True)
                (self.state / "pxadb.pid").write_text(str(unrelated.pid))
                self.run_simulator("service", "stop")
                self.assertIsNone(unrelated.poll())
            finally:
                unrelated.terminate()
                unrelated.wait(timeout=5)

    def test_refresh_keeps_explicit_service_persistent(self) -> None:
        self.run_simulator("service", "start")
        (self.state / "pxadb.revision").write_text("stale")
        self.environment["PXA_SIMULATOR_AUTOSTART_PXADB"] = "1"
        self.run_simulator("ui", "run")
        self.run_simulator("service", "status")
        self.assertFalse((self.state / "pxadb.autostart").exists())

    def test_automatic_service_stops_when_runner_exits(self) -> None:
        self.environment["PXA_SIMULATOR_AUTOSTART_PXADB"] = "1"
        self.run_simulator("ui", "run")
        result = self.run_simulator("service", "status", check=False)
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.state / "pxadb.pid").exists())


if __name__ == "__main__":
    unittest.main()
