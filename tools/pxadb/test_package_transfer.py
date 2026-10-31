"""Volume admission, LAN source lifetime and transport fallback contracts."""
import http.client
import pathlib
import socket
import struct
import tempfile
import unittest
from unittest import mock
from urllib.parse import urlsplit

import pxadb
import test_pxadb as fixtures


class PackageTransferTest(unittest.TestCase):
    def test_pending_cleanup_retries_without_masking_storage_errors(self):
        client = object.__new__(pxadb.PxaDbClient)
        client.timeout, client.sequence = 30, 0
        client._write_command = mock.Mock()
        client._collect_serial_frames = mock.Mock()
        ready = [pxadb.Frame(1, "READY", "5")]
        client._collect_serial_frames.side_effect = [pxadb.PxaDbRejectedError("package_cleanup_pending"), ready]
        with mock.patch.object(pxadb.time, "sleep"), mock.patch.object(pxadb.time, "monotonic", return_value=10):
            self.assertEqual(pxadb.begin_file_upload(client, "FSPUT path 5 hash", 30), ready)
        self.assertEqual(client._write_command.call_args_list,
                         [mock.call(1, "FSPUT path 5 hash"), mock.call(2, "FSPUT path 5 hash")])
        self.assertEqual([call.args[-1] for call in client._collect_serial_frames.call_args_list], [40, 40])
        client._collect_serial_frames.side_effect = pxadb.PxaDbRejectedError("insufficient_storage")
        with self.assertRaisesRegex(pxadb.PxaDbError, "insufficient_storage"):
            pxadb.begin_file_upload(client, "FSPUT path 5 hash", 30)

    def test_package_run_cleanup_wait_is_bounded_and_timeouts_are_not_replayed(self):
        client = object.__new__(pxadb.PxaDbClient)
        client.timeout, client.sequence = 30, 0
        client._write_command = mock.Mock()
        client._collect_serial_frames = mock.Mock(side_effect=pxadb.PxaDbRejectedError("package_cleanup_pending"))
        with mock.patch.object(pxadb.time, "monotonic", side_effect=[10, 11]), mock.patch.object(pxadb.time, "sleep") as sleep:
            with self.assertRaisesRegex(pxadb.PxaDbRejectedError, "package_cleanup_pending"):
                client.request("PACKAGE run hello", timeout=.5)
            sleep.assert_not_called()
        self.assertEqual(client._write_command.call_count, 1)
        client._write_command.reset_mock()
        client._collect_serial_frames.side_effect = pxadb.PxaDbError("timed out")
        with self.assertRaisesRegex(pxadb.PxaDbError, "timed out"):
            client.request("PACKAGE run hello")
        self.assertEqual(client._write_command.call_count, 1)
    def test_native_usb_auto_stays_on_usb_and_bridge_cannot_force_usb(self):
        client = mock.Mock(device_info="transport=usb-jtag;fs_fast=1;stream_fetch=1")
        self.assertFalse(pxadb.fetch_package_stream(client, pathlib.Path("unused"), "unused", 2, "auto"))
        client.request.assert_not_called()
        client.device_info = "transport=uart;fs_fast=1"
        with self.assertRaisesRegex(pxadb.PxaDbError, "USB-to-UART"):
            pxadb.fetch_package_stream(client, pathlib.Path("unused"), "unused", 2, "usb")

    def test_disabled_legacy_rejected_before_any_device_mutation(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            client = mock.Mock(device_info="package_stream=2;package_legacy=0")
            with self.assertRaisesRegex(pxadb.PxaDbError, "legacy Container 0.1 support is disabled"):
                pxadb.check_container_features(client, source)
            client.request.assert_not_called()
            for capabilities in ("package_stream=2;package_legacy=1", ""):
                client.device_info = capabilities
                pxadb.check_container_features(client, source)

    def test_new_codec_rejected_before_any_device_mutation(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            raw = bytearray(source.read_bytes())
            struct.pack_into("<H", raw, 6, 2)
            struct.pack_into("<HH", raw, 12, 2, 12)
            source.write_bytes(raw)
            client = mock.Mock(device_info="package_stream=2;package_stream_volume=1;package_codecs=0,1")
            with self.assertRaisesRegex(pxadb.PxaDbError, "codec 2"):
                pxadb.stage_package_on_volume(client, source, "sd")
            client.request.assert_not_called()
            client.device_info = "package_stream=2;package_codecs=0,1,2;package_max_block=4096"
            pxadb.check_container_features(client, source)
            struct.pack_into("<H", raw, 14, 15); source.write_bytes(raw)
            with self.assertRaisesRegex(pxadb.PxaDbError, "block exceeds"):
                pxadb.check_container_features(client, source)
            # Old hosts may expose neither storage metrics nor stream support.
            # Refuse before INFO or staging commands, with an actionable build option.
            client.device_info = "fs_offset=1"
            with self.assertRaisesRegex(pxadb.PxaDbError, "--legacy"):
                pxadb.stage_package_files(pxadb.NormalFsClient(client), source, "probe", 10)
            client.request.assert_not_called()

    def test_http_source_exposes_only_one_file_to_one_peer_and_closes(self):
        with tempfile.TemporaryDirectory() as folder:
            source = pathlib.Path(folder) / "private.pxa"
            source.write_bytes(bytes(range(256)) * 300)
            with pxadb.package_http_source(source, "127.0.0.1") as url:
                target = urlsplit(url)
                connection = http.client.HTTPConnection(target.hostname, target.port, timeout=2)
                connection.request("GET", target.path)
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(response.read(), source.read_bytes())
                connection.close()
                for path, peer in [("/private.pxa", "127.0.0.1"),
                                   ("/../private.pxa", "127.0.0.1"),
                                   (target.path, "127.0.0.2")]:
                    connection = http.client.HTTPConnection(target.hostname, target.port, timeout=2,
                                                            source_address=(peer, 0))
                    connection.request("GET", path)
                    response = connection.getresponse()
                    self.assertEqual(response.status, 404)
                    response.read()
                    connection.close()
            with self.assertRaises(OSError):
                socket.create_connection((target.hostname, target.port), timeout=.2)

    @staticmethod
    def volume(**changes):
        value = dict(epoch="2", capabilities="71", free="100000000", known="3", ambiguous="0",
                     relative="sdcard", path="/littlefs/sdcard", state="2", unit="32768", private_bytes="1048576")
        value.update(changes)
        return value

    def test_volume_selection_rejects_stale_read_only_ambiguous_and_multiple(self):
        for volumes in [[self.volume(state="1")], [self.volume(capabilities="69")],
                        [self.volume(ambiguous="1")], [self.volume(relative="")],
                        [self.volume(), self.volume(epoch="3")]]:
            with mock.patch.object(pxadb, "device_volumes", return_value=volumes):
                with self.assertRaises(pxadb.PxaDbError):
                    pxadb.select_install_volume(None, "sd")
        with mock.patch.object(pxadb, "device_volumes", return_value=[self.volume(), self.volume(epoch="3")]):
            self.assertEqual(pxadb.select_install_volume(None, "3")["epoch"], "3")

    def test_wifi_failure_aborts_receiver_before_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            client = mock.Mock(device_info="stream_fetch=1", timeout=10)
            client.request.side_effect = [[pxadb.Frame(1, "OK", "ipv4=127.0.0.1")],
                                          pxadb.PxaDbRejectedError("stream_fetch_failed"), []]
            client.request_until.return_value = [pxadb.Frame(1, "READY", "9")]
            self.assertFalse(pxadb.fetch_package_stream(client, source, "sdcard/PXA/cache/store/.stream-12345678", 2, "auto"))
            self.assertEqual(client.request.call_args_list[-1], mock.call("FSABORT", timeout=40))
            self.assertTrue(client.request_until.call_args.args[0].endswith(" 2"))

    def test_forced_wifi_never_silently_uses_serial(self):
        client = mock.Mock(device_info="stream_fetch=1", timeout=10)
        client.request.side_effect = pxadb.PxaDbRejectedError("network_unavailable")
        with self.assertRaisesRegex(pxadb.PxaDbError, "Wi-Fi package transfer failed"):
            pxadb.fetch_package_stream(client, pathlib.Path("unused"), "unused", 2, "wifi")
        client.request_until.assert_not_called()
        client.request.assert_called_once_with("NETINFO")

    def test_memory_exhaustion_cleans_receiver_without_transport_retry(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            client = mock.Mock(device_info="stream_fetch=1", timeout=10)
            client.request.side_effect = [[pxadb.Frame(1, "OK", "ipv4=127.0.0.1")],
                                          pxadb.PxaDbRejectedError("insufficient_memory"), []]
            client.request_until.return_value = [pxadb.Frame(1, "READY", "9")]
            with self.assertRaisesRegex(pxadb.PxaDbError, "insufficient installation memory"):
                pxadb.fetch_package_stream(client, source, "candidate", 2, "auto")
            self.assertEqual(client.request.call_args_list[-1], mock.call("FSABORT", timeout=40))
            client.fast_upload_active = True
            fs = pxadb.NormalFsClient(client)
            with mock.patch.object(fs, "_put", side_effect=pxadb.PxaDbRejectedError("insufficient_memory")):
                with self.assertRaisesRegex(pxadb.PxaDbError, "--chunk-size 4096"):
                    fs.put(source, "candidate", stream_install=True)
            client.cancel_fast_upload.assert_called_once()

    def test_storage_admission_precedes_network_and_volume_receiver(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            data = bytearray(source.read_bytes())
            struct.pack_into("<H", data, 6, 2)
            struct.pack_into("<H", data, 14, 12)
            struct.pack_into("<I", data, 56, 48)
            source.write_bytes(data + bytes(400))
            client = mock.Mock(device_info="package_stream=2;package_stream_volume=1")
            with mock.patch.object(pxadb, "select_install_volume", return_value=self.volume(free="1")), \
                    mock.patch.object(pxadb, "fetch_package_stream") as fetch, \
                    mock.patch.object(pxadb, "NormalFsClient") as fs:
                with self.assertRaisesRegex(pxadb.PxaDbError, "insufficient device storage"):
                    pxadb.stage_package_on_volume(client, source, "sd")
                fetch.assert_not_called()
                fs.assert_not_called()

    def test_volume_cli_commits_same_candidate_and_releases_on_error(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            arguments = pxadb.build_parser().parse_args([
                "package", "install", str(source), "--volume", "sd", "--via", "serial", "--yes", "--port", "/dev/mock"])
            client = mock.MagicMock()
            client.__enter__.return_value = client
            client.request.side_effect = [[], pxadb.PxaDbRejectedError("commit_failed"), []]
            remote = "sdcard/PXA/cache/store/.stream-12345678"
            with mock.patch.object(pxadb, "open_client", return_value=client), \
                    mock.patch.object(pxadb, "stage_package_on_volume", return_value=(2, remote, "serial")), \
                    mock.patch.object(pxadb, "clear_staged_source") as inbox:
                with self.assertRaisesRegex(pxadb.PxaDbError, "commit_failed"):
                    pxadb.command_package_install(arguments)
                inbox.assert_not_called()
            client.request.assert_any_call("PACKAGE install-volume 2 " + pxadb.client_encode_path(remote), timeout=60)
            self.assertEqual(client.request.call_args, mock.call("FSABORT", timeout=40))


class LargePackageDeadlineTest(unittest.TestCase):
    @staticmethod
    def container(root, total=64 * 1024 * 1024):
        source = fixtures.PackageStorageTest.container(root)
        source.write_bytes(source.read_bytes().replace(struct.pack("<Q", 500000),
                                                       struct.pack("<Q", total - 700000)))
        return source

    def test_tiny_archive_gets_decoded_size_budget_with_a_cap(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            source = self.container(root)
            self.assertLess(source.stat().st_size, 1024)
            self.assertEqual(pxadb.package_verification_timeout(source), 542)
            package = root / "directory"
            package.mkdir()
            (package / "manifest.pxm").write_bytes(pxadb.read_package_manifest(source))
            self.assertEqual(pxadb.package_verification_timeout(package), 542)
            self.assertEqual(pxadb.package_verification_timeout(self.container(root, 1 << 40)), 900)

    def test_large_local_installs_wait_for_commit_and_respect_longer_override(self):
        for volume in (False, True):
            for timeout in (10, 1200):
                with self.subTest(volume=volume, timeout=timeout), tempfile.TemporaryDirectory() as folder:
                    source = self.container(pathlib.Path(folder))
                    args = ["package", "install", str(source), "--yes", "--port", "/dev/mock",
                            "--timeout", str(timeout)]
                    if volume:
                        args += ["--volume", "sd", "--via", "serial"]
                    client = mock.MagicMock()
                    client.__enter__.return_value = client
                    def respond(command, **kwargs):
                        if command.startswith(("PACKAGE deploy ", "PACKAGE install-volume ")):
                            if kwargs["timeout"] < 500:
                                raise pxadb.PxaDbError("timed out verifying decoded files")
                            self.assertEqual(kwargs["timeout"], max(timeout, 542))
                        return []
                    client.request.side_effect = respond
                    with mock.patch.object(pxadb, "open_client", return_value=client) as connect, \
                            mock.patch.object(pxadb, "stage_package_on_volume", return_value=(2, "candidate", "serial")), \
                            mock.patch.object(pxadb, "stage_package_files"), \
                            mock.patch.object(pxadb, "clear_staged_source"):
                        self.assertEqual(pxadb.command_package_install(pxadb.build_parser().parse_args(args)), 0)
                    self.assertEqual(connect.call_args.args[1], max(timeout, 60))

    def test_explicit_staging_uses_decoded_size_deadline(self):
        with tempfile.TemporaryDirectory() as folder:
            source = self.container(pathlib.Path(folder))
            fs = mock.Mock()
            with mock.patch.object(pxadb, "check_package_storage", return_value={"package_receive": "1"}), \
                    mock.patch.object(pxadb, "_stage_package_files"):
                pxadb.stage_package_files(fs, source, "probe", 10)
            fs.client.request.assert_called_once_with("PACKAGE publish probe", timeout=542)

    def test_wifi_accounts_for_expansion_and_longer_user_deadline(self):
        with tempfile.TemporaryDirectory() as folder:
            source = self.container(pathlib.Path(folder))
            for timeout in (10, 1200):
                with self.subTest(timeout=timeout):
                    client = mock.Mock(device_info="stream_fetch=1", timeout=timeout)
                    client.request.side_effect = [[pxadb.Frame(1, "OK", "ipv4=127.0.0.1")], []]
                    client.request_until.return_value = [pxadb.Frame(1, "READY", "1")]
                    with mock.patch.object(pxadb, "package_http_source") as server:
                        server.return_value.__enter__.return_value = "http://127.0.0.1/package"
                        self.assertTrue(pxadb.fetch_package_stream(client, source, "candidate", 2, "wifi"))
                    self.assertEqual(client.request.call_args.kwargs["timeout"], max(timeout, 542))


if __name__ == "__main__":
    unittest.main()
