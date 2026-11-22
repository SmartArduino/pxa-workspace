"""Volume admission, LAN source lifetime and transport fallback contracts."""
import base64
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
    def test_borrowed_uart_connection_is_never_reopened_closed_or_retried(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            args = pxadb.build_parser().parse_args([
                "package", "install", str(source), "--yes", "--volume", "internal"])
            for error in (None, pxadb.PxaDbError("uncertain commit")):
                client = mock.MagicMock()
                client.request.return_value = []
                with self.subTest(error=error), \
                     mock.patch.object(pxadb, "open_client") as opened, \
                     mock.patch.object(pxadb, "resolve_pxadb_port") as discover, \
                     mock.patch.object(pxadb, "stage_package_on_volume",
                                       return_value=(42, "candidate", "serial")) as stream:
                    if error:
                        client.request.side_effect = lambda command, **kw: (
                            (_ for _ in ()).throw(error) if command.startswith("PACKAGE install-volume") else [])
                        with self.assertRaisesRegex(pxadb.PxaDbError, "uncertain commit"):
                            pxadb.command_package_install(args, connection=client)
                    else:
                        self.assertEqual(pxadb.command_package_install(args, connection=client), 0)
                    opened.assert_not_called()
                    discover.assert_not_called()
                    client.close.assert_not_called()
                    client.__exit__.assert_not_called()
                    stream.assert_called_once()
                    self.assertEqual(sum(call.args[0].startswith("PACKAGE install-volume")
                                         for call in client.request.call_args_list), 1)

    def test_composite_identity_uses_bounded_root_and_original_key_lineage(self):
        record=lambda tag,value:struct.pack('<HH',tag,len(value))+value
        root,signer=b'a'*32,b'b'*32
        lineage=b'PXKL'+struct.pack('<HHHHI',0,1,16,1,80)+struct.pack('<II',1,0)+root+signer+bytes(8)
        body=record(1,signer)+record(2,b'pxa-test')+record(10,lineage)
        manifest=b'PXAM'+struct.pack('<HHI',0,7,len(body))+body
        with mock.patch.object(pxadb,'read_package_manifest',return_value=manifest):
            self.assertEqual(pxadb.package_composite_identity(pathlib.Path('candidate')),root.hex()+':pxa-test')
        for bad in (record(1,b'x'*31)+record(2,b'pxa-test'),
                    record(1,root)+record(1,signer)+record(2,b'pxa-test'),
                    record(1,root)+record(2,b'bad id'),
                    record(1,root)+record(2,b'pxa-test')+record(10,b'broken')):
            with self.subTest(body=bad),mock.patch.object(pxadb,'read_package_manifest',return_value=b'x'*12+bad),self.assertRaises(pxadb.PxaDbError):
                pxadb.package_composite_identity(pathlib.Path('candidate'))

    def test_update_volume_uses_complete_publisher_identity_and_keeps_legacy(self):
        source = pathlib.Path("candidate.pxa")
        root, other = bytes(range(32)), bytes(reversed(range(32)))
        client = mock.Mock(device_info="package_location=1")
        def row(publisher, state):
            return pxadb.Frame(1, "PKG", publisher.hex()+":pxa-test\tTest\t1\t"+state)
        with mock.patch.object(pxadb, "read_package_manifest", return_value=b"x"*12), \
             mock.patch.object(pxadb, "manifest_records", return_value=[(1, root)]):
            client.request.return_value = [row(other, "installed=1;volume=99"), row(root, "installed=1;volume=42")]
            self.assertEqual(pxadb.existing_package_volume(client, source, "pxa-test"), "42")
            for state, expected in (("installed=1;volume=0", "0"), ("installed=1", "0"),
                                    ("staged=1;volume=42", None)):
                client.request.return_value = [row(root, state)]
                self.assertEqual(pxadb.existing_package_volume(client, source, "pxa-test"), expected)
            client.request.return_value = [row(root, "installed=0;archived=1;volume=42")]
            self.assertEqual(pxadb.existing_package_volume(client, source, "pxa-test"), "42")
            client.request.return_value = [row(root, "installed=1;ambiguous=1;volume=0")]
            with self.assertRaisesRegex(pxadb.PxaDbError, "original --volume"):
                pxadb.existing_package_volume(client, source, "pxa-test")
            client.request.return_value = [row(root, "installed=1;volume=-1")]
            with self.assertRaisesRegex(pxadb.PxaDbError, "invalid installed volume"):
                pxadb.existing_package_volume(client, source, "pxa-test")
        client.device_info = "package_stream_volume=1"
        client.request.reset_mock()
        self.assertIsNone(pxadb.existing_package_volume(client, source, "pxa-test"))
        client.request.assert_not_called()

    def test_rotated_signer_placement_uses_original_root(self):
        root, signer = b"a"*32, b"b"*32
        lineage = b"PXKL"+struct.pack("<HHHHI", 0, 1, 16, 1, 80)+struct.pack("<II", 1, 0)+root+signer+bytes(8)
        client = mock.Mock(device_info="package_location=1")
        client.request.return_value = [pxadb.Frame(1, "PKG", root.hex()+":pxa-test\tTest\t1\tinstalled=1;volume=7")]
        with mock.patch.object(pxadb, "read_package_manifest", return_value=b"x"*12), \
             mock.patch.object(pxadb, "manifest_records", return_value=[(1, signer), (10, lineage)]):
            self.assertEqual(pxadb.existing_package_volume(client, pathlib.Path("candidate"), "pxa-test"), "7")

    def test_default_update_streams_to_existing_volume_without_legacy_staging(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            args = pxadb.build_parser().parse_args(["package", "install", str(source), "--yes", "--port", "/dev/mock"])
            client = mock.MagicMock(); client.__enter__.return_value = client; client.request.return_value = []
            with mock.patch.object(pxadb, "open_client", return_value=client), \
                 mock.patch.object(pxadb, "existing_package_volume", return_value="42"), \
                 mock.patch.object(pxadb, "stage_package_on_volume", return_value=(42, "candidate", "serial")) as stream, \
                 mock.patch.object(pxadb, "stage_package_files") as legacy:
                self.assertEqual(pxadb.command_package_install(args), 0)
                self.assertEqual(stream.call_args.args[2], "42")
                legacy.assert_not_called()
                client.request.assert_any_call("PACKAGE install-volume 42 "+pxadb.client_encode_path("candidate"), timeout=60)

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

    def test_fresh_install_uses_sd_only_when_internal_cannot_fit(self):
        client = mock.Mock(device_info="package_location=1;package_stream_volume=1")
        internal = self.volume(epoch="1", capabilities="67", free="40960")
        sd = self.volume(epoch="2", free="3000000")
        with mock.patch.object(pxadb, "device_volumes", return_value=[sd, internal]), \
             mock.patch.object(pxadb, "stream_container_prefix", return_value=4096), \
             mock.patch.object(pxadb, "package_publisher_root", return_value=bytes(32)), \
             mock.patch.object(pxadb, "streamed_install_space", return_value=2000000):
            source = pathlib.Path("new.pxa")
            self.assertEqual(pxadb.choose_fresh_install_volume(client, source), "2")
            internal["free"] = "4000000"
            self.assertEqual(pxadb.choose_fresh_install_volume(client, source), "1")
            internal["known"] = sd["known"] = "0"
            self.assertIsNone(pxadb.choose_fresh_install_volume(client, source))

    def test_streamed_space_counts_shared_directories_and_nonoverlapping_peaks(self):
        def record(tag, value):
            return struct.pack("<HH", tag, len(value)) + value

        def file(path):
            return record(17, record(1, path) + record(2, struct.pack("<Q", 4096)))

        manifest = bytes(12) + file(b"assets/a") + file(b"assets/b") + file(b"assets/nested/c")
        internal = self.volume(unit="4096", capabilities="195")
        removable = self.volume(unit="4096", private_bytes="69632")
        with mock.patch.object(pxadb, "read_package_manifest", return_value=manifest):
            data_and_dirs = (3 * 2 + 2) * 4096
            safe_margin = 128 * 1024
            manifest_and_signature = 3 * 4096
            self.assertEqual(pxadb.streamed_install_space(None, 8192, internal),
                             data_and_dirs + manifest_and_signature + safe_margin)
            self.assertEqual(pxadb.streamed_install_space(None, 32 * 4096, internal),
                             data_and_dirs + manifest_and_signature + 37 * 4096)
            self.assertEqual(pxadb.streamed_install_space(None, 8192, removable),
                             data_and_dirs + safe_margin + 18 * 4096 + 69632)

    def test_direct_install_checks_compatibility_once_and_preserves_strict(self):
        with tempfile.TemporaryDirectory() as folder:
            source = fixtures.PackageStorageTest.container(pathlib.Path(folder))
            client = mock.Mock(device_info="package_stream_volume=1")
            args = pxadb.build_parser().parse_args([
                "package", "install", str(source), "--yes", "--volume", "internal",
                "--strict-compatibility"])
            report = {"status": "unknown", "checks": [
                {"subject": "runtime", "status": "unknown", "message": "firmware unreported"}]}
            with mock.patch.object(pxadb, "stream_container_prefix", return_value=4096), \
                 mock.patch.object(pxadb, "package_compatibility", return_value=report) as compatibility, \
                 mock.patch.object(pxadb, "check_container_features"), \
                 mock.patch.object(pxadb, "select_install_volume", return_value=self.volume()) as volume, \
                 mock.patch.object(pxadb, "fetch_package_stream", return_value=True):
                with self.assertRaisesRegex(pxadb.PxaDbError, "firmware unreported"):
                    pxadb.command_package_install(args, connection=client)
                compatibility.assert_called_once_with(client, source)
                volume.assert_not_called()
                client.request.assert_not_called()
                compatibility.reset_mock()
                args.strict_compatibility = False
                self.assertEqual(pxadb.command_package_install(args, connection=client, quiet=True), 0)
                compatibility.assert_called_once_with(client, source)

    def test_streaming_preview_uses_manifest_name_and_version(self):
        source = pathlib.Path("arbitrary-filename.pxa")
        client = mock.Mock(device_info="package_install_meta=1")
        fields = [(2, b"pxa-puzzle"), (3, b"1.2.3"), (4, " 拼图\n游戏 ".encode())]
        with mock.patch.object(pxadb, "read_package_manifest", return_value=bytes(12)), \
             mock.patch.object(pxadb, "manifest_records", return_value=fields):
            command = pxadb.stream_upload_command(client, source, "somewhere", 123, "sha", None)
        fields = command.split()
        self.assertEqual(fields[:5], ["FSINSTALL", pxadb.client_encode_path("somewhere"), "123", "sha", "-"])
        self.assertEqual([base64.b64decode(field).decode() for field in fields[5:]], ["拼图 游戏", "1.2.3"])

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
            client.request.return_value=[]
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
            client.request.side_effect = [pxadb.PxaDbRejectedError("commit_failed"), []]
            remote = "sdcard/PXA/cache/store/.stream-12345678"
            with mock.patch.object(pxadb, "open_client", return_value=client), \
                    mock.patch.object(pxadb, "package_compatibility",return_value={'status':'compatible','checks':[]}), \
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
            fs = mock.Mock(client=mock.Mock(device_info='package_stream=2;package_codecs=0,1,2'))
            with mock.patch.object(pxadb, "check_package_storage", return_value={"package_receive": "1"}), \
                    mock.patch.object(pxadb, "package_compatibility",return_value={'status':'compatible','checks':[]}), \
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
