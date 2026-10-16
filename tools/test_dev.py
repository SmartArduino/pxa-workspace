import pathlib
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dev


class DeviceLaunchTest(unittest.TestCase):
    def setUp(self) -> None:
        args = SimpleNamespace(
            mode="device", app_id="game-render-bench", board="esp32s31-korvo-1",
            port="/dev/ttyUSB0", baud=2000000, no_logcat=True,
        )
        self.developer = dev.Developer(args, mock.Mock(), pathlib.Path("."))
        self.developer.package_id = "pxa-game-render-bench"

    def test_failed_build_keeps_running_application_and_logs(self) -> None:
        runner, logcat = mock.Mock(), mock.Mock()
        self.developer.runner, self.developer.logcat = runner, logcat
        with mock.patch.object(self.developer, "build", side_effect=dev.DevError("compile failed")), \
                mock.patch.object(dev, "stop_process") as stop:
            with self.assertRaisesRegex(dev.DevError, "compile failed"):
                self.developer.deploy()
        stop.assert_not_called()
        self.assertIs(self.developer.runner, runner)
        self.assertIs(self.developer.logcat, logcat)

    def test_reads_identity_from_signed_package(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            package = root / "local/app-output/esp32s31-korvo-1/pxa-game-render-bench.pxa"
            package.parent.mkdir(parents=True)
            package.touch()
            with mock.patch.object(dev, "ROOT", root), \
                    mock.patch.object(self.developer, "build"), \
                    mock.patch.object(self.developer, "install"), \
                    mock.patch.object(self.developer, "start_app"), \
                    mock.patch.object(dev, "package_manifest_identity",
                                      return_value="pxa-cpp-counter") as read_identity:
                self.developer.deploy()
            read_identity.assert_called_once_with(package)
            self.assertEqual(self.developer.package_id, "pxa-cpp-counter")

    def test_retries_catalog_sync_delay(self) -> None:
        with mock.patch.object(dev, "stream_command", side_effect=[
            dev.DevError("pxadb: error: package_launch_failed"), None,
        ]) as run, mock.patch.object(dev.time, "sleep") as sleep:
            self.developer.start_app()
        self.assertEqual(run.call_count, 2)
        sleep.assert_called_once_with(0.3)

    def test_other_launch_errors_are_not_retried(self) -> None:
        with mock.patch.object(dev, "stream_command", side_effect=dev.DevError(
            "pxadb: error: pxa_unavailable"
        )) as run, mock.patch.object(dev.time, "sleep") as sleep:
            with self.assertRaises(dev.DevError):
                self.developer.start_app()
        run.assert_called_once()
        sleep.assert_not_called()

    def test_launch_uses_package_manifest_identity(self) -> None:
        self.developer.package_id = "pxa-cpp-counter"
        with mock.patch.object(dev, "stream_command") as run:
            self.developer.start_app()
        command = run.call_args.args[0]
        self.assertEqual(command[2:5], ["package", "run", "pxa-cpp-counter"])

    def test_build_selects_board_architecture(self) -> None:
        for board, target in (
            ("esp-mosaico", "esp32s31"),
            ("esp32s31-korvo-1", "esp32s31"),
            ("doit-gamekit", "esp32s31"),
            ("pai-touch", "esp32s3"),
            ("sensecap-watcher", "esp32s3"),
        ):
            with self.subTest(board=board), \
                    mock.patch.object(dev, "stream_command") as run:
                self.developer.args.board = board
                self.developer.build()
                command = run.call_args.args[0]
                self.assertEqual(command[command.index("--target") + 1], target)
                self.assertIn("--aot-only", command)

    def test_doit_uart_default_applies_to_install_run_and_logs(self) -> None:
        self.developer.args.board = "doit-gamekit"
        self.developer.args.baud = None
        with mock.patch.dict(os.environ, {}, clear=True):
            for arguments in (("package", "install", "store.pxa"),
                              ("package", "run", "pxa-store"), ("logcat",)):
                with self.subTest(arguments=arguments):
                    command = self.developer.pxadb(*arguments)
                    self.assertEqual(command[-2:], ["--baud", "2000000"])

    def test_doit_uart_explicit_and_environment_overrides(self) -> None:
        self.developer.args.board = "doit-gamekit"
        with mock.patch.dict(os.environ, {"PXADB_BAUD": "460800"}):
            self.developer.args.baud = None
            self.assertNotIn("--baud", self.developer.pxadb("info"))
            self.developer.args.baud = 921600
            self.assertEqual(self.developer.pxadb("info")[-2:], ["--baud", "921600"])

    def test_other_boards_keep_pxadb_baud_default(self) -> None:
        self.developer.args.baud = None
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertNotIn("--baud", self.developer.pxadb("info"))


class SimulatorProfileTest(unittest.TestCase):
    def test_board_selects_matching_profile(self) -> None:
        for board in ("sensecap-watcher", "esp-mosaico"):
            with self.subTest(board=board), \
                    mock.patch.object(sys, "argv", ["dev.sh", "sim", "pixel-dungeon",
                                                     "--board", board]):
                args = dev.parse_arguments()
                self.assertEqual(args.profile, board)

    def test_explicit_profile_overrides_board(self) -> None:
        with mock.patch.object(sys, "argv", ["dev.sh", "sim", "pixel-dungeon",
                                             "--board", "sensecap-watcher",
                                             "--profile", "pai-touch"]):
            args = dev.parse_arguments()
        self.assertEqual(args.profile, "pai-touch")

    def test_instance_is_only_supported_for_simulators(self) -> None:
        with mock.patch.object(sys, "argv", ["dev.sh", "sim", "voxel-craft",
                                             "--instance", "dpi-qa"]):
            self.assertEqual(dev.parse_arguments().instance, "dpi-qa")
        with mock.patch.object(sys, "argv", ["dev.sh", "device", "voxel-craft",
                                             "--instance", "dpi-qa"]), \
                mock.patch.object(sys, "stderr"), self.assertRaises(SystemExit):
            dev.parse_arguments()


class SimulatorLaunchTest(unittest.TestCase):
    def setUp(self) -> None:
        self.args = SimpleNamespace(
            mode="sim", app_id="voxel-craft", board="esp-mosaico",
            profile="esp-mosaico", instance=None,
        )
        self.developer = dev.Developer(self.args, mock.Mock(), pathlib.Path("."))
        # Launch must use the manifest ID, not the source directory name.
        self.developer.package_id = "pxa-cpp-counter"

    def test_build_omits_wasm_fallback(self) -> None:
        with mock.patch.object(dev, "stream_command") as stream:
            self.developer.build()
        command = stream.call_args.args[0]
        self.assertEqual(command[command.index("--target") + 1], "simulator")
        self.assertIn("--aot-only", command)

    def test_opens_app_inside_full_system(self) -> None:
        with mock.patch.object(dev, "start_logged_process") as start:
            self.developer.start_app()
        command = start.call_args.args[0]
        self.assertEqual(command, [
            str(dev.SIMULATOR_TOOL), "ui", "run", "--profile", "esp-mosaico",
            "--", "--launch", "pxa-cpp-counter",
        ])
        self.assertIs(self.developer.runner, start.return_value)

    def test_instance_routes_install_and_system_to_same_state(self) -> None:
        self.args.instance = "dpi-qa"
        with mock.patch.object(dev, "stream_command") as stream, \
                mock.patch.object(dev, "start_logged_process") as start:
            self.developer.start_simulator_service()
            self.developer.install()
            self.developer.start_app()
        service = stream.call_args_list[0].args[0]
        install = stream.call_args_list[1].args[0]
        system = start.call_args.args[0]
        self.assertEqual(service[-4:], ["--profile", "esp-mosaico", "--instance", "dpi-qa"])
        self.assertEqual(install[-2:], ["--simulator", "esp-mosaico@dpi-qa"])
        self.assertEqual(system[-5:], ["--instance", "dpi-qa", "--", "--launch", "pxa-cpp-counter"])


@unittest.skipUnless(hasattr(os, "fork") and pathlib.Path("/proc").is_dir(),
                     "process-group cleanup integration requires Linux")
class ProcessCleanupTest(unittest.TestCase):
    def test_cleans_runner_when_shell_exits_first(self) -> None:
        self.check_process_group_cleanup(False)

    def test_cleans_runner_when_shell_already_exited(self) -> None:
        self.check_process_group_cleanup(True)

    def check_process_group_cleanup(self, shell_exited: bool) -> None:
        with tempfile.TemporaryDirectory() as directory:
            ready = pathlib.Path(directory) / "runner.pid"
            script = """
import os, pathlib, signal, sys, time
if os.fork() == 0:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    pathlib.Path(sys.argv[1]).write_text(str(os.getpid()))
    time.sleep(60)
elif sys.argv[2] == 'stay':
    time.sleep(60)
"""
            process = subprocess.Popen([sys.executable, "-c", script, str(ready),
                                        "exit" if shell_exited else "stay"],
                                       start_new_session=True)
            try:
                deadline = time.monotonic() + 3
                while (not ready.exists() or not ready.stat().st_size) and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists() and ready.stat().st_size)
                runner_pid = int(ready.read_text())
                if shell_exited:
                    process.wait(timeout=3)
                dev.stop_process(process, "test runner", mock.Mock(), timeout=0.15)
                # A killed orphan may briefly await reaping by the OS.
                state = pathlib.Path(f"/proc/{runner_pid}/stat")
                deadline = time.monotonic() + 2
                while state.exists() and state.read_text().split()[2] != "Z" \
                        and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(not state.exists() or state.read_text().split()[2] == "Z")
                self.assertIsNotNone(process.returncode)
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()


if __name__ == "__main__":
    unittest.main()
