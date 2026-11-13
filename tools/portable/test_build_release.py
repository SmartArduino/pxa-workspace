"""Exercise a standalone host-tools archive without workspace import fallbacks."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

WORKSPACE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('host_release', Path(__file__).with_name('build_release.py'))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


@unittest.skipUnless(shutil.which('git') and shutil.which('openssl'), 'Git and OpenSSL are required')
class StandaloneHostArchive(unittest.TestCase):
    def test_interaction_detection_works_from_the_signed_archive_alone(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            source = folder / 'source'
            inputs = [('tools/portable/' + name) for name in
                      ('pxa_container.py', 'verify_manifest.py', 'Dockerfile', 'README.md',
                       'README.en.md', 'requirements.txt')]
            inputs += [('tools/pxadb/' + name) for name in
                       ('pxadb.py', 'pxa_local_socket.py', 'ui_query.py',
                        'package_compatibility.py', 'interaction_contract.py')]
            inputs += ['deps/pxa-system/LICENSE', 'deps/pxa-system/config/release-public.pem']
            for name in inputs:
                destination = source / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(WORKSPACE / name, destination)
            def git(*arguments):
                return subprocess.run(['git', '-C', str(source), *arguments], check=True,
                                      capture_output=True, text=True)
            git('init', '-b', 'main')
            git('add', '.')
            git('-c', 'user.name=PXA test', '-c', 'user.email=pxa@example.test',
                '-c', 'commit.gpgsign=false', 'commit', '-m', 'Archive test fixture')
            key = folder / 'test-key.pem'
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'EC', '-pkeyopt',
                            'ec_paramgen_curve:P-256', '-out', str(key)],
                           check=True, capture_output=True)
            image = folder / 'image-fixture'
            image.write_bytes(b'container fixture; this test exercises host-tools packaging')
            output = folder / 'output'
            manifest = release.build(source, output, image, 'sha256:' + 'f' * 64, 'c' * 64, key)
            archive = output / manifest['artifacts'][0]['file']
            self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(),
                             manifest['artifacts'][0]['sha256'])
            extracted = folder / 'extracted'
            with tarfile.open(archive) as stream:
                stream.extractall(extracted, filter='data')
            tree = extracted / ('pxa-host-tools-' + release.VERSION)
            inventory = json.loads((tree / 'manifest.json').read_text())['files']
            self.assertIn('interaction_contract.py', inventory)
            for name, digest in inventory.items():
                self.assertEqual(hashlib.sha256((tree / name).read_bytes()).hexdigest(), digest)
            # -I drops PYTHONPATH and the current working directory. The client
            # must find all its helpers inside its own extracted distribution.
            result = subprocess.run([sys.executable, '-I', '-c',
                'import runpy,sys,types; m=runpy.run_path(sys.argv[1]); '
                'p=m["interaction_profile"](types.SimpleNamespace(device_info="")); '
                'assert p["status"]=="unknown" and p["reason"]=="legacy_firmware"; '
                'print("legacy interaction detection passed")', str(tree / 'pxadb.py')],
                cwd=folder, check=True, capture_output=True, text=True)
            self.assertIn('legacy interaction detection passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
