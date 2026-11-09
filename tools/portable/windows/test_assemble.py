"""Test the user-facing ZIP, including Clang's empty libc++ version marker."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('pxa_windows_assemble',
                                             Path(__file__).with_name('assemble.py'))
assemble = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assemble)


class WindowsArchiveTests(unittest.TestCase):
    def test_empty_version_marker_and_header_survive_zip_extraction(self):
        with tempfile.TemporaryDirectory() as folder:
            temp = Path(folder)
            root = temp / 'devkit'
            marker = Path('toolchains/wasi-sdk/share/wasi-sysroot/include/c++/v1')
            (root / marker).mkdir(parents=True)
            header = Path('toolchains/wasi-sdk/share/wasi-sysroot/include/wasm32-wasip1/noeh/c++/v1/coroutine')
            (root / header).parent.mkdir(parents=True)
            (root / header).write_bytes(b'// libc++ header fixture\n')
            first, second = temp / 'first.zip', temp / 'second.zip'
            assemble.zip_archive(root, first)
            (root / header).touch()
            assemble.zip_archive(root, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as package:
                self.assertTrue(package.getinfo('devkit/' + marker.as_posix() + '/').is_dir())
                package.extractall(temp / 'extracted')
            unpacked = temp / 'extracted/devkit'
            self.assertTrue((unpacked / marker).is_dir())
            self.assertEqual(list((unpacked / marker).iterdir()), [])
            self.assertEqual((unpacked / header).read_bytes(), (root / header).read_bytes())


if __name__ == '__main__':
    unittest.main()
