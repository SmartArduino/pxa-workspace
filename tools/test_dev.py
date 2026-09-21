import pathlib
import sys
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


if __name__ == "__main__":
    unittest.main()
