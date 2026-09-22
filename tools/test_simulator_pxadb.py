import base64
import hashlib
import pathlib
import sys
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
