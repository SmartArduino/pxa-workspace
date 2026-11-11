#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression coverage for target selection and native USB re-enumeration."""
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import usb_esptool as tool

MAC = "1c:29:04:d0:af:0a"


def usb_port(device="/dev/ttyACM3", product=tool.APP_PRODUCT, mac=MAC, vid=tool.USB_VID):
    return SimpleNamespace(device=device, product=product, serial_number=mac, vid=vid)


class UsbDownloadTests(unittest.TestCase):
    def setUp(self):
        access = patch.object(tool.os, 'access', return_value=True)
        self.access = access.start()
        self.addCleanup(access.stop)
        self.arguments = ["--chip", "esp32p4", "-p", "/dev/ttyACM3", "-b", "460800",
                          "--before=default-reset", "--after=hard-reset", "--no-stub",
                          "write-flash", "@flash_args", "--skip-flashed"]

    @patch.object(tool, "request_download")
    @patch.object(tool, "select_port")
    @patch.object(tool, "wait_for_download", return_value="/dev/ttyACM7")
    def test_application_reconnect_preserves_flash_arguments(self, wait, select, request):
        select.return_value = usb_port()
        arguments = tool.prepare_download(self.arguments)
        request.assert_called_once_with("/dev/ttyACM3")
        wait.assert_called_once_with(MAC)
        self.assertEqual(arguments, ["--port", "/dev/ttyACM7", "--before", "no-reset",
                                     *self.arguments[:2], *self.arguments[4:6],
                                     *self.arguments[7:]])

    @patch.object(tool, "request_download")
    @patch.object(tool, "select_port")
    def test_rom_does_not_receive_touch(self, select, request):
        select.return_value = usb_port(product=tool.ROM_PRODUCT)
        result = tool.prepare_download(self.arguments)
        self.assertIn("--no-stub", result)
        self.assertEqual(result[result.index("--before") + 1], "no-reset")
        self.assertNotIn("--before=default-reset", result)
        request.assert_not_called()

    @patch.object(tool, "select_port")
    def test_offline_image_generation_does_not_inspect_usb(self, select):
        arguments = ["--chip", "esp32p4", "elf2image", "-o", "app.bin", "app.elf"]
        self.assertEqual(tool.prepare_download(arguments), arguments)
        select.assert_not_called()

    @patch.object(tool, "select_port")
    def test_explicit_no_reset_is_respected(self, select):
        arguments = ["--before", "no-reset", "write-flash", "@flash_args"]
        self.assertEqual(tool.prepare_download(arguments), arguments)
        select.assert_not_called()

    @patch.dict(tool.os.environ, {"ESPPORT": "/dev/ttyACM9"})
    @patch.object(tool, "select_port", return_value=None)
    def test_environment_port_is_used_without_command_line_port(self, select):
        arguments = ["--chip", "esp32p4", "write-flash", "@flash_args"]
        self.assertEqual(tool.prepare_download(arguments), arguments)
        select.assert_called_once_with("/dev/ttyACM9")

    @patch.dict(tool.os.environ, {"ESPPORT": "/dev/ttyACM9"})
    @patch.object(tool, "select_port", return_value=None)
    def test_command_line_port_overrides_environment(self, select):
        self.assertEqual(tool.prepare_download(self.arguments), self.arguments)
        select.assert_called_once_with("/dev/ttyACM3")

    @patch.object(tool, "request_download")
    @patch.object(tool, "select_port")
    def test_other_usb_product_passes_through(self, select, request):
        select.return_value = usb_port(product="USB JTAG/serial debug unit")
        self.assertEqual(tool.prepare_download(self.arguments), self.arguments)
        request.assert_not_called()

    @patch.object(tool, "request_download")
    @patch.object(tool, "select_port")
    def test_missing_mac_fails_before_touch(self, select, request):
        select.return_value = usb_port(mac="123456")
        with self.assertRaisesRegex(RuntimeError, "MAC"):
            tool.prepare_download(self.arguments)
        request.assert_not_called()

    @patch.object(tool, "request_download", side_effect=BrokenPipeError("USB reset"))
    @patch.object(tool, "select_port")
    @patch.object(tool, "wait_for_download", return_value="/dev/ttyACM7")
    def test_control_disconnect_continues_only_after_matching_rom(self, wait, select, request):
        select.return_value = usb_port()
        result = tool.prepare_download(self.arguments)
        self.assertEqual(result[:4], ["--port", "/dev/ttyACM7", "--before", "no-reset"])
        wait.assert_called_once_with(MAC)

    @patch.object(tool.list_ports, "comports")
    def test_ambiguous_boards_require_explicit_port(self, ports):
        ports.return_value = [usb_port(), usb_port("/dev/ttyACM6", mac="1c:29:04:d0:af:10")]
        with self.assertRaisesRegex(RuntimeError, "Specify -p"):
            tool.select_port(None)
        self.assertEqual(tool.select_port("/dev/ttyACM3").serial_number, MAC)

    @patch.object(tool.list_ports, "comports")
    def test_automatic_selection_ignores_other_usb_devices(self, ports):
        ports.return_value = [usb_port(product="Espressif Device", mac="123456"), usb_port()]
        self.assertEqual(tool.select_port(None).serial_number, MAC)

    @patch.object(tool.time, "sleep")
    @patch.object(tool.list_ports, "comports")
    def test_reconnect_waits_for_same_mac_and_rom_product(self, ports, sleep):
        other_rom = usb_port("/dev/ttyACM8", product=tool.ROM_PRODUCT, mac="1c:29:04:d0:af:10")
        target_rom = usb_port("/dev/ttyACM7", product=tool.ROM_PRODUCT, mac=MAC.upper())
        ports.side_effect = [[usb_port(), other_rom], [], [other_rom, target_rom]]
        self.assertEqual(tool.wait_for_download(MAC), "/dev/ttyACM7")
        self.assertEqual(ports.call_count, 3)

    @patch.object(tool.time, "monotonic", side_effect=[0, 0, 16])
    @patch.object(tool.time, "sleep")
    @patch.object(tool.list_ports, "comports", return_value=[])
    def test_reconnect_timeout(self, ports, sleep, monotonic):
        with self.assertRaisesRegex(RuntimeError, "Timed out"):
            tool.wait_for_download(MAC)

    @unittest.skipUnless(tool.os.name == 'posix', 'POSIX device permissions')
    @patch.object(tool.time, 'sleep')
    @patch.object(tool.list_ports, 'comports')
    def test_matching_rom_waits_for_udev_permissions_without_opening(self, ports, sleep):
        ports.return_value = [usb_port('/dev/rom', product=tool.ROM_PRODUCT)]
        self.access.side_effect = [False, False, True]
        with patch.object(tool.serial, 'Serial') as serial:
            self.assertEqual(tool.wait_for_download(MAC), '/dev/rom')
            serial.assert_not_called()
        self.assertEqual(ports.call_count, 3)

    def test_replaces_supported_port_spellings(self):
        for option in (["-p", "old"], ["--port", "old"], ["--port=old"], ["-pold"]):
            self.assertEqual(tool.download_arguments([*option, "write-flash", "@flash_args"], "new"),
                             ["--port", "new", "--before", "no-reset", "write-flash", "@flash_args"])

    def test_download_connection_replaces_separate_reset_option(self):
        arguments = ["--before", "default-reset", "-p", "old", "write-flash", "@flash_args"]
        self.assertEqual(tool.download_arguments(arguments, "new"),
                         ["--port", "new", "--before", "no-reset", "write-flash", "@flash_args"])

    @patch.object(tool, "preserve_modem_lines_on_close")
    @patch.object(tool.time, "sleep")
    @patch.object(tool.serial, "Serial")
    def test_touch_opens_configured_port_and_closes_it(self, serial_class, sleep, preserve):
        port = Mock()
        port.__enter__ = Mock(return_value=port)
        port.__exit__ = Mock(return_value=False)
        serial_class.return_value = port
        tool.request_download("/dev/ttyACM3")
        self.assertEqual(port.port, "/dev/ttyACM3")
        self.assertFalse(port.dtr)
        self.assertFalse(port.rts)
        preserve.assert_called_once_with(port)
        port.__exit__.assert_called_once()

    @patch.object(tool, "preserve_modem_lines_on_close")
    @patch.object(tool.time, "sleep")
    @patch.object(tool.serial.Serial, "close", autospec=True)
    @patch.object(tool.serial.Serial, "open", autospec=True)
    def test_pyserial_context_opens_only_after_configuration(self, open_port, close_port, sleep, preserve):
        def check_configuration(port):
            self.assertEqual(port.port, "/dev/ttyACM3")
            self.assertEqual(port.baudrate, 1200)
            self.assertTrue(port.dtr)
            self.assertFalse(port.rts)
        open_port.side_effect = check_configuration
        tool.request_download("/dev/ttyACM3")
        open_port.assert_called_once()
        self.assertGreaterEqual(close_port.call_count, 1)

    @patch.object(tool, "prepare_download", side_effect=RuntimeError("No board"))
    @patch.object(tool.subprocess, "call")
    def test_reconnect_failure_never_runs_esptool(self, call, prepare):
        self.assertEqual(tool.main(["esptool", *self.arguments]), 1)
        call.assert_not_called()

    @patch.object(tool, "prepare_download", side_effect=lambda arguments: arguments)
    @patch.object(tool.subprocess, "call", return_value=2)
    def test_esptool_failure_is_propagated(self, call, prepare):
        self.assertEqual(tool.main(["esptool", "--chip", "esp32p4", "elf2image"]), 2)
        call.assert_called_once_with([sys.executable, "-m", "esptool", "--chip", "esp32p4", "elf2image"])


if __name__ == "__main__":
    unittest.main()
