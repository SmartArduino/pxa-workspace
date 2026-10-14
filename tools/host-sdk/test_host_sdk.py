import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("pxa_host_sdk", Path(__file__).with_name("pxa_host_sdk.py"))
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


class HostSdkTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "source"
        self.root.mkdir()
        (self.root / "runtime.c").write_text("immutable source\n")
        self.manifest = {"schema": sdk.SCHEMA, "version": "0.2.0-rc.1+idf.1",
                         "pxa_version": "0.2.0-rc.1", "sources": {"system": "fixed"},
                         "files": {"runtime.c": {"sha256": sdk.digest((self.root / "runtime.c").read_bytes()), "mode": 0o644}}}
        data = sdk.json_bytes(self.manifest)
        (self.root / "host-sdk.json").write_bytes(data)
        self.lock = {"schema": sdk.LOCK_SCHEMA,
                     **{k: self.manifest[k] for k in ("version", "pxa_version", "sources")},
                     "manifest_sha256": sdk.digest(data), "archive": "sdk.tar.gz",
                     "install_dir": "deps/sdk"}

    def make_archive(self, extra=None):
        archive = self.base / "sdk.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            for path in self.root.iterdir():
                output.add(path, arcname=path.name)
            if extra:
                info, data = extra
                output.addfile(info, io.BytesIO(data))
        self.lock["archive_sha256"] = sdk.digest(archive.read_bytes())
        path = self.base / "pxa-host-sdk.lock.json"
        path.write_bytes(sdk.json_bytes(self.lock))
        return path

    def test_fixed_offline_install_and_idempotence(self):
        lock = self.make_archive()
        result = sdk.install(lock, None, None)
        self.assertEqual(sdk.verify(result, self.lock)["sources"]["system"], "fixed")
        self.assertEqual(sdk.install(lock, None, None), result)

    def test_source_edit_and_extra_file_are_rejected(self):
        sdk.verify(self.root, self.lock)
        (self.root / "runtime.c").write_text("product patch")
        with self.assertRaisesRegex(ValueError, "modified"):
            sdk.verify(self.root, self.lock)
        (self.root / "runtime.c").write_text("immutable source\n")
        (self.root / "unapproved.c").write_text("")
        with self.assertRaisesRegex(ValueError, "inventory"):
            sdk.verify(self.root, self.lock)

    def test_manifest_and_archive_mismatch_are_rejected(self):
        lock = self.make_archive()
        (self.root / "host-sdk.json").write_bytes(b"{}")
        with self.assertRaisesRegex(ValueError, "manifest"):
            sdk.verify(self.root, self.lock)
        (self.base / "sdk.tar.gz").write_bytes(b"wrong artifact")
        with self.assertRaisesRegex(ValueError, "archive SHA"):
            sdk.install(lock, None, None)

    def test_install_does_not_overwrite_patched_dependency(self):
        lock = self.make_archive()
        root = sdk.install(lock, None, None)
        (root / "runtime.c").write_text("local changes")
        with self.assertRaisesRegex(ValueError, "modified"):
            sdk.install(lock, None, None)
        self.assertEqual((root / "runtime.c").read_text(), "local changes")

    def test_archive_escape_and_links_are_rejected_before_extraction(self):
        for name, kind in [("../outside", tarfile.REGTYPE), ("link", tarfile.SYMTYPE)]:
            with self.subTest(name=name):
                info = tarfile.TarInfo(name)
                info.type = kind
                info.linkname = "../outside"
                lock = self.make_archive((info, b""))
                with self.assertRaises(ValueError):
                    sdk.install(lock, None, None)
                self.assertFalse((self.base / "deps/sdk").exists())
                self.assertFalse((self.base / "deps/outside").exists())

    def test_incremental_build_rejects_source_edits_and_reloads_new_lock(self):
        if shutil.which('cmake') is None:
            self.skipTest('CMake unavailable')
        upstream = Path(__file__).resolve().parents[2]
        for name in ['tools/host-sdk/pxa_host_sdk.py', 'cmake/pxa_host_sdk.cmake']:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(upstream / name, path)
            self.manifest['files'][name] = {'sha256': sdk.digest(path.read_bytes()), 'mode': 0o644}
        self.manifest['components'] = ['pxa']
        (self.root / 'host-sdk.json').write_bytes(sdk.json_bytes(self.manifest))
        self.lock['manifest_sha256'] = sdk.digest((self.root / 'host-sdk.json').read_bytes())
        lock = self.base / 'pxa-host-sdk.lock.json'
        self.lock['install_dir'] = 'source'
        lock.write_bytes(sdk.json_bytes(self.lock))
        (self.base / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
file(READ "${CMAKE_CURRENT_LIST_DIR}/pxa-host-sdk.lock.json" lock)
string(JSON sdk_dir GET "${lock}" install_dir)
include("${CMAKE_CURRENT_LIST_DIR}/${sdk_dir}/cmake/pxa_host_sdk.cmake")
project(sdk_guard NONE)
''')
        build = self.base / 'build'
        def run(*arguments):
            return subprocess.run(['cmake', *map(str, arguments)], capture_output=True, text=True)
        configured = run('-S', self.base, '-B', build)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        self.assertEqual(run('--build', build).returncode, 0)
        (self.root / 'runtime.c').write_text('product patch after configuration\n')
        rejected = run('--build', build)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn('modified', rejected.stdout + rejected.stderr)
        (self.root / 'runtime.c').write_text('immutable source\n')
        verifier = self.root / 'tools/host-sdk/pxa_host_sdk.py'
        verifier_bytes = verifier.read_bytes()
        verifier.write_text('raise SystemExit(0)\n')
        rejected = run('--build', build)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn('verifier was modified', rejected.stdout + rejected.stderr)
        verifier.write_bytes(verifier_bytes)
        second = self.base / 'source-2'
        shutil.copytree(self.root, second)
        self.manifest['version'] = '0.2.0-rc.1+idf.2'
        (second / 'host-sdk.json').write_bytes(sdk.json_bytes(self.manifest))
        self.lock.update(version=self.manifest['version'], install_dir='source-2',
                         manifest_sha256=sdk.digest((second / 'host-sdk.json').read_bytes()))
        lock.write_bytes(sdk.json_bytes(self.lock))
        rebuilt = run('--build', build)
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout + rebuilt.stderr)
        self.assertIn('0.2.0-rc.1+idf.2', rebuilt.stdout)


if __name__ == "__main__":
    unittest.main()
