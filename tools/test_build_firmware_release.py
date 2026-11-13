import importlib.util
from pathlib import Path
import unittest
import hashlib
import struct
import tempfile

spec = importlib.util.spec_from_file_location('firmware_release', Path(__file__).with_name('build_firmware_release.py'))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class FirmwareLayout(unittest.TestCase):
    def test_maintained_gamekit_replaces_retired_revisions(self):
        self.assertIn('doit-gamekit-2', release.BOARDS)
        self.assertNotIn('doit-gamekit', release.BOARDS)
        self.assertNotIn('doit-gamekit-1', release.BOARDS)

    def test_patch_firmware_uses_its_declared_older_store_snapshot(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'VERSION').write_text('0.11.1\n')
            header = root / 'pxa_store_host_profile.h'
            header.write_text('\n'.join(f'return "esp32-{target}-pxa-0.11.0";'
                                        for target in ('s3', 's31', 'p4')))
            self.assertEqual(release.store_profile_release(header), '0.11.0')

    def test_missing_duplicate_or_different_store_snapshots_fail_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            header = Path(folder) / 'profile.h'
            for profiles in [[], [('s3', '0.11.0'), ('s31', '0.11.0')],
                             [('s3', '0.11.0'), ('s31', '0.11.0'), ('s31', '0.11.0')],
                             [('s3', '0.11.0'), ('s31', '0.11.0'), ('p4', '0.11.1')]]:
                with self.subTest(profiles=profiles):
                    header.write_text('\n'.join(f'return "esp32-{target}-pxa-{version}";'
                                                for target, version in profiles))
                    with self.assertRaisesRegex(ValueError, 'common Store capability snapshot'):
                        release.store_profile_release(header)

    def test_factory_tree_is_bound_to_container_inventory_and_version(self):
        def tlv(tag, data):
            return struct.pack('<HH', tag, len(data)) + data
        with tempfile.TemporaryDirectory() as folder:
            package = Path(folder) / 'pxa-store'
            package.mkdir()
            content = b'compiled-store'
            (package / 'code.aot').write_bytes(content)
            body = tlv(2, b'pxa-store') + tlv(3, b'0.9.3') + tlv(9, struct.pack('<Q', 33))
            body += tlv(17, tlv(1, b'code.aot') + tlv(2, struct.pack('<Q', len(content)))
                        + tlv(3, hashlib.sha256(content).digest()))
            manifest = b'PXAM' + struct.pack('<HHI', 0, 8, len(body)) + body
            (package / 'manifest.pxm').write_bytes(manifest)
            signature = b'signature'
            (package / 'signature.pxs').write_bytes(signature)
            header = bytearray(64)
            header[:4] = b'PXAC'
            struct.pack_into('<H', header, 6, 2)
            struct.pack_into('<II', header, 16, len(manifest), len(signature))
            container = package.with_suffix('.pxa')
            container.write_bytes(header + manifest + signature)
            self.assertEqual(release.store_identity(package, container)['version'], '0.9.3')
            self.assertEqual(release.store_identity(package, container)['release_sequence'], 33)
            (package / 'code.aot').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'signed inventory'):
                release.store_identity(package, container)
            (package / 'code.aot').write_bytes(content)
            (package / 'extra').write_bytes(b'unlisted')
            with self.assertRaisesRegex(ValueError, 'unlisted'):
                release.store_identity(package, container)
            (package / 'extra').unlink()
            (package / 'signature.pxs').write_bytes(b'wrongsign')
            with self.assertRaisesRegex(ValueError, 'differs from container'):
                release.store_identity(package, container)

    def test_factory_parameters_match_board_mounts(self):
        for name in release.BOARDS:
            board = release.ROOT / 'firmware/boards' / name
            self.assertEqual(release.config(board, 'CONFIG_LITTLEFS_OBJ_NAME_LEN', ''), '192')
            self.assertEqual(release.config(board, 'CONFIG_PXA_BUILTIN_PACKAGE_ROOT', ''), 'system/pxa/factory')
    def test_units_and_hex(self):
        for value, expected in [('16MB', 16 << 20), ('32MB', 32 << 20), ('0x410000', 0x410000), ('4096', 4096)]:
            self.assertEqual(release.byte_size(value), expected)
        for value in ['16Mb', '-1', '', '0xgg']:
            with self.assertRaises(ValueError):
                release.byte_size(value)

    def test_overlap_and_capacity(self):
        def region(offset, size):
            return dict(file='image.bin', offset=offset, size=size)
        release.validate_regions([region(0, 8192), region(8192, 4096)], 16384)
        for rows in [[region(0, 8193), region(8192, 4096)], [region(12000, 8192)], [region(0, 0)]]:
            with self.assertRaises(ValueError):
                release.validate_regions(rows, 16384)


if __name__ == '__main__':
    unittest.main()
