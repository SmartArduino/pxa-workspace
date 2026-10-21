import base64
import hashlib
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import simulator_pxadb


class FakeConnection:
    def __init__(self) -> None:
        self.frames: list[tuple[int, bytes]] = []

    def write_frame(self, kind: int, sequence: int, payload: bytes) -> None:
        self.frames.append((sequence, payload))

    def decoded(self) -> list[tuple[str, str]]:
        responses: list[tuple[str, str]] = []
        for _sequence, payload in self.frames:
            response_kind, _separator, detail = payload.decode().partition("\0")
            responses.append((response_kind, detail))
        return responses


class SimulatorFileCommandTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.directory.name)
        self.service = simulator_pxadb.SimulatorPxaDb(
            root, root / "installer", root / "publisher.der")
        self.connection = FakeConnection()

    def tearDown(self) -> None:
        self.directory.cleanup()

    def storage(self, relative: str) -> pathlib.Path:
        path = self.service.state_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    @staticmethod
    def encoded(relative: str) -> str:
        return base64.b64encode(relative.encode()).decode()

    def test_file_list_returns_sorted_entries_and_ok(self) -> None:
        (self.service.state_root / "assets").mkdir()
        (self.service.state_root / "b.txt").write_bytes(b"bc")
        (self.service.state_root / "a.txt").write_bytes(b"a")
        self.service.file_list(self.connection, 1, [self.encoded(".")])
        responses = self.connection.decoded()
        self.assertEqual(responses[-1], ("OK", ""))
        entries = [detail.split("\t") for kind, detail in responses[:-1]
                   if kind == "ENTRY"]
        self.assertEqual([(entry[0], entry[2]) for entry in entries],
                         [("F", "a.txt"), ("D", "assets"), ("F", "b.txt")])
        self.assertEqual([entries[0][1], entries[2][1]], ["1", "2"])

    def test_file_list_rejects_a_missing_directory(self) -> None:
        with self.assertRaisesRegex(simulator_pxadb.ServiceError,
                                    "path_not_found"):
            self.service.file_list(self.connection, 1,
                                   [self.encoded("missing")])

    def test_file_digest_reports_sha256_and_size(self) -> None:
        content = b"payload"
        self.storage("file.bin").write_bytes(content)
        self.service.file_digest(self.connection, 7, [self.encoded("file.bin")])
        digest = hashlib.sha256(content).hexdigest()
        self.assertEqual(self.connection.decoded(),
                         [("OK", f"sha256={digest};bytes={len(content)}")])

    def test_file_read_streams_offset_tagged_crc_chunks(self) -> None:
        payload = bytes(index % 251 for index in range(300 * 1024))
        self.storage("block.bin").write_bytes(payload)
        self.service.file_read(self.connection, 2, [self.encoded("block.bin")])
        responses = self.connection.decoded()
        self.assertEqual(responses[-1], ("OK", "more"))
        reassembled = bytearray()
        for kind, detail in responses[:-1]:
            self.assertEqual(kind, "DATA")
            offset_text, _separator, rest = detail.partition("\t")
            crc_text, _separator, encoded = rest.partition("\t")
            self.assertEqual(int(offset_text), len(reassembled))
            chunk = base64.b64decode(encoded)
            self.assertEqual(int(crc_text, 16), zlib.crc32(chunk) & 0xFFFFFFFF)
            reassembled.extend(chunk)
        self.assertEqual(len(responses) - 1,
                         simulator_pxadb.FS_READ_WINDOW_BYTES //
                         simulator_pxadb.MAX_CHUNK_BYTES)
        self.assertEqual(bytes(reassembled), payload[:len(reassembled)])

        self.connection.frames.clear()
        self.service.file_read(self.connection, 3,
                               [self.encoded("block.bin"),
                                str(len(reassembled))])
        responses = self.connection.decoded()
        self.assertEqual(responses[-1], ("OK", "eof"))
        self.assertEqual(len(responses) - 1, 1)

    def test_file_read_rejects_an_offset_past_the_end(self) -> None:
        self.storage("small.bin").write_bytes(b"abc")
        with self.assertRaisesRegex(simulator_pxadb.ServiceError,
                                    "invalid_offset"):
            self.service.file_read(self.connection, 1,
                                   [self.encoded("small.bin"), "99"])

    def test_file_remove_tree_deletes_a_nested_tree(self) -> None:
        nested = self.service.state_root / "pxa-state" / "inbox" / "app"
        nested.mkdir(parents=True)
        (nested / "artifact.bin").write_bytes(b"x")
        self.service.file_remove_tree(self.connection, 4, [
            self.encoded("pxa-state/inbox/app"),
        ])
        self.assertFalse(nested.exists())
        self.assertEqual(self.connection.decoded(), [("OK", "removed")])

    def test_dispatch_routes_read_only_file_commands(self) -> None:
        self.storage("a.txt").write_bytes(b"a")
        self.service.dispatch(self.connection, 5, "FSLIST",
                              [self.encoded(".")])
        self.assertEqual(self.connection.decoded()[0][0], "ENTRY")
        self.connection.frames.clear()
        self.service.dispatch(self.connection, 6, "FSREAD",
                              [self.encoded("a.txt")])
        self.assertEqual(self.connection.decoded()[-1], ("OK", "eof"))

    def test_storage_path_rejects_traversal(self) -> None:
        with self.assertRaisesRegex(simulator_pxadb.ServiceError,
                                    "path must remain inside"):
            self.service.storage_path("../outside")


class SimulatorMultitouchTest(unittest.TestCase):
    setUp = SimulatorFileCommandTest.setUp
    tearDown = SimulatorFileCommandTest.tearDown

    def test_independent_contacts_and_real_cancellation(self) -> None:
        with patch.object(self.service, "control", return_value=b"max_pointers=5;width=800;height=480") as owner:
            self.service.input(["POINTER", "DOWN", "75", "150", "0"])
            self.service.input(["POINTER", "DOWN", "225", "115", "1"])
            self.service.input(["POINTER", "MOVE", "75", "100", "0"])
            self.service.input(["POINTER", "CANCEL", "225", "115", "1"])
            self.service.input(["CANCEL"])
            self.assertEqual([call.args[0] for call in owner.call_args_list], [
                "CAPABILITIES", "TOUCH DOWN 0 75 150", "TOUCH DOWN 1 225 115",
                "TOUCH MOVE 0 75 100", "TOUCH CANCEL 1 225 115",
                "TOUCH CANCEL_ALL",
            ])
            caps = self.service.input(["CAPABILITIES"])
            self.assertIn("max_pointers=5", caps)

    def test_unsupported_identity_never_reaches_owner(self) -> None:
        with patch.object(self.service, "control", return_value=b"max_pointers=5;width=800;height=480") as owner:
            for identity in ("-1", "16", "255", "bad"):
                with self.assertRaises(simulator_pxadb.ServiceError):
                    self.service.input(["POINTER", "DOWN", "75", "150", identity])
            owner.assert_not_called()

    def test_tap_and_swipe_use_contact_zero(self) -> None:
        with patch.object(self.service, "control") as owner, patch("simulator_pxadb.time.sleep"):
            self.service.input(["TAP", "148", "120"])
            self.service.input(["SWIPE", "75", "150", "75", "100", "20", "2"])
            self.assertEqual([call.args[0] for call in owner.call_args_list], [
                "TOUCH DOWN 0 148 120", "TOUCH UP 0 148 120",
                "TOUCH DOWN 0 75 150", "TOUCH MOVE 0 75 125", "TOUCH UP 0 75 100",
            ])


    def test_fifth_contact_and_legacy_owner_limit(self) -> None:
        with patch.object(self.service, "control", return_value=b"max_pointers=5") as owner:
            self.service.input(["POINTER", "DOWN", "100", "120", "4"])
            self.assertEqual(owner.call_args.args[0], "TOUCH DOWN 4 100 120")
            with self.assertRaises(simulator_pxadb.ServiceError):
                self.service.input(["POINTER", "DOWN", "100", "120", "5"])
        self.service.pointer_limit = None
        with patch.object(self.service, "control", return_value=b"") as owner:
            with self.assertRaises(simulator_pxadb.ServiceError):
                self.service.input(["POINTER", "DOWN", "100", "120", "2"])
            self.assertEqual(owner.call_args_list, [unittest.mock.call("CAPABILITIES")])


class SimulatorPackageCommandTest(unittest.TestCase):
    setUp = SimulatorFileCommandTest.setUp
    tearDown = SimulatorFileCommandTest.tearDown
    storage = SimulatorFileCommandTest.storage
    def test_lifecycle_uses_live_owner_with_unprefixed_and_canonical_ids(self) -> None:
        with patch.object(self.service, "control") as owner, patch("simulator_pxadb.subprocess.run") as installer:
            self.service.dispatch(self.connection, 1, "PACKAGE", ["run", "hello"])
            self.service.dispatch(self.connection, 2, "PACKAGE", ["stop", 'a' * 64 + ':hello'])
            self.assertEqual([call.args[0] for call in owner.call_args_list], [
                'PACKAGE run hello', 'PACKAGE stop ' + 'a' * 64 + ':hello'])
            installer.assert_not_called()

    def test_management_uses_live_ui_owner(self) -> None:
        control = self.storage("control.sock"); control.touch()
        self.service.control_socket = control
        with patch.object(self.service, "control") as owner, patch("simulator_pxadb.subprocess.run") as installer:
            self.service.dispatch(self.connection, 1, "PACKAGE", ["uninstall", "pxa-reader"])
            owner.assert_called_once_with("PACKAGE uninstall pxa-reader")
            installer.assert_not_called()
        self.assertEqual(self.connection.decoded(), [("OK", "accepted")])

    def test_management_does_not_bypass_owner_failure(self) -> None:
        control = self.storage("control.sock"); control.touch()
        self.service.control_socket = control
        with patch.object(self.service, "control", side_effect=simulator_pxadb.ServiceError("package_action_failed")), patch("simulator_pxadb.subprocess.run") as installer:
            with self.assertRaisesRegex(simulator_pxadb.ServiceError, "package_action_failed"):
                self.service.dispatch(self.connection, 1, "PACKAGE", ["clear-data", "pxa-reader"])
            installer.assert_not_called()

    def test_offline_management_uses_package_helper(self) -> None:
        with patch("simulator_pxadb.subprocess.run", return_value=Mock(returncode=0)) as installer:
            self.service.dispatch(self.connection, 1, "PACKAGE", ["clear-data", "pxa-reader"])
            command = installer.call_args.args[0]
            self.assertEqual(command[-4:], ["--action", "clear-data", "--expected-id", "pxa-reader"])
        self.assertEqual(self.connection.decoded(), [("OK", "completed")])

    def test_management_rejects_traversal_before_calling_helper(self) -> None:
        with patch("simulator_pxadb.subprocess.run") as installer:
            with self.assertRaisesRegex(simulator_pxadb.ServiceError, "invalid_identity"):
                self.service.dispatch(self.connection, 1, "PACKAGE", ["uninstall", "../pxa-reader"])
            installer.assert_not_called()

    def test_listing_reflects_disabled_marker(self) -> None:
        self.storage("packages/pxa-reader/current/manifest.pxm").touch()
        self.storage("packages/pxa-reader/.disabled").touch()
        self.service.package_list(self.connection, 1)
        self.assertIn("enabled=0", self.connection.decoded()[0][1])


if __name__ == "__main__":
    unittest.main()
