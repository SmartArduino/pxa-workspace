import os
import errno
import pathlib
import sys
import threading
import unittest
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "pxadb"))

import pxadb

from pxadb_gui.geometry import device_point, fitted_rect
from pxadb_gui.images import capture_to_image
from pxadb_gui.panels import human_size
from pxadb_gui.session import (
    DeviceChoice,
    DeviceSession,
    InputQueue,
    SessionSignals,
    reconnect_target,
    screenshot_request,
    transport_failed,
)


class GeometryTest(unittest.TestCase):
    def test_fitted_rect_centers_content(self) -> None:
        self.assertEqual(fitted_rect(100, 100, 200, 100), (50.0, 0.0, 100.0, 100.0))
        self.assertEqual(fitted_rect(0, 10, 200, 100), (0.0, 0.0, 0.0, 0.0))

    def test_device_point_maps_corners_and_center(self) -> None:
        self.assertEqual(device_point(50, 50, 100, 100, 200, 100), (0, 50))
        self.assertEqual(device_point(149, 50, 100, 100, 200, 100), (99, 50))
        self.assertEqual(device_point(100, 50, 100, 100, 200, 100), (50, 50))

    def test_device_point_rejects_letterbox_area(self) -> None:
        self.assertIsNone(device_point(10, 50, 100, 100, 200, 100))
        self.assertIsNone(device_point(50, 10, 100, 50, 100, 100))
        self.assertIsNone(device_point(0, 0, 0, 0, 200, 200))


class InputQueueTest(unittest.TestCase):
    def test_moves_coalesce_to_the_newest_sample(self) -> None:
        queue = InputQueue()
        queue.pointer("move", 1, 1)
        queue.pointer("move", 2, 2)
        queue.pointer("move", 3, 3)
        self.assertEqual(len(queue), 1)
        self.assertEqual(queue.pop(), "INPUT POINTER MOVE 3 3 0")
        self.assertIsNone(queue.pop())

    def test_pointer_edge_drops_stale_moves_and_keeps_order(self) -> None:
        queue = InputQueue()
        queue.pointer("move", 1, 1)
        queue.pointer("down", 2, 2)
        queue.pointer("move", 3, 3)
        queue.pointer("move", 4, 4)
        queue.pointer("up", 5, 5)
        self.assertEqual([
            queue.pop(), queue.pop(), queue.pop(), queue.pop(),
        ], [
            "INPUT POINTER DOWN 2 2 0",
            "INPUT POINTER MOVE 4 4 0",
            "INPUT POINTER UP 5 5 0",
            None,
        ])

    def test_tap_and_keys_keep_fifo_order(self) -> None:
        queue = InputQueue()
        queue.tap(7, 8)
        queue.key("home")
        queue.command("INPUT SYNC")
        self.assertEqual([queue.pop(), queue.pop(), queue.pop()],
                         ["INPUT TAP 7 8", "INPUT KEY HOME", "INPUT SYNC"])


class TransportFailureTest(unittest.TestCase):
    def test_port_loss_is_fatal(self) -> None:
        self.assertTrue(transport_failed(OSError(errno.EIO, "Input/output error")))
        self.assertTrue(transport_failed(OSError(errno.ENXIO, "no such device")))
        self.assertTrue(transport_failed(Exception(5, "Input/output error")))
        self.assertTrue(transport_failed(OSError(
            "device reports readiness to read but returned no data")))
        self.assertTrue(transport_failed(pxadb.PxaDbError(
            "PXADB2 peer disconnected")))
        self.assertTrue(transport_failed(pxadb.PxaDbError(
            "write failed: [Errno 5] Input/output error")))

    def test_device_level_errors_keep_the_connection(self) -> None:
        self.assertFalse(transport_failed(pxadb.PxaDbError(
            "device did not respond to INPUT; verify that PXADB is enabled")))
        self.assertFalse(transport_failed(pxadb.PxaDbError(
            "screenshot_rate_limited")))
        self.assertFalse(transport_failed(ValueError("invalid path")))


class ReconnectTargetTest(unittest.TestCase):
    def test_usb_device_is_found_by_serial_after_reenumeration(self) -> None:
        devices = [
            pxadb.Device("/dev/ttyACM1", "USB JTAG",
                         "protocol=1;serial=aa:bb;target=esp32s3"),
            pxadb.Device("/dev/ttyACM2", "other", "protocol=1;serial=cc:dd"),
        ]
        choice = DeviceChoice("/dev/ttyACM0", "/dev/ttyACM0")
        with mock.patch.object(pxadb, "discover_devices", return_value=devices):
            found = reconnect_target(choice, "aa:bb")
        self.assertIsNotNone(found)
        self.assertEqual(found.target, "/dev/ttyACM1")

    def test_single_device_is_a_fallback_when_the_serial_is_unknown(self) -> None:
        devices = [pxadb.Device("/dev/ttyACM3", "USB", "protocol=1;serial=zz")]
        choice = DeviceChoice("/dev/ttyACM0", "/dev/ttyACM0")
        with mock.patch.object(pxadb, "discover_devices", return_value=devices):
            found = reconnect_target(choice, "aa:bb")
        self.assertIsNotNone(found)
        self.assertEqual(found.target, "/dev/ttyACM3")

    def test_simulator_and_tcp_endpoints_keep_their_address(self) -> None:
        simulator = DeviceChoice("simulator:pai-touch", "unix:/tmp/x.sock",
                                 "", "pai-touch")
        self.assertEqual(reconnect_target(simulator, ""), simulator)
        tcp = DeviceChoice("tcp:host:9222", "tcp:host:9222|token")
        self.assertEqual(reconnect_target(tcp, ""), tcp)


class ScreenshotRetryTest(unittest.TestCase):
    @staticmethod
    def _session() -> DeviceSession:
        session = object.__new__(DeviceSession)
        session.signals = SessionSignals()
        session._condition = threading.Condition()
        session._client = mock.MagicMock()
        session._client.device_info = "protocol=1;capabilities=screenshot-jpeg"
        session._interval = 0.3
        session._next_capture = 0.0
        session._preview_error = ""
        return session

    def test_preview_retries_a_dropped_data_line(self) -> None:
        session = self._session()
        session._client.request.side_effect = [
            pxadb.PxaDbError("screenshot chunk offset mismatch: expected 1, got 2"),
            [pxadb.Frame(1, "OK", "")],
        ]
        with mock.patch.object(pxadb, "screenshot_capture", return_value=pxadb.Screenshot(
                b"jpeg", {"format": "jpeg"})), \
                mock.patch("pxadb_gui.session.capture_to_image", return_value="image"), \
                mock.patch("pxadb_gui.session.time.sleep") as sleep:
            frame = session._capture_frame()
        self.assertIsNotNone(frame)
        self.assertEqual(session._client.request.call_count, 2)
        sleep.assert_called_once_with(0.3)

    def test_preview_does_not_retry_rate_limiting(self) -> None:
        session = self._session()
        session._client.request.side_effect = pxadb.PxaDbError(
            "screenshot_rate_limited")
        self.assertIsNone(session._capture_frame())
        self.assertEqual(session._client.request.call_count, 1)


class ScreenViewTest(unittest.TestCase):
    def test_frame_during_drag_keeps_the_gesture_alive(self) -> None:
        from PySide6.QtCore import QPoint, Qt
        from PySide6.QtGui import QImage
        from PySide6.QtTest import QTest
        from PySide6.QtWidgets import QApplication

        from pxadb_gui.preview import ScreenView

        QApplication.instance() or QApplication([])
        view = ScreenView()
        view.resize(400, 400)
        view.set_image(QImage(100, 100, QImage.Format.Format_RGB16))
        events: list[tuple] = []
        view.pointer.connect(lambda action, x, y: events.append((action, x, y)))
        view.tap.connect(lambda x, y: events.append(("tap", x, y)))

        QTest.mousePress(view, Qt.MouseButton.LeftButton, pos=QPoint(200, 200))
        QTest.mouseMove(view, QPoint(240, 220))
        # A preview frame arriving mid-drag must not cancel the gesture.
        view.set_image(QImage(100, 100, QImage.Format.Format_RGB16))
        QTest.mouseRelease(view, Qt.MouseButton.LeftButton, pos=QPoint(240, 220))

        self.assertEqual([event[0] for event in events], ["down", "move", "up"])

    def test_click_without_movement_becomes_a_tap(self) -> None:
        from PySide6.QtCore import QPoint, Qt
        from PySide6.QtGui import QImage
        from PySide6.QtTest import QTest
        from PySide6.QtWidgets import QApplication

        from pxadb_gui.preview import ScreenView

        QApplication.instance() or QApplication([])
        view = ScreenView()
        view.resize(400, 400)
        view.set_image(QImage(100, 100, QImage.Format.Format_RGB16))
        events: list[tuple] = []
        view.pointer.connect(lambda action, x, y: events.append((action, x, y)))
        view.tap.connect(lambda x, y: events.append(("tap", x, y)))

        QTest.mouseClick(view, Qt.MouseButton.LeftButton, pos=QPoint(200, 200))
        self.assertEqual([event[0] for event in events], ["tap"])


class ScreenshotRequestTest(unittest.TestCase):
    def test_prefers_jpeg_when_the_firmware_advertises_it(self) -> None:
        self.assertEqual(
            screenshot_request(
                "protocol=1;capabilities=info,screenshot-rgb565,screenshot-jpeg"),
            "SCREENSHOT JPEG")

    def test_falls_back_to_rgb565_without_the_capability(self) -> None:
        self.assertEqual(
            screenshot_request(
                "protocol=1;capabilities=info,screenshot-rgb565"),
            "SCREENSHOT")
        self.assertEqual(screenshot_request(""), "SCREENSHOT")
        self.assertEqual(
            screenshot_request("protocol=2;screenshot=png;simulator=1"),
            "SCREENSHOT")


class FakeCapture:
    def __init__(self, data: bytes, metadata: dict[str, str]) -> None:
        self.data = data
        self.metadata = metadata


class ImageConversionTest(unittest.TestCase):
    def test_rgb565_frames_map_to_qt_pixels(self) -> None:
        pixels = bytes((0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00))
        capture = FakeCapture(pixels, {
            "format": "rgb565le", "width": "3", "height": "1", "stride": "6",
        })
        image = capture_to_image(capture)
        self.assertEqual((image.width(), image.height()), (3, 1))
        self.assertEqual(image.pixelColor(0, 0).getRgb()[:3], (255, 0, 0))
        self.assertEqual(image.pixelColor(1, 0).getRgb()[:3], (0, 255, 0))
        self.assertEqual(image.pixelColor(2, 0).getRgb()[:3], (0, 0, 255))

    def test_png_frames_decode_directly(self) -> None:
        from PySide6.QtCore import QBuffer, QByteArray, QIODevice
        from PySide6.QtGui import QImage

        source = QImage(2, 1, QImage.Format.Format_RGB32)
        source.fill(0xFF3366)
        payload = QByteArray()
        buffer = QBuffer(payload)
        buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        self.assertTrue(source.save(buffer, "PNG"))
        buffer.close()
        image = capture_to_image(FakeCapture(bytes(payload), {"format": "png"}))
        self.assertEqual((image.width(), image.height()), (2, 1))
        self.assertEqual(image.pixelColor(1, 0).getRgb()[:3], (0xFF, 0x33, 0x66))

    def test_human_size_formats_units(self) -> None:
        self.assertEqual(human_size(512), "512 B")
        self.assertEqual(human_size(2048), "2.0 KiB")
        self.assertEqual(human_size(3 * 1024 * 1024), "3.0 MiB")


if __name__ == "__main__":
    unittest.main()
