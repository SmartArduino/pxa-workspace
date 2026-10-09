import pathlib
import sys
import tempfile
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
            ("pai-touch", "esp32s3"),
            ("sensecap-watcher", "esp32s3"),
        ):
            with self.subTest(board=board), \
                    mock.patch.object(dev, "stream_command") as run:
                self.developer.args.board = board
                self.developer.build()
                command = run.call_args.args[0]
                self.assertEqual(command[command.index("--target") + 1], target)


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


if __name__ == "__main__":
    unittest.main()
