import base64
import contextlib
import hashlib
import io
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pxadb


def encoded_frame(sequence: int, kind: str, payload: str) -> bytes:
    encoded = "-" if not payload else base64.b64encode(payload.encode("utf-8")).decode("ascii")
    return f"PXADB1 {sequence} {kind} {encoded}\n".encode("ascii")


class FakeSerial:
    def __init__(self, lines: list[bytes]) -> None:
        self.lines = list(lines)
        self.writes: list[bytes] = []
        self.in_waiting = 0
        self.closed = False
        self.output_resets = 0

    def write(self, value: bytes) -> int:
        self.writes.append(value)
        return len(value)

    def flush(self) -> None:
        pass

    def reset_output_buffer(self) -> None:
        self.output_resets += 1

    def readline(self) -> bytes:
        return self.lines.pop(0) if self.lines else b""

    def close(self) -> None:
        self.closed = True


class FakeBinaryTransport:
    def __init__(self, frames: list[tuple[int, int, int, bytes]]) -> None:
        self.frames = list(frames)
        self.writes: list[tuple[int, int, int, bytes]] = []

    def write_frame(self, kind: int, flags: int, sequence: int, payload: bytes) -> None:
        self.writes.append((kind, flags, sequence, payload))

    def read_frame(self) -> tuple[int, int, int, bytes] | None:
        return self.frames.pop(0) if self.frames else None

    def close(self) -> None:
        pass


def fake_client(lines: list[bytes]) -> pxadb.PxaDbClient:
    client = object.__new__(pxadb.PxaDbClient)
    client.serial = FakeSerial(lines)
    client.timeout = 0.05
    client.sequence = 0
    client.pending_frames = []
    client.log_subscribed = False
    client.device_info = ""
    return client


class PxaDbLogStreamingTest(unittest.TestCase):
    def test_request_ignores_raw_console_output_before_response(self) -> None:
        client = fake_client([
            b"I (42) app: boot complete\r\n",
            encoded_frame(1, "OK", "ready"),
        ])
        frames = client.request("HELLO")
        self.assertEqual(frames[-1].payload, "ready")

    def test_hello_retries_after_a_missed_response(self) -> None:
        client = fake_client([encoded_frame(2, "OK", "ready")])
        calls = 0
        timeouts: list[float | None] = []

        def request(command: str,
                    timeout: float | None = None) -> list[pxadb.Frame]:
            nonlocal calls
            calls += 1
            timeouts.append(timeout)
            if calls == 1:
                raise pxadb.PxaDbError("device did not respond to HELLO")
            return [pxadb.Frame(2, "OK", "ready")]

        client.request = request  # type: ignore[method-assign]
        self.assertEqual(client.hello(attempts=2), "ready")
        self.assertEqual(calls, 2)
        self.assertTrue(all(value is not None and value <= client.timeout
                            for value in timeouts))

    def test_command_write_does_not_wait_for_driver_flush(self) -> None:
        client = fake_client([])
        client._write_command(7, "PING")
        self.assertEqual(client.serial.writes, [b"PXADB1 7 PING\n"])

    def test_close_preserves_pointer_session_until_explicit_end(self) -> None:
        client = fake_client([])
        client.device_info = "protocol=1"
        serial_port = client.serial
        client.close()
        self.assertEqual(serial_port.writes, [])
        self.assertEqual(serial_port.output_resets, 1)
        self.assertTrue(serial_port.closed)

    def test_explicit_session_end_cancels_pointer_before_bye(self) -> None:
        client = fake_client([
            encoded_frame(1, "OK", "processed"),
            encoded_frame(2, "OK", "disconnected"),
        ])
        pxadb.end_control_session(client)
        writes = [entry.decode("ascii") for entry in client.serial.writes]
        self.assertIn("INPUT CANCEL", writes[0])
        self.assertIn("BYE", writes[1])

    def test_offset_upload_seeks_to_confirmed_position(self) -> None:
        payload = b"abc"
        client = fake_client([
            encoded_frame(1, "READY", "3"),
            encoded_frame(2, "READY", "1"),
            encoded_frame(3, "OK", ""),
        ])
        client.device_info = "max_chunk=2;fs_offset=1"
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "payload.bin"
            source.write_bytes(payload)
            pxadb.NormalFsClient(client).put(source, "pxa-state/inbox/payload.bin")
        writes = [entry.decode("ascii") for entry in client.serial.writes]
        digest = hashlib.sha256(payload).hexdigest()
        self.assertIn(f"FSPUT cHhhLXN0YXRlL2luYm94L3BheWxvYWQuYmlu 3 {digest}", writes[0])
        self.assertIn("FSDATA 0 YWI=", writes[1])
        self.assertIn("FSDATA 2 Yw==", writes[2])

    def test_uart_upload_chunk_override(self) -> None:
        payload = b"abcdef"
        client = fake_client([
            encoded_frame(1, "READY", "6"),
            encoded_frame(2, "READY", "4"),
            encoded_frame(3, "READY", "2"),
            encoded_frame(4, "OK", ""),
        ])
        client.device_info = "max_chunk=1024;fs_offset=1"
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "payload.bin"
            source.write_bytes(payload)
            with mock.patch.dict("os.environ", {"PXADB_UART_UPLOAD_CHUNK": "2"}):
                pxadb.NormalFsClient(client).put(source, "pxa-state/inbox/payload.bin")
        writes = [entry.decode("ascii") for entry in client.serial.writes]
        self.assertEqual([int(entry.split(" ")[3]) for entry in writes[1:]],
                         [0, 2, 4])

    def test_uart_upload_defaults_to_safe_chunk(self) -> None:
        payload = b"x" * 130
        client = fake_client([
            encoded_frame(1, "READY", "130"),
            encoded_frame(2, "READY", "66"),
            encoded_frame(3, "READY", "2"),
            encoded_frame(4, "OK", ""),
        ])
        client.device_info = "max_chunk=1024;fs_offset=1"
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "payload.bin"
            source.write_bytes(payload)
            with mock.patch.dict("os.environ", {}, clear=True):
                pxadb.NormalFsClient(client).put(source, "pxa-state/inbox/payload.bin")
        writes = [entry.decode("ascii") for entry in client.serial.writes]
        self.assertEqual([int(entry.split(" ")[3]) for entry in writes[1:]],
                         [0, 64, 128])

    def test_binary_upload_keeps_file_data_unencoded(self) -> None:
        responses = [
            (pxadb.BINARY_RESPONSE, 0, 1, b"READY\x003"),
            (pxadb.BINARY_RESPONSE, 0, 2, b"OK\x00"),
        ]
        client = object.__new__(pxadb.PxaDbClient)
        client.binary_transport = FakeBinaryTransport(responses)
        client.tcp_token = ""
        client.timeout = 0.05
        client.sequence = 0
        client.pending_frames = []
        client.log_subscribed = False
        client.device_info = "max_chunk=65536;fs_offset=1;binary=1"
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "payload.bin"
            source.write_bytes(b"abc")
            pxadb.NormalFsClient(client).put(source, "pxa-state/inbox/payload.bin")
        writes = client.binary_transport.writes
        self.assertEqual(writes[0][0], pxadb.BINARY_COMMAND)
        self.assertEqual(writes[1][0], pxadb.BINARY_UPLOAD_DATA)
        self.assertEqual(writes[1][3][8:], b"abc")

    def test_doctor_parser(self) -> None:
        arguments = pxadb.build_parser().parse_args(["doctor", "--port", "/dev/ttyACM1"])
        self.assertEqual(arguments.command, "doctor")
        self.assertEqual(arguments.port, "/dev/ttyACM1")

    def test_simulator_parser_selects_a_unix_socket(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "package", "list", "--simulator", "pai-touch"
        ])
        self.assertTrue(arguments.port.startswith("unix:"))
        self.assertTrue(arguments.port.endswith("/pai-touch.sock"))

    def test_simulator_parser_selects_an_instance_socket(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "package", "list", "--simulator", "pai-touch@demo-a"
        ])
        self.assertTrue(arguments.port.endswith("/pai-touch@demo-a.sock"))

    def test_automatic_selection_uses_the_only_running_simulator(self) -> None:
        simulator = pxadb.Device("simulator:pai-touch", "desktop simulator",
                                 "protocol=2;simulator=1")
        with mock.patch.object(pxadb, "discover_simulators", return_value=[simulator]), \
                mock.patch.object(pxadb, "discover_devices", return_value=[]):
            port = pxadb.resolve_pxadb_port(None, 0.1)
        self.assertTrue(port.startswith("unix:"))
        self.assertTrue(port.endswith("/pai-touch.sock"))

    def test_package_run_launches_the_selected_simulator_profile(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "package", "run", "pxa-voxel-craft", "--simulator", "pai-touch"
        ])
        completed = mock.MagicMock(returncode=0)
        with mock.patch.object(pxadb, "resolve_pxadb_port", return_value=arguments.port), \
                mock.patch.object(pxadb.subprocess, "run", return_value=completed) as run:
            self.assertEqual(pxadb.command_package_run(arguments), 0)
        self.assertEqual(run.call_args.args[0][-4:],
                         ["--profile", "pai-touch", "--installed", "pxa-voxel-craft"])

    def test_package_run_forwards_the_selected_simulator_instance(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "package", "run", "pxa-voxel-craft", "--simulator", "pai-touch@demo-a"
        ])
        completed = mock.MagicMock(returncode=0)
        with mock.patch.object(pxadb, "resolve_pxadb_port", return_value=arguments.port), \
                mock.patch.object(pxadb.subprocess, "run", return_value=completed) as run:
            self.assertEqual(pxadb.command_package_run(arguments), 0)
        self.assertEqual(run.call_args.args[0][-6:], [
            "--profile", "pai-touch", "--instance", "demo-a",
            "--installed", "pxa-voxel-craft",
        ])

    def test_package_run_on_device_requests_a_launch(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "package", "run", "pxa-voxel-craft", "--port", "/dev/ttyACM0"
        ])
        client = mock.MagicMock()
        with mock.patch.object(pxadb, "open_client", return_value=client):
            self.assertEqual(pxadb.command_package_run(arguments), 0)
        client.__enter__().request.assert_called_once_with(
            "PACKAGE run pxa-voxel-craft")

    def test_package_stop_on_device_requests_a_stop(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "package", "stop", "pxa-voxel-craft", "--port", "/dev/ttyACM0"
        ])
        client = mock.MagicMock()
        with mock.patch.object(pxadb, "open_client", return_value=client):
            self.assertEqual(pxadb.command_package_stop(arguments), 0)
        client.__enter__().request.assert_called_once_with(
            "PACKAGE stop pxa-voxel-craft")

    def test_staging_directory_removes_same_identity_pxa_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "pxa-example"
            artifacts = source / "artifacts"
            artifacts.mkdir(parents=True)
            (source / "manifest.pxm").write_bytes(b"manifest")
            (source / "signature.pxs").write_bytes(b"signature")
            (artifacts / "main.wasm").write_bytes(b"wasm")
            client = mock.MagicMock()
            with mock.patch.object(pxadb, "open_file_client", return_value=client), \
                    mock.patch.object(pxadb, "fs_remove") as remove:
                pxadb.stage_package(source, "pxa-example", "/dev/ttyACM0", 1.0)
        root = "pxa-state/inbox/pxa-example"
        self.assertEqual(remove.call_args_list, [
            mock.call(client, f"{root}.pxa", 1.0),
        ])
        self.assertIn(
            mock.call(f"FSRMTREE {pxadb.client_encode_path(root)}", timeout=1.0),
            client.client.request.call_args_list,
        )

    def test_devices_includes_running_simulator(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            socket_path = pathlib.Path(directory) / "pai-touch.sock"
            socket_path.touch()
            client = mock.MagicMock()
            client.device_info = "protocol=2;simulator=1"
            with mock.patch.dict("os.environ", {"PXA_SIMULATOR_SOCKET_ROOT": directory}), \
                    mock.patch.object(pxadb, "open_client",
                                      return_value=contextlib.nullcontext(client)), \
                    mock.patch.object(pxadb, "discover_devices", return_value=[]), \
                    mock.patch("sys.stdout", new_callable=io.StringIO) as output:
                pxadb.command_devices(mock.MagicMock(timeout=0.1))
            self.assertIn("List of devices attached", output.getvalue())
            self.assertIn("simulator:pai-touch\tdevice\tprotocol=2;simulator=1",
                          output.getvalue())

    def test_request_streams_logs_and_raw_crash_output(self) -> None:
        client = fake_client([
            encoded_frame(0, "LOG", "123\tinstall started"),
            b"Guru Meditation Error: Core  0 panic'ed\r\n",
            encoded_frame(1, "PKG", "pxa-test"),
            encoded_frame(1, "OK", "done"),
        ])
        client.log_subscribed = True
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            frames = client.request("PACKAGES")
        self.assertEqual([frame.kind for frame in frames], ["PKG", "OK"])
        self.assertIn("install started", stdout.getvalue())
        self.assertIn("[serial] Guru Meditation Error", stderr.getvalue())

    def test_subscribe_prints_history(self) -> None:
        client = fake_client([
            encoded_frame(1, "LOG", "456\tboot complete"),
            encoded_frame(1, "OK", "subscribed"),
        ])
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            client.subscribe_logs()
        self.assertTrue(client.log_subscribed)
        self.assertIn("boot complete", stdout.getvalue())

    def test_install_accepts_logcat_option(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "package", "install", "package.pxa", "--logcat"
        ])
        self.assertTrue(arguments.logcat)

    def test_poweroff_command_and_unsupported_error(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "poweroff", "--port", "/dev/ttyACM0"
        ])
        client = mock.MagicMock()
        with mock.patch.object(pxadb, "open_client", return_value=client):
            self.assertEqual(pxadb.command_poweroff(arguments), 0)
        client.__enter__().request.assert_called_once_with("POWEROFF")

        client.__enter__().request.side_effect = pxadb.PxaDbError(
            "poweroff_not_supported")
        with mock.patch.object(pxadb, "open_client", return_value=client), \
                self.assertRaisesRegex(pxadb.PxaDbError,
                                       "does not support software power-off"):
            pxadb.command_poweroff(arguments)

    def test_input_and_screenshot_parsers(self) -> None:
        touch = pxadb.build_parser().parse_args([
            "input", "touch", "move", "148", "120", "--port", "/dev/ttyACM0"
        ])
        self.assertEqual((touch.action, touch.x, touch.y), ("move", 148, 120))
        key = pxadb.build_parser().parse_args(["input", "key", "home"])
        self.assertEqual(key.key, "home")
        swipe = pxadb.build_parser().parse_args([
            "input", "swipe", "40", "120", "250", "120",
            "--duration-ms", "300", "--steps", "12",
        ])
        self.assertEqual((swipe.x1, swipe.x2, swipe.duration_ms, swipe.steps),
                         (40, 250, 300, 12))
        sync = pxadb.build_parser().parse_args(["sync"])
        self.assertEqual(sync.command, "sync")
        screenshot = pxadb.build_parser().parse_args([
            "screenshot", "capture.png", "--after-present"
        ])
        self.assertEqual((screenshot.output, screenshot.after_present),
                         ("capture.png", True))

    def test_screenshot_data_frames_decode_binary(self) -> None:
        payload = b"\xff\xd8\x00\xff\xd9"
        frames = [
            pxadb.Frame(1, "META", "format=jpeg;bytes=5"),
            pxadb.Frame(1, "DATA", base64.b64encode(payload).decode("ascii")),
            pxadb.Frame(1, "OK", ""),
        ]
        self.assertEqual(pxadb.screenshot_bytes(frames), payload)

    def test_rgb565_screenshot_checks_offsets_hash_and_converts_png(self) -> None:
        payload = bytes((0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00))
        digest = hashlib.sha256(payload).hexdigest()
        frames = [
            pxadb.Frame(
                1, "META",
                f"format=rgb565le;width=3;height=1;stride=6;bytes=6;sha256={digest}",
            ),
            pxadb.Frame(1, "DATA", "0\t" + base64.b64encode(payload[:4]).decode()),
            pxadb.Frame(1, "DATA", "4\t" + base64.b64encode(payload[4:]).decode()),
            pxadb.Frame(1, "OK", ""),
        ]
        capture = pxadb.screenshot_capture(frames)
        png = pxadb.rgb565le_to_png(capture.data, 3, 1, 6)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        offset = 8
        image_data = b""
        while offset < len(png):
            length = struct.unpack(">I", png[offset:offset + 4])[0]
            kind = png[offset + 4:offset + 8]
            chunk = png[offset + 8:offset + 8 + length]
            if kind == b"IDAT":
                image_data += chunk
            offset += 12 + length
        self.assertEqual(zlib.decompress(image_data),
                         b"\x00\xff\x00\x00\x00\xff\x00\x00\x00\xff")

    def test_screenshot_rejects_wrong_offset_and_hash(self) -> None:
        encoded = base64.b64encode(b"ab").decode()
        with self.assertRaisesRegex(pxadb.PxaDbError, "offset mismatch"):
            pxadb.screenshot_capture([
                pxadb.Frame(1, "META", "format=rgb565le;bytes=2"),
                pxadb.Frame(1, "DATA", "1\t" + encoded),
            ])
        with self.assertRaisesRegex(pxadb.PxaDbError, "SHA-256"):
            pxadb.screenshot_capture([
                pxadb.Frame(1, "META", "format=rgb565le;bytes=2;sha256=00"),
                pxadb.Frame(1, "DATA", "0\t" + encoded),
            ])

    def test_device_scenario_uses_shared_command_syntax(self) -> None:
        commands: list[str] = []

        class Client:
            timeout = 0.1

            def request(self, command: str, timeout: float | None = None) -> list[pxadb.Frame]:
                del timeout
                commands.append(command)
                return [pxadb.Frame(1, "OK", "")]

        with tempfile.TemporaryDirectory() as directory:
            script = pathlib.Path(directory) / "input.pxauto"
            script.write_text(
                "tap 10 20\n"
                "swipe 1 2 3 4 50\n"
                "pointer down 5 6\n"
                "pointer up 5 6\n"
                "key home\n"
                "sync\n"
            )
            pxadb.run_device_scenario(Client(), script)  # type: ignore[arg-type]
        self.assertEqual(commands, [
            "INPUT TAP 10 20",
            "INPUT SWIPE 1 2 3 4 50 12",
            "INPUT POINTER DOWN 5 6 0",
            "INPUT POINTER UP 5 6 0",
            "INPUT KEY HOME",
            "INPUT SYNC",
        ])


class PxaDbFileTransferTest(unittest.TestCase):
    class ScriptedClient:
        def __init__(self, responses: list[list[pxadb.Frame]]) -> None:
            self.responses = responses
            self.commands: list[str] = []
            self.device_info = "fs_sha256=1"

        def request(self, command: str,
                    timeout: float | None = None) -> list[pxadb.Frame]:
            del timeout
            self.commands.append(command)
            return self.responses.pop(0)

    @staticmethod
    def data_frame(offset: int, data: bytes, crc: int | None = None) -> pxadb.Frame:
        checksum = zlib.crc32(data) & 0xFFFFFFFF if crc is None else crc
        encoded = base64.b64encode(data).decode("ascii")
        return pxadb.Frame(1, "DATA", f"{offset}\t{checksum:08x}\t{encoded}")

    def client(self, responses: list[list[pxadb.Frame]]) -> pxadb.NormalFsClient:
        return pxadb.NormalFsClient(self.ScriptedClient(responses))  # type: ignore[arg-type]

    def test_windowed_download_resumes_at_first_gap(self) -> None:
        first = bytes(range(192))
        second = bytes((value + 64) % 256 for value in range(192))
        third = bytes(range(192))[::-1]
        client = self.client([
            [
                self.data_frame(0, first),
                self.data_frame(384, third),
                pxadb.Frame(1, "OK", "more"),
            ],
            [
                self.data_frame(192, second),
                self.data_frame(384, third),
                pxadb.Frame(1, "OK", "eof"),
            ],
        ])
        data = client._download_windowed("pxa-state/inbox/x", 576)
        self.assertEqual(bytes(data), first + second + third)
        encoded = pxadb.client_encode_path("pxa-state/inbox/x")
        self.assertEqual(client.client.commands,
                         [f"FSREAD {encoded} 0", f"FSREAD {encoded} 192"])

    def test_corrupted_chunk_crc_is_rejected(self) -> None:
        client = object.__new__(pxadb.NormalFsClient)
        output = bytearray()
        frame = self.data_frame(0, b"payload", crc=0xDEADBEEF)
        self.assertFalse(client._append_data_frame(frame, output))
        self.assertEqual(output, b"")

    def test_offset_tagged_frame_without_crc_is_accepted(self) -> None:
        client = object.__new__(pxadb.NormalFsClient)
        output = bytearray()
        frame = pxadb.Frame(1, "DATA", "0\t" + base64.b64encode(b"legacy").decode("ascii"))
        self.assertTrue(client._append_data_frame(frame, output))
        self.assertEqual(output, b"legacy")

    def test_bare_chunk_is_only_allowed_for_legacy_burst(self) -> None:
        client = object.__new__(pxadb.NormalFsClient)
        frame = pxadb.Frame(1, "DATA", base64.b64encode(b"legacy").decode("ascii"))
        output = bytearray()
        self.assertFalse(client._append_data_frame(frame, output))
        self.assertEqual(output, b"")
        self.assertTrue(
            client._append_data_frame(frame, output, require_offset=False)
        )
        self.assertEqual(output, b"legacy")

    def test_fs_unlock_requests_system_unlock(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "fs", "unlock", "--minutes", "5", "--port", "/dev/ttyACM0"
        ])
        client = mock.MagicMock()
        client.client.request.return_value = [
            pxadb.Frame(1, "OK", "system;seconds=300")
        ]
        with mock.patch.object(pxadb, "open_file_client", return_value=client):
            self.assertEqual(pxadb.command_fs(arguments), 0)
        client.client.request.assert_called_once_with("FSUNLOCK 5",
                                                      timeout=10.0)

    def test_fs_rmtree_uses_one_tree_command(self) -> None:
        arguments = pxadb.build_parser().parse_args([
            "fs", "rmtree", "system/pxa/builtin", "--port", "/dev/ttyACM0"
        ])
        client = mock.MagicMock()
        with mock.patch.object(pxadb, "open_file_client", return_value=client):
            self.assertEqual(pxadb.command_fs(arguments), 0)
        command = client.client.request.call_args.args[0]
        self.assertEqual(
            command,
            "FSRMTREE " + pxadb.client_encode_path("system/pxa/builtin"),
        )


if __name__ == "__main__":
    unittest.main()
