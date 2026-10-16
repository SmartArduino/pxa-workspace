import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('firmware_release', Path(__file__).with_name('build_firmware_release.py'))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class FirmwareLayout(unittest.TestCase):
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
