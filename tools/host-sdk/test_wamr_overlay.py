import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
import pxa_host_sdk as sdk
import prepare_wamr_overlay as overlay


class ArchiveOverlayTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'sdk'
        self.system = self.root / 'deps/pxa-system'
        self.upstream = Path(os.environ.get('PXA_SYSTEM_ROOT',
            Path(__file__).resolve().parents[2] / 'deps/pxa-system'))
        self.metadata = self.system / 'config/wamr.json'
        self.series = self.system / 'platforms/esp-idf/wamr/patches/series'
        for relative in ['config/wamr.json', 'tools/wamr/prepare_overlay.py']:
            dest = self.system / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.upstream / relative, dest)
        shutil.copytree(self.upstream / 'platforms/esp-idf/wamr/patches', self.series.parent)
        spec = importlib.util.spec_from_file_location(
            'test_core_wamr_overlay', self.upstream / 'tools/wamr/prepare_overlay.py')
        self.core = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = self.core
        spec.loader.exec_module(self.core)
        for name in self.series.read_text().splitlines():
            if not name.strip() or name.lstrip().startswith('#'):
                continue
            for patch in self.core.parse_patch(self.series.parent / name):
                relative = Path('wamr') / patch.path
                dest = self.system / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(self.upstream / relative, dest)
        files = {p.relative_to(self.root).as_posix(): {'sha256': sdk.digest(p.read_bytes()), 'mode': 0o644}
                 for p in self.root.rglob('*') if p.is_file()}
        manifest = {'schema': sdk.SCHEMA, 'sources': {'wamr': json.loads(self.metadata.read_text())['commit']},
                    'files': files}
        (self.root / 'host-sdk.json').write_bytes(sdk.json_bytes(manifest))
        self.output = Path(self.temporary.name) / 'build-overlay'

    def prepare(self):
        return overlay.prepare(self.root, self.system / 'wamr', self.output, self.metadata, self.series)

    def test_archive_matches_core_git_overlay_and_stays_read_only(self):
        expected = Path(self.temporary.name) / 'git-overlay'
        self.core.prepare(self.upstream / 'wamr', expected,
                          self.upstream / 'config/wamr.json',
                          self.upstream / 'platforms/esp-idf/wamr/patches/series')
        paths = self.prepare()
        self.assertTrue(paths)
        for p in paths:
            self.assertEqual(p.read_bytes(), (expected / p.relative_to(self.output)).read_bytes())
        self.assertFalse(list(self.root.rglob('.git')))
        self.assertFalse(list(self.root.rglob('__pycache__')))
        sdk.verify(self.root)

    def test_modified_archive_preserves_previous_build_output(self):
        self.prepare()
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        source = self.system / 'wamr/core/shared/platform/esp-idf/platform_internal.h'
        source.write_text(source.read_text() + '\n/* product patch */\n')
        with self.assertRaisesRegex(ValueError, 'modified'):
            self.prepare()
        self.assertEqual(before, {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()})

    def test_output_cannot_replace_sdk_sources_or_ancestor(self):
        for output in [self.system / 'wamr', self.root.parent]:
            with self.subTest(output=output), self.assertRaisesRegex(ValueError, 'outside'):
                overlay.prepare(self.root, self.system / 'wamr', output, self.metadata, self.series)


if __name__ == '__main__':
    unittest.main()
